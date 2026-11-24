#!/usr/bin/env bash
# Format all C++ sources in place, or with --check fail if anything would change.
set -euo pipefail
cd "$(dirname "$0")/.."
files=$(find include src tests benchmarks -name '*.hpp' -o -name '*.cpp')
if [[ "${1:-}" == "--check" ]]; then
    clang-format --dry-run --Werror $files
else
    clang-format -i $files
fi
