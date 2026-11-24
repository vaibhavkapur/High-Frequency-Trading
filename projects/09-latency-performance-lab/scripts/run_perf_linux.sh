#!/usr/bin/env bash
# Linux production profiling script: pins process with taskset (and numactl if available),
# captures hardware counters with `perf stat`, and records a call-graph profile (`perf record`).
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "This script requires Linux perf/taskset. On macOS, run ./scripts/run_lab.sh instead."
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RESULTS_DIR="${ROOT_DIR}/results"
CORE_ID="${CORE_ID:-0}"
NUMA_NODE="${NUMA_NODE:-0}"

mkdir -p "${RESULTS_DIR}"

PIN_CMD=(taskset -c "${CORE_ID}")
if command -v numactl >/dev/null 2>&1; then
  PIN_CMD=(numactl --cpunodebind="${NUMA_NODE}" --membind="${NUMA_NODE}" taskset -c "${CORE_ID}")
fi

EVENTS="cycles,instructions,cache-references,cache-misses,branches,branch-misses,L1-dcache-load-misses,context-switches,cpu-migrations"

echo "[1/3] Capturing perf stat for microbenchmark suite on core ${CORE_ID}..."
perf stat -r 3 -e "${EVENTS}" -o "${RESULTS_DIR}/perf_stat_micro.txt" -- \
  "${PIN_CMD[@]}" "${BUILD_DIR}/benchmarks/bench_suite" --out-dir "${RESULTS_DIR}" --pin-core "${CORE_ID}"

echo "[2/3] Capturing perf stat for matching engine A/B on core ${CORE_ID}..."
perf stat -r 3 -e "${EVENTS}" -o "${RESULTS_DIR}/perf_stat_matching_ab.txt" -- \
  "${PIN_CMD[@]}" "${BUILD_DIR}/benchmarks/bench_matching_ab" --out-dir "${RESULTS_DIR}" --pin-core "${CORE_ID}"

echo "[3/3] Recording call-graph profile for flamegraph generation..."
perf record -F 999 -g --call-graph dwarf -o "${RESULTS_DIR}/perf_matching_ab.data" -- \
  "${PIN_CMD[@]}" "${BUILD_DIR}/benchmarks/bench_matching_ab" --out-dir "${RESULTS_DIR}" --reps 5 --pin-core "${CORE_ID}"

if [[ -n "${FLAMEGRAPH_DIR:-}" && -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" ]]; then
  perf script -i "${RESULTS_DIR}/perf_matching_ab.data" \
    | "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" \
    | "${FLAMEGRAPH_DIR}/flamegraph.pl" > "${RESULTS_DIR}/flamegraph_matching_ab.svg"
  echo "Generated flamegraph: ${RESULTS_DIR}/flamegraph_matching_ab.svg"
else
  echo "Tip: set FLAMEGRAPH_DIR=/path/to/FlameGraph to auto-generate flamegraph SVG."
fi
