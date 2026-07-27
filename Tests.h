#include <vector>

#include "Clock.h"
#include "PoolAllocator.h"

namespace Tests {
  struct Order {
    int id{};
    double price{};
  };

  inline void testMemLeaks() {
    // initialize pool then let function end, sending out of scope, should deallcoate
    [[maybe_unused]] PoolAllocator<Order, 10UL> orderPool{};  // allocate 10 blocks of order memory
    orderPool.~PoolAllocator();
    system("leaks PoolAllocator");
  }

  template <typename T>
  void testAllignment(T* ptr) {
    std::string type_str{typeid(T).name()};

    std::cout << " -- Testing aligned for type: " << type_str << '\n';
    if (reinterpret_cast<std::uintptr_t>(ptr) % alignof(T) == 0)
      std::cout << " -- Test PASSED for " << type_str << " pool -- \n";
    else
      std::cout << " -- Test FAILED for " << type_str << " pool -- \n";

    std::cout << '\n';
  }

  inline void testAllignmentTypes() {
    // test the alignment of the memory addresses returned by the pool
    PoolAllocator<int, 1> intPool{};
    int* intPtr{intPool.allocate()};
    testAllignment(intPtr);
    intPool.deallocate(intPtr);

    PoolAllocator<char, 1> charPool{};
    char* charPtr{charPool.allocate()};
    testAllignment(charPtr);
    charPool.deallocate(charPtr);

    PoolAllocator<float, 1> floatPool{};
    float* floatPtr{floatPool.allocate()};
    testAllignment(floatPtr);
    floatPool.deallocate(floatPtr);

    PoolAllocator<double, 1> doublePool{};
    double* doublePtr{doublePool.allocate()};
    testAllignment(doublePtr);
    doublePool.deallocate(doublePtr);

    PoolAllocator<Order, 1> orderPool{};
    Order* orderPtr{orderPool.allocate()};
    testAllignment(orderPtr);
    orderPool.deallocate(orderPtr);
  }

  inline void testLifoBehavior() {
    // test that the pool behaves like a stack
    // allocate three blocks
    PoolAllocator<int, 3> pool{};

    std::cout << " -- Allocating three blocks (A, B, C) with pool -- \n";

    [[maybe_unused]] int* blockA{pool.allocate()};
    [[maybe_unused]] int* blockB{pool.allocate()};
    [[maybe_unused]] int* blockC{pool.allocate()};

    std::cout << " -- Saving address of Block B to local var -- \n";
    std::uintptr_t blockBAddr{reinterpret_cast<std::uintptr_t>(blockB)};

    // deallocate block b
    std::cout << " -- Deallocating Block B -- \n";
    pool.deallocate(blockB);

    // reallocate a new block, should have the same address of block b
    std::cout << " -- Reallocating new Block -- \n";
    int* newBlock{pool.allocate()};

    std::uintptr_t newBlockAddr{reinterpret_cast<std::uintptr_t>(newBlock)};

    if (blockBAddr == newBlockAddr)
      std::cout << " -- PASS: New Block and original Block B contain the same address -- \n";
    else
      std::cout << " -- FAILED: New Block and original Block B have different addresses -- \n";
  }

  inline void benchmarkPool() {
    constexpr size_t BENCHMARK_SIZE{100000};

    PoolAllocator<Order, BENCHMARK_SIZE> orderPool{};

    std::vector<Order*> vec(BENCHMARK_SIZE);

    std::cout << " -- Allocating 100,000 Orders with new -- \n";
    // begin timing
    Util::Timer time{};
    for (size_t i{}; i < BENCHMARK_SIZE; ++i) {
      vec[i] = static_cast<Order*>(::operator new(sizeof(Order)));
    }

    std::cout << " -- Allocation time: " << time.elapsed() << '\n';

    time.reset();
    std::cout << " -- Deallocating 100,000 Orders with delete -- \n";
    for (size_t i{}; i < BENCHMARK_SIZE; ++i) {
      delete vec[i];
    }
    std::cout << " -- Deallocation time: " << time.elapsed() << '\n';

    std::cout << '\n';

    std::cout << " -- Allocating 100,000 Orders with pool -- \n";

    time.reset();
    for (size_t i{}; i < BENCHMARK_SIZE; ++i) {
      vec[i] = orderPool.allocate();
    }

    std::cout << " -- Allocation time: " << time.elapsed() << '\n';

    time.reset();
    std::cout << " -- Deallocating 100,000 Orders with pool -- \n";
    for (size_t i{}; i < BENCHMARK_SIZE; ++i) {
      orderPool.deallocate(vec[i]);
    }
    std::cout << " -- Deallocation time: " << time.elapsed() << '\n';
  }
};  // namespace Tests
