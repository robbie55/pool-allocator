#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>

template <typename T, std::size_t BlockCount>
class PoolAllocator {
  static_assert(BlockCount > 0, "a pool needs at least one block");

 public:
  PoolAllocator();
  PoolAllocator(PoolAllocator const& other) = delete;
  PoolAllocator& operator=(PoolAllocator const& rhs) = delete;
  PoolAllocator(PoolAllocator&& other) = delete;
  PoolAllocator& operator=(PoolAllocator&& rhs) = delete;
  ~PoolAllocator();

  T* allocate();
  T* tryAllocate() noexcept;

  void deallocate(T* ptr) noexcept;

  // Total blocks, fixed when the type is instantiated. Callers can size their
  // own arrays with it or check it in a static_assert.
  static constexpr std::size_t capacity() noexcept { return BlockCount; }

  // Free blocks and handed-out blocks. Both read m_free_count, which allocate
  // and deallocate keep up to date. Counting the free list instead would be
  // O(n) per call.
  std::size_t available() const noexcept { return m_free_count; }
  std::size_t used() const noexcept { return BlockCount - m_free_count; }
  bool empty() const noexcept { return m_free_count == BlockCount; }
  bool full() const noexcept { return m_free_count == 0; }

 private:
  struct Node {
    Node* next{};
  };

  void printMemLayout();

  // A block must satisfy BOTH T's alignment and Node's (free blocks hold a Node*).
  static constexpr std::size_t ALIGNMENT{std::max(alignof(T), alignof(Node))};

  // Widen for types smaller than a pointer, then round the stride up to ALIGNMENT
  // so every block lands on an aligned address.
  static constexpr std::size_t MIN_BLOCK{std::max(sizeof(T), sizeof(Node))};
  static constexpr std::size_t BLOCK_SIZE{(MIN_BLOCK + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT};

  static constexpr std::size_t POOL_SIZE{BlockCount * BLOCK_SIZE};

  // ---------------------------------------------------------------------------
  // Double-free guard.
  //
  // An assert against the free-list head only catches deallocate(p) twice in a
  // row. Free a, free b, free a walks straight past it. Walking the whole free
  // list catches every case but costs O(n) on the hot path.
  //
  // Instead, one bit per block, set while that block is handed out. That is
  // ceil(BlockCount/8) bytes, so a 1024-block pool spends 128 bytes and sits in
  // L1 beside the head pointer. Each call pays a bounds compare, a bit test and
  // a bit write, and the branch predicts one way in normal use. The same
  // arithmetic rejects foreign and interior pointers.
  // ---------------------------------------------------------------------------
  static constexpr std::size_t LIVE_BYTES{(BlockCount + 7) / 8};

  bool isLive(std::size_t idx) const noexcept {
    return (m_live[idx >> 3] & bitFor(idx)) != std::byte{0};
  }
  void markLive(std::size_t idx) noexcept { m_live[idx >> 3] |= bitFor(idx); }
  void markFree(std::size_t idx) noexcept { m_live[idx >> 3] &= ~bitFor(idx); }

  static constexpr std::byte bitFor(std::size_t idx) noexcept {
    return static_cast<std::byte>(1u << (idx & 7u));
  }

  // Returns ptr's block index, or BlockCount if ptr did not come out of this
  // pool at a block boundary.
  //
  // This compares integers instead of subtracting pointers. Subtracting a
  // pointer that is not into m_pool is undefined behaviour, and those are the
  // pointers we need to catch.
  std::size_t indexOf(T* ptr) const noexcept {
    std::uintptr_t const addr{reinterpret_cast<std::uintptr_t>(ptr)};
    std::uintptr_t const base{reinterpret_cast<std::uintptr_t>(m_pool)};

    if (addr < base || addr - base >= POOL_SIZE) return BlockCount;

    std::size_t const offset{static_cast<std::size_t>(addr - base)};

    // BLOCK_SIZE is a compile-time constant, so the compiler lowers both of
    // these to a shift or a multiply-high, never a hardware divide.
    if (offset % BLOCK_SIZE != 0) return BlockCount;

    return offset / BLOCK_SIZE;
  }

  [[noreturn]] static void onBadFree(T* ptr, char const* why) noexcept {
    std::fprintf(stderr, "PoolAllocator: %s at %p\n", why, static_cast<void*>(ptr));
    std::abort();
  }

  std::byte* m_pool{};

  void* m_free_list_head{};

  std::size_t m_free_count{};

  std::byte m_live[LIVE_BYTES]{};
};

template <typename T, std::size_t BlockCount>
void PoolAllocator<T, BlockCount>::printMemLayout() {
  Node* p{reinterpret_cast<Node*>(m_free_list_head)};
  while (p && p->next) {
    std::uintptr_t curAddr{reinterpret_cast<std::uintptr_t>(p)};
    std::uintptr_t nextAddr{reinterpret_cast<std::uintptr_t>(p->next)};

    std::cout << "Printing ADDR dif: " << nextAddr - curAddr << '\n';

    p = p->next;
  }
}

template <typename T, std::size_t BlockCount>
void PoolAllocator<T, BlockCount>::deallocate(T* ptr) noexcept {
  // though this function is called deallocate, the goal is to reinsert the block back into the
  // memory pool

  // A null free is a no-op, matching ::operator delete.
  if (ptr == nullptr) return;

  std::size_t const idx{indexOf(ptr)};

  // Not one of our blocks, or a pointer into the middle of one. Either way it
  // must not go on the free list. A later allocate() would hand it straight
  // back out and the crash would land somewhere unrelated.
  if (idx == BlockCount) onBadFree(ptr, "deallocate of a foreign or misaligned pointer");

  // The block is already on the free list. Pushing it again splices the list
  // into a cycle, and allocate() then hands the same address out twice.
  if (!isLive(idx)) onBadFree(ptr, "double deallocate");

  markFree(idx);

  Node* newHead{reinterpret_cast<Node*>(ptr)};
  newHead->next = reinterpret_cast<Node*>(m_free_list_head);
  m_free_list_head = newHead;

  ++m_free_count;
}

template <typename T, std::size_t BlockCount>
T* PoolAllocator<T, BlockCount>::allocate() {
  T* block{tryAllocate()};

  // if we try to allocate when we're empty, throw
  if (block == nullptr) throw std::bad_alloc();

  return block;
}

template <typename T, std::size_t BlockCount>
T* PoolAllocator<T, BlockCount>::tryAllocate() noexcept {
  // if we try to allocate when we're empty, hand back null
  if (m_free_list_head == nullptr) {
    return nullptr;
  };

  // get next node to assign as new head
  Node* head{reinterpret_cast<Node*>(m_free_list_head)};
  m_free_list_head = head->next;

  // The free list should never hand out a block that is already live. If it
  // does, something has corrupted the list, most likely a stray write through
  // a freed pointer, and carrying on would alias two live objects.
  std::size_t const idx{indexOf(reinterpret_cast<T*>(head))};
  if (idx == BlockCount || isLive(idx))
    onBadFree(reinterpret_cast<T*>(head), "free list corrupted");

  markLive(idx);
  --m_free_count;

  return reinterpret_cast<T*>(head);
}

template <typename T, std::size_t BlockCount>
PoolAllocator<T, BlockCount>::PoolAllocator() {
  static_assert(BLOCK_SIZE % ALIGNMENT == 0, "block stride must preserve alignment");

  m_pool = static_cast<std::byte*>(::operator new(POOL_SIZE, std::align_val_t{ALIGNMENT}));

  m_free_list_head = reinterpret_cast<void*>(m_pool);

  // Every block starts on the free list. m_live is all-zero from its default
  // member initialiser, so no block reads as handed out yet.
  m_free_count = BlockCount;

  for (size_t i{0}; i < BlockCount - 1; ++i) {
    // calc address of current block, in bytes
    std::byte* curBytes{&m_pool[i * BLOCK_SIZE]};

    // get next block
    std::byte* nextBytes{&m_pool[(i + 1) * BLOCK_SIZE]};

    // tell compiler to treat a group of bytes as a node
    Node* curNode{reinterpret_cast<Node*>(curBytes)};

    // tell compiler to treat the next group of bytes as a node as well
    Node* next{reinterpret_cast<Node*>(nextBytes)};

    // write the address of next inside the first block of nodes
    curNode->next = next;
  }

  // for final byte, write nullptr inside so we know its the end
  std::byte* lastBytes{&m_pool[(BlockCount - 1) * BLOCK_SIZE]};
  Node* lastNode{reinterpret_cast<Node*>(lastBytes)};

  lastNode->next = nullptr;

  // printMemLayout();
}

template <typename T, std::size_t BlockCount>
PoolAllocator<T, BlockCount>::~PoolAllocator() {
  m_free_list_head = nullptr;
  // delete[] on aligned - operator new storage is undefined behaviour
  // ::operator delete(m_pool, POOL_SIZE, std::align_val_t{ALIGNMENT});
  ::operator delete(m_pool, std::align_val_t{ALIGNMENT});
}
