#!/usr/bin/env bash
# Linux only: measure cache-line contention for the padded vs unpadded SPSC
# queue with perf. Not runnable on macOS (no perf); results/ contains no data
# from this script unless someone has run it on Linux.
#
#   scripts/perf_c2c.sh [producer_cpu] [consumer_cpu]
#
# Needs: perf, and usually `sudo sysctl kernel.perf_event_paranoid=1` (or root).
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -s)" != "Linux" ]] || ! command -v perf >/dev/null; then
  echo "perf_c2c.sh requires Linux with perf installed" >&2
  exit 1
fi

PCPU=${1:-2}
CCPU=${2:-4}
OUT="results/perf-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "${OUT}"
BENCH="build/benchmarks/bench_throughput --messages 20000000 --runs 1 --sizes 64 --capacities 1024 --producer-cpu ${PCPU} --consumer-cpu ${CCPU}"

for q in spsc spsc_unpadded; do
  # Counter view: cache misses and (where the PMU exposes it) HITM events.
  # shellcheck disable=SC2086
  perf stat -e cycles,instructions,cache-misses,L1-dcache-load-misses \
    -o "${OUT}/${q}.stat.txt" -- ${BENCH} --queues "${q}" --csv "${OUT}/${q}.csv"
  # Attribution view: which cache lines bounce between cores (false sharing
  # shows up as HITM on the line holding both cursors).
  # shellcheck disable=SC2086
  perf c2c record -o "${OUT}/${q}.c2c.data" -- ${BENCH} --queues "${q}" --csv /dev/null
  perf c2c report -i "${OUT}/${q}.c2c.data" --stdio > "${OUT}/${q}.c2c.txt" 2>/dev/null || true
  rm -f "${OUT}/${q}.c2c.data"
done
echo "wrote ${OUT}"
