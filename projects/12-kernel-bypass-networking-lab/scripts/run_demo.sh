#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=== Kernel Bypass Networking Lab — Reproducible Demo ==="
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

echo "=== Generating canonical deterministic fixture ==="
"${BUILD_DIR}/kbnl_bench" --generate-fixture "${PROJECT_DIR}/fixtures/deterministic_feed_v1.bin"

echo "=== Running automated unit, boundary, and integration tests ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "=== Running benchmark suite and acceptance verification ==="
bash "${SCRIPT_DIR}/run_benchmarks.sh"

echo "=== Demo complete. Summary available in reports/comparison_summary.json and reports/acceptance.md ==="
