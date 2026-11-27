#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
REPORTS_DIR="${PROJECT_DIR}/reports"

mkdir -p "${REPORTS_DIR}"

echo "[1/5] Collecting hardware/environment manifest..."
python3 "${SCRIPT_DIR}/collect_env_manifest.py" "${REPORTS_DIR}/environment_manifest.json"

echo "[2/5] Running baseline loopback scenarios (identical workload across supported receive policies)..."
"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/baseline_loopback_10k.json" \
  --backend posix_udp_blocking \
  --output "${REPORTS_DIR}/baseline_loopback_10k_blocking.json"

"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/baseline_loopback_10k.json" \
  --backend posix_udp_busy_poll \
  --output "${REPORTS_DIR}/baseline_loopback_10k_busy_poll.json"

"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/baseline_loopback_50k.json" \
  --backend posix_udp_blocking \
  --output "${REPORTS_DIR}/baseline_loopback_50k_blocking.json"

"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/baseline_loopback_50k.json" \
  --backend posix_udp_busy_poll \
  --output "${REPORTS_DIR}/baseline_loopback_50k_busy_poll.json"

"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/burst_loopback.json" \
  --backend posix_udp_blocking \
  --output "${REPORTS_DIR}/burst_loopback_blocking.json"

"${BUILD_DIR}/kbnl_bench" \
  --config "${PROJECT_DIR}/benchmarks/scenarios/burst_loopback.json" \
  --backend posix_udp_busy_poll \
  --output "${REPORTS_DIR}/burst_loopback_busy_poll.json"

echo "[3/5] Running domain hot-path microbenchmark (in-memory SPSC + decode + sequence tracker)..."
"${BUILD_DIR}/kbnl_hotpath_bench" \
  --iterations 200000 \
  --output "${REPORTS_DIR}/hotpath_microbench.json"

echo "[4/5] Probing feature-gated Linux/NIC bypass backends (explicit NOT_RUN if unavailable)..."
"${BUILD_DIR}/kbnl_bench" --probe posix_udp_recvmmsg --output "${REPORTS_DIR}/recvmmsg_status.json"
"${BUILD_DIR}/kbnl_bench" --probe af_xdp --interface eth0 --output "${REPORTS_DIR}/af_xdp_status.json"
"${BUILD_DIR}/kbnl_bench" --probe dpdk --interface eth0 --output "${REPORTS_DIR}/dpdk_status.json"

echo "[5/5] Replaying deterministic fixture and verifying acceptance invariants..."
"${BUILD_DIR}/kbnl_bench" \
  --replay-fixture "${PROJECT_DIR}/fixtures/deterministic_feed_v1.bin" \
  --output "${REPORTS_DIR}/fixture_replay_result.json"

python3 "${SCRIPT_DIR}/verify_acceptance.py" "${PROJECT_DIR}"
echo "All benchmark artifacts generated and verified in ${REPORTS_DIR}."
