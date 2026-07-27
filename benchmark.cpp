// Resume-grade benchmark for PoolAllocator vs global new/delete.
// Multiple trials + median to reject outliers, touches memory to defeat
// dead-code elimination, reports ns/op, throughput, and speedup.
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

  std::vector<double> newAlloc, newFree, poolAlloc, poolFree;
  newAlloc.reserve(TRIALS);
  newFree.reserve(TRIALS);
  poolAlloc.reserve(TRIALS);
  poolFree.reserve(TRIALS);

  // One pool reused across trials (construction cost is one-time, not per-op).
  static PoolAllocator<Order, N> pool{};

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
  }

  const double mNewAlloc = median(newAlloc);
  const double mNewFree = median(newFree);
  const double mPoolAlloc = median(poolAlloc);
  const double mPoolFree = median(poolFree);

  auto nsPerOp = [](double seconds) { return seconds / N * 1e9; };
  auto opsPerSec = [](double seconds) { return N / seconds; };

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "PoolAllocator benchmark\n";
  std::cout << "  payload: Order{int,double} = " << sizeof(Order) << " bytes\n";
  std::cout << "  allocations per trial: " << N << "\n";
  std::cout << "  trials: " << TRIALS << " (median reported)\n\n";

  std::cout << "                       ns/op      M ops/sec     median ms/trial\n";
  std::cout << "  new   allocate:   " << std::setw(8) << nsPerOp(mNewAlloc) << std::setw(13)
            << opsPerSec(mNewAlloc) / 1e6 << std::setw(16) << mNewAlloc * 1e3 << "\n";
  std::cout << "  new   deallocate: " << std::setw(8) << nsPerOp(mNewFree) << std::setw(13)
            << opsPerSec(mNewFree) / 1e6 << std::setw(16) << mNewFree * 1e3 << "\n";
  std::cout << "  pool  allocate:   " << std::setw(8) << nsPerOp(mPoolAlloc) << std::setw(13)
            << opsPerSec(mPoolAlloc) / 1e6 << std::setw(16) << mPoolAlloc * 1e3 << "\n";
  std::cout << "  pool  deallocate: " << std::setw(8) << nsPerOp(mPoolFree) << std::setw(13)
            << opsPerSec(mPoolFree) / 1e6 << std::setw(16) << mPoolFree * 1e3 << "\n\n";

  std::cout << "  speedup (allocate):    " << mNewAlloc / mPoolAlloc << "x\n";
  std::cout << "  speedup (deallocate):  " << mNewFree / mPoolFree << "x\n";
  std::cout << "  speedup (alloc+free):  " << (mNewAlloc + mNewFree) / (mPoolAlloc + mPoolFree)
            << "x\n";

  // Make sink observable so the optimizer can't drop the loops.
  if (sink == 0xFFFFFFFFFFFFFFFFULL) std::cout << "";
  return 0;
}
