cmake_minimum_required(VERSION 3.20)
project(order_book_compression_engine
    VERSION 1.0.0
    DESCRIPTION "Lossless Order Book Compression Engine (L2/L3 Reference Binary & Delta/Varint with Checkpoints)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OBCE_BUILD_TESTS "Build unit and integration test suites" ON)
option(OBCE_BUILD_BENCHMARKS "Build benchmark harness" ON)
option(OBCE_ENABLE_ZSTD "Enable optional zstd compression adapter if libzstd is installed" ON)
option(OBCE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Detect optional libzstd
set(OBCE_HAVE_ZSTD 0)
if(OBCE_ENABLE_ZSTD)
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd libzstd)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
        set(OBCE_HAVE_ZSTD 1)
        message(STATUS "OBCE: Found libzstd (${ZSTD_LIBRARY}); zstd adapter enabled.")
    else()
        message(STATUS "OBCE: libzstd not found; building without optional zstd adapter.")
    endif()
endif()

# Core domain library (isolated from I/O and CLI)
add_library(obce_core STATIC
    src/crc32.cpp
    src/reference_codec.cpp
    src/delta_codec.cpp
    src/zstd_adapter.cpp
    src/fixture_generator.cpp
)

target_include_directories(obce_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(obce_core PUBLIC OBCE_HAVE_ZSTD=${OBCE_HAVE_ZSTD})

if(OBCE_HAVE_ZSTD)
    target_include_directories(obce_core PRIVATE ${ZSTD_INCLUDE_DIR})
    target_link_libraries(obce_core PUBLIC ${ZSTD_LIBRARY})
endif()

# Strict compiler warnings
if(MSVC)
    target_compile_options(obce_core PUBLIC /W4)
    if(OBCE_WARNINGS_AS_ERRORS)
        target_compile_options(obce_core PUBLIC /WX)
    endif()
else()
    target_compile_options(obce_core PUBLIC -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(OBCE_WARNINGS_AS_ERRORS)
        target_compile_options(obce_core PUBLIC -Werror)
    endif()
endif()

# I/O & serialization adapter library (kept separate from hot-path core)
add_library(obce_io STATIC
    src/io_adapter.cpp
)
target_link_libraries(obce_io PUBLIC obce_core)

# CLI tool for demo, validation, encode/decode, inspect, and recovery
add_executable(obce_cli src/cli_main.cpp)
target_link_libraries(obce_cli PRIVATE obce_io)

# Tests
if(OBCE_BUILD_TESTS)
    enable_testing()

    add_executable(test_varint_crc tests/test_varint_crc.cpp)
    target_link_libraries(test_varint_crc PRIVATE obce_core)
    add_test(NAME test_varint_crc COMMAND test_varint_crc)

    add_executable(test_reference_codec tests/test_reference_codec.cpp)
    target_link_libraries(test_reference_codec PRIVATE obce_io)
    add_test(NAME test_reference_codec COMMAND test_reference_codec --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

    add_executable(test_delta_codec tests/test_delta_codec.cpp)
    target_link_libraries(test_delta_codec PRIVATE obce_io)
    add_test(NAME test_delta_codec COMMAND test_delta_codec)

    add_executable(test_integration_resilience tests/test_integration_resilience.cpp)
    target_link_libraries(test_integration_resilience PRIVATE obce_io)
    add_test(NAME test_integration_resilience COMMAND test_integration_resilience)

    add_executable(test_operational tests/test_operational.cpp)
    target_link_libraries(test_operational PRIVATE obce_io)
    add_test(NAME test_operational COMMAND test_operational --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()

# Benchmarks
if(OBCE_BUILD_BENCHMARKS)
    add_executable(obce_bench benchmarks/bench_main.cpp)
    target_link_libraries(obce_bench PRIVATE obce_io)
    target_compile_definitions(obce_bench PRIVATE
        OBCE_BUILD_TYPE="$<CONFIG>"
        OBCE_CXX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    )
endif()
