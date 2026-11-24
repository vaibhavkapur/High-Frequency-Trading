#!/usr/bin/env bash
# End-to-end demo: generate a synthetic ITCH file, replay it, write CSVs,
# show a few rows of each, and cross-check against the Python reference.
#
#   scripts/demo.sh            # uses ./build, builds it if needed
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
WORK="${WORK:-$BUILD/demo}"

if [[ ! -x "$BUILD/itch_replay" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j >/dev/null
fi
mkdir -p "$WORK"

echo '$ itch_gen --out demo.itch --messages 500000 --symbols 40 --seed 7'
"$BUILD/itch_gen" --out "$WORK/demo.itch" --messages 500000 --symbols 40 --seed 7 |
  sed "s|$WORK/||"
echo
echo '$ itch_replay demo.itch --depth 5 --show 2 --bbo-csv bbo.csv --trades-csv trades.csv \'
echo '              --snapshot-csv snapshot.csv --summary-csv summary.csv'
"$BUILD/itch_replay" "$WORK/demo.itch" --depth 5 --show 2 \
  --bbo-csv "$WORK/bbo.csv" --trades-csv "$WORK/trades.csv" \
  --snapshot-csv "$WORK/snapshot.csv" --summary-csv "$WORK/summary.csv" |
  sed "s|$WORK/||; /^elapsed/d"
for f in bbo trades snapshot summary; do
  echo
  echo "\$ head -4 $f.csv   # $(($(wc -l <"$WORK/$f.csv") - 1)) rows"
  head -4 "$WORK/$f.csv"
done
echo
echo '$ itch_replay demo.itch --show 0   # second run: same digest'
"$BUILD/itch_replay" "$WORK/demo.itch" --show 0 | grep 'book digest'
echo
echo '$ scripts/crosscheck.py --gen itch_gen --replay itch_replay --messages 100000'
python3 "$ROOT/scripts/crosscheck.py" --gen "$BUILD/itch_gen" --replay "$BUILD/itch_replay" \
  --messages 100000
