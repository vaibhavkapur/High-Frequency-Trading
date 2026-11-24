#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not installed; skipping formatting check."
  exit 0
fi

FILES=$(find "${ROOT_DIR}/include" "${ROOT_DIR}/src" "${ROOT_DIR}/tests" "${ROOT_DIR}/benchmarks" \
  -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

if [[ "${1:-}" == "--check" ]]; then
  clang-format --dry-run --Werror ${FILES}
  echo "clang-format check passed."
else
  clang-format -i ${FILES}
  echo "Formatted C++ sources."
fi
