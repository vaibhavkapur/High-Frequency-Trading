#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
REPORT_DIR="${PROJECT_ROOT}/reports"

mkdir -p "${REPORT_DIR}"

echo "[1/5] Configuring and building (Release, C++20)..."
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

echo "[2/5] Running CTest unit & integration suite..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "[3/5] Validating canonical fixtures..."
"${BUILD_DIR}/numa_trading_engine" --mode validate-fixture --fixture "${PROJECT_ROOT}/fixtures/valid_small_events.csv"
"${BUILD_DIR}/numa_trading_engine" --mode validate-fixture --fixture "${PROJECT_ROOT}/fixtures/boundary_events.csv"

echo "[4/5] Running topology probe and comparison benchmark..."
"${BUILD_DIR}/numa_trading_engine" --mode topology --output "${REPORT_DIR}/topology.json" > /dev/null
"${BUILD_DIR}/bench_placements" \
  --events 200000 \
  --warmup 10000 \
  --capacity 4096 \
  --reps 5 \
  --seed 42 \
  --json "${REPORT_DIR}/raw_benchmark_results.json" \
  --csv "${REPORT_DIR}/benchmark_summary.csv"

echo "[5/5] Verifying acceptance criteria against raw reports..."
python3 "${PROJECT_ROOT}/scripts/verify_acceptance.py" \
  --benchmark-json "${REPORT_DIR}/raw_benchmark_results.json"

echo "Demo & validation pipeline completed successfully."
