cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ITCH_BUILD_TESTS "Build the Catch2 test suite" ON)
option(ITCH_BUILD_BENCHMARKS "Build itch_bench" ON)
option(ITCH_BUILD_FUZZERS "Build libFuzzer targets (needs a clang with libFuzzer)" OFF)
option(ITCH_WERROR "Treat warnings as errors" OFF)
set(ITCH_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")

add_library(itch_options INTERFACE)
target_compile_options(itch_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(ITCH_WERROR)
  target_compile_options(itch_options INTERFACE -Werror)
endif()
if(ITCH_SANITIZE)
  target_compile_options(itch_options INTERFACE
    -fsanitize=${ITCH_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(itch_options INTERFACE -fsanitize=${ITCH_SANITIZE})
endif()

add_library(itch STATIC
  src/mapped_file.cpp
  src/synth.cpp
)
target_include_directories(itch PUBLIC include)
target_link_libraries(itch PRIVATE itch_options)

add_executable(itch_replay tools/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE itch itch_options)

add_executable(itch_gen tools/itch_gen.cpp)
target_link_libraries(itch_gen PRIVATE itch itch_options)

add_executable(example_top_of_book examples/top_of_book.cpp)
target_link_libraries(example_top_of_book PRIVATE itch itch_options)

if(ITCH_BUILD_BENCHMARKS)
  add_executable(itch_bench benchmarks/itch_bench.cpp)
  target_link_libraries(itch_bench PRIVATE itch itch_options)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _itch_bt)
  string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_itch_bt}} -std=c++20" _itch_flags)
  target_compile_definitions(itch_bench PRIVATE ITCH_BUILD_FLAGS="${_itch_flags}")
endif()

if(ITCH_BUILD_FUZZERS)
  add_executable(fuzz_feed tests/fuzz/fuzz_feed.cpp)
  target_link_libraries(fuzz_feed PRIVATE itch itch_options)
  target_compile_options(fuzz_feed PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_feed PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(ITCH_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.7.1
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  endif()
  include(Catch)

  add_executable(itch_tests
    tests/test_endian.cpp
    tests/test_decoder.cpp
    tests/test_framing.cpp
    tests/test_order_index.cpp
    tests/test_book.cpp
    tests/test_feed_handler.cpp
    tests/test_replay.cpp
  )
  target_link_libraries(itch_tests PRIVATE itch itch_options Catch2::Catch2WithMain)
  catch_discover_tests(itch_tests)

  # Independent second implementation (Python) must agree with the C++ pipeline
  # on BBO history, trades, final depth and message counts.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME crosscheck_python
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/crosscheck.py
              --gen $<TARGET_FILE:itch_gen> --replay $<TARGET_FILE:itch_replay>
              --messages 60000 --symbols 20)
  endif()
endif()
