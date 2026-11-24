#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
AUDIT_PATH="${1:-$ROOT_DIR/results/demo_audit.jsonl}"

if [[ ! -x "$BUILD_DIR/sor" ]]; then
  echo "Building sor in $BUILD_DIR..."
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" -j
fi

mkdir -p "$(dirname "$AUDIT_PATH")"
"$BUILD_DIR/sor" --demo --audit-out "$AUDIT_PATH"

echo ""
echo "=== Sample Structured Audit Log (first 6 JSONL entries) ==="
head -n 6 "$AUDIT_PATH"
