#!/usr/bin/env bash
# Format all C++ sources in place, or with --check fail if any file would change.
# CI pins clang-format 18.1.8 (pip install clang-format==18.1.8).
set -euo pipefail
cd "$(dirname "$0")/.."
files=$(find include src tools tests benchmarks examples -name '*.hpp' -o -name '*.cpp')
if [[ "${1:-}" == "--check" ]]; then
  # shellcheck disable=SC2086
  clang-format --dry-run --Werror $files
else
  # shellcheck disable=SC2086
  clang-format -i $files
fi
