include(FetchContent)

# ---------------------------------------------------------------------------
# xxHash: XXH3_64 for key hashing, deterministic sampling and fingerprints.
# Only the headers are used (XXH_INLINE_ALL inside src/core/hash.cpp), so the
# upstream CMake project is never configured.
# ---------------------------------------------------------------------------
FetchContent_Declare(xxhash
  URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.2.tar.gz
  URL_HASH SHA256=baee0c6afd4f03165de7a4e67988d16f0f2b257b51d0e3cb91909302a26a79c4
  SOURCE_SUBDIR do-not-configure
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(xxhash)

add_library(clache_xxhash INTERFACE)
target_include_directories(clache_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
if(CLACHE_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Google Benchmark
# ---------------------------------------------------------------------------
if(CLACHE_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(benchmark)
endif()
