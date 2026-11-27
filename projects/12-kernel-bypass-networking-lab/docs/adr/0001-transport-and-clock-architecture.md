# ADR-0001: Transport Abstraction, Clock Policy, and Honest Capability Reporting

**Date:** 2026-11-27  
**Status:** Accepted  
**Project:** Kernel Bypass Networking Lab (`projects/12-kernel-bypass-networking-lab`)

## Context

Benchmarking market-data receive paths requires comparing conventional OS sockets against kernel-bypass alternatives (`AF_XDP`, `DPDK`) under identical packet payloads, pacing rates, and sequence accounting. However, developer workstations and CI runners (including macOS Apple Silicon and unprivileged Linux containers) often lack `AF_XDP` privileges, NIC hardware queues, or DPDK hugepages/VFIO bindings.

A common anti-pattern in networking benchmarks is silently falling back to a software mock or synthetic latency sleep when a bypass backend is unavailable, contaminating reports with simulated numbers presented as hardware measurements.

## Decision

1. **Pure Domain Isolation:** Separate packet encoding/decoding (`PacketCodec`), lock-free queuing (`SpscRingBuffer`), sequence accounting (`SequenceTracker`), pacing (`PacerSchedule`), and latency quantiles (`LatencyStats`) from all socket/NIC I/O. Every transport adapter feeds `ReceivedFrame` instances into the exact same consumer pipeline.
2. **Feature-Gated Bypass Adapters with Explicit Runtime Probes:**
   - Gate `AF_XDP` behind `KBNL_ENABLE_AF_XDP` and `DPDK` behind `KBNL_ENABLE_DPDK`.
   - Always compile the adapter interfaces and capability probes on every OS so tests can verify failure behavior everywhere.
   - When an adapter is unavailable due to OS, compile flag, missing capability, or missing NIC, emit `execution_status: "NOT_RUN"`, `is_simulated: false`, and an explicit `StatusCode` (`UNSUPPORTED_PLATFORM`, `FEATURE_DISABLED`, `MISSING_CAPABILITY`, `DEVICE_UNAVAILABLE`). Never fall back silently or emit synthetic latency numbers.
3. **Like-for-Like Baseline Comparison on All Hosts:** Provide two genuinely runnable POSIX UDP receive policies (`posix_udp_blocking` using `poll()+recvfrom()` and `posix_udp_busy_poll` using a non-blocking spin drain) plus Linux `posix_udp_recvmmsg`, enabling real apples-to-apples comparisons even on macOS while keeping unrun bypass paths explicitly labeled `NOT_RUN`.
4. **Documented Same-Host Monotonic Clock:** Standardize on `CLOCK_MONOTONIC_RAW` (`clock_gettime`), measure and record call overhead in every report, and mark `same_host_domain_required: true`. Cross-host measurements require external PTP/hardware timestamping and are excluded from loopback claims.

## Consequences

- **Positive:** Every reported metric is either a genuine measured observation (`MEASURED`), a deterministic oracle check (`FIXTURE_VERIFICATION`), a labeled in-memory domain microbenchmark (`DOMAIN_HOTPATH_MICROBENCH`), or an explicit `NOT_RUN` probe. Zero misleading numbers.
- **Positive:** CI can validate all core domain invariants, boundary failures, and loopback benchmarks on both Linux and macOS without privileged hardware runners.
- **Trade-off:** Obtaining measured `AF_XDP` and `DPDK` latency curves requires executing the compiled binary on a privileged Linux host with an XDP/DPDK-capable NIC.
