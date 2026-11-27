#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
REPORTS_DIR="${PROJECT_DIR}/reports"

mkdir -p "${REPORTS_DIR}"

echo "=== [1/4] Configuring & building (Release, C++20) ==="
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel

echo "=== [2/4] Running all unit, integration, resilience & operational tests ==="
ctest --test-dir "${BUILD_DIR}" --verbose --output-on-failure | tee "${REPORTS_DIR}/test_output.txt"

echo "=== [3/4] Running reproducible CLI demo ==="
"${BUILD_DIR}/obce_cli" demo --events 10000 --seed 20261127 --checkpoint 256 --schema 2 | tee "${REPORTS_DIR}/demo_output.txt"

echo "=== [4/4] Running compression, latency & random-access benchmarks ==="
"${BUILD_DIR}/obce_bench" \
  --events 50000 \
  --json "${REPORTS_DIR}/benchmark_results.json" \
  --csv "${REPORTS_DIR}/benchmark_summary.csv"

cat > "${REPORTS_DIR}/environment.json" <<EOF
{
  "os": "$(uname -s)",
  "os_release": "$(uname -r)",
  "arch": "$(uname -m)",
  "cpu": "$(sysctl -n machdep.cpu.brand_string 2>/dev/null || lscpu 2>/dev/null | awk -F: '/Model name/{gsub(/^[ \t]+/,"",$2); print $2; exit}' || echo unknown)",
  "logical_cpus": "$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo unknown)",
  "compiler": "$(c++ --version | head -n 1 | sed 's/"/\\"/g')",
  "cmake": "$(cmake --version | head -n 1 | sed 's/"/\\"/g')",
  "build_type": "Release",
  "cxx_standard": "C++20"
}
EOF

echo "=== All checks and reports completed successfully ==="
