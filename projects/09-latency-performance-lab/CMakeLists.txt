cmake_minimum_required(VERSION 3.20)
project(latency_performance_lab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LPL_BUILD_TESTS "Build unit, concurrency, and fuzz tests" ON)
option(LPL_BUILD_BENCHMARKS "Build reproducible benchmark executables" ON)
option(LPL_BUILD_GBENCH "Build optional Google Benchmark suite (uses system or FetchContent)" OFF)
option(LPL_BUILD_EXAMPLES "Build standalone examples" ON)
option(LPL_WERROR "Treat compiler warnings as errors" ON)
option(LPL_MARCH_NATIVE "Enable -march=native (machine-specific; document portability)" OFF)
option(LPL_LIBFUZZER "Build tests/test_matching_fuzz.cpp with -fsanitize=fuzzer" OFF)
set(LPL_SANITIZE "" CACHE STRING "Sanitizers, e.g. 'address,undefined' or 'thread'")

find_package(Threads REQUIRED)

add_library(lpl_build_flags INTERFACE)
target_compile_options(lpl_build_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  $<$<BOOL:${LPL_WERROR}>:-Werror>
  $<$<BOOL:${LPL_MARCH_NATIVE}>:-march=native>
)

if(LPL_SANITIZE)
  target_compile_options(lpl_build_flags INTERFACE
    -fsanitize=${LPL_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(lpl_build_flags INTERFACE -fsanitize=${LPL_SANITIZE})
endif()

add_library(lpl STATIC
  src/affinity.cpp
  src/environment.cpp
  src/perf_counters.cpp
  src/bench_harness.cpp
  src/matching_engine.cpp
)
add_library(lpl::lpl ALIAS lpl)
target_include_directories(lpl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lpl PUBLIC cxx_std_20)
target_link_libraries(lpl PUBLIC Threads::Threads PRIVATE lpl_build_flags)

# Embed build metadata macros so binaries report exact compiler/flags/build-type.
string(TOUPPER "${CMAKE_BUILD_TYPE}" LPL_BUILD_TYPE_UPPER)
string(JOIN " " LPL_EFFECTIVE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS}"
  "${CMAKE_CXX_FLAGS_${LPL_BUILD_TYPE_UPPER}}"
)
target_compile_definitions(lpl PRIVATE
  LPL_BUILD_TYPE_STR="${CMAKE_BUILD_TYPE}"
  LPL_CXX_COMPILER_ID_STR="${CMAKE_CXX_COMPILER_ID}"
  LPL_CXX_COMPILER_VER_STR="${CMAKE_CXX_COMPILER_VERSION}"
  LPL_CXX_FLAGS_STR="${LPL_EFFECTIVE_CXX_FLAGS}"
  LPL_MARCH_NATIVE_ENABLED=$<BOOL:${LPL_MARCH_NATIVE}>
)

if(LPL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LPL_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(LPL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
