#pragma once

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <set>
#include <type_traits>
#include <vector>

#include "PoolAllocator.h"

namespace Tests {

  // ---------------------------------------------------------------------------
  // Payload types under test.
  //
  //   Order : the benchmark's payload -- 16 bytes, 8-byte aligned. The easy case.
  //   Wide  : over-aligned. Broken by any pool that allocates its buffer with
  //           plain new[], which only guarantees __STDCPP_DEFAULT_NEW_ALIGNMENT__.
  //   Odd   : sizeof 12 / alignof 1. The awkward stride -- if BLOCK_SIZE is not
  //           rounded up to ALIGNMENT, blocks 1, 3, 5... land four bytes off a
  //           pointer boundary and the intrusive Node* write is misaligned.
  // ---------------------------------------------------------------------------
  struct Order {
    int id{};
    double price{};
  };

  struct alignas(64) Wide {
    double v[8];
  };

  struct Odd {
    char c[12];
  };

  // A block must be legally usable as a T *and* as a free-list node, and the
  // node stores a pointer -- so the real requirement is the stricter of the two.
  template <typename T>
  constexpr std::size_t requiredAlign() {
    return std::max(alignof(T), alignof(void*));
  }

  template <typename T>
  bool isAligned(T* ptr) {
    return reinterpret_cast<std::uintptr_t>(ptr) % requiredAlign<T>() == 0;
  }

  // Drain a pool completely, returning every block handed out.
  template <typename T, std::size_t N>
  std::vector<T*> drain(PoolAllocator<T, N>& pool) {
    std::vector<T*> blocks;
    blocks.reserve(N);
    for (std::size_t i{0}; i < N; ++i) blocks.push_back(pool.allocate());
    return blocks;
  }

  template <typename T, std::size_t N>
  void release(PoolAllocator<T, N>& pool, std::vector<T*> const& blocks) {
    for (T* p : blocks) pool.deallocate(p);
  }

#define POOL_TYPES char, int, float, double, Order, Wide, Odd

  // ---------------------------------------------------------------------------
  // Alignment
  // ---------------------------------------------------------------------------

  TEST_CASE_TEMPLATE("every block in a full pool is correctly aligned", T, POOL_TYPES) {
    constexpr std::size_t N{16};
    PoolAllocator<T, N> pool{};

    const std::vector<T*> blocks{drain(pool)};

    for (std::size_t i{0}; i < N; ++i) {
      REQUIRE(blocks[i] != nullptr);
      // Checked for every block, not just the first -- aligning the buffer alone
      // fixes block 0 while leaving the stride free to drift.
      CHECK(isAligned(blocks[i]));
    }

    release(pool, blocks);
  }

  TEST_CASE("an over-aligned type really does get its 64-byte boundary") {
    PoolAllocator<Wide, 8> pool{};
    const std::vector<Wide*> blocks{drain(pool)};

    for (Wide* p : blocks) CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);

    release(pool, blocks);
  }

  // ---------------------------------------------------------------------------
  // Block layout
  // ---------------------------------------------------------------------------

  TEST_CASE_TEMPLATE("blocks are distinct and never overlap", T, POOL_TYPES) {
    constexpr std::size_t N{16};
    PoolAllocator<T, N> pool{};

    const std::vector<T*> blocks{drain(pool)};

    std::vector<std::uintptr_t> addrs;
    addrs.reserve(N);
    for (T* p : blocks) addrs.push_back(reinterpret_cast<std::uintptr_t>(p));
    std::sort(addrs.begin(), addrs.end());

    CHECK(std::adjacent_find(addrs.begin(), addrs.end()) == addrs.end());

    for (std::size_t i{1}; i < addrs.size(); ++i)
      CHECK(addrs[i] - addrs[i - 1] >= sizeof(T));

    release(pool, blocks);
  }

  TEST_CASE("live blocks do not alias -- writes stay independent") {
    constexpr std::size_t N{64};
    PoolAllocator<Order, N> pool{};

    const std::vector<Order*> blocks{drain(pool)};

    for (std::size_t i{0}; i < N; ++i) {
      blocks[i]->id = static_cast<int>(i);
      blocks[i]->price = static_cast<double>(i) * 1.5;
    }

    for (std::size_t i{0}; i < N; ++i) {
      CHECK(blocks[i]->id == static_cast<int>(i));
      CHECK(blocks[i]->price == doctest::Approx(static_cast<double>(i) * 1.5));
    }

    release(pool, blocks);
  }

  // ---------------------------------------------------------------------------
  // Capacity and exhaustion
  // ---------------------------------------------------------------------------

  TEST_CASE("a pool yields exactly BlockCount blocks, then throws") {
    constexpr std::size_t N{8};
    PoolAllocator<Order, N> pool{};

    std::vector<Order*> blocks{drain(pool)};

    CHECK_THROWS_AS(pool.allocate(), std::bad_alloc);
    CHECK_THROWS_AS(pool.allocate(), std::bad_alloc);  // and it stays exhausted

    // Returning one block buys exactly one more allocation.
    pool.deallocate(blocks.back());
    blocks.pop_back();

    Order* revived{nullptr};
    REQUIRE_NOTHROW(revived = pool.allocate());
    CHECK(revived != nullptr);
    blocks.push_back(revived);

    CHECK_THROWS_AS(pool.allocate(), std::bad_alloc);

    release(pool, blocks);
  }

  TEST_CASE("a single-block pool behaves") {
    PoolAllocator<Order, 1> pool{};

    Order* only{pool.allocate()};
    REQUIRE(only != nullptr);
    CHECK(isAligned(only));
    CHECK_THROWS_AS(pool.allocate(), std::bad_alloc);

    pool.deallocate(only);
    CHECK(pool.allocate() == only);
  }

  // ---------------------------------------------------------------------------
  // Free-list semantics
  // ---------------------------------------------------------------------------

  TEST_CASE("the free list is LIFO -- the most recently freed block returns first") {
    PoolAllocator<int, 3> pool{};

    int* a{pool.allocate()};
    int* b{pool.allocate()};
    int* c{pool.allocate()};

    // Freeing the middle block and immediately reallocating must hand it back:
    // this is what keeps a recycling workload's memory hot in L1.
    pool.deallocate(b);
    CHECK(pool.allocate() == b);

    // Two frees, then two allocations, must unwind in reverse order -- LIFO,
    // not FIFO. Freeing c then a means a comes back first.
    pool.deallocate(c);
    pool.deallocate(a);
    CHECK(pool.allocate() == a);
    CHECK(pool.allocate() == c);

    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);
  }

  TEST_CASE("a full drain/release cycle loses and duplicates nothing") {
    constexpr std::size_t N{16};
    PoolAllocator<Order, N> pool{};

    auto addressSet = [&pool] {
      const std::vector<Order*> blocks{drain(pool)};
      std::set<std::uintptr_t> addrs;
      for (Order* p : blocks) addrs.insert(reinterpret_cast<std::uintptr_t>(p));
      release(pool, blocks);
      return addrs;
    };

    const std::set<std::uintptr_t> first{addressSet()};
    const std::set<std::uintptr_t> second{addressSet()};

    CHECK(first.size() == N);
    CHECK(first == second);
  }

  // ---------------------------------------------------------------------------
  // Ownership
  // ---------------------------------------------------------------------------

  TEST_CASE("the pool is a non-transferable resource owner") {
    using P = PoolAllocator<Order, 4>;

    // Enforced at compile time; the runtime CHECKs exist so the guarantee is
    // visible in the ctest output rather than silently absent.
    static_assert(!std::is_copy_constructible_v<P>);
    static_assert(!std::is_copy_assignable_v<P>);
    static_assert(!std::is_move_constructible_v<P>);
    static_assert(!std::is_move_assignable_v<P>);

    CHECK_FALSE(std::is_copy_constructible_v<P>);
    CHECK_FALSE(std::is_copy_assignable_v<P>);
    CHECK_FALSE(std::is_move_constructible_v<P>);
    CHECK_FALSE(std::is_move_assignable_v<P>);
  }

#undef POOL_TYPES

}  // namespace Tests
