#!/usr/bin/env bash
# Builds an optimized binary, records the machine/environment, runs every
# benchmark configuration and writes raw results plus a summary to
#   results/<UTC timestamp>/
#
# Usage: scripts/run_benchmarks.sh [--quick]
#   --quick   fewer commands and one repetition per configuration (smoke test)
#
# On Linux, set ME_PIN_CPU=<cpu> to pin the benchmark processes with taskset.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

COUNT=2000000
REPS=3
GBENCH_ARGS=(--benchmark_min_time=0.5s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true)
if [[ "${1:-}" == "--quick" ]]; then
  COUNT=200000
  REPS=1
  GBENCH_ARGS=(--benchmark_min_time=0.05s)
fi

BUILD=build-bench
FLAGS="-O3 -DNDEBUG"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="$FLAGS" -DME_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j >/dev/null

OUT="results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT/latency"

PIN=()
if [[ -n "${ME_PIN_CPU:-}" ]] && command -v taskset >/dev/null; then
  PIN=(taskset -c "$ME_PIN_CPU")
fi

# ---------------------------------------------------------------- environment
{
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "source_revision: $(git rev-parse HEAD 2>/dev/null || echo 'not a git checkout')"
  echo "source_dirty: $(git status --porcelain 2>/dev/null | wc -l | tr -d ' ') modified files"
  echo "os: $(uname -srm)"
  if [[ "$(uname)" == "Darwin" ]]; then
    echo "os_version: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "cores_physical: $(sysctl -n hw.physicalcpu)"
    echo "cores_performance: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo n/a)"
    echo "cores_efficiency: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo n/a)"
    echo "memory_bytes: $(sysctl -n hw.memsize)"
    echo "l1d_bytes: $(sysctl -n hw.perflevel0.l1dcachesize 2>/dev/null || echo n/a)"
    echo "l2_bytes: $(sysctl -n hw.perflevel0.l2cachesize 2>/dev/null || echo n/a)"
    echo "power: $(pmset -g batt 2>/dev/null | head -1)"
    echo "affinity: none (macOS has no CPU pinning API; scheduler picks the core)"
    echo "governor: n/a (macOS)"
  else
    echo "cpu: $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p')"
    echo "topology: $(lscpu 2>/dev/null | grep -E '^(Socket|Core|Thread)' | tr -s ' ' | tr '\n' ';')"
    echo "memory_kb: $(sed -n 's/^MemTotal: *//p' /proc/meminfo)"
    echo "affinity: ${ME_PIN_CPU:-none}"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  fi
  echo "load_average: $(uptime | sed 's/.*load averages*: *//')"
  echo "compiler: $(c++ --version | head -1)"
  echo "cmake: $(cmake --version | head -1)"
  echo "build_type: Release"
  echo "cxx_flags: -std=c++20 $FLAGS"
  echo "measured_commands_per_run: $COUNT"
  echo "repetitions: $REPS"
} > "$OUT/environment.txt"
cat "$OUT/environment.txt"

# -------------------------------------------------------------- latency sweeps
# label | extra me_latency arguments
CONFIGS=(
  "baseline_live10k|--live 10000"
  "depth_live1k|--live 1000"
  "depth_live100k|--live 100000"
  "depth_live1m|--live 1000000"
  "levels_4096_live100k|--live 100000 --depth-levels 4096"
  "symbols16_live100k|--live 100000 --symbols 16"
  "symbols256_live100k|--live 100000 --symbols 256"
  "randomids_live10k|--live 10000 --random-ids"
  "randomids_live1m|--live 1000000 --random-ids"
  # Same workload as symbols256_live100k, but every book sized for the whole market
  # (200k orders each) instead of its own share: measures the cost of over-provisioning.
  "symbols256_live100k_oversized|--live 100000 --symbols 256 --max-orders 200000"
)
printf '%s\n' "${CONFIGS[@]%%|*}" > "$OUT/configs.txt"

for config in "${CONFIGS[@]}"; do
  label=${config%%|*}
  args=${config#*|}
  live=$(sed -n 's/.*--live \([0-9]*\).*/\1/p' <<<"$args")
  warmup=$(( live * 4 > 500000 ? live * 4 : 500000 ))  # long enough to reach the target book size
  for book in pool map; do
    for rep in $(seq 1 "$REPS"); do
      name="${label}.${book}.run${rep}"
      echo "== $name"
      # shellcheck disable=SC2086
      "${PIN[@]+"${PIN[@]}"}" "$BUILD/me_latency" --book "$book" --label "$label" --count "$COUNT" \
        --warmup "$warmup" $args --json "$OUT/latency/$name.json" | tee "$OUT/latency/$name.txt" | grep -E '^(throughput|allocations)'
      echo "command: $BUILD/me_latency --book $book --label $label --count $COUNT --warmup $warmup $args" \
        >> "$OUT/latency/$name.txt"
    done
  done
done

# ------------------------------------------------------------ microbenchmarks
echo "== google benchmark"
"${PIN[@]+"${PIN[@]}"}" "$BUILD/me_bench" "${GBENCH_ARGS[@]}" \
  --benchmark_out="$OUT/microbench.json" --benchmark_out_format=json > "$OUT/microbench.txt" 2>&1

python3 "$ROOT/scripts/summarize.py" "$OUT" > "$OUT/summary.md"
echo
echo "raw results and summary written to $OUT"
