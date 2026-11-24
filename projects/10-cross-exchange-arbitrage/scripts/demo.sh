#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" -j >/dev/null

TMP_FEED="${ROOT_DIR}/examples/sample_feed.jsonl"
TMP_BIN="${ROOT_DIR}/examples/sample_feed.xarb"

echo "$ xarb generate --out examples/sample_feed.jsonl --steps 2500 --seed 20261124"
"${BUILD_DIR}/xarb" generate --out "${TMP_FEED}" --steps 2500 --seed 20261124

echo ""
echo "$ xarb convert --in examples/sample_feed.jsonl --out examples/sample_feed.xarb --to binary"
"${BUILD_DIR}/xarb" convert --in "${TMP_FEED}" --out "${TMP_BIN}" --to binary

echo ""
echo "$ xarb replay --feed examples/sample_feed.xarb --scenario demo_5ms --order-latency-ms 5 --out-dir results/demo"
"${BUILD_DIR}/xarb" replay --feed "${TMP_BIN}" --scenario demo_5ms --order-latency-ms 5 --out-dir "${ROOT_DIR}/results/demo"

echo ""
echo "$ xarb sweep --feed examples/sample_feed.xarb --latencies-ms 0,2,5,15,50"
"${BUILD_DIR}/xarb" sweep --feed "${TMP_BIN}" --latencies-ms 0,2,5,15,50
