#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found in PATH; skipping formatting check."
  exit 0
fi

FILES=$(find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/tests" "${ROOT_DIR}/benchmarks" "${ROOT_DIR}/examples" \
  -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

clang-format --dry-run --Werror ${FILES}
echo "Formatting check passed."
