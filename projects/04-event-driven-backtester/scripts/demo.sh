#!/usr/bin/env bash
# Complete workflow on the bundled fixtures: build, test, replay, report.
set -euo pipefail
cd "$(dirname "$0")/.."
PY=${PYTHON:-python3}

echo "### build + test"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
cmake --build build -j >/dev/null
ctest --test-dir build --output-on-failure | tail -n 3

echo; echo "### 13-event fixture with full event trace"
mkdir -p results/tiny_demo
build/hftbt run --config examples/configs/tiny_demo.ini
echo "--- results/tiny_demo/events.log (first 12 lines)"
head -n 12 results/tiny_demo/events.log

echo; echo "### market-making baseline on the synthetic L3 sample"
build/hftbt run --config examples/configs/market_maker.ini

echo; echo "### imbalance-taking baseline"
build/hftbt run --config examples/configs/imbalance.ini

echo; echo "### determinism: same input + seed + config -> same event hash"
a=$(build/hftbt run --config examples/configs/market_maker.ini --quiet)
b=$(build/hftbt run --config examples/configs/market_maker.ini --quiet)
echo "run 1: $a"; echo "run 2: $b"; [[ "$a" == "$b" ]]

if PYTHONPATH=python "$PY" -c "import pandas, pyarrow" 2>/dev/null; then
    echo; echo "### independent recomputation of the report from the fill tape"
    PYTHONPATH=python "$PY" -m hftbt_tools.report results/sample_market_maker 2>/dev/null
else
    echo; echo "(skipping Python report: pip install -r requirements.txt)"
fi
