#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found on PATH; skipping format check."
  exit 0
fi

FILES=$(find include src tests benchmarks examples -type f \( -name '*.hpp' -o -name '*.cpp' \))
clang-format --dry-run --Werror $FILES
echo "clang-format check passed."
