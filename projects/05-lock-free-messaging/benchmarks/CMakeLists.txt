add_executable(bench_throughput bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE lfm::lfm lfm_build_flags)

add_executable(bench_latency bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE lfm::lfm lfm_build_flags)

if(LFM_BUILD_GBENCH)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(bench_micro bench_micro.cpp)
  target_link_libraries(bench_micro PRIVATE lfm::lfm lfm_build_flags benchmark::benchmark)
endif()
