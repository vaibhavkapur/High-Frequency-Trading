cmake_minimum_required(VERSION 3.20)
project(ouch_order_gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OUCH_BUILD_TESTS "Build unit and integration tests" ON)
option(OUCH_BUILD_BENCHMARKS "Build benchmarks" ON)
option(OUCH_BUILD_EXAMPLES "Build examples" ON)
option(OUCH_BUILD_FUZZERS "Build libFuzzer targets (needs a clang with libFuzzer)" OFF)
option(OUCH_WERROR "Treat warnings as errors" OFF)
set(OUCH_SANITIZE "" CACHE STRING "Sanitizers to enable: address, undefined, thread (comma separated)")

add_library(ouch_options INTERFACE)
target_compile_options(ouch_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(OUCH_WERROR)
  target_compile_options(ouch_options INTERFACE -Werror)
endif()
if(OUCH_SANITIZE)
  target_compile_options(ouch_options INTERFACE -fsanitize=${OUCH_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(ouch_options INTERFACE -fsanitize=${OUCH_SANITIZE})
endif()

find_package(Threads REQUIRED)

# Gateway side: codec, session framing, OMS, risk, journal, TCP session.
add_library(ouch_core
  src/wire.cpp
  src/soupbin.cpp
  src/order_manager.cpp
  src/risk.cpp
  src/event_log.cpp
  src/socket.cpp
  src/gateway.cpp)
target_include_directories(ouch_core PUBLIC include)
target_link_libraries(ouch_core PRIVATE ouch_options)

# Exchange simulator: matching core, sequenced sessions, fault injection.
add_library(ouch_sim
  src/sim/matching_engine.cpp
  src/sim/exchange.cpp)
target_link_libraries(ouch_sim PUBLIC ouch_core Threads::Threads PRIVATE ouch_options)

if(OUCH_BUILD_EXAMPLES)
  add_executable(ouch_demo examples/demo.cpp)
  target_link_libraries(ouch_demo PRIVATE ouch_sim ouch_options)
  add_executable(ouch_exchange_sim examples/exchange_sim_main.cpp)
  target_link_libraries(ouch_exchange_sim PRIVATE ouch_sim ouch_options)
  add_executable(ouch_journal_dump examples/journal_dump.cpp)
  target_link_libraries(ouch_journal_dump PRIVATE ouch_core ouch_options)
endif()

if(OUCH_BUILD_BENCHMARKS)
  add_executable(codec_bench benchmarks/codec_bench.cpp)
  target_link_libraries(codec_bench PRIVATE ouch_core ouch_options)
  add_executable(gateway_bench benchmarks/gateway_bench.cpp)
  target_link_libraries(gateway_bench PRIVATE ouch_sim ouch_options)
endif()

if(OUCH_BUILD_FUZZERS)
  foreach(target fuzz_decode fuzz_frames fuzz_oms)
    add_executable(${target} tests/fuzz/${target}.cpp)
    target_link_libraries(${target} PRIVATE ouch_core ouch_options)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

if(OUCH_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(ouch_tests
    tests/test_wire.cpp
    tests/test_soupbin.cpp
    tests/test_order_manager.cpp
    tests/test_risk.cpp
    tests/test_matching_engine.cpp
    tests/test_event_log.cpp
    tests/test_property.cpp
    tests/test_integration.cpp)
  target_link_libraries(ouch_tests PRIVATE ouch_sim GTest::gtest_main ouch_options)
  include(GoogleTest)
  gtest_discover_tests(ouch_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
