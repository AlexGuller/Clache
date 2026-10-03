cmake_minimum_required(VERSION 3.24)

project(clache
  VERSION 0.1.0
  DESCRIPTION "Cost-aware adaptive cache policy engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CLACHE_BUILD_TESTS "Build unit/property/integration tests" ON)
option(CLACHE_BUILD_BENCHMARKS "Build Google Benchmark micro benchmarks" ON)
option(CLACHE_CHECK_INVARIANTS "Run full invariant checks after every engine op (always on in Debug)" OFF)
option(CLACHE_WITH_REDIS "Build the Redis storage adapter (Phase 4, needs hiredis)" OFF)
option(CLACHE_WERROR "Treat compiler warnings as errors" OFF)
set(CLACHE_SANITIZE "" CACHE STRING "Sanitizers: address, undefined, address,undefined, or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)    # must come before any target is defined
include(Warnings)
include(Dependencies)

add_subdirectory(src)
add_subdirectory(apps)

if(CLACHE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CLACHE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
