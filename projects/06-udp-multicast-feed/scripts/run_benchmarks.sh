#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p results

echo "=== Building Release benchmarks ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mdf_codec_bench mdf_multicast_bench mdf_demo -j

ENV_FILE="results/environment.txt"
{
  echo "timestamp_utc=$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  echo "os_kernel=$(uname -srm)"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "cpu_model=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "cpu_cores=$(sysctl -n hw.ncpu 2>/dev/null || echo unknown)"
  else
    echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs || echo unknown)"
    echo "cpu_cores=$(nproc 2>/dev/null || echo unknown)"
  fi
  echo "compiler=$(c++ --version | head -n 1)"
  echo "cmake_build_type=Release"
  echo "git_rev=$(git rev-parse --short HEAD 2>/dev/null || echo uncommitted)"
} > "$ENV_FILE"

echo "=== Environment Metadata ==="
cat "$ENV_FILE"

echo "=== Running Codec Microbenchmark ==="
./build/mdf_codec_bench --warmup 20000 --iterations 200000 | tee results/codec_bench.txt

echo "=== Running Live Multicast & Recovery Benchmarks ==="
./build/mdf_multicast_bench --out-dir results --batch-pkts 10000 --burst-pkts 8000 --loss-pkts 2000 | tee results/multicast_bench.txt

echo "=== Capturing Live Terminal Demo Output ==="
./build/mdf_demo | tee docs/sample-output.txt

echo "Results written to results/ and docs/sample-output.txt"
