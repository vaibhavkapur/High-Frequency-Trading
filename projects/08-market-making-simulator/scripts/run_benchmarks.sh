#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT_DIR="${ROOT_DIR}/results/benchmarks"
mkdir -p "${OUT_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DMMSIM_WERROR=ON >/dev/null
cmake --build "${BUILD_DIR}" --target bench_sim -j >/dev/null

META_FILE="${OUT_DIR}/environment.txt"
{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "os=$(uname -srm)"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    echo "cores_logical=$(sysctl -n hw.logicalcpu 2>/dev/null || echo unknown)"
    echo "cores_physical=$(sysctl -n hw.physicalcpu 2>/dev/null || echo unknown)"
    echo "mem_bytes=$(sysctl -n hw.memsize 2>/dev/null || echo unknown)"
  else
    echo "cpu=$(lscpu | grep 'Model name' | sed 's/.*: *//' || echo unknown)"
    echo "cores_logical=$(nproc || echo unknown)"
  fi
  echo "compiler=$(c++ --version | head -n 1)"
  echo "build_type=Release (-O3 -DNDEBUG)"
  echo "git_rev=$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unversioned)"
  echo "command=${BUILD_DIR}/bench_sim --duration 120 --runs 7 --out-csv ${OUT_DIR}/bench_results.csv"
} > "${META_FILE}"

"${BUILD_DIR}/bench_sim" --duration 120 --runs 7 --out-csv "${OUT_DIR}/bench_results.csv" | tee "${OUT_DIR}/bench_stdout.txt"
echo ""
echo "Saved raw results and environment metadata to ${OUT_DIR}"
