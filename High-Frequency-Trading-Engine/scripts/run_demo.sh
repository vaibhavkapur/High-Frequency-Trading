#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO="${ROOT_DIR}/build/engine/hft_demo"

if [[ ! -x "${DEMO}" ]]; then
  cmake -S "${ROOT_DIR}/engine" -B "${ROOT_DIR}/build/engine" -DCMAKE_BUILD_TYPE=Release -DHFT_WERROR=ON
  cmake --build "${ROOT_DIR}/build/engine" --parallel
fi

echo "================================================================"
echo " High-Frequency Trading Engine — End-to-End Portfolio Showcase"
echo " (Educational / Simulated Research Platform — Not Live Trading)"
echo "================================================================"

echo ""
echo "[1/3] Deterministic single-venue golden verification + all scenarios"
"${DEMO}" --mode deterministic --scenario single-venue \
  --verify-golden "${ROOT_DIR}/engine/tests/golden/single_venue.golden.json"
"${DEMO}" --mode deterministic --scenario all

echo ""
echo "[2/3] Network-lab scenarios (MDF1 multicast recovery + SPSC pipeline)"
if [[ "${LIVE_SOCKETS:-0}" == "1" ]]; then
  "${DEMO}" --mode network-lab --scenario all --live-sockets
else
  "${DEMO}" --mode network-lab --scenario all
fi

echo ""
echo "[3/3] Performance snapshot (wall-clock steady timing)"
"${DEMO}" --mode performance --scenario all

echo ""
echo "Showcase completed successfully with all invariants PASS."
