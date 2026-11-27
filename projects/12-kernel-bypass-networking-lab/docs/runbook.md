# Operator & Developer Runbook — Kernel Bypass Networking Lab

## 1. Prerequisites

- **macOS (tested on Apple M5, macOS 26.5.1, AppleClang 21.0.0) or Linux (GCC 11+ / Clang 14+)**
- CMake `>= 3.20`, Make or Ninja, Python `>= 3.9`
- **Optional (Linux kernel-bypass runs only):**
  - AF_XDP: Linux kernel `>= 4.18`, `<linux/if_xdp.h>`, `CAP_NET_RAW`/`CAP_NET_ADMIN`/`CAP_BPF` (or `sudo`), XDP-capable NIC or `veth` pair.
  - DPDK: `libdpdk-dev` discoverable via `pkg-config`, mounted 2 MB / 1 GB hugepages, VFIO/UIO-bound NIC.

## 2. Build and Full Demo Walkthrough

From `projects/12-kernel-bypass-networking-lab/`:

```bash
bash scripts/run_demo.sh
```

This runs four steps end-to-end:
1. Configures and builds all libraries, CLIs, microbenchmarks, and tests in `Release` mode.
2. Generates the canonical deterministic fixture `fixtures/deterministic_feed_v1.bin`.
3. Runs all 4 CTest suites (`protocol_and_config`, `domain_sequence_and_ring`, `adapters_and_failures`, `integration_loopback`).
4. Runs `scripts/run_benchmarks.sh` to collect `reports/environment_manifest.json`, execute all supported loopback benchmarks, run the domain hot-path microbenchmark, probe gated Linux/NIC backends, replay the deterministic fixture, and verify all invariants via `scripts/verify_acceptance.py`.

## 3. Manual CLI Usage

### Standalone Paced Sender & Receiver (Two Terminals)
Terminal 1 (receiver):
```bash
./build/kbnl_receiver --backend posix_udp_busy_poll --bind-ip 127.0.0.1 --port 9000 --batch 32
```
Terminal 2 (sender):
```bash
./build/kbnl_sender --target-ip 127.0.0.1 --port 9000 --pps 25000 --warmup 1000 --count 10000 --bytes 64 --seed 20261127
```

### Benchmark Runner & Capability Probe
```bash
# Run a scenario with explicit backend override
./build/kbnl_bench --config benchmarks/scenarios/baseline_loopback_10k.json \
                   --backend posix_udp_busy_poll \
                   --output reports/custom_run.json

# Probe AF_XDP / DPDK availability without faking results
./build/kbnl_bench --probe af_xdp --interface eth0
./build/kbnl_bench --probe dpdk --interface eth0

# Replay deterministic binary fixture
./build/kbnl_bench --replay-fixture fixtures/deterministic_feed_v1.bin
```

## 4. Enabling Feature-Gated Linux Paths (Linux Hosts Only)

### AF_XDP Build & Execution
```bash
cmake -S . -B build-xdp -DCMAKE_BUILD_TYPE=Release -DKBNL_ENABLE_AF_XDP=ON
cmake --build build-xdp -j
sudo ./build-xdp/kbnl_bench --config benchmarks/scenarios/linux_af_xdp_template.json \
                            --output reports/af_xdp_run.json
```
*(Adjust `interface_name`, `bind_ip`, `target_ip`, and `rx_queue_id` in the template config to match your NIC/veth pair and ensure an XDP redirect program or `XSKMAP` is attached for queue steering.)*

### DPDK Build & Execution
```bash
cmake -S . -B build-dpdk -DCMAKE_BUILD_TYPE=Release -DKBNL_ENABLE_DPDK=ON
cmake --build build-dpdk -j
# Configure hugepages and bind NIC to vfio-pci first, then:
sudo ./build-dpdk/kbnl_bench --config benchmarks/scenarios/linux_af_xdp_template.json \
                             --backend dpdk --output reports/dpdk_run.json
```

## 5. Diagnostics & Recovery

| Symptom / Status Code | Meaning | Recovery Action |
|---|---|---|
| `INVALID_CONFIG` / exit code 2 | Schema violation (e.g., `packet_bytes < 64`, `target_pps == 0`, non-power-of-two `ring_capacity`) | Fix the reported field in JSON config; compare against `docs/contracts.md`. |
| `UNSUPPORTED_PLATFORM` (`NOT_RUN`) | `af_xdp`, `dpdk`, `posix_udp_recvmmsg`, or `strict_pinning=true` requested on non-Linux OS | Expected on macOS. Run on a Linux host for kernel-bypass paths, or set `strict_pinning: false` for baseline macOS runs. |
| `FEATURE_DISABLED` (`NOT_RUN`) | Binary built without `-DKBNL_ENABLE_AF_XDP=ON` or `-DKBNL_ENABLE_DPDK=ON` | Reconfigure CMake with the desired feature flag on Linux. |
| `MISSING_CAPABILITY` (`NOT_RUN`) | Unprivileged user attempted `AF_XDP` or `DPDK` | Run with `CAP_NET_RAW`/`CAP_NET_ADMIN`/`CAP_BPF` or `sudo` on dedicated lab host. |
| `IO_ERROR: sendto() failed: Operation not permitted` | Process sandbox or firewall blocked loopback UDP egress | Run outside restrictive network sandbox or allow `127.0.0.1` UDP traffic. |
| Non-zero `ring_drops` | Consumer thread fell behind receiver I/O thread | Increase `topology.ring_capacity` (power of two) or reduce `target_pps` / isolate cores. |
| Non-zero `inferred_total_lost` on UDP | Kernel socket buffer overflow under burst load | Increase `socket_rcvbuf_bytes` (and OS `net.core.rmem_max` / `kern.ipc.maxsockbuf`), or reduce burst size. |
