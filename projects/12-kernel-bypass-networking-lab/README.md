# Kernel Bypass Networking Lab

An educational and research C++20 benchmark suite for comparing market-data UDP receive paths under controlled, reproducible load — spanning baseline POSIX UDP sockets (`poll()+recvfrom()`, non-blocking busy-poll, Linux `recvmmsg()`), a feature-gated Linux `AF_XDP` adapter, and an optional feature-gated `DPDK` adapter.

> **Scope & Honesty Notice:** Educational/research prototype using synthetic sequenced UDP feeds only. No live capital, customer data, credentials, or proprietary artifacts are included. Paths requiring unavailable OS, privilege, or NIC hardware capabilities (`AF_XDP`, `DPDK`, Linux `recvmmsg()`) are explicitly marked **`NOT_RUN`** rather than simulated.

---

## Architecture Overview

- **Pure Domain Layer (`include/kbnl/`):** Explicit big-endian wire codec with CRC32 (`protocol.hpp`), cache-line aligned lock-free bounded SPSC ring buffer (`ring_buffer.hpp`), deterministic 1-based sequence tracker with a 1,024-sequence reorder bitmap (`sequence_tracker.hpp`), warmup-excluding latency quantile collector (`latency_stats.hpp`), and deterministic pacer (`pacer.hpp`).
- **Transport Adapters (`src/*_transport.cpp`):** Uniform `PacketReceiver` / `PacketSender` boundaries for `posix_udp_blocking`, `posix_udp_busy_poll`, Linux `posix_udp_recvmmsg`, feature-gated `af_xdp` (`-DKBNL_ENABLE_AF_XDP=ON`), and feature-gated `dpdk` (`-DKBNL_ENABLE_DPDK=ON`).
- **Harness & Reproducibility (`src/benchmark_runner.cpp`, `scripts/`):** Documented `CLOCK_MONOTONIC_RAW` clock calibration, CPU affinity reporting, deterministic binary fixture replay, hardware/environment manifest collection, and automated acceptance verification.

See [docs/architecture.md](docs/architecture.md) for the full Mermaid data-flow diagram and memory-ordering invariants, [docs/contracts.md](docs/contracts.md) for wire/JSON schemas, [docs/integration.md](docs/integration.md) for reusable library boundaries, [docs/runbook.md](docs/runbook.md) for diagnostics and Linux AF_XDP/DPDK instructions, and [docs/adr/0001-transport-and-clock-architecture.md](docs/adr/0001-transport-and-clock-architecture.md) for the architecture decision record.

---

## Quickstart & Reproducible Demo

### Prerequisites
- C++20 compiler (AppleClang 21 / Clang 14+ / GCC 11+)
- CMake `>= 3.20`, Make or Ninja, Python `>= 3.9` (standard library only; zero network fetches required)

### One-Command Build, Test, Benchmark & Verification
```bash
cd projects/12-kernel-bypass-networking-lab
bash scripts/run_demo.sh
```

### Manual Commands
```bash
# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Generate canonical deterministic fixture and run all 4 CTest suites
./build/kbnl_bench --generate-fixture fixtures/deterministic_feed_v1.bin
ctest --test-dir build --output-on-failure

# Run standalone paced sender and receiver (in separate terminals)
./build/kbnl_receiver --backend posix_udp_busy_poll --bind-ip 127.0.0.1 --port 9000
./build/kbnl_sender   --target-ip 127.0.0.1 --port 9000 --pps 10000 --warmup 1000 --count 10000

# Run full benchmark suite & verify acceptance invariants
bash scripts/run_benchmarks.sh
```

---

## Measured Results (Verified Host Observation)

All results below were **actually measured** on the host documented in [reports/environment_manifest.json](reports/environment_manifest.json):
- **Host:** macOS 26.5.1 (Darwin 25.5.0), Apple M5 (`arm64`), 10 logical CPUs, 16 GB RAM
- **Toolchain:** AppleClang 21.0.0, CMake 4.5.1, `-DCMAKE_BUILD_TYPE=Release`, C++20
- **Clock:** `CLOCK_MONOTONIC_RAW` (`clock_gettime`, same-host loopback domain)
- **Thread Pinning:** Requested (`sender_cpu=0`, `receiver_cpu=1`, `strict_pinning=false`); macOS Darwin does not expose strict userspace core pinning, recorded explicitly as `macos_unpinned_advisory` (`applied: false`).

### 1. Loopback UDP Receive Path Comparison (Measured, `is_simulated: false`)

| Scenario | Backend | Payload / Mode | Sent (Warmup+Meas) | Measured Recv | Lost | Achieved Recv PPS | p50 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) | Seq Verified | Raw Report |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|---|
| `baseline_loopback_10k` | `posix_udp_blocking` | 64 B / constant 10k pps | 11,000 | 10,000 | 0 | 10,001.0 | 13,375 | 38,417 | 59,000 | 74,916 | Yes | [json](reports/baseline_loopback_10k_blocking.json) |
| `baseline_loopback_10k` | `posix_udp_busy_poll` | 64 B / constant 10k pps | 11,000 | 10,000 | 0 | 10,001.0 | 9,125 | 27,708 | 48,125 | 63,334 | Yes | [json](reports/baseline_loopback_10k_busy_poll.json) |
| `baseline_loopback_50k` | `posix_udp_blocking` | 64 B / constant 50k pps | 27,000 | 25,000 | 0 | 50,002.0 | 13,875 | 27,166 | 48,708 | 83,666 | Yes | [json](reports/baseline_loopback_50k_blocking.json) |
| `baseline_loopback_50k` | `posix_udp_busy_poll` | 64 B / constant 50k pps | 27,000 | 25,000 | 0 | 50,002.0 | 9,000 | 25,167 | 55,708 | 74,458 | Yes | [json](reports/baseline_loopback_50k_busy_poll.json) |
| `burst_loopback` | `posix_udp_blocking` | 128 B / 64-pkt burst / 2 ms | 10,752 | 10,240 | 0 | 32,173.2 | 11,666 | 46,000 | 147,333 | 212,000 | Yes | [json](reports/burst_loopback_blocking.json) |
| `burst_loopback` | `posix_udp_busy_poll` | 128 B / 64-pkt burst / 2 ms | 10,752 | 10,240 | 0 | 32,170.6 | 18,083 | 70,167 | 100,209 | 104,917 | Yes | [json](reports/burst_loopback_busy_poll.json) |

**Observed takeaways:**
- Under steady 10k–50k pps loopback traffic, `posix_udp_busy_poll` reduced median (`p50`) one-way latency by **32%–35%** (~9.0–9.1 µs vs ~13.4–13.9 µs) by eliminating kernel `poll()` sleep/wakeup latency.
- Under 64-packet micro-bursts without strict OS core pinning on macOS, `posix_udp_busy_poll` lowered extreme tail latency (`p99.9`: 100.2 µs vs 147.3 µs; `max`: 104.9 µs vs 212.0 µs) while experiencing higher `p50`/`p99` due to sender/receiver CPU contention on unpinned cores.

### 2. Unrun Feature-Gated Linux / Kernel-Bypass Paths (Explicit `NOT_RUN`)

| Backend | Status | Reason Code | Reason | Evidence |
|---|---|---|---|---|
| `posix_udp_recvmmsg` | **`NOT_RUN`** | `UNSUPPORTED_PLATFORM` | `recvmmsg()` is a Linux-specific syscall unavailable on macOS | [recvmmsg_status.json](reports/recvmmsg_status.json) |
| `af_xdp` | **`NOT_RUN`** | `UNSUPPORTED_PLATFORM` | Requires Linux kernel `>= 4.18`, `<linux/if_xdp.h>`, `CAP_NET_RAW`/`CAP_NET_ADMIN`/`CAP_BPF`, and XDP-capable NIC/veth | [af_xdp_status.json](reports/af_xdp_status.json) |
| `dpdk` | **`NOT_RUN`** | `UNSUPPORTED_PLATFORM` | Requires Linux, `libdpdk`, hugepages, and VFIO/UIO-bound NIC | [dpdk_status.json](reports/dpdk_status.json) |

### 3. Pure Domain Hot-Path Microbenchmark (Not a Network Benchmark)

Measured in-memory overhead of `SpscRingBuffer` push/pop + CRC32 `decode_packet()` + `SequenceTracker` + `LatencyStats` over 200,000 iterations (after 10,000 warmup iterations) — explicitly labeled `DOMAIN_HOTPATH_MICROBENCH`:
- **Throughput:** 3,191,997 ops/sec | **p50:** 125 ns | **p99:** 250 ns | **p99.9:** 292 ns | **Drops / Errors:** 0 ([hotpath_microbench.json](reports/hotpath_microbench.json))

### 4. Deterministic Fixture Verification

Replaying [fixtures/deterministic_feed_v1.bin](fixtures/deterministic_feed_v1.bin) against the independent oracle [fixtures/expected_reference_result.json](fixtures/expected_reference_result.json) verified exact agreement on all sequence counters (12 sent, 10 unique received, 2 lost, 1 out-of-order fill, 1 duplicate, 1 CRC error, 2 warmup excluded) and exact latency quantiles (`min=1000`, `mean=1837.5`, `p50=1800`, `p99=3000`, `max=3000` ns) ([fixture_replay_result.json](reports/fixture_replay_result.json)).

---

## Acceptance Evidence & Known Limitations

- Full acceptance criteria and test-matrix mapping: **[reports/acceptance.md](reports/acceptance.md)**
- Consolidated machine-readable summary: **[reports/comparison_summary.json](reports/comparison_summary.json)**
- **Known limitations:**
  1. `AF_XDP`, `DPDK`, and Linux `recvmmsg()` cannot execute on macOS and are reported as `NOT_RUN`; run on a privileged Linux NIC host per [docs/runbook.md](docs/runbook.md) to capture kernel-bypass hardware numbers.
  2. Strict thread-to-core pinning (`pthread_setaffinity_np`) is Linux-only; macOS runs record pinning as advisory (`applied: false`), and `strict_pinning: true` fails explicitly.
  3. One-way latency uses `CLOCK_MONOTONIC_RAW` and is valid only within a single host clock domain; cross-host benchmarking requires hardware timestamping / PTP.
  4. CI workflow is provided at [.github/workflows/ci.yml](.github/workflows/ci.yml) (copy to repository root `.github/workflows/` if activating GitHub Actions for the parent monorepo).
