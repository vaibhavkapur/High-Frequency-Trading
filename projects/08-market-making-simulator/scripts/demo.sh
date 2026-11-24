#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DEMO_DIR="${ROOT_DIR}/results/demo"
mkdir -p "${DEMO_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" --target mmsim_cli -j >/dev/null

echo "[1/3] Generating deterministic 30s high_vol_burst binary replay..."
"${BUILD_DIR}/mmsim" generate \
  --out "${DEMO_DIR}/demo_session.mmbin" \
  --regime high_vol_burst \
  --seed 202608 \
  --duration 30

echo ""
echo "[2/3] Running Naive Symmetric baseline..."
"${BUILD_DIR}/mmsim" simulate \
  --input "${DEMO_DIR}/demo_session.mmbin" \
  --config "${ROOT_DIR}/examples/naive_symmetric.cfg" \
  --out-dir "${DEMO_DIR}/naive_symmetric"

echo ""
echo "[3/3] Running Inventory-Aware strategy..."
"${BUILD_DIR}/mmsim" simulate \
  --input "${DEMO_DIR}/demo_session.mmbin" \
  --config "${ROOT_DIR}/examples/inventory_aware.cfg" \
  --out-dir "${DEMO_DIR}/inventory_aware"

echo ""
echo "Demo artifacts written to ${DEMO_DIR}"
