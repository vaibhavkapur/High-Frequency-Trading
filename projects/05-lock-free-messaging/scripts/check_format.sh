#!/usr/bin/env bash
# Verifies formatting (or fixes it with --fix). CI pins clang-format 19.1.7.
set -euo pipefail
cd "$(dirname "$0")/.."
FILES=$(find include src tests benchmarks examples -name '*.hpp' -o -name '*.cpp')
if [[ "${1:-}" == "--fix" ]]; then
  # shellcheck disable=SC2086
  clang-format -i ${FILES}
else
  # shellcheck disable=SC2086
  clang-format --dry-run --Werror ${FILES}
fi
