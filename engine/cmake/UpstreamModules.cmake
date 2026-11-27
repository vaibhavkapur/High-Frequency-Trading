# Isolated static wrapper targets around the ten standalone projects from
# https://github.com/vaibhavkapur/High-Frequency-Trading (pinned baseline commit).
#
# Resolution order for HFT_PROJECTS_ROOT:
#   1. Explicit -DHFT_PROJECTS_ROOT=<path> or $ENV{HFT_PROJECTS_ROOT}
#   2. Local checkout at ../projects, ../High-Frequency-Trading/projects, or ../../High-Frequency-Trading/projects
#   3. Automatic CMake FetchContent clone of vaibhavkapur/High-Frequency-Trading (when HFT_FETCH_UPSTREAM=ON)

set(HFT_PROJECTS_ROOT "" CACHE PATH "Path to the projects/ directory from vaibhavkapur/High-Frequency-Trading")
option(HFT_FETCH_UPSTREAM "Automatically fetch vaibhavkapur/High-Frequency-Trading if projects/ is not present locally" ON)
set(HFT_UPSTREAM_REPO "https://github.com/vaibhavkapur/High-Frequency-Trading.git" CACHE STRING "Upstream repository URL")
set(HFT_UPSTREAM_TAG "8aa7bdd4a4c066c0c252ef232d8f14cf4f481a89" CACHE STRING "Pinned upstream commit SHA")

if(NOT HFT_PROJECTS_ROOT AND DEFINED ENV{HFT_PROJECTS_ROOT} AND NOT "$ENV{HFT_PROJECTS_ROOT}" STREQUAL "")
  set(HFT_PROJECTS_ROOT "$ENV{HFT_PROJECTS_ROOT}")
endif()

if(NOT HFT_PROJECTS_ROOT)
  foreach(_candidate
      "${CMAKE_CURRENT_SOURCE_DIR}/../projects"
      "${CMAKE_CURRENT_SOURCE_DIR}/../High-Frequency-Trading/projects"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../High-Frequency-Trading/projects")
    if(EXISTS "${_candidate}/01-matching-engine/include/me/engine.hpp")
      get_filename_component(HFT_PROJECTS_ROOT "${_candidate}" ABSOLUTE)
      break()
    endif()
  endforeach()
endif()

if(NOT HFT_PROJECTS_ROOT AND HFT_FETCH_UPSTREAM)
  include(FetchContent)
  message(STATUS "Local projects/ not found; fetching ${HFT_UPSTREAM_REPO} (${HFT_UPSTREAM_TAG})")
  FetchContent_Declare(
    hft_upstream_repo
    GIT_REPOSITORY "${HFT_UPSTREAM_REPO}"
    GIT_TAG "${HFT_UPSTREAM_TAG}"
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(hft_upstream_repo)
  set(HFT_PROJECTS_ROOT "${hft_upstream_repo_SOURCE_DIR}/projects")
endif()

if(NOT EXISTS "${HFT_PROJECTS_ROOT}/01-matching-engine/include/me/engine.hpp")
  message(FATAL_ERROR
    "Could not locate upstream projects/ directory (checked '${HFT_PROJECTS_ROOT}'). "
    "Clone https://github.com/vaibhavkapur/High-Frequency-Trading and pass "
    "-DHFT_PROJECTS_ROOT=/path/to/High-Frequency-Trading/projects, or configure with -DHFT_FETCH_UPSTREAM=ON.")
endif()

message(STATUS "Using upstream HFT modules from: ${HFT_PROJECTS_ROOT}")

add_library(hft_upstream_flags INTERFACE)
target_compile_features(hft_upstream_flags INTERFACE cxx_std_20)
if(HFT_SANITIZE)
  target_compile_options(hft_upstream_flags INTERFACE
    -fsanitize=${HFT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(hft_upstream_flags INTERFACE -fsanitize=${HFT_SANITIZE})
endif()

# 01 · Matching Engine (header-only core: PoolBook, MapBook, Engine)
add_library(hft_upstream_me INTERFACE)
target_include_directories(hft_upstream_me SYSTEM INTERFACE
  "${HFT_PROJECTS_ROOT}/01-matching-engine/include")
target_compile_features(hft_upstream_me INTERFACE cxx_std_20)

# 02 · ITCH Feed Handler
add_library(hft_upstream_itch STATIC
  "${HFT_PROJECTS_ROOT}/02-itch-feed-handler/src/mapped_file.cpp"
  "${HFT_PROJECTS_ROOT}/02-itch-feed-handler/src/synth.cpp")
target_include_directories(hft_upstream_itch SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/02-itch-feed-handler/include")
target_link_libraries(hft_upstream_itch PRIVATE hft_upstream_flags)

# 03 · OUCH Order Gateway & Exchange Simulator
add_library(hft_upstream_ouch STATIC
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/wire.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/soupbin.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/order_manager.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/risk.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/event_log.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/socket.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/gateway.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/sim/matching_engine.cpp"
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/src/sim/exchange.cpp")
target_include_directories(hft_upstream_ouch SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/03-ouch-order-gateway/include")
target_link_libraries(hft_upstream_ouch PUBLIC Threads::Threads PRIVATE hft_upstream_flags)

# 04 · Event-Driven Backtester
add_library(hft_upstream_hftbt STATIC
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/config.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/engine.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/exchange.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/feed.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/oms.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/order_book.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/portfolio.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/report.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/strategies.cpp"
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/src/synth.cpp")
target_include_directories(hft_upstream_hftbt SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/04-event-driven-backtester/include")
target_link_libraries(hft_upstream_hftbt PRIVATE hft_upstream_flags)

# 05 · Lock-Free Messaging (header-only queues + affinity helper)
add_library(hft_upstream_lfm STATIC
  "${HFT_PROJECTS_ROOT}/05-lock-free-messaging/src/affinity.cpp")
target_include_directories(hft_upstream_lfm SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/05-lock-free-messaging/include")
target_link_libraries(hft_upstream_lfm PUBLIC Threads::Threads PRIVATE hft_upstream_flags)

# 06 · UDP Multicast Feed (MDF1/MDR1 protocol + recovery + fault injector)
add_library(hft_upstream_mdf STATIC
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/protocol.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/order_book.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/fault_injector.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/socket_utils.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/multicast_transport.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/recovery_service.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/channel_receiver.cpp"
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/src/exchange_simulator.cpp")
target_include_directories(hft_upstream_mdf SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/06-udp-multicast-feed/include")
target_link_libraries(hft_upstream_mdf PUBLIC Threads::Threads PRIVATE hft_upstream_flags)

# 07 · Smart Order Router
add_library(hft_upstream_sor STATIC
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/audit_log.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/config.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/consolidated_book.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/json_util.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/oms.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/quote_normalizer.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/risk_manager.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/route_planner.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/simulator.cpp"
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/src/venue_health.cpp")
target_include_directories(hft_upstream_sor SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/07-smart-order-router/include")
target_link_libraries(hft_upstream_sor PRIVATE hft_upstream_flags)

# 08 · Market-Making Simulator
add_library(hft_upstream_mmsim STATIC
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/config.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/engine.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/exchange.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/features.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/feed.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/order_book.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/portfolio.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/protocol.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/risk.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/strategy.cpp"
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/src/synth.cpp")
target_include_directories(hft_upstream_mmsim SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/08-market-making-simulator/include")
target_link_libraries(hft_upstream_mmsim PRIVATE hft_upstream_flags)

# 09 · Latency Performance Lab
add_library(hft_upstream_lpl STATIC
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/src/affinity.cpp"
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/src/environment.cpp"
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/src/perf_counters.cpp"
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/src/bench_harness.cpp"
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/src/matching_engine.cpp")
target_include_directories(hft_upstream_lpl SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/09-latency-performance-lab/include")
string(TOUPPER "${CMAKE_BUILD_TYPE}" HFT_LPL_BUILD_TYPE_UPPER)
string(JOIN " " HFT_LPL_EFFECTIVE_CXX_FLAGS
  "${CMAKE_CXX_FLAGS}"
  "${CMAKE_CXX_FLAGS_${HFT_LPL_BUILD_TYPE_UPPER}}")
target_compile_definitions(hft_upstream_lpl PRIVATE
  LPL_BUILD_TYPE_STR="${CMAKE_BUILD_TYPE}"
  LPL_CXX_COMPILER_ID_STR="${CMAKE_CXX_COMPILER_ID}"
  LPL_CXX_COMPILER_VER_STR="${CMAKE_CXX_COMPILER_VERSION}"
  LPL_CXX_FLAGS_STR="${HFT_LPL_EFFECTIVE_CXX_FLAGS}"
  LPL_MARCH_NATIVE_ENABLED=0)
target_link_libraries(hft_upstream_lpl PUBLIC Threads::Threads PRIVATE hft_upstream_flags)

# 10 · Cross-Exchange Arbitrage
add_library(hft_upstream_xarb STATIC
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/json_lite.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/venue_adapter.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/feed_codec.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/scanner.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/paper_executor.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/audit.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/replay_engine.cpp"
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/src/synth_feed.cpp")
target_include_directories(hft_upstream_xarb SYSTEM PUBLIC
  "${HFT_PROJECTS_ROOT}/10-cross-exchange-arbitrage/include")
target_link_libraries(hft_upstream_xarb PRIVATE hft_upstream_flags)
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  target_compile_definitions(hft_upstream_xarb PUBLIC XARB_HAS_SQLITE3=1)
  if(TARGET SQLite3::SQLite3)
    target_link_libraries(hft_upstream_xarb PUBLIC SQLite3::SQLite3)
  else()
    target_link_libraries(hft_upstream_xarb PUBLIC SQLite::SQLite3)
  endif()
endif()
