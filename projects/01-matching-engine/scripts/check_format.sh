#!/usr/bin/env bash
# Checks (or with --fix, applies) clang-format on every C++ source file.
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG_FORMAT=${CLANG_FORMAT:-$(command -v clang-format || xcrun --find clang-format 2>/dev/null || true)}
if [[ -z "$CLANG_FORMAT" ]]; then
  echo "clang-format not found; set CLANG_FORMAT=/path/to/clang-format" >&2
  exit 2
fi

FILES=$(find include src tests benchmarks -name '*.hpp' -o -name '*.cpp' | sort)
if [[ "${1:-}" == "--fix" ]]; then
  # shellcheck disable=SC2086
  "$CLANG_FORMAT" -i $FILES
else
  # shellcheck disable=SC2086
  "$CLANG_FORMAT" --dry-run --Werror $FILES
  echo "formatting OK ($(wc -w <<<"$FILES" | tr -d ' ') files)"
fi
