cmake_minimum_required(VERSION 3.20)
project(high_frequency_trading_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HFT_BUILD_TESTS "Build integrated contract, integration, failure, and golden tests" ON)
option(HFT_BUILD_BENCHMARKS "Build end-to-end performance lab executable" ON)
option(HFT_WERROR "Treat compiler warnings in engine code as errors" ON)
set(HFT_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread'")

find_package(Threads REQUIRED)

add_library(hft_engine_options INTERFACE)
target_compile_options(hft_engine_options INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  $<$<BOOL:${HFT_WERROR}>:-Werror>)
if(HFT_SANITIZE)
  target_compile_options(hft_engine_options INTERFACE
    -fsanitize=${HFT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(hft_engine_options INTERFACE -fsanitize=${HFT_SANITIZE})
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/UpstreamModules.cmake")

# Canonical contracts (header-only)
add_library(hft_contracts INTERFACE)
target_include_directories(hft_contracts INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hft_contracts INTERFACE cxx_std_20)

# Thin adapters wrapping projects/01..10
add_library(hft_adapters STATIC
  src/adapters/matching_adapter.cpp
  src/adapters/itch_adapter.cpp
  src/adapters/ouch_adapter.cpp
  src/adapters/backtester_adapter.cpp
  src/adapters/spsc_adapter.cpp
  src/adapters/multicast_adapter.cpp
  src/adapters/router_adapter.cpp
  src/adapters/strategy_adapter.cpp
  src/adapters/perf_adapter.cpp
  src/adapters/arb_adapter.cpp)
target_include_directories(hft_adapters PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hft_adapters
  PUBLIC
    hft_contracts
    hft_upstream_me
    hft_upstream_itch
    hft_upstream_ouch
    hft_upstream_hftbt
    hft_upstream_lfm
    hft_upstream_mdf
    hft_upstream_sor
    hft_upstream_mmsim
    hft_upstream_lpl
    hft_upstream_xarb
    Threads::Threads
  PRIVATE
    hft_engine_options)

# Runtime orchestration (scheduler, canonical book, risk, OMS, ledger, fault injection, pipelines)
add_library(hft_runtime STATIC
  src/runtime/canonical_book.cpp
  src/runtime/scheduler.cpp
  src/runtime/risk_gate.cpp
  src/runtime/ledger.cpp
  src/runtime/oms_reconciler.cpp
  src/runtime/fault_injector.cpp
  src/runtime/pipeline.cpp)
target_include_directories(hft_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hft_runtime PUBLIC hft_adapters PRIVATE hft_engine_options)

# Main end-to-end demo executable
add_executable(hft_demo apps/hft_demo.cpp)
target_link_libraries(hft_demo PRIVATE hft_runtime hft_engine_options)
target_compile_definitions(hft_demo PRIVATE
  HFT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  HFT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")

if(HFT_BUILD_BENCHMARKS)
  add_executable(hft_bench apps/hft_bench.cpp)
  target_link_libraries(hft_bench PRIVATE hft_runtime hft_engine_options)
  target_compile_definitions(hft_bench PRIVATE
    HFT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

if(HFT_BUILD_TESTS)
  enable_testing()

  function(hft_add_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(${target_name} PRIVATE
      HFT_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
      HFT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
      HFT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../integration/fixtures")
    target_link_libraries(${target_name} PRIVATE hft_runtime hft_engine_options)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES TIMEOUT 120)
  endfunction()

  hft_add_test(test_contracts tests/contract/test_contracts.cpp)
  hft_add_test(test_matching_differential tests/integration/test_matching_differential.cpp)
  hft_add_test(test_deterministic_e2e tests/integration/test_deterministic_e2e.cpp)
  hft_add_test(test_failure_recovery tests/failure/test_failure_recovery.cpp)
  hft_add_test(test_multicast_spsc tests/integration/test_multicast_spsc.cpp)
  hft_add_test(test_multi_venue_strategies tests/integration/test_multi_venue_strategies.cpp)
endif()
