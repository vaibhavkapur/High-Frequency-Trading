#!/usr/bin/env bash
# Release build + tests, then the same tests under ASan/UBSan and under TSan.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOUCH_WERROR=ON >/dev/null
cmake --build build -j
ctest --test-dir build --output-on-failure -j 4

for san in address,undefined thread; do
  dir="build-${san%%,*}"
  cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOUCH_SANITIZE="$san" \
        -DOUCH_BUILD_BENCHMARKS=OFF -DOUCH_WERROR=ON >/dev/null
  cmake --build "$dir" -j
  UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
    "$dir/ouch_tests" --gtest_brief=1
done
echo "all test configurations passed"
