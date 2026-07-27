#include <cstdlib>

#include "PoolAllocator.h"
#include "Tests.h"

int main(void) {
  // Tests::testLifoBehavior();
  // Tests::benchmarkPool();

  PoolAllocator<int, 5> testPool{};

  int* testPtr{testPool.allocate()};

  std::cout << testPtr << '\n';

  return 0;
}
