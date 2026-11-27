cmake_minimum_required(VERSION 3.20)
project(kernel_bypass_networking_lab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KBNL_ENABLE_AF_XDP "Enable Linux AF_XDP transport adapter (requires Linux headers / libxdp)" OFF)
option(KBNL_ENABLE_DPDK "Enable DPDK transport adapter (requires DPDK pkg-config libdpdk)" OFF)
option(KBNL_BUILD_TESTS "Build unit and integration tests" ON)
option(KBNL_BUILD_BENCHMARKS "Build microbenchmarks" ON)

add_library(kbnl_core STATIC
    src/protocol.cpp
    src/clock.cpp
    src/config.cpp
    src/sequence_tracker.cpp
    src/latency_stats.cpp
    src/cpu_affinity.cpp
    src/pacer.cpp
    src/json_writer.cpp
    src/posix_udp_transport.cpp
    src/af_xdp_transport.cpp
    src/dpdk_transport.cpp
    src/benchmark_runner.cpp
)

target_include_directories(kbnl_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(kbnl_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
)

find_package(Threads REQUIRED)
target_link_libraries(kbnl_core PUBLIC Threads::Threads)

if(KBNL_ENABLE_AF_XDP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING "KBNL_ENABLE_AF_XDP requested on non-Linux platform (${CMAKE_SYSTEM_NAME}); adapter will report UnsupportedPlatform at runtime.")
    else()
        include(CheckIncludeFileCXX)
        check_include_file_cxx("linux/if_xdp.h" KBNL_HAVE_LINUX_IF_XDP_H)
        if(KBNL_HAVE_LINUX_IF_XDP_H)
            target_compile_definitions(kbnl_core PUBLIC KBNL_ENABLE_AF_XDP=1)
        else()
            message(FATAL_ERROR "KBNL_ENABLE_AF_XDP=ON but <linux/if_xdp.h> was not found.")
        endif()
    endif()
endif()

if(KBNL_ENABLE_DPDK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(WARNING "KBNL_ENABLE_DPDK requested on non-Linux platform (${CMAKE_SYSTEM_NAME}); adapter will report UnsupportedPlatform at runtime.")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET libdpdk)
        target_compile_definitions(kbnl_core PUBLIC KBNL_ENABLE_DPDK=1)
        target_link_libraries(kbnl_core PUBLIC PkgConfig::DPDK)
    endif()
endif()

add_executable(kbnl_sender src/main_sender.cpp)
target_link_libraries(kbnl_sender PRIVATE kbnl_core)

add_executable(kbnl_receiver src/main_receiver.cpp)
target_link_libraries(kbnl_receiver PRIVATE kbnl_core)

add_executable(kbnl_bench src/main_bench.cpp)
target_link_libraries(kbnl_bench PRIVATE kbnl_core)

if(KBNL_BUILD_BENCHMARKS)
    add_executable(kbnl_hotpath_bench benchmarks/bench_ring_and_tracker.cpp)
    target_link_libraries(kbnl_hotpath_bench PRIVATE kbnl_core)
endif()

if(KBNL_BUILD_TESTS)
    enable_testing()

    add_executable(test_protocol_and_config tests/test_protocol_and_config.cpp)
    target_link_libraries(test_protocol_and_config PRIVATE kbnl_core)
    target_compile_definitions(test_protocol_and_config PRIVATE KBNL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME protocol_and_config COMMAND test_protocol_and_config)

    add_executable(test_domain_sequence_and_ring tests/test_domain_sequence_and_ring.cpp)
    target_link_libraries(test_domain_sequence_and_ring PRIVATE kbnl_core)
    target_compile_definitions(test_domain_sequence_and_ring PRIVATE KBNL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME domain_sequence_and_ring COMMAND test_domain_sequence_and_ring)

    add_executable(test_adapters_and_failures tests/test_adapters_and_failures.cpp)
    target_link_libraries(test_adapters_and_failures PRIVATE kbnl_core)
    target_compile_definitions(test_adapters_and_failures PRIVATE KBNL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME adapters_and_failures COMMAND test_adapters_and_failures)

    add_executable(test_integration_loopback tests/test_integration_loopback.cpp)
    target_link_libraries(test_integration_loopback PRIVATE kbnl_core)
    target_compile_definitions(test_integration_loopback PRIVATE KBNL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME integration_loopback COMMAND test_integration_loopback)
endif()
