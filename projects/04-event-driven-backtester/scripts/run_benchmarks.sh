#!/usr/bin/env bash
# Reproducible benchmark run. Writes raw CSVs and machine metadata to
# results/benchmarks/. Usage: scripts/run_benchmarks.sh [events] [reps]
set -euo pipefail
cd "$(dirname "$0")/.."
EVENTS=${1:-2000000}
REPS=${2:-5}
OUT=results/benchmarks
mkdir -p "$OUT"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j --target bench_replay >/dev/null

{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "source_revision: $(git rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')"
    echo "command: scripts/run_benchmarks.sh $EVENTS $REPS"
    echo "os: $(uname -srm)"
    if [[ "$(uname)" == "Darwin" ]]; then
        echo "os_version: $(sw_vers -productName) $(sw_vers -productVersion)"
        echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
        echo "cores_physical: $(sysctl -n hw.physicalcpu)"
        echo "cores_logical: $(sysctl -n hw.logicalcpu)"
        echo "core_topology: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?') performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?') efficiency"
        echo "memory_gb: $(( $(sysctl -n hw.memsize) / 1073741824 ))"
        echo "affinity: not pinned (macOS has no thread-affinity API for user processes)"
        echo "governor: n/a (macOS); power source: $(pmset -g batt 2>/dev/null | head -n 1 | sed "s/.*'\(.*\)'.*/\1/")"
    else
        echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
        echo "cores_logical: $(nproc)"
        echo "memory_gb: $(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576 ))"
        echo "affinity: not pinned (prefix with taskset -c N to pin)"
        echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    fi
    echo "compiler: $(c++ --version | head -n 1)"
    echo "cmake_build_type: Release"
    echo "cxx_flags: $(grep -m1 'CMAKE_CXX_FLAGS_RELEASE:' build/CMakeCache.txt | cut -d= -f2) -std=c++20"
    echo "dataset: in-process synthetic L3 flow, seed 42, $EVENTS events per scenario (hftbt::generate_l3)"
    echo "warmup: none; each scenario runs $REPS times, best and median reported"
} > "$OUT/environment.txt"
cat "$OUT/environment.txt"

echo; echo "== replay scenarios"
build/bench_replay --events "$EVENTS" --reps "$REPS" --out "$OUT/replay.csv"

echo; echo "== simulation length vs memory (one process per size)"
echo "feed_events,dispatched_events,best_s,dispatched_per_s,peak_rss_after_load_mb,peak_rss_mb,reproducible,event_hash" > "$OUT/scale.csv"
for n in 1000000 4000000 16000000 64000000; do
    build/bench_replay --scale "$n" | tee -a "$OUT/scale.csv"
done
echo; echo "raw results: $OUT/{environment.txt,replay.csv,scale.csv}"
