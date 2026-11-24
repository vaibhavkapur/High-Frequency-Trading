#!/usr/bin/env bash
# Builds a Release tree and runs the full benchmark matrix, writing raw results
# and machine metadata to results/<timestamp>/. Nothing here is network-facing:
# the exchange simulator and the gateway both run in one process on loopback.
#
# Usage: scripts/run_benchmarks.sh [orders]      (default 200000 orders per run)
set -euo pipefail

cd "$(dirname "$0")/.."
ORDERS="${1:-200000}"
WARMUP=$((ORDERS / 10))
BUILD=build-bench
OUT="results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DOUCH_BUILD_TESTS=OFF \
      -DOUCH_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "$BUILD" -j >/dev/null

# ---- environment metadata ---------------------------------------------------
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_revision: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
  echo "git_dirty: $(git status --porcelain 2>/dev/null | wc -l | tr -d ' ') files"
  echo "os: $(uname -srm)"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "os_version: $(sw_vers -productName) $(sw_vers -productVersion)"
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "cores_physical: $(sysctl -n hw.physicalcpu)"
    echo "cores_logical: $(sysctl -n hw.logicalcpu)"
    echo "cores_performance: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo n/a)"
    echo "cores_efficiency: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo n/a)"
    echo "memory_bytes: $(sysctl -n hw.memsize)"
    echo "governor: n/a (macOS; no user-selectable CPU frequency governor)"
    echo "affinity: none (macOS has no thread-to-core pinning API)"
    echo "power: $(pmset -g batt 2>/dev/null | head -1 || echo unknown)"
  else
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cores_logical: $(nproc)"
    echo "memory_kb: $(grep MemTotal /proc/meminfo | awk '{print $2}')"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "affinity: none (run under taskset to pin)"
  fi
  echo "compiler: $(c++ --version | head -1)"
  echo "cmake: $(cmake --version | head -1)"
  echo "build_type: Release"
  echo "cxx_flags: $(grep -m1 '^CMAKE_CXX_FLAGS_RELEASE' "$BUILD/CMakeCache.txt" | cut -d= -f2-) -std=c++20"
  echo "orders_per_run: $ORDERS"
  echo "warmup_orders: $WARMUP"
  echo "command: scripts/run_benchmarks.sh $ORDERS"
} > "$OUT/environment.txt"

run() {  # label, extra args...
  local label="$1"; shift
  echo "== $label" | tee -a "$OUT/gateway_bench.log"
  "$BUILD/gateway_bench" --orders "$ORDERS" --warmup "$WARMUP" --label "$label" \
      --json "$OUT/gateway_bench.jsonl" --samples "$OUT/rtt_${label}.csv" "$@" \
      | tee -a "$OUT/gateway_bench.log"
}

echo "== codec" | tee "$OUT/codec_bench.log"
"$BUILD/codec_bench" --csv "$OUT/codec_bench.csv" | tee -a "$OUT/codec_bench.log"

# Baseline: one request in flight at a time, so RTT is not inflated by queueing.
run baseline_closed_loop        --window 1
# Tradeoff 1: busy-polling vs blocking in poll().
run spin_closed_loop            --window 1 --spin 1
# Tradeoff 2: Nagle's algorithm on (TCP_NODELAY off) on both ends.
run nagle_closed_loop           --window 1 --nodelay 0
run nagle_window8               --window 8 --nodelay 0
run nodelay_window8             --window 8
# Tradeoff 3: journal off / buffered / flushed to the OS on every record.
run journal_buffered            --window 1 --journal buffered
run journal_flush               --window 1 --journal flush
# Throughput: many requests in flight.
run throughput_window64         --window 64
run throughput_window64_spin    --window 64 --spin 1
# Open loop at a fixed arrival rate.
run rate_20k_per_s              --window 1024 --rate 20000
# Injected faults (simulated): 0%, 1%, 5% per-event fault probability.
run faults_0pct                 --window 16 --fault-rate 0
run faults_1pct                 --window 16 --fault-rate 0.01 --seed 1
run faults_5pct                 --window 16 --fault-rate 0.05 --seed 1

python3 scripts/summarize_results.py "$OUT" > "$OUT/SUMMARY.md"
echo
echo "results written to $OUT"
