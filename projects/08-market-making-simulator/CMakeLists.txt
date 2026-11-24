cmake_minimum_required(VERSION 3.20)
project(mmsim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MMSIM_BUILD_TESTS "Build C++ unit, fuzz, and integration tests" ON)
option(MMSIM_BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(MMSIM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(MMSIM_WERROR "Treat compiler warnings as errors" OFF)

add_library(mmsim_options INTERFACE)
target_compile_options(mmsim_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(MMSIM_WERROR)
  target_compile_options(mmsim_options INTERFACE -Werror)
endif()
if(MMSIM_SANITIZE)
  target_compile_options(mmsim_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(mmsim_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(mmsim STATIC
  src/config.cpp
  src/engine.cpp
  src/exchange.cpp
  src/features.cpp
  src/feed.cpp
  src/order_book.cpp
  src/portfolio.cpp
  src/protocol.cpp
  src/risk.cpp
  src/strategy.cpp
  src/synth.cpp)
target_include_directories(mmsim PUBLIC include)
target_link_libraries(mmsim PRIVATE mmsim_options)

add_executable(mmsim_cli src/main.cpp)
set_target_properties(mmsim_cli PROPERTIES OUTPUT_NAME mmsim)
target_link_libraries(mmsim_cli PRIVATE mmsim mmsim_options)

if(MMSIM_BUILD_BENCHMARKS)
  add_executable(bench_sim benchmarks/bench_sim.cpp)
  target_link_libraries(bench_sim PRIVATE mmsim mmsim_options)
endif()

if(MMSIM_BUILD_TESTS)
  enable_testing()
  set(MMSIM_TEST_TARGETS
    test_protocol_fuzz
    test_order_book
    test_features
    test_risk_and_pnl
    test_exchange
    test_integration)
  foreach(t ${MMSIM_TEST_TARGETS})
    add_executable(${t} tests/${t}.cpp)
    target_include_directories(${t} PRIVATE tests)
    target_compile_definitions(${t} PRIVATE MMSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${t} PRIVATE mmsim mmsim_options)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
