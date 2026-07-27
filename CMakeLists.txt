cmake_minimum_required(VERSION 3.25)
project(Phase1_Systems CXX)

# Force C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# The warning set is carried on an INTERFACE target rather than set directory-wide,
# so -Werror applies to this project's code only. A directory-wide add_compile_options
# would also be inherited by anything FetchContent pulls in, letting a dependency's
# headers fail our build.
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
  -Wall -Wextra -Werror -Wconversion -pedantic -Wsign-conversion -Wfloat-conversion)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Commands to run only for Debug builds
    add_compile_definitions(DEBUG_MODE_ENABLED)

    add_compile_options(-g -O0 -fsanitize=address)
    add_link_options(-fsanitize=address)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Commands to run only for Release builds
    add_compile_definitions(RELEASE_MODE_ENABLED)
    add_compile_options(-O3)
endif()

# ---- benchmark ---------------------------------------------------------------

add_executable(PoolAllocator
  Clock.h
  PoolAllocator.h
  benchmark.cpp
)
target_link_libraries(PoolAllocator PRIVATE project_warnings)

# ---- tests -------------------------------------------------------------------

include(FetchContent)

# SYSTEM marks doctest's include directory as a system path, which keeps our
# -Werror from being tripped by warnings inside the framework's own headers.
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.5.3
  GIT_SHALLOW    TRUE
  SYSTEM
)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(PoolAllocatorTests
  PoolAllocator.h
  Tests.h
  tests.cpp
)
target_link_libraries(PoolAllocatorTests PRIVATE project_warnings doctest::doctest)
target_include_directories(PoolAllocatorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Registers each TEST_CASE with CTest individually, so `ctest` reports per-case
# pass/fail and -R can select a single case.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(PoolAllocatorTests)
