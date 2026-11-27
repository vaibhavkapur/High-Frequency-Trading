cmake_minimum_required(VERSION 3.20)
project(market_microstructure_analytics_engine
    VERSION 0.1.0
    DESCRIPTION "Causal Market Microstructure Analytics Engine (L2/L3 replay, trade ambiguity, queue & flow features)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MMAE_BUILD_TESTS "Build C++ unit and integration tests" ON)
option(MMAE_BUILD_BENCHMARKS "Build baseline vs optimized benchmarks" ON)

add_library(mmae_core STATIC
    src/order_book.cpp
    src/feature_engine.cpp
    src/io_adapter.cpp
)

target_include_directories(mmae_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(mmae_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

add_executable(mmae_cli
    src/cli_main.cpp
)
target_link_libraries(mmae_cli PRIVATE mmae_core)
target_compile_options(mmae_cli PRIVATE -Wall -Wextra -Wpedantic)

if(MMAE_BUILD_BENCHMARKS)
    add_executable(mmae_bench
        benchmarks/bench_engine.cpp
    )
    target_link_libraries(mmae_bench PRIVATE mmae_core)
    target_compile_options(mmae_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MMAE_BUILD_TESTS)
    enable_testing()
    add_executable(mmae_tests
        tests/cpp/test_mmae.cpp
    )
    target_link_libraries(mmae_tests PRIVATE mmae_core)
    target_compile_options(mmae_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(
        NAME mmae_cpp_suite
        COMMAND mmae_tests --fixtures-dir ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    )
endif()
