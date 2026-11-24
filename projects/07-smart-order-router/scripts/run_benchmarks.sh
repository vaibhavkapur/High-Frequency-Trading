#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
OUT_DIR="$ROOT_DIR/results/benchmarks"
WARMUP="${WARMUP:-20000}"
SAMPLES="${SAMPLES:-200000}"
DEPTH="${DEPTH:-5}"
SEED="${SEED:-42}"

mkdir -p "$OUT_DIR"

if [[ ! -x "$BUILD_DIR/bench_router" ]]; then
  echo "Building bench_router in $BUILD_DIR..."
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" -j --target bench_router
fi

OS_NAME="$(uname -s)"
KERNEL="$(uname -r)"
ARCH="$(uname -m)"
if [[ "$OS_NAME" == "Darwin" ]]; then
  CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
  LOGICAL_CORES="$(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
  PHYS_CORES="$(sysctl -n hw.physicalcpu 2>/dev/null || echo unknown)"
  GOVERNOR="macOS default (no user-space governor)"
else
  CPU_MODEL="$(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null | xargs || echo unknown)"
  LOGICAL_CORES="$(nproc 2>/dev/null || echo unknown)"
  PHYS_CORES="$(lscpu 2>/dev/null | awk -F: '/Core\(s\) per socket/{print $2; exit}' | xargs || echo unknown)"
  GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
fi

COMPILER_VER="$($(cmake -LA -N "$BUILD_DIR" 2>/dev/null | awk -F= '/^CMAKE_CXX_COMPILER:FILEPATH=/{print $2}') --version 2>/dev/null | head -n 1 || c++ --version | head -n 1)"
CXX_FLAGS="$(cmake -LA -N "$BUILD_DIR" 2>/dev/null | awk -F= '/^CMAKE_CXX_FLAGS_RELEASE:STRING=/{print $2}' || echo '-O3 -DNDEBUG')"
GIT_REV="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'uncommitted')"
TIMESTAMP_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$OUT_DIR/environment.json" <<EOF
{
  "timestamp_utc": "$TIMESTAMP_UTC",
  "git_revision": "$GIT_REV",
  "os": "$OS_NAME",
  "kernel": "$KERNEL",
  "arch": "$ARCH",
  "cpu_model": "$CPU_MODEL",
  "physical_cores": "$PHYS_CORES",
  "logical_cores": "$LOGICAL_CORES",
  "governor": "$GOVERNOR",
  "compiler": "$COMPILER_VER",
  "cxx_flags_release": "$CXX_FLAGS",
  "warmup": $WARMUP,
  "samples": $SAMPLES,
  "depth": $DEPTH,
  "seed": $SEED,
  "command": "./scripts/run_benchmarks.sh"
}
EOF

echo "Running router benchmarks (warmup=$WARMUP, samples=$SAMPLES, depth=$DEPTH, seed=$SEED)..."
"$BUILD_DIR/bench_router" --warmup "$WARMUP" --samples "$SAMPLES" --depth "$DEPTH" --seed "$SEED" --json \
  > "$OUT_DIR/router_latency.json"

python3 - "$OUT_DIR/router_latency.json" "$OUT_DIR/router_latency.txt" <<'PYEOF'
import json, sys
rows = json.load(open(sys.argv[1]))
lines = [
    f"{'Benchmark':<26}{'Venues':>7}{'Depth':>7}{'Ops/sec':>12}{'p50(ns)':>10}{'p99(ns)':>10}{'p99.9(ns)':>11}{'max(ns)':>10}",
    "-" * 93,
]
for r in rows:
    lines.append(
        f"{r['name']:<26}{r['venues']:>7}{r['depth']:>7}{r['throughput_ops_per_sec']:>12.0f}"
        f"{r['p50_ns']:>10.1f}{r['p99_ns']:>10.1f}{r['p999_ns']:>11.1f}{r['max_ns']:>10.1f}"
    )
text = "\n".join(lines) + "\n"
open(sys.argv[2], "w").write(text)
print(text, end="")
PYEOF

echo ""
echo "Saved raw results and metadata to $OUT_DIR/"
