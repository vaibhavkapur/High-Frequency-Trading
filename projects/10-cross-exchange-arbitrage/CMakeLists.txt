cmake_minimum_required(VERSION 3.20)
project(cross_exchange_arbitrage VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XARB_BUILD_TESTS "Build unit, failure-mode, fuzz, and concurrency tests" ON)
option(XARB_BUILD_BENCHMARKS "Build reproducible benchmark harnesses" ON)
option(XARB_WERROR "Treat compiler warnings as errors" OFF)
option(XARB_USE_SQLITE3 "Enable SQLite3 audit database writer if libsqlite3 is available" ON)
set(XARB_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread'")

add_library(xarb_options INTERFACE)
target_compile_options(xarb_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(XARB_WERROR)
  target_compile_options(xarb_options INTERFACE -Werror)
endif()
if(XARB_SANITIZE)
  target_compile_options(xarb_options INTERFACE
    -fsanitize=${XARB_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(xarb_options INTERFACE -fsanitize=${XARB_SANITIZE})
endif()

add_library(xarb_core STATIC
  src/json_lite.cpp
  src/venue_adapter.cpp
  src/feed_codec.cpp
  src/scanner.cpp
  src/paper_executor.cpp
  src/audit.cpp
  src/replay_engine.cpp
  src/synth_feed.cpp
)
target_include_directories(xarb_core PUBLIC include)
target_link_libraries(xarb_core PRIVATE xarb_options)

if(XARB_USE_SQLITE3)
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    target_compile_definitions(xarb_core PUBLIC XARB_HAS_SQLITE3=1)
    if(TARGET SQLite3::SQLite3)
      target_link_libraries(xarb_core PUBLIC SQLite3::SQLite3)
    else()
      target_link_libraries(xarb_core PUBLIC SQLite::SQLite3)
    endif()
    message(STATUS "SQLite3 found: enabling SQLite audit database export")
  else()
    message(STATUS "SQLite3 not found: audit export will use JSON/CSV only")
  endif()
endif()

add_executable(xarb src/main.cpp)
target_link_libraries(xarb PRIVATE xarb_core xarb_options)

if(XARB_BUILD_BENCHMARKS)
  add_executable(xarb_bench benchmarks/bench_engine.cpp)
  target_link_libraries(xarb_bench PRIVATE xarb_core xarb_options)
endif()

if(XARB_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)

  add_executable(test_order_book tests/test_order_book.cpp)
  target_link_libraries(test_order_book PRIVATE xarb_core xarb_options)
  add_test(NAME order_book_and_normalization COMMAND test_order_book)

  add_executable(test_venue_adapters tests/test_venue_adapters.cpp)
  target_link_libraries(test_venue_adapters PRIVATE xarb_core xarb_options)
  add_test(NAME venue_adapters_and_recovery COMMAND test_venue_adapters)

  add_executable(test_scanner_and_paper tests/test_scanner_and_paper.cpp)
  target_link_libraries(test_scanner_and_paper PRIVATE xarb_core xarb_options)
  add_test(NAME scanner_risk_and_paper_sim COMMAND test_scanner_and_paper)

  add_executable(test_replay_and_fuzz tests/test_replay_and_fuzz.cpp)
  target_link_libraries(test_replay_and_fuzz PRIVATE xarb_core xarb_options)
  add_test(NAME replay_determinism_and_fuzz COMMAND test_replay_and_fuzz)

  add_executable(test_concurrency tests/test_concurrency.cpp)
  target_link_libraries(test_concurrency PRIVATE xarb_core xarb_options Threads::Threads)
  add_test(NAME spsc_concurrent_ingestion COMMAND test_concurrency)
endif()
