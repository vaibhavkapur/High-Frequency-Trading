# UDP Multicast Market Data Distribution & Recovery

> A C++20 market-data network lab with binary packet framing, CRC32 validation, bounded reordering, sequence-gap detection, TCP retransmission/snapshot recovery, and fault injection.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20, POSIX/Linux/macOS sockets, IPv4 UDP multicast, TCP, CMake, tcpdump  
**Status:** Implemented and verified — all unit, property/fuzz, sanitizer (ASan/UBSan/TSan), and live multicast integration tests pass, with reproducible localhost lab benchmarks in [`results/`](results/).

---

## Why build this?

Simulated exchange multicast publisher and multiple concurrent UDP multicast subscribers that maintain deterministic, byte-for-byte identical order-book state under synthetic packet drops, reordering, duplication, CRC corruption, and full disconnects.

---

## Architecture

```text
ExchangeSimulator ──▶ MulticastFeedPublisher ──(IPv4 UDP Multicast, MDF1)──▶ Multicast Group (239.255.x.x)
                            │                                                      │             │
                            │ record_packet() +                                    ▼             ▼
                            │ replay checkpoints                            Subscriber A   Subscriber B
                            ▼                                               (Reference)    (+FaultInjector)
                      RecoveryStore                                                │             │
                      (ring buffer + checkpoints)                                  └──────┬──────┘
                            ▲                                                             │
                            └─────── TcpRecoveryServer (MDR1) ◀── TCP Retransmit / Snapshot Reqs
```

See [`docs/protocol.md`](docs/protocol.md) for the exact byte-level wire layout (`MDF1` UDP packets and `MDR1` TCP recovery frames) and [`docs/design.md`](docs/design.md) for the receiver state machine, invariants, and OS socket tuning notes.

---

## Implemented Capabilities

- **Binary wire protocol (`MDF1`)**: 36-byte big-endian header (`magic`, `version`, `flags`, `channel_id`, `msg_count`, `payload_len`, `seq_num`, `send_ts_ns`, `crc32`) separated from length-prefixed market events (`AddOrder`, `ModifyOrder`, `CancelOrder`, `Trade`, `ClearBook`), allowing `0..N` events per MTU-bounded UDP datagram plus non-sequencing heartbeats for tail-loss detection.
- **IPv4 UDP multicast transport**: Configurable group IP, interface IP, port, TTL, `IP_MULTICAST_LOOP`, `SO_REUSEADDR`/`SO_REUSEPORT` (multi-subscriber same-host fan-out), `SO_RCVBUF`/`SO_SNDBUF` sizing with actual kernel-granted buffer inspection, Linux `recvmmsg()` batch receive (with portable `recvfrom()`/`MSG_DONTWAIT` drain fallback on macOS), optional Linux `SO_BUSY_POLL` and `pthread_setaffinity_np` pinning.
- **Sequencer & recovery state machine (`ChannelReceiver`)**:
  - Per-channel `LIVE` → `GAP_WAIT` → `SYNCING_SNAPSHOT` → `LIVE` state machine with gap alarms and per-channel telemetry counters.
  - Bounded reordering window (`reorder_window_packets`, `reorder_timeout_us`, `max_reorder_buffer`) that resolves naturally reordered datagrams locally before invoking TCP.
  - Explicit rejection and counting of duplicates, stale incrementals, malformed frames, and CRC32 failures.
  - Automatic escalation from TCP retransmission to **Snapshot + Incremental Synchronization** whenever a gap is evicted from the server's bounded retransmit ring or exceeds `max_retransmit_range`.
  - Strict snapshot-plus-incremental invariant: loading a replay checkpoint at `as_of_seq = N` discards all staged incrementals `<= N` and replays only incrementals strictly beyond `N`.
- **Separate TCP snapshot & retransmission service (`TcpRecoveryServer` / `TcpRecoveryClient`)**: Multi-client `MDR1` binary framed TCP service backed by a thread-safe `RecoveryStore` with bounded per-channel retransmission ring buffers and automatic consistent replay checkpoints.
- **Deterministic fault injector & state verification**: PRNG-seeded drop, explicit disconnect/burst drop ranges, reordering, duplication, single-bit corruption, and jitter injector, validated with canonical 64-bit FNV-1a `ChannelMarketState::state_hash()` and full structural equality checks.

---

## Repository Structure

```text
06-udp-multicast-feed/
├── CMakeLists.txt
├── .clang-format
├── .github/workflows/ci.yml
├── include/mdf/
│   ├── crc32.hpp                   # Constexpr table-driven IEEE CRC32
│   ├── protocol.hpp                # MDF1 UDP & MDR1 TCP wire codecs
│   ├── order_book.hpp              # L3/L2 ChannelMarketState, snapshots, canonical hash
│   ├── fault_injector.hpp          # Deterministic loss/reorder/dup/corrupt/jitter injector
│   ├── socket_utils.hpp            # POSIX socket tuning, TCP framing, affinity/busy-poll helpers
│   ├── multicast_transport.hpp     # UdpMulticastSender & UdpMulticastReceiver
│   ├── recovery_service.hpp        # RecoveryStore, TcpRecoveryServer, TcpRecoveryClient
│   ├── channel_receiver.hpp        # Gap detection, reorder window, recovery state machine
│   └── exchange_simulator.hpp      # ExchangeSimulator, MulticastFeedPublisher, FeedSubscriber
├── src/                            # Production implementations
├── tests/
│   ├── test_framework.hpp          # Zero-dependency C++20 test harness
│   ├── test_wire.cpp               # Codec round-trips, endianness, CRC32 & negative cases
│   ├── test_order_book.cpp         # OrderBook ops, snapshots, order-independent hashing
│   ├── test_sequencer.cpp          # Gap alarms, reorder window, dup/corrupt, snapshot@N invariant
│   ├── test_fault_recovery.cpp     # Deterministic retransmit, snapshot escalation, multi-channel
│   ├── test_fuzz_property.cpp      # 50k mutated/random binary fuzz inputs + 40 randomized fault trials
│   └── test_multicast_integration.cpp # Live IPv4 UDP multicast + multi-subscriber TCP recovery
├── benchmarks/
│   ├── codec_bench.cpp             # MDF1 encode/decode + CRC32 microbenchmark
│   └── multicast_bench.cpp         # Batching, socket buffer/polling, and loss-recovery lab benchmarks
├── examples/
│   ├── demo.cpp                    # Live 3-subscriber convergence demo
│   ├── publisher_main.cpp          # Standalone CLI multicast publisher + TCP recovery server
│   ├── subscriber_main.cpp         # Standalone CLI multicast subscriber
│   └── sample_config.txt           # Two-terminal CLI + tcpdump instructions
├── scripts/
│   ├── test_all.sh                 # Release + ASan/UBSan + TSan build & test runner
│   ├── run_benchmarks.sh           # Reproducible benchmark + metadata capture script
│   ├── capture_tcpdump.sh          # Localhost tcpdump filter for MDF1/MDR1 packets
│   └── check_format.sh             # clang-format verification script
├── docs/
│   ├── protocol.md                 # Wire format specification
│   ├── design.md                   # Architecture, state machine, assumptions & platform notes
│   └── sample-output.txt           # Captured output from ./build/mdf_demo
└── results/                        # Raw benchmark outputs & environment metadata
```

---

## Benchmarks & Measured Evidence

All measurements below were captured on a single host over localhost IPv4 UDP multicast (`239.255.20.x`, interface `127.0.0.1`) and loopback TCP (`127.0.0.1`) via `./scripts/run_benchmarks.sh`.

### Environment Metadata ([`results/environment.txt`](results/environment.txt))

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5 (10 cores, arm64) |
| OS / kernel | Darwin 25.5.0 arm64 (macOS) |
| Compiler / version / flags | Apple clang version 21.0.0 (`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`) |
| Dataset / event mix / sample count | Synthetic `ExchangeSimulator` (4 instruments; 45% Add, 20% Modify, 20% Cancel, 15% Trade); 10k packets/batching run, 8k unpaced burst, 2k packets/loss scenario |
| Warmup / affinity / governor | 1,000-packet warmup (batching), 20,000-iter warmup (codec); macOS default scheduler (`pthread_setaffinity_np` / `SO_BUSY_POLL` / `recvmmsg` are Linux-only) |
| Peak measured event throughput | **2,343,201 events/sec** (32 events/datagram, 73,225 pkts/sec) over live localhost UDP multicast; **18.16M events/sec** wire decode+CRC32 in-memory |
| End-to-end latency (p50 / p99 / p99.9) | **9.3 us / 82.5 us / 334.1 us** (4 events/pkt); **10.6 us / 64.2 us / 296.6 us** (16 events/pkt) |
| Raw results and reproduction command | [`results/batching_tradeoff.csv`](results/batching_tradeoff.csv), [`results/socket_tuning_tradeoff.csv`](results/socket_tuning_tradeoff.csv), [`results/loss_recovery_bench.csv`](results/loss_recovery_bench.csv), [`results/codec_bench.txt`](results/codec_bench.txt) — reproduce with `./scripts/run_benchmarks.sh` |

### 1. Batching Tradeoff: Events per Datagram ([`results/batching_tradeoff.csv`](results/batching_tradeoff.csv))

| Events/Packet | Packets/sec | Market Events/sec | E2E p50 (us) | E2E p99 (us) | E2E p99.9 (us) | Gaps | Process CPU (ms) | Book Hash Match |
|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| 1 | 142,045 | 142,045 | 11.6 | 1,599.9 | 1,930.0 | 0 | 115.6 | OK |
| 4 | 160,539 | 642,154 | 9.3 | 82.5 | 334.1 | 0 | 101.0 | OK |
| 16 | 111,541 | 1,784,652 | 10.6 | 64.2 | 296.6 | 0 | 158.0 | OK |
| 32 | 73,225 | 2,343,201 | 14.7 | 152.8 | 666.5 | 0 | 248.1 | OK |

**Takeaway:** Packing 16 events per datagram raises event throughput by **12.6x** over 1-event datagrams while cutting p99 latency from 1,599.9 us to 64.2 us by amortizing per-datagram syscall and socket-queue overhead. Packing 32 events pushes throughput to 2.34M events/sec at the cost of higher per-packet decode/CRC32/book-apply tail latency.

### 2. Socket Buffer & Polling Mode Tradeoff ([`results/socket_tuning_tradeoff.csv`](results/socket_tuning_tradeoff.csv))

Unpaced full-speed burst of 8,000 packets (64,000 market events) to expose `SO_RCVBUF` pressure and polling tradeoffs:

| Configuration | Actual `SO_RCVBUF` | Packets/sec | E2E p50 (us) | E2E p99 (us) | Gaps Detected | Retransmitted Pkts | Process CPU (ms) | Book Hash Match |
|---|---:|---:|---:|---:|---:|---:|---:|:---:|
| `rcvbuf_64k_blocking_b1` | 65,536 B | 108,823 | 284.5 | 2,557.3 | 30 | 506 | 86.2 | OK |
| `rcvbuf_2m_blocking_b1` | 2,097,152 B | 114,433 | 10.7 | 371.4 | 0 | 0 | 81.1 | OK |
| `rcvbuf_2m_blocking_b32` | 2,097,152 B | 128,203 | 9.5 | 39.8 | 0 | 0 | 73.5 | OK |
| `rcvbuf_2m_nonblock_b32` | 2,097,152 B | 122,848 | 10.2 | 30.6 | 0 | 0 | 89.6 | OK |

**Takeaway:** A 64 KiB socket receive buffer overflows under an unpaced burst, dropping 506 packets across 30 gaps (all recovered via TCP to an exact book hash match, at 284.5 us median latency). Raising `SO_RCVBUF` to 2 MiB eliminates all kernel drops; adding batch draining (`b32`) reduces p99 latency by **9.3x** (371.4 us → 39.8 us) with lower CPU; switching to non-blocking busy-spin pushes p99 down further to **30.6 us** at the cost of **+22% CPU time** (89.6 ms vs 73.5 ms).

### 3. Injected Packet Loss & Recovery Latency ([`results/loss_recovery_bench.csv`](results/loss_recovery_bench.csv))

2,000 packets (8,000 events), server retransmit ring = 150 packets, checkpoint interval = 75 packets:

| Scenario | Injected Drops | Gaps Detected | Retransmitted Pkts | Snapshots Applied | Recovery p50 (us) | Recovery p99 (us) | Book Hash Match |
|---|---:|---:|---:|---:|---:|---:|:---:|
| `loss_0.0pct` | 0 | 0 | 0 | 0 | 0.0 | 0.0 | OK |
| `loss_0.5pct` | 17 | 17 | 17 | 0 | 73.9 | 374.4 | OK |
| `loss_2.0pct` | 44 | 40 | 44 | 0 | 116.7 | 917.4 | OK |
| `loss_5.0pct` | 104 | 83 | 104 | 0 | 101.2 | 3,615.6 | OK |
| `disconnect_250pkts` | 250 | 1 | 24 | 1 | 408.2 | 408.2 | OK |

### 4. Wire Codec + CRC32 Microbenchmark ([`results/codec_bench.txt`](results/codec_bench.txt))

| Events/Packet | Wire Size (B) | Encode (Mpps) | Decode (Mpps) | Decode (M events/s) | Encode p50 (ns) | Decode p50 (ns) | Decode p99 (ns) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 6.60 | 14.25 | 14.25 | 125 | 42 | 84 |
| 4 | 148 | 2.93 | 3.48 | 13.92 | 333 | 250 | 417 |
| 16 | 484 | 0.85 | 0.95 | 15.25 | 1,125 | 1,000 | 1,458 |
| 32 | 932 | 0.49 | 0.57 | 18.16 | 2,000 | 1,750 | 2,167 |

---

## Local Development & Verification

```bash
# Release build and full test suite (unit, property/fuzz, and live multicast integration)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Run Release + ASan/UBSan + TSan builds and tests
./scripts/test_all.sh

# Run the live 3-subscriber convergence demo (sample output in docs/sample-output.txt)
./build/mdf_demo

# Reproduce all benchmarks and capture environment metadata
./scripts/run_benchmarks.sh
```

To run publisher and subscriber in separate terminals (with optional `tcpdump` capture), see [`examples/sample_config.txt`](examples/sample_config.txt).

---

## Correctness & Failure-Mode Coverage

- [x] A dropped packet triggers a gap alarm rather than silent book corruption (`test_sequencer`).
- [x] Recovery yields identical book state and 64-bit state hash to an uninterrupted receiver (`test_fault_recovery`, `test_multicast_integration`).
- [x] Out-of-order, duplicate, and CRC-corrupted datagrams are handled and counted explicitly (`test_sequencer`, `test_wire`).
- [x] A snapshot at sequence `N` discards stale buffered incrementals `<= N` and applies only incrementals strictly beyond `N` (`test_sequencer`).
- [x] Multiple live multicast subscribers converge after random drops, reordering, corruption, and an induced 200+ packet disconnect (`test_multicast_integration`, `mdf_demo`).
- [x] 50,000 mutated/random binary decoder fuzz inputs + 40 randomized fault-schedule property runs, verified under `-Werror`, ASan+UBSan, and TSan (`test_fuzz_property`, `scripts/test_all.sh`).

---

## Definition of Done

- [x] Working implementation with a documented supported feature/protocol subset ([`docs/protocol.md`](docs/protocol.md))
- [x] Unit, integration, negative-case and replay/stress/fuzz tests ([`tests/`](tests/))
- [x] Reproducible benchmark script and machine/environment metadata ([`scripts/run_benchmarks.sh`](scripts/run_benchmarks.sh), [`results/environment.txt`](results/environment.txt))
- [x] Measured performance tradeoffs (batching, `SO_RCVBUF`/polling mode, and loss/recovery latency), with raw CSV results ([`results/`](results/))
- [x] Architecture diagram and explicit assumptions/limitations ([`docs/design.md`](docs/design.md))
- [x] Terminal demo and captured sample output showing the complete workflow ([`examples/demo.cpp`](examples/demo.cpp), [`docs/sample-output.txt`](docs/sample-output.txt))
- [x] CI that builds, runs tests (Release + ASan/UBSan + TSan) and checks formatting ([`.github/workflows/ci.yml`](.github/workflows/ci.yml))

---

## CV Bullet

> Built a C++20 IPv4 UDP multicast market-data transport (`MDF1`) and TCP snapshot/retransmission service (`MDR1`) with CRC32 framing, bounded reordering, gap detection, and deterministic fault injection; validated exact order-book hash convergence across 0.5%–5% packet loss, bit corruption, and 250-packet disconnects (408 us snapshot+incremental recovery, up to 2.34M events/sec localhost multicast throughput).

---

## Caveats / Non-Goals

- Kernel bypass (DPDK/AF_XDP) is out of scope for this portable POSIX lab.
- Localhost loopback multicast measurements are not physical NIC or exchange-colocation measurements.
- `MDF1`/`MDR1` is a documented educational protocol and does not claim wire compatibility with proprietary venue feeds.
