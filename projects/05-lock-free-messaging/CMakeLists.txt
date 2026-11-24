cmake_minimum_required(VERSION 3.20)
project(lock_free_messaging VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LFM_BUILD_TESTS "Build tests" ON)
option(LFM_BUILD_BENCHMARKS "Build the throughput/latency benchmark harnesses" ON)
option(LFM_BUILD_GBENCH "Build Google Benchmark microbenchmarks (fetched from GitHub)" ON)
option(LFM_BUILD_EXAMPLES "Build examples" ON)
option(LFM_WERROR "Treat warnings as errors" ON)
option(LFM_LIBFUZZER "Build tests/fuzz_ops.cpp as a libFuzzer target" OFF)
set(LFM_SANITIZE "" CACHE STRING "Sanitizers, e.g. 'address,undefined' or 'thread'")

find_package(Threads REQUIRED)

# Build flags shared by everything in this project (not exported to users).
add_library(lfm_build_flags INTERFACE)
target_compile_options(lfm_build_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  $<$<BOOL:${LFM_WERROR}>:-Werror>)
if(LFM_SANITIZE)
  target_compile_options(lfm_build_flags INTERFACE
    -fsanitize=${LFM_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(lfm_build_flags INTERFACE -fsanitize=${LFM_SANITIZE})
endif()

# The queues are header-only; the compiled part is the affinity helper.
add_library(lfm STATIC src/affinity.cpp)
add_library(lfm::lfm ALIAS lfm)
target_include_directories(lfm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lfm PUBLIC cxx_std_20)
target_link_libraries(lfm PUBLIC Threads::Threads PRIVATE lfm_build_flags)

if(LFM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LFM_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(LFM_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
