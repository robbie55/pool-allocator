cmake_minimum_required(VERSION 3.23)
project(Phase1_Systems CXX)

# Force C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror -Wconversion -pedantic -Wsign-conversion -Wfloat-conversion)

# add_compile_options(
#   $<$<CONFIG:Debug>:-g -O0 -fsanitize=address>
# )
#
# add_link_options(
#   $<$<CONFIG:Debug>:-fsanitize=address>
# )

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

add_executable(PoolAllocator
  Clock.h
  PoolAllocator.h
  benchmark.cpp
)
