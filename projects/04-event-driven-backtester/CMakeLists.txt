cmake_minimum_required(VERSION 3.20)
project(hftbt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HFTBT_BUILD_TESTS "Build unit/integration tests (fetches GoogleTest)" ON)
option(HFTBT_BUILD_BENCHMARKS "Build the replay benchmark" ON)
option(HFTBT_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(HFTBT_WERROR "Treat warnings as errors" OFF)

add_library(hftbt_options INTERFACE)
target_compile_options(hftbt_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(HFTBT_WERROR)
  target_compile_options(hftbt_options INTERFACE -Werror)
endif()
if(HFTBT_SANITIZE)
  target_compile_options(hftbt_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(hftbt_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(hftbt
  src/config.cpp
  src/engine.cpp
  src/exchange.cpp
  src/feed.cpp
  src/oms.cpp
  src/order_book.cpp
  src/portfolio.cpp
  src/report.cpp
  src/strategies.cpp
  src/synth.cpp)
target_include_directories(hftbt PUBLIC include)
target_link_libraries(hftbt PRIVATE hftbt_options)

add_executable(hftbt_cli src/main.cpp)
set_target_properties(hftbt_cli PROPERTIES OUTPUT_NAME hftbt)
target_link_libraries(hftbt_cli PRIVATE hftbt hftbt_options)

if(HFTBT_BUILD_BENCHMARKS)
  add_executable(bench_replay benchmarks/bench_replay.cpp)
  target_link_libraries(bench_replay PRIVATE hftbt hftbt_options)
endif()

if(HFTBT_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(hftbt_tests
    tests/test_book.cpp
    tests/test_causality.cpp
    tests/test_engine.cpp
    tests/test_exchange.cpp
    tests/test_feed.cpp
    tests/test_fuzz.cpp
    tests/test_portfolio.cpp
    tests/test_strategies.cpp)
  target_include_directories(hftbt_tests PRIVATE tests)
  target_link_libraries(hftbt_tests PRIVATE hftbt hftbt_options GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(hftbt_tests DISCOVERY_TIMEOUT 60)
endif()
