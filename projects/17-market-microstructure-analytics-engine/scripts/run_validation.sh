#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "=== 1. Build C++20 engine, CLI, tests, and benchmarks (Release) ==="
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

echo "=== 2. Run CTest (C++ unit, edge-case, causal, capacity, and parity tests) ==="
ctest --test-dir "${BUILD_DIR}" --output-on-failure

echo "=== 3. Run Python end-to-end, integration, causal, reproducibility, and operational tests ==="
MMAE_CLI_BIN="${BUILD_DIR}/mmae_cli" PYTHONPATH="${PROJECT_DIR}/python" \
  python3 -m unittest discover -s "${PROJECT_DIR}/tests/python" -v

echo "=== ALL VALIDATION SUITES PASSED ==="
