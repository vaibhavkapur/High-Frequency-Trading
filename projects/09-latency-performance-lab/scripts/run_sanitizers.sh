#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== [1/2] Building and testing with ASan + UBSan ==="
cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-asan" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLPL_SANITIZE="address,undefined" \
  -DLPL_BUILD_BENCHMARKS=OFF \
  -DLPL_WERROR=ON
cmake --build "${ROOT_DIR}/build-asan" -j
ctest --test-dir "${ROOT_DIR}/build-asan" --output-on-failure

echo "=== [2/2] Building and testing with TSan ==="
cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build-tsan" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLPL_SANITIZE="thread" \
  -DLPL_BUILD_BENCHMARKS=OFF \
  -DLPL_WERROR=ON
cmake --build "${ROOT_DIR}/build-tsan" -j
ctest --test-dir "${ROOT_DIR}/build-tsan" --output-on-failure

echo "All sanitizer runs passed cleanly!"
