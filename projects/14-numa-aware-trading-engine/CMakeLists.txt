cmake_minimum_required(VERSION 3.20)
project(numa_aware_trading_engine
    VERSION 1.0.0
    DESCRIPTION "NUMA-aware low-latency event pipeline and placement benchmark prototype"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NUMA_ENGINE_ENABLE_LIBNUMA "Enable libnuma support when available on Linux" ON)
option(NUMA_ENGINE_ENABLE_GBENCH "Build optional Google Benchmark target if installed" ON)
option(NUMA_ENGINE_BUILD_TESTS "Build unit and integration test suite" ON)
option(NUMA_ENGINE_BUILD_BENCHMARKS "Build placement benchmark harness" ON)

find_package(Threads REQUIRED)

set(NUMA_ENGINE_HAS_LIBNUMA 0)
set(NUMA_LIB "")
if(UNIX AND NOT APPLE AND NUMA_ENGINE_ENABLE_LIBNUMA)
    find_path(NUMA_INCLUDE_DIR numa.h)
    find_library(NUMA_LIB numa)
    if(NUMA_INCLUDE_DIR AND NUMA_LIB)
        set(NUMA_ENGINE_HAS_LIBNUMA 1)
        message(STATUS "Found libnuma: ${NUMA_LIB} (${NUMA_INCLUDE_DIR})")
    else()
        message(STATUS "libnuma not found; Linux builds will use syscall/fallback paths")
    endif()
endif()

add_library(numa_engine_lib STATIC
    src/topology.cpp
    src/memory.cpp
    src/affinity.cpp
    src/matching_engine.cpp
    src/event_generator.cpp
    src/perf_counters.cpp
    src/pipeline.cpp
)

target_include_directories(numa_engine_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(numa_engine_lib PUBLIC
    NUMA_ENGINE_HAS_LIBNUMA=${NUMA_ENGINE_HAS_LIBNUMA}
    NUMA_ENGINE_VERSION="${PROJECT_VERSION}"
    NUMA_ENGINE_BUILD_TYPE="$<CONFIG>"
    NUMA_ENGINE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    NUMA_ENGINE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
)

target_compile_options(numa_engine_lib PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
)

target_link_libraries(numa_engine_lib PUBLIC Threads::Threads)
if(NUMA_ENGINE_HAS_LIBNUMA)
    target_include_directories(numa_engine_lib PUBLIC ${NUMA_INCLUDE_DIR})
    target_link_libraries(numa_engine_lib PUBLIC ${NUMA_LIB})
endif()

# Main CLI executable
add_executable(numa_trading_engine src/main.cpp)
target_link_libraries(numa_trading_engine PRIVATE numa_engine_lib)
target_compile_options(numa_trading_engine PRIVATE -Wall -Wextra -Wpedantic)

# Benchmarks
if(NUMA_ENGINE_BUILD_BENCHMARKS)
    add_executable(bench_placements benchmarks/bench_placements.cpp)
    target_link_libraries(bench_placements PRIVATE numa_engine_lib)
    target_compile_options(bench_placements PRIVATE -Wall -Wextra -Wpedantic)

    if(NUMA_ENGINE_ENABLE_GBENCH)
        find_package(benchmark QUIET)
        if(benchmark_FOUND)
            message(STATUS "Found Google Benchmark; building gbench_pipeline")
            add_executable(gbench_pipeline benchmarks/gbench_pipeline.cpp)
            target_link_libraries(gbench_pipeline PRIVATE numa_engine_lib benchmark::benchmark)
            target_compile_options(gbench_pipeline PRIVATE -Wall -Wextra -Wpedantic)
        else()
            message(STATUS "Google Benchmark not found; skipping optional gbench_pipeline target")
        endif()
    endif()
endif()

# Tests
if(NUMA_ENGINE_BUILD_TESTS)
    enable_testing()

    set(TEST_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

    function(add_numa_test TARGET_NAME SOURCE_FILE)
        add_executable(${TARGET_NAME} ${SOURCE_FILE})
        target_link_libraries(${TARGET_NAME} PRIVATE numa_engine_lib)
        target_compile_options(${TARGET_NAME} PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            NUMA_ENGINE_FIXTURE_DIR="${TEST_FIXTURE_DIR}"
        )
        add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    endfunction()

    add_numa_test(test_contracts_and_fixtures tests/test_contracts_and_fixtures.cpp)
    add_numa_test(test_domain_equivalence tests/test_domain_equivalence.cpp)
    add_numa_test(test_backpressure tests/test_backpressure.cpp)
    add_numa_test(test_numa_fallback_and_topology tests/test_numa_fallback_and_topology.cpp)
endif()
