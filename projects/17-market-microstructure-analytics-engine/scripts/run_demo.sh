#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
REPORTS_DIR="${PROJECT_DIR}/reports"
WORK_DIR="${BUILD_DIR}/demo_artifacts"

mkdir -p "${REPORTS_DIR}" "${WORK_DIR}"

echo "=== 1. Configure & build C++20 Release targets ==="
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

echo "=== 2. Run hand-worked L3 reference fixture ==="
"${BUILD_DIR}/mmae_cli" \
  --input "${PROJECT_DIR}/fixtures/hand_worked_l3.csv" \
  --output-features "${WORK_DIR}/hand_worked_features.csv" \
  --output-diagnostics "${REPORTS_DIR}/hand_worked_diagnostics.json" \
  --depth-k 2 \
  --tick-size 0.01 \
  --run-id "hand-worked-l3-v1" \
  --strict-fail-on-invalid

echo "=== 3. Generate deterministic synthetic signed-flow stream (seed=42) ==="
PYTHONPATH="${PROJECT_DIR}/python" python3 -m mmae_analytics.cli generate-synthetic \
  --output "${WORK_DIR}/synthetic_signed_flow.csv" \
  --seed 42 \
  --steps 1200 \
  --symbol "SYNTH_FLOW"

echo "=== 4. Replay synthetic stream with baseline and optimized variants ==="
"${BUILD_DIR}/mmae_cli" \
  --config "${PROJECT_DIR}/fixtures/synthetic_signed_flow_config.json" \
  --variant baseline \
  --input "${WORK_DIR}/synthetic_signed_flow.csv" \
  --output-features "${WORK_DIR}/synthetic_features_baseline.csv" \
  --output-diagnostics "${REPORTS_DIR}/synthetic_diagnostics_baseline.json" \
  --run-id "synthetic-seed42-baseline" \
  --strict-fail-on-invalid

"${BUILD_DIR}/mmae_cli" \
  --config "${PROJECT_DIR}/fixtures/synthetic_signed_flow_config.json" \
  --variant optimized \
  --input "${WORK_DIR}/synthetic_signed_flow.csv" \
  --output-features "${WORK_DIR}/synthetic_features_optimized.csv" \
  --output-diagnostics "${REPORTS_DIR}/synthetic_diagnostics_optimized.json" \
  --run-id "synthetic-seed42-optimized" \
  --strict-fail-on-invalid

echo "=== 5. Verify exact baseline vs optimized parity ==="
PYTHONPATH="${PROJECT_DIR}/python" python3 -m mmae_analytics.cli verify-parity \
  --baseline-features "${WORK_DIR}/synthetic_features_baseline.csv" \
  --optimized-features "${WORK_DIR}/synthetic_features_optimized.csv" \
  --output "${REPORTS_DIR}/parity_report.json"

echo "=== 6. Run chronological price-response, adverse-selection & OOS evaluation ==="
PYTHONPATH="${PROJECT_DIR}/python" python3 -m mmae_analytics.cli evaluate \
  --features "${WORK_DIR}/synthetic_features_optimized.csv" \
  --diagnostics "${REPORTS_DIR}/synthetic_diagnostics_optimized.json" \
  --output "${REPORTS_DIR}/synthetic_evaluation.json" \
  --horizons-ms "50,100,250,500" \
  --train-fraction 0.70

echo "=== 7. Run baseline vs optimized C++ hot-path benchmark ==="
"${BUILD_DIR}/mmae_bench" \
  --events 120000 \
  --warmup 20000 \
  --seed 20261127 \
  --output-json "${REPORTS_DIR}/benchmark_results.json"

echo "=== Demo complete. Raw reports written to ${REPORTS_DIR} ==="
