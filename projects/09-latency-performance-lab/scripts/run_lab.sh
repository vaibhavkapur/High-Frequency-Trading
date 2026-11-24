#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RESULTS_DIR="${ROOT_DIR}/results"

mkdir -p "${RESULTS_DIR}"

echo "[1/5] Configuring and building Release..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DLPL_WERROR=ON
cmake --build "${BUILD_DIR}" -j

echo "[2/5] Running correctness, concurrency, and fuzz tests..."
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "[3/5] Running microbenchmark suite..."
"${BUILD_DIR}/benchmarks/bench_suite" --out-dir "${RESULTS_DIR}" --reps 7 --samples 25 --warmup 3

echo "[4/5] Running integrated Matching Engine A/B benchmark..."
"${BUILD_DIR}/benchmarks/bench_matching_ab" --out-dir "${RESULTS_DIR}" --orders 200000 --reps 7 --batch 32

echo "[5/5] Extracting kernel disassembly & generating comparison report..."
"${ROOT_DIR}/scripts/dump_disasm.sh" "${BUILD_DIR}/benchmarks/bench_suite" "${RESULTS_DIR}/disasm_kernels.txt" || true
python3 "${ROOT_DIR}/scripts/analyze_results.py" \
  --results-dir "${RESULTS_DIR}" \
  --out-md "${RESULTS_DIR}/comparison_report.md" \
  --out-svg "${RESULTS_DIR}/throughput_vs_latency.svg"

echo "Done! Report written to ${RESULTS_DIR}/comparison_report.md"
