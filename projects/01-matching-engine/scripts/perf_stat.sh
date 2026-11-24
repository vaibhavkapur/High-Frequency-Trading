#!/usr/bin/env bash
# Linux only: hardware-counter profile of the mixed workload with `perf stat`.
# Reports cycles, instructions, branch misses and last-level-cache misses for
# each book, so they can be divided by the command count printed by me_latency.
#
# Usage: scripts/perf_stat.sh [extra me_latency args, e.g. --live 100000]
#
# This script has not produced any published numbers yet: the results in
# docs/BENCHMARKS.md were measured on macOS, where these counters are not
# available to unprivileged tools.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname)" != "Linux" ]] || ! command -v perf >/dev/null; then
  echo "perf_stat.sh needs Linux with perf installed" >&2
  exit 2
fi

BUILD=build-bench
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DME_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j >/dev/null

OUT="results/perf-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"
EVENTS=cycles,instructions,branches,branch-misses,LLC-loads,LLC-load-misses,cache-misses
for book in pool map; do
  perf stat -e "$EVENTS" -o "$OUT/$book.perf.txt" -- \
    ${ME_PIN_CPU:+taskset -c "$ME_PIN_CPU"} "$BUILD/me_latency" --book "$book" "$@" | tee "$OUT/$book.txt"
  cat "$OUT/$book.perf.txt"
done
echo "written to $OUT (counters cover the whole process, including workload generation and warm-up)"
