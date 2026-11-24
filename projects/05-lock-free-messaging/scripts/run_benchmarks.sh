#!/usr/bin/env bash
# Builds Release, runs every benchmark and writes raw results plus the machine
# and build metadata needed to interpret them into results/<label>/.
#
#   scripts/run_benchmarks.sh [label]            (default label: UTC timestamp)
#
# Environment knobs:
#   LFM_BENCH_PLACEMENT  extra placement flags passed to the harnesses, e.g.
#                        Linux:  "--producer-cpu 2 --consumer-cpu 4"
#                        macOS:  "--producer-qos p --consumer-qos p" (default)
#   LFM_BENCH_QUICK=1    small message counts, for smoke testing the pipeline
set -euo pipefail
cd "$(dirname "$0")/.."

LABEL=${1:-$(date -u +%Y%m%dT%H%M%SZ)}
OUT="results/${LABEL}"
mkdir -p "${OUT}"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

if [[ "$(uname -s)" == "Darwin" ]]; then
  PLACEMENT=${LFM_BENCH_PLACEMENT:-"--producer-qos p --consumer-qos p"}
else
  PLACEMENT=${LFM_BENCH_PLACEMENT:-""}
fi

if [[ "${LFM_BENCH_QUICK:-0}" == "1" ]]; then
  TP_ARGS="--messages 200000 --runs 1"
  LAT_ARGS="--seconds 1 --max-samples 200000 --warmup 10000"
  MICRO_ARGS="--benchmark_min_time=0.05s"
else
  TP_ARGS="--messages 4000000 --runs 5"
  LAT_ARGS="--seconds 3 --max-samples 5000000 --warmup 100000"
  MICRO_ARGS="--benchmark_repetitions=5 --benchmark_report_aggregates_only=false"
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j "${JOBS}" >/dev/null

# ---------------------------------------------------------------- metadata
{
  echo "# Benchmark environment"
  echo
  echo "- date (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "- source revision: $(git rev-parse HEAD 2>/dev/null || echo 'not a git checkout')$(git diff --quiet 2>/dev/null || echo ' (dirty or untracked)')"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "- CPU: $(sysctl -n machdep.cpu.brand_string)"
    echo "- cores: $(sysctl -n hw.ncpu) logical ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?') performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?') efficiency)"
    echo "- cache line: $(sysctl -n hw.cachelinesize) bytes"
    echo "- memory: $(( $(sysctl -n hw.memsize) / 1073741824 )) GiB"
    echo "- OS: $(sw_vers -productName) $(sw_vers -productVersion) ($(uname -sr))"
    echo "- power: $(pmset -g batt | head -1 | sed 's/Now drawing from //'), low power mode: $(pmset -g | awk '/lowpowermode/ {print $2}')"
    echo "- frequency governor: not applicable (macOS manages P/E-core scheduling and DVFS)"
  else
    echo "- CPU: $(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2; exit}')"
    echo "- topology: $(lscpu | awk -F: '/^(Socket|Core|Thread)/ {gsub(/^ +/, "", $2); printf "%s=%s ", $1, $2}')"
    echo "- OS: $(uname -sr)"
    echo "- frequency governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "- isolcpus: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo unknown)"
  fi
  echo "- load average at start: $(uptime | sed 's/.*load averages*: //')"
  echo "- compiler: $(grep -m1 CMAKE_CXX_COMPILER: build/CMakeCache.txt | cut -d= -f2) — $($(grep -m1 CMAKE_CXX_COMPILER: build/CMakeCache.txt | cut -d= -f2) --version | head -1)"
  echo "- flags: -std=c++20 $(grep -m1 CMAKE_CXX_FLAGS_RELEASE: build/CMakeCache.txt | cut -d= -f2)"
  echo "- placement flags: ${PLACEMENT:-none}"
  echo "- clock: $([[ "$(uname -s)" == "Darwin" ]] && echo 'clock_gettime_nsec_np(CLOCK_UPTIME_RAW)' || echo 'clock_gettime(CLOCK_MONOTONIC)'); step and read cost are measured per run and recorded in latency.csv"
  echo
  echo "## Commands"
  echo
  echo '```'
  echo "build/benchmarks/bench_throughput ${TP_ARGS} ${PLACEMENT} --csv ${OUT}/throughput.csv"
  echo "build/benchmarks/bench_latency ${LAT_ARGS} ${PLACEMENT} --csv ${OUT}/latency.csv"
  echo "build/benchmarks/bench_micro ${MICRO_ARGS} --benchmark_out=${OUT}/micro.json --benchmark_out_format=json"
  echo '```'
} > "${OUT}/environment.md"

# -------------------------------------------------------------------- runs
# shellcheck disable=SC2086
build/benchmarks/bench_throughput ${TP_ARGS} ${PLACEMENT} --csv "${OUT}/throughput.csv"
# shellcheck disable=SC2086
build/benchmarks/bench_latency ${LAT_ARGS} ${PLACEMENT} --csv "${OUT}/latency.csv"
if [[ -x build/benchmarks/bench_micro ]]; then
  # shellcheck disable=SC2086
  build/benchmarks/bench_micro ${MICRO_ARGS} --benchmark_out="${OUT}/micro.json" \
    --benchmark_out_format=json > "${OUT}/micro.txt"
fi
echo "- load average at end: $(uptime | sed 's/.*load averages*: //')" >> "${OUT}/environment.md"

python3 scripts/summarize.py "${OUT}" > "${OUT}/summary.md"
echo "wrote ${OUT}/{environment.md,throughput.csv,latency.csv,micro.json,summary.md}"
