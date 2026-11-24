#!/usr/bin/env bash
# Complete workflow in one terminal session: build, test, replay the sample
# session, then generate and replay a 1M-command binary log with both books and
# verify that the report streams are byte-identical.
set -euo pipefail
cd "$(dirname "$0")/.."

run() { echo; echo "\$ $*"; "$@"; }

run cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
run cmake --build build -j
echo; echo "\$ ctest --test-dir build | tail -3"
ctest --test-dir build -j 4 | tail -3

run build/me_cli replay --text-in --text --symbols 2 --tick 5 --lot 10 --depth 5 \
  --in examples/sample_session.txt --out -

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
run build/me_cli gen --out "$TMP/commands.bin" --count 1000000 --seed 42 --symbols 4 --live 20000
run build/me_cli replay --book pool --symbols 4 --in "$TMP/commands.bin" --out "$TMP/reports.pool.bin" --depth 3
run build/me_cli replay --book map --symbols 4 --in "$TMP/commands.bin" --out "$TMP/reports.map.bin"
run cmp "$TMP/reports.pool.bin" "$TMP/reports.map.bin"
echo "report streams are byte-identical"
echo; echo "\$ build/me_cli dump --in reports.pool.bin | tail -3"
build/me_cli dump --in "$TMP/reports.pool.bin" | tail -3
