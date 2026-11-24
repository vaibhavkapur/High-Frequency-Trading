cmake_minimum_required(VERSION 3.20)
project(matching_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ME_BUILD_TESTS "Build the test suite" ON)
option(ME_BUILD_BENCHMARKS "Build the benchmark harnesses" ON)
option(ME_BUILD_FUZZERS "Build libFuzzer targets (requires a clang with libFuzzer)" OFF)
option(ME_WERROR "Treat compiler warnings as errors" OFF)
set(ME_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread'")

# Warnings and sanitizers apply to this project's targets only, not to fetched dependencies.
add_library(me_options INTERFACE)
target_compile_options(me_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(ME_WERROR)
  target_compile_options(me_options INTERFACE -Werror)
endif()
if(ME_SANITIZE)
  target_compile_options(me_options INTERFACE -fsanitize=${ME_SANITIZE} -fno-omit-frame-pointer
                                              -fno-sanitize-recover=all)
  target_link_options(me_options INTERFACE -fsanitize=${ME_SANITIZE})
endif()

# Header-only core: types, wire protocol, both books and the engine.
add_library(me_core INTERFACE)
target_include_directories(me_core INTERFACE include)

# Support code that is not on the hot path: text formats and the workload generator.
add_library(me_support STATIC src/text_format.cpp src/workload.cpp)
target_link_libraries(me_support PUBLIC me_core PRIVATE me_options)

# Counting operator new/delete. Link only into binaries that measure allocations.
add_library(me_alloc_counter STATIC src/alloc_counter.cpp)
target_link_libraries(me_alloc_counter PUBLIC me_core PRIVATE me_options)

add_executable(me_cli src/cli_main.cpp)
target_link_libraries(me_cli PRIVATE me_support me_options)

include(FetchContent)

if(ME_BUILD_TESTS)
  enable_testing()
  find_package(GTest 1.12 QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)

  add_executable(me_tests
    tests/test_matching.cpp
    tests/test_validation.cpp
    tests/test_differential.cpp
    tests/test_protocol.cpp)
  target_compile_definitions(me_tests PRIVATE ME_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  target_link_libraries(me_tests PRIVATE me_support me_options GTest::gtest_main)
  gtest_discover_tests(me_tests DISCOVERY_TIMEOUT 60)

  # Separate binary: replacing operator new must not leak into the other tests.
  add_executable(me_alloc_tests tests/test_alloc.cpp)
  target_link_libraries(me_alloc_tests PRIVATE me_support me_alloc_counter me_options GTest::gtest_main)
  gtest_discover_tests(me_alloc_tests DISCOVERY_TIMEOUT 60)

  # End-to-end check of the CLI: gen -> replay (x2, both books) -> byte comparison.
  add_test(NAME cli_replay_is_byte_identical
    COMMAND ${CMAKE_COMMAND}
      -DME_CLI=$<TARGET_FILE:me_cli>
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli_test
      -DEXAMPLES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_replay_test.cmake)
endif()

if(ME_BUILD_BENCHMARKS)
  find_package(benchmark QUIET)
  if(NOT benchmark_FOUND)
    FetchContent_Declare(googlebenchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
      URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
  endif()

  # Google Benchmark microbenchmarks: add / cancel / aggressive match vs. book depth.
  add_executable(me_bench benchmarks/bench_book.cpp)
  target_link_libraries(me_bench PRIVATE me_support me_options benchmark::benchmark)

  # Stand-alone harness: per-operation latency percentiles, throughput, allocations per event.
  add_executable(me_latency benchmarks/latency_main.cpp)
  target_link_libraries(me_latency PRIVATE me_support me_alloc_counter me_options)
endif()

if(ME_BUILD_FUZZERS)
  add_executable(me_fuzz_engine tests/fuzz_engine.cpp)
  target_link_libraries(me_fuzz_engine PRIVATE me_support me_options)
  target_compile_options(me_fuzz_engine PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(me_fuzz_engine PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
