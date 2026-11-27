cmake_minimum_required(VERSION 3.20)
project(fpga_order_book_accelerator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(fpga_ob_core STATIC
    src/contracts.cpp
    src/reference_book.cpp
    src/rtl_model.cpp
)

target_include_directories(fpga_ob_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(fpga_ob_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(fpga_ob_cli
    src/cli_main.cpp
)
target_link_libraries(fpga_ob_cli PRIVATE fpga_ob_core)
target_compile_options(fpga_ob_cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_unit
    tests/test_unit.cpp
)
target_link_libraries(test_unit PRIVATE fpga_ob_core)
target_compile_options(test_unit PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_integration
    tests/test_integration.cpp
)
target_link_libraries(test_integration PRIVATE fpga_ob_core)
target_compile_options(test_integration PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_differential
    tests/test_differential.cpp
)
target_link_libraries(test_differential PRIVATE fpga_ob_core)
target_compile_options(test_differential PRIVATE -Wall -Wextra -Wpedantic)

add_executable(bench_latency
    benchmarks/bench_latency.cpp
)
target_link_libraries(bench_latency PRIVATE fpga_ob_core)
target_compile_options(bench_latency PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_test(NAME unit_tests COMMAND test_unit
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME integration_tests COMMAND test_integration
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME differential_tests COMMAND test_differential --events 25000 --seed 20261127
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME python_reference_cross_check
    COMMAND python3 scripts/reference_model.py
            --fixture tests/fixtures/deterministic_basic.json
            --expected tests/fixtures/deterministic_basic.expected.json
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
