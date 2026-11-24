#!/usr/bin/env bash
# Reproducible benchmark run: builds Release, generates the synthetic inputs,
# runs every mode on every profile, and records raw results plus the
# environment they were measured in.
#
#   scripts/run_benchmarks.sh                 # defaults below
#   MESSAGES=20000000 RUNS=11 scripts/run_benchmarks.sh
#   OUT=results/my-box scripts/run_benchmarks.sh
#
# Outputs (in $OUT): bench.jsonl (raw, one JSON object per configuration),
# environment.txt, benchmarks.md (table generated from bench.jsonl).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESSAGES="${MESSAGES:-5000000}"
SYMBOLS="${SYMBOLS:-500}"
SEED="${SEED:-1}"
RUNS="${RUNS:-15}"
BUILD="${BUILD:-$ROOT/build-bench}"
OUT="${OUT:-$ROOT/results/$(date +%F)-$(uname -s | tr 'A-Z' 'a-z')-$(uname -m)}"
PROFILES=(balanced add-heavy churn)
MODES=(frame decode book-flat book-std)

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DITCH_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j --target itch_gen itch_bench >/dev/null

mkdir -p "$OUT" "$BUILD/data"
: >"$OUT/bench.jsonl"

{
  echo "date_utc:      $(date -u +%FT%TZ)"
  echo "command:       MESSAGES=$MESSAGES SYMBOLS=$SYMBOLS SEED=$SEED RUNS=$RUNS scripts/run_benchmarks.sh"
  echo "source_rev:    $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 'not a git checkout')"
  echo "uname:         $(uname -srm)"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "os:            $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    echo "cpu:           $(sysctl -n machdep.cpu.brand_string)"
    echo "cores:         $(sysctl -n hw.physicalcpu) physical / $(sysctl -n hw.logicalcpu) logical" \
         "(performance: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?')," \
         "efficiency: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?'))"
    echo "memory_bytes:  $(sysctl -n hw.memsize)"
    echo "power:         $(pmset -g batt 2>/dev/null | head -1)"
    echo "affinity:      none (macOS has no thread pinning API); itch_bench requests QOS_CLASS_USER_INTERACTIVE"
    echo "governor:      n/a (macOS)"
  else
    echo "os:            $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "cpu:           $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p')"
    echo "cores:         $(nproc) logical"
    echo "memory_kb:     $(sed -n 's/^MemTotal: *//p' /proc/meminfo)"
    echo "governor:      $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "affinity:      ${TASKSET:-none} (set TASKSET='taskset -c N' to pin)"
  fi
  echo "compiler:      $(grep -m1 CMAKE_CXX_COMPILER: "$BUILD/CMakeCache.txt" | cut -d= -f2)"
  echo "compiler_ver:  $("$(grep -m1 CMAKE_CXX_COMPILER: "$BUILD/CMakeCache.txt" | cut -d= -f2)" --version | head -1)"
  echo "build_type:    Release (flags are recorded per row in bench.jsonl)"
  echo "warmup:        1 untimed pass per configuration, then $RUNS timed passes; median reported"
  echo "dataset:       synthetic, itch_gen --messages $MESSAGES --symbols $SYMBOLS --seed $SEED, profiles: ${PROFILES[*]}"
} >"$OUT/environment.txt"

for profile in "${PROFILES[@]}"; do
  file="$BUILD/data/$profile.itch"
  "$BUILD/itch_gen" --out "$file" --messages "$MESSAGES" --symbols "$SYMBOLS" --seed "$SEED" \
    --profile "$profile" >/dev/null
  for mode in "${MODES[@]}"; do
    echo "running $profile / $mode" >&2
    ${TASKSET:-} "$BUILD/itch_bench" --file "$file" --mode "$mode" --runs "$RUNS" \
      --label "$profile" >>"$OUT/bench.jsonl"
  done
done

python3 "$ROOT/scripts/bench_report.py" "$OUT/bench.jsonl" "$OUT/environment.txt" \
  >"$OUT/benchmarks.md"
echo "wrote $OUT/{bench.jsonl,environment.txt,benchmarks.md}" >&2
