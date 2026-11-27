#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/integration/results/latest}"
EVENTS="${EVENTS:-500}"
REPS="${REPS:-5}"
SAMPLES="${SAMPLES:-20}"

cmake -S "${ROOT_DIR}/engine" -B "${ROOT_DIR}/build/engine" -DCMAKE_BUILD_TYPE=Release -DHFT_WERROR=ON
cmake --build "${ROOT_DIR}/build/engine" --target hft_bench --parallel

mkdir -p "${OUT_DIR}"
"${ROOT_DIR}/build/engine/hft_bench" \
  --out-dir "${OUT_DIR}" \
  --events "${EVENTS}" \
  --reps "${REPS}" \
  --samples "${SAMPLES}"

if command -v git >/dev/null 2>&1; then
  git -C "${ROOT_DIR}" rev-parse HEAD > "${OUT_DIR}/git_sha.txt" || true
fi

if command -v python3 >/dev/null 2>&1; then
  python3 "${ROOT_DIR}/scripts/summarize_benchmarks.py" "${OUT_DIR}" > "${OUT_DIR}/summary.md"
  echo "Generated ${OUT_DIR}/summary.md"
fi
