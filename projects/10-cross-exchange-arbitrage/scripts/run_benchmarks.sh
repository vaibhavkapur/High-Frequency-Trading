#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RESULTS_DIR="${ROOT_DIR}/results"

mkdir -p "${RESULTS_DIR}"

echo "=== Building Release ==="
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DXARB_WERROR=ON
cmake --build "${BUILD_DIR}" -j

echo "=== Running Unit, Failure-Mode, Fuzz, and Concurrency Tests ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "=== Capturing Hardware / Compiler Environment Metadata ==="
{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "uname=$(uname -a)"
  if command -v sysctl >/dev/null 2>&1; then
    echo "cpu_brand=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
    echo "cpu_cores=$(sysctl -n hw.ncpu 2>/dev/null || true)"
    echo "mem_bytes=$(sysctl -n hw.memsize 2>/dev/null || true)"
  else
    echo "cpu_brand=$(lscpu 2>/dev/null | awk -F: '/Model name/{print $2; exit}' || true)"
    echo "cpu_cores=$(nproc 2>/dev/null || true)"
  fi
  echo "compiler=$(c++ --version | head -n 1)"
  echo "cmake=$(cmake --version | head -n 1)"
} > "${RESULTS_DIR}/environment.txt"
cat "${RESULTS_DIR}/environment.txt"

echo "=== Generating Deterministic 3-Venue Sample Feed (30,000+ events with faults) ==="
"${BUILD_DIR}/xarb" generate --out "${RESULTS_DIR}/sample_feed.xarb" --binary --steps 10000 --seed 20261124
"${BUILD_DIR}/xarb" inspect --feed "${RESULTS_DIR}/sample_feed.xarb"

echo "=== Running Baseline Paper Replay (5ms order latency, Limit-IOC) ==="
"${BUILD_DIR}/xarb" replay \
  --feed "${RESULTS_DIR}/sample_feed.xarb" \
  --scenario baseline_5ms_limit_ioc \
  --order-latency-ms 5 \
  --order-jitter-ms 1 \
  --limit-ioc 1 \
  --out-dir "${RESULTS_DIR}/baseline_5ms"

echo "=== Running Latency Sweep (Limit-IOC: models miss & one-legged risk) ==="
"${BUILD_DIR}/xarb" sweep \
  --feed "${RESULTS_DIR}/sample_feed.xarb" \
  --latencies-ms 0,1,2,5,10,25,50,100 \
  --limit-ioc 1 \
  --out-csv "${RESULTS_DIR}/latency_sweep_limit_ioc.csv"

echo "=== Running Latency Sweep (Market-IOC: models slippage risk) ==="
"${BUILD_DIR}/xarb" sweep \
  --feed "${RESULTS_DIR}/sample_feed.xarb" \
  --latencies-ms 0,1,2,5,10,25,50,100 \
  --limit-ioc 0 \
  --out-csv "${RESULTS_DIR}/latency_sweep_market_ioc.csv"

echo "=== Running Microbenchmarks & End-to-End Throughput Benchmark ==="
"${BUILD_DIR}/xarb_bench" --out-dir "${RESULTS_DIR}" --steps 15000 --book-ops 400000 | tee "${RESULTS_DIR}/benchmark_output.txt"

echo "=== Cross-Checking Audit Outputs with Python Analytics ==="
python3 "${ROOT_DIR}/scripts/analyze_results.py" \
  --run-dir "${RESULTS_DIR}/baseline_5ms" \
  --sweep-csv "${RESULTS_DIR}/latency_sweep_limit_ioc.csv" | tee "${RESULTS_DIR}/analytics_summary.txt"

echo "=== Done ==="
