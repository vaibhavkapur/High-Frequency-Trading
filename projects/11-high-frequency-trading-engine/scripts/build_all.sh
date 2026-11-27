#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DHFT_WERROR=ON)
if [[ -n "${HFT_PROJECTS_ROOT:-}" ]]; then
  CMAKE_ARGS+=(-DHFT_PROJECTS_ROOT="${HFT_PROJECTS_ROOT}")
fi

echo "=== Building integrated engine (${BUILD_TYPE}) ==="
cmake -S "${ROOT_DIR}/engine" -B "${ROOT_DIR}/build/engine" "${CMAKE_ARGS[@]}"
cmake --build "${ROOT_DIR}/build/engine" --parallel
ctest --test-dir "${ROOT_DIR}/build/engine" --output-on-failure

echo "Build and test complete."
