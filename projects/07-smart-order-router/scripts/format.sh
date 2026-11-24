#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK_MODE=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_MODE=1
fi

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found on PATH" >&2
  exit 1
fi

FILES=$(find "$ROOT_DIR/include" "$ROOT_DIR/src" "$ROOT_DIR/tests" "$ROOT_DIR/benchmarks" \
  -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

if [[ "$CHECK_MODE" -eq 1 ]]; then
  clang-format --dry-run --Werror $FILES
  echo "All C++ files pass clang-format check."
else
  clang-format -i $FILES
  echo "Formatted all C++ files."
fi
