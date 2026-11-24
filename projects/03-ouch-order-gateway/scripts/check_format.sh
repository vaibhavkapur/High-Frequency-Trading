#!/usr/bin/env bash
# Fails if any source file is not clang-format clean. Pass --fix to rewrite files.
set -euo pipefail
cd "$(dirname "$0")/.."
CF="${CLANG_FORMAT:-$(command -v clang-format || xcrun -f clang-format 2>/dev/null || true)}"
if [[ -z "$CF" ]]; then
  echo "clang-format not found (set CLANG_FORMAT=/path/to/clang-format)" >&2
  exit 2
fi
FILES=$(find include src tests benchmarks examples -name '*.hpp' -o -name '*.cpp' | sort)
if [[ "${1:-}" == "--fix" ]]; then
  # shellcheck disable=SC2086
  "$CF" -i $FILES
else
  # shellcheck disable=SC2086
  "$CF" --dry-run --Werror $FILES
fi
