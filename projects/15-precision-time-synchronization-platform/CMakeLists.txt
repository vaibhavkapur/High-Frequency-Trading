cmake_minimum_required(VERSION 3.20)
project(precision_time_synchronization_platform
    VERSION 0.1.0
    DESCRIPTION "Precision Time Synchronization Platform — clock provenance, robust drift/offset estimation, timestamp adapters, and fail-closed ordering"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PTSP_ENABLE_LINUX_PTP "Enable Linux SO_TIMESTAMPING / /dev/ptp* hardware probe support (Linux only)" ON)
option(PTSP_BUILD_TESTS "Build C++ unit, integration, and failure tests" ON)
option(PTSP_BUILD_BENCHMARKS "Build baseline vs optimized microbenchmarks" ON)

add_library(ptsp_core STATIC
    src/contracts.cpp
    src/estimator.cpp
    src/fail_closed_orderer.cpp
    src/adapters.cpp
    src/io.cpp
    src/runner.cpp
)

target_include_directories(ptsp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(ptsp_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
)

find_package(Threads REQUIRED)
target_link_libraries(ptsp_core PUBLIC Threads::Threads)

if(PTSP_ENABLE_LINUX_PTP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckIncludeFileCXX)
    check_include_file_cxx("linux/net_tstamp.h" PTSP_HAVE_LINUX_NET_TSTAMP_H)
    check_include_file_cxx("linux/ptp_clock.h" PTSP_HAVE_LINUX_PTP_CLOCK_H)
    if(PTSP_HAVE_LINUX_NET_TSTAMP_H)
        target_compile_definitions(ptsp_core PUBLIC PTSP_HAVE_LINUX_NET_TSTAMP_H=1)
    endif()
    if(PTSP_HAVE_LINUX_PTP_CLOCK_H)
        target_compile_definitions(ptsp_core PUBLIC PTSP_HAVE_LINUX_PTP_CLOCK_H=1)
    endif()
endif()

add_executable(ptsp_cli src/main_cli.cpp)
target_link_libraries(ptsp_cli PRIVATE ptsp_core)
target_compile_options(ptsp_cli PRIVATE -Wall -Wextra -Wpedantic)

if(PTSP_BUILD_BENCHMARKS)
    add_executable(ptsp_bench benchmarks/bench_ptsp.cpp)
    target_link_libraries(ptsp_bench PRIVATE ptsp_core)
    target_compile_options(ptsp_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PTSP_BUILD_TESTS)
    enable_testing()

    set(PTSP_TEST_TARGETS
        test_contracts_and_validation
        test_estimator_domain
        test_integration_and_ordering
        test_adapters_and_hw_failure
    )

    foreach(target IN LISTS PTSP_TEST_TARGETS)
        add_executable(${target} tests/${target}.cpp)
        target_link_libraries(${target} PRIVATE ptsp_core)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_definitions(${target} PRIVATE PTSP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
endif()
