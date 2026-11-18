// Resume-grade benchmark for PoolAllocator vs global new/delete.
// Multiple trials + median to reject outliers, touches memory to defeat
// dead-code elimination, reports ns/op, throughput, and speedup.
// Both pool modes run in the same trial loop so drift hits both.
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "Clock.h"
#include "PoolAllocator.h"

struct Order {
  int id{};
  double price{};
};

static constexpr std::size_t N = 100000;  // allocations per trial
static constexpr int TRIALS = 51;         // odd -> clean median

// Returns median of a vector of doubles (sorts in place).
static double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

int main() {
  std::vector<Order*> ptrs(N);

  std::vector<double> newAlloc, newFree, poolAlloc, poolFree, unguardedAlloc, unguardedFree;
  newAlloc.reserve(TRIALS);
  newFree.reserve(TRIALS);
  poolAlloc.reserve(TRIALS);
  poolFree.reserve(TRIALS);
  unguardedAlloc.reserve(TRIALS);
  unguardedFree.reserve(TRIALS);

  // One pool reused across trials (construction cost is one-time, not per-op).
  static PoolAllocator<Order, N> pool{};
  static PoolAllocator<Order, N, false> unguarded{};

  std::uint64_t sink = 0;  // accumulates writes so nothing is optimized away

  for (int t = 0; t < TRIALS; ++t) {
    // ---- baseline: global operator new / delete ----
    Util::Timer timer{};
    for (std::size_t i = 0; i < N; ++i) {
      ptrs[i] = static_cast<Order*>(::operator new(sizeof(Order)));
      ptrs[i]->id = static_cast<int>(i);  // touch memory
    }
    newAlloc.push_back(timer.elapsed());

    timer.reset();
    for (std::size_t i = 0; i < N; ++i) {
      sink += static_cast<std::uint64_t>(ptrs[i]->id);
      ::operator delete(ptrs[i]);
    }
    newFree.push_back(timer.elapsed());

    // ---- pool allocator ----
    timer.reset();
    for (std::size_t i = 0; i < N; ++i) {
      ptrs[i] = pool.allocate();
      ptrs[i]->id = static_cast<int>(i);  // touch memory
    }
    poolAlloc.push_back(timer.elapsed());

    timer.reset();
    for (std::size_t i = 0; i < N; ++i) {
      sink += static_cast<std::uint64_t>(ptrs[i]->id);
      pool.deallocate(ptrs[i]);
    }
    poolFree.push_back(timer.elapsed());

    // ---- pool allocator, Guard = false ----
    timer.reset();
    for (std::size_t i = 0; i < N; ++i) {
      ptrs[i] = unguarded.allocate();
      ptrs[i]->id = static_cast<int>(i);  // touch memory
    }
    unguardedAlloc.push_back(timer.elapsed());

    timer.reset();
    for (std::size_t i = 0; i < N; ++i) {
      sink += static_cast<std::uint64_t>(ptrs[i]->id);
      unguarded.deallocate(ptrs[i]);
    }
    unguardedFree.push_back(timer.elapsed());
  }

  const double mNewAlloc = median(newAlloc);
  const double mNewFree = median(newFree);
  const double mPoolAlloc = median(poolAlloc);
  const double mPoolFree = median(poolFree);
  const double mUnAlloc = median(unguardedAlloc);
  const double mUnFree = median(unguardedFree);

  auto nsPerOp = [](double seconds) { return seconds / N * 1e9; };
  auto opsPerSec = [](double seconds) { return N / seconds; };

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "PoolAllocator benchmark\n";
  std::cout << "  payload: Order{int,double} = " << sizeof(Order) << " bytes\n";
  std::cout << "  allocations per trial: " << N << "\n";
  std::cout << "  trials: " << TRIALS << " (median reported)\n\n";

  auto row = [&](char const* label, double seconds) {
    std::cout << label << std::setw(8) << nsPerOp(seconds) << std::setw(13)
              << opsPerSec(seconds) / 1e6 << std::setw(16) << seconds * 1e3 << "\n";
  };

  std::cout << "                                    ns/op      M ops/sec     median ms/trial\n";
  row("  new   allocate:                ", mNewAlloc);
  row("  new   deallocate:              ", mNewFree);
  row("  pool  allocate (guard on):     ", mPoolAlloc);
  row("  pool  deallocate (guard on):   ", mPoolFree);
  row("  pool  allocate (guard off):    ", mUnAlloc);
  row("  pool  deallocate (guard off):  ", mUnFree);
  std::cout << "\n";

  std::cout << "  speedup (allocate, guard on):     " << mNewAlloc / mPoolAlloc << "x\n";
  std::cout << "  speedup (deallocate, guard on):   " << mNewFree / mPoolFree << "x\n";
  std::cout << "  speedup (alloc+free, guard on):   "
            << (mNewAlloc + mNewFree) / (mPoolAlloc + mPoolFree) << "x\n";
  std::cout << "  speedup (allocate, guard off):    " << mNewAlloc / mUnAlloc << "x\n";
  std::cout << "  speedup (deallocate, guard off):  " << mNewFree / mUnFree << "x\n";
  std::cout << "  speedup (alloc+free, guard off):  "
            << (mNewAlloc + mNewFree) / (mUnAlloc + mUnFree) << "x\n";
  std::cout << "  guard cost: " << nsPerOp(mPoolAlloc + mPoolFree) - nsPerOp(mUnAlloc + mUnFree)
            << " ns per alloc+free pair\n";

  // Make sink observable so the optimizer can't drop the loops.
  if (sink == 0xFFFFFFFFFFFFFFFFULL) std::cout << "";
  return 0;
}
