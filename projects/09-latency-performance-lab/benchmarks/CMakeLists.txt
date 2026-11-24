add_executable(bench_suite bench_suite.cpp)
target_link_libraries(bench_suite PRIVATE lpl::lpl lpl_build_flags)

add_executable(bench_matching_ab bench_matching_ab.cpp)
target_link_libraries(bench_matching_ab PRIVATE lpl::lpl lpl_build_flags)

if(LPL_BUILD_GBENCH)
  find_package(benchmark QUIET)
  if(NOT benchmark_FOUND)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googlebenchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG v1.9.1
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googlebenchmark)
  endif()

  add_executable(bench_gbench bench_gbench.cpp)
  target_link_libraries(bench_gbench PRIVATE lpl::lpl lpl_build_flags benchmark::benchmark)
endif()
