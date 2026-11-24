#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "=== [1/3] Release build & test ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMDF_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

echo "=== [2/3] AddressSanitizer + UBSan build & test ==="
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMDF_SANITIZE=address,undefined -DMDF_WERROR=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

echo "=== [3/3] ThreadSanitizer build & test ==="
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMDF_SANITIZE=thread -DMDF_WERROR=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

echo "All builds and tests passed."
