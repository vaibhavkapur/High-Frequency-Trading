# Integration & Adapter Boundaries — Kernel Bypass Networking Lab

## 1. Repository Context

This project lives in `Trading-High-Frequency/projects/12-kernel-bypass-networking-lab/` alongside sibling projects `01`–`17` under `projects/`. It is self-contained with zero cross-project build coupling and exposes clean public C++20 interfaces designed for downstream composition rather than copy-pasted dependencies.

No proprietary, employer, or SAAT code, credentials, or production data is referenced or imported.

## 2. Reusable Public C++20 Interfaces (`kbnl_core`)

Other projects in `Trading-High-Frequency` can link against the `kbnl_core` static library and consume these stable boundaries:

| Header | Primary Interface | Intended Integration Consumer |
|---|---|---|
| `include/kbnl/transport.hpp` | `PacketReceiver`, `PacketSender`, `probe_backend_capability()` | `numa-aware-trading-engine` feed ingestion adapters |
| `include/kbnl/ring_buffer.hpp` | `SpscRingBuffer<T>` | Lock-free single-producer/single-consumer handoff between NIC poll thread and matching/book thread |
| `include/kbnl/protocol.hpp` | `encode_packet()`, `decode_packet()`, `PacketHeader` | Synthetic sequenced feed replay for `fpga-order-book-accelerator` or `order-book-compression-engine` |
| `include/kbnl/sequence_tracker.hpp` | `SequenceTracker`, `SequenceCounters` | Gap detection and loss/reorder accounting in downstream market-data handlers |
| `include/kbnl/clock.hpp` | `now_ns()`, `get_clock_metadata()` | Extension point for `precision-time-synchronization-platform` (swap/augment with PTP hardware timestamps) |
| `include/kbnl/latency_stats.hpp` | `LatencyStats`, `LatencySummary` | Uniform p50/p95/p99/p99.9 latency reporting across benchmarks |

## 3. Transport Backend Matrix & Feature Gates

| Backend Name | CMake Gate | OS Requirement | Runtime Privilege / Hardware Requirement | Behavior When Unavailable |
|---|---|---|---|---|
| `posix_udp_blocking` | Always enabled | macOS, Linux, POSIX | None (loopback or NIC) | Available everywhere |
| `posix_udp_busy_poll` | Always enabled | macOS, Linux, POSIX | None (loopback or NIC) | Available everywhere |
| `posix_udp_recvmmsg` | Always compiled on Linux | Linux only | None (loopback or NIC) | Returns `NOT_RUN` (`UNSUPPORTED_PLATFORM`) on non-Linux |
| `af_xdp` | `-DKBNL_ENABLE_AF_XDP=ON` | Linux `>= 4.18` with `<linux/if_xdp.h>` | Root or `CAP_NET_RAW` + `CAP_NET_ADMIN` + `CAP_BPF`, valid NIC/veth | Returns `NOT_RUN` (`UNSUPPORTED_PLATFORM`, `FEATURE_DISABLED`, `MISSING_CAPABILITY`, or `DEVICE_UNAVAILABLE`) |
| `dpdk` | `-DKBNL_ENABLE_DPDK=ON` | Linux with `libdpdk` (`pkg-config`) | Root/VFIO, hugepages, DPDK-bound Ethernet port | Returns `NOT_RUN` (`UNSUPPORTED_PLATFORM`, `FEATURE_DISABLED`, `MISSING_CAPABILITY`, or `DEVICE_UNAVAILABLE`) |

## 4. Compatibility & Dependency Provenance

- **Language standard:** C++20 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, zero warnings).
- **Build system:** CMake `>= 3.20` with standard `Threads::Threads`.
- **Third-party runtime dependencies:** None required for baseline build, tests, and benchmarks. Optional Linux integrations use standard kernel UAPI (`<linux/if_xdp.h>`, GPL-2.0 WITH Linux-syscall-note) and optional `libdpdk` (BSD-3-Clause) strictly behind off-by-default CMake feature gates.
- **Scripts:** Python `>= 3.9` standard library only (`json`, `os`, `platform`, `shutil`, `subprocess`, `sys`, `datetime`).
