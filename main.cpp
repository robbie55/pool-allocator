// Scratch driver for poking at the pool by hand. The real tests live in
// Tests.h and run via `ctest`; this file is not part of any CMake target.
#include <cstdlib>

#include "PoolAllocator.h"

int main(void) {
  PoolAllocator<int, 5> testPool{};

  int* testPtr{testPool.allocate()};

  std::cout << testPtr << '\n';

  return 0;
}
