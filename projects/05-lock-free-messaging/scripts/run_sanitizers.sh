#!/usr/bin/env bash
# Builds and runs the test suite under AddressSanitizer+UBSan and under
# ThreadSanitizer (separate builds: the two cannot be combined).
#
# TSan limitations worth remembering when reading a clean report:
#   * it only sees interleavings that actually happened in this run
#   * it checks data races under its own happens-before model; it does not
#     explore weak-memory behaviours the hardware/compiler could produce
#     (e.g. it will not prove an acquire/release pair is sufficient)
#   * a clean run is evidence, not proof
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

run() {
  local name=$1 sanitize=$2
  echo "=== ${name} (-fsanitize=${sanitize}) ==="
  cmake -S . -B "build-${name}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DLFM_SANITIZE="${sanitize}" -DLFM_BUILD_BENCHMARKS=OFF -DLFM_BUILD_EXAMPLES=ON >/dev/null
  cmake --build "build-${name}" -j "${JOBS}"
  ctest --test-dir "build-${name}" --output-on-failure
  "build-${name}/examples/market_data_pipeline" --quotes 200000 >/dev/null
}

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
export TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1}

run asan address,undefined
run tsan thread
echo "sanitizer runs passed"
