cmake_minimum_required(VERSION 3.20)
project(udp_multicast_feed VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MDF_BUILD_TESTS "Build unit, property, and integration tests" ON)
option(MDF_BUILD_BENCHMARKS "Build throughput, latency, and recovery benchmarks" ON)
option(MDF_BUILD_EXAMPLES "Build demo and CLI tools" ON)
option(MDF_BUILD_FUZZERS "Build libFuzzer targets (requires clang with libFuzzer)" OFF)
option(MDF_WERROR "Treat compiler warnings as errors" ON)
set(MDF_SANITIZE "" CACHE STRING "Sanitizers to enable: e.g. 'address,undefined' or 'thread'")

find_package(Threads REQUIRED)

add_library(mdf_options INTERFACE)
target_compile_options(mdf_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${MDF_WERROR}>:-Werror>)
if(MDF_SANITIZE)
  target_compile_options(mdf_options INTERFACE
    -fsanitize=${MDF_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(mdf_options INTERFACE -fsanitize=${MDF_SANITIZE})
endif()

add_library(mdf_core STATIC
  src/protocol.cpp
  src/order_book.cpp
  src/fault_injector.cpp
  src/socket_utils.cpp
  src/multicast_transport.cpp
  src/recovery_service.cpp
  src/channel_receiver.cpp
  src/exchange_simulator.cpp)
add_library(mdf::core ALIAS mdf_core)
target_include_directories(mdf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mdf_core PUBLIC cxx_std_20)
target_link_libraries(mdf_core PUBLIC Threads::Threads PRIVATE mdf_options)

if(MDF_BUILD_EXAMPLES)
  add_executable(mdf_demo examples/demo.cpp)
  target_link_libraries(mdf_demo PRIVATE mdf::core mdf_options)

  add_executable(mdf_publisher examples/publisher_main.cpp)
  target_link_libraries(mdf_publisher PRIVATE mdf::core mdf_options)

  add_executable(mdf_subscriber examples/subscriber_main.cpp)
  target_link_libraries(mdf_subscriber PRIVATE mdf::core mdf_options)
endif()

if(MDF_BUILD_BENCHMARKS)
  add_executable(mdf_codec_bench benchmarks/codec_bench.cpp)
  target_link_libraries(mdf_codec_bench PRIVATE mdf::core mdf_options)

  add_executable(mdf_multicast_bench benchmarks/multicast_bench.cpp)
  target_link_libraries(mdf_multicast_bench PRIVATE mdf::core mdf_options)
endif()

if(MDF_BUILD_FUZZERS)
  add_executable(fuzz_wire tests/test_fuzz_property.cpp)
  target_compile_definitions(fuzz_wire PRIVATE MDF_LIBFUZZER)
  target_compile_options(fuzz_wire PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(fuzz_wire PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_wire PRIVATE mdf::core mdf_options)
endif()

if(MDF_BUILD_TESTS)
  enable_testing()

  function(mdf_add_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE mdf::core mdf_options)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
  endfunction()

  mdf_add_test(test_wire)
  mdf_add_test(test_order_book)
  mdf_add_test(test_sequencer)
  mdf_add_test(test_fault_recovery)
  mdf_add_test(test_fuzz_property)
  mdf_add_test(test_multicast_integration)
endif()
