cmake_minimum_required(VERSION 3.20)
project(sor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SOR_BUILD_TESTS "Build unit, integration, and fuzz tests (GoogleTest)" ON)
option(SOR_BUILD_BENCHMARKS "Build router decision latency benchmarks" ON)
option(SOR_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(SOR_WERROR "Treat compiler warnings as errors" OFF)

add_library(sor_options INTERFACE)
target_compile_options(sor_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(SOR_WERROR)
  target_compile_options(sor_options INTERFACE -Werror)
endif()
if(SOR_SANITIZE)
  target_compile_options(sor_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(sor_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(sor_lib
  src/audit_log.cpp
  src/config.cpp
  src/consolidated_book.cpp
  src/json_util.cpp
  src/oms.cpp
  src/quote_normalizer.cpp
  src/risk_manager.cpp
  src/route_planner.cpp
  src/simulator.cpp
  src/venue_health.cpp)
target_include_directories(sor_lib PUBLIC include)
target_link_libraries(sor_lib PRIVATE sor_options)

add_executable(sor src/main.cpp)
target_link_libraries(sor PRIVATE sor_lib sor_options)

if(SOR_BUILD_BENCHMARKS)
  add_executable(bench_router benchmarks/bench_router.cpp)
  target_link_libraries(bench_router PRIVATE sor_lib sor_options)
endif()

if(SOR_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(sor_tests
    tests/test_config_fuzz.cpp
    tests/test_oms_risk.cpp
    tests/test_quote_book.cpp
    tests/test_router.cpp
    tests/test_simulator.cpp)
  target_include_directories(sor_tests PRIVATE tests)
  target_link_libraries(sor_tests PRIVATE sor_lib sor_options GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(sor_tests DISCOVERY_TIMEOUT 60)
endif()
