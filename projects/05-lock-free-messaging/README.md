# Lock-Free Ring Buffers & Low-Latency Messaging Library

> A correctness-first SPSC queue plus carefully scoped multi-producer experiments.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20 atomics, Google Benchmark, CMake, ThreadSanitizer, Linux perf  
**Status:** Implemented (M1–M5). Tests pass under ASan+UBSan and TSan on macOS/arm64. Benchmark numbers below are from one unpinned, loaded laptop — read the caveats before quoting them. The [project workflow](.github/workflows/ci.yml) records the CI commands.

## Why build this?

Show mastery of cache coherence and memory ordering by implementing a bounded SPSC queue, benchmarking it against a mutex queue and optionally extending to MPSC.

**Hiring signal:** Demonstrates design tradeoffs, production-style correctness tests, and measurable engineering relevant to HFT software roles. A finished implementation with reproducible results is more persuasive than an ambitious design alone.

## Architecture

```text
producer -> [cache-padded write cursor | power-of-two ring | read cursor] -> consumer
                release store                         acquire load
```

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
build/examples/market_data_pipeline        # backpressure vs drop demo
scripts/run_sanitizers.sh                  # ASan+UBSan and TSan test runs
scripts/run_benchmarks.sh my-label         # writes results/my-label/
```

Requires CMake ≥ 3.20 and a C++20 compiler (tested: Apple clang 21). Google
Benchmark is fetched at configure time for `bench_micro`; pass
`-DLFM_BUILD_GBENCH=OFF` to build offline. Tests have no external dependencies.

```cpp
#include "lfm/spsc_queue.hpp"

lfm::SpscQueue<Tick> q(1024);              // capacity: power of two, all slots usable

// producer thread                          // consumer thread
if (!q.try_push(tick)) { /* full */ }       Tick t;
q.try_push_batch(std::span{ticks});         if (q.try_pop(t)) { /* use t */ }
                                            q.consume([](Tick&& t) { /* in place */ });
```

`lfm::Producer` / `lfm::Consumer` (`lfm/endpoints.hpp`) add the overload policy
(`push` = backpressure, `push_or_drop` = drop newest), counters and wait
policies on top.

## What is implemented

| Requirement | Where |
|---|---|
| Fixed-capacity power-of-two SPSC queue, no allocation in push/pop | `include/lfm/spsc_queue.hpp` |
| Acquire/release ordering, padded cursor separation | same; rationale in [docs/design.md](docs/design.md) §3, §6 |
| `try_push`/`try_pop` full/empty semantics, batch APIs | `try_push`, `try_emplace`, `try_pop`, `try_push_batch`, `try_pop_batch`, `consume` |
| Ownership invariants | documented contract ([docs/design.md](docs/design.md) §10); not runtime-checked |
| Backpressure/drop counters, spin/yield wait policies | `include/lfm/endpoints.hpp`, `include/lfm/wait_policy.hpp` |
| Separate MPSC with reservation/publication rules | `include/lfm/mpsc_queue.hpp`; invariants + exhaustive model check in `tests/mpsc_model_check.cpp` |
| Mutex + condition-variable reference queue | `include/lfm/mutex_queue.hpp` |
| CPU affinity | `include/lfm/affinity.hpp` (hard pin on Linux; QoS hint only on macOS) |

## Core requirements

- Fixed-capacity power-of-two SPSC queue with no allocation during push/pop
- Correct acquire/release ordering and padded producer/consumer cursor separation
- Explicit `try_push`/`try_pop` behavior on full/empty and optional batch APIs
- Single producer and single consumer ownership invariants
- Backpressure/drop counters and optional spin/yield wait policies
- Optional **separate** MPSC implementation after defining reservation/publication rules

## Key engineering decisions

- Prefer an SPSC design first: it is easier to prove and benchmark than a generic “lock-free queue.”
- Use storage that correctly manages object construction/destruction; start with trivially copyable fixed-size messages.
- Bound the capacity and document non-blocking semantics. “Lock-free” means something specific; a consumer busy-looping is not itself proof.
- Benchmark with producers/consumers pinned to particular cores; compare same-socket and cross-core setups.

## Suggested repository structure

```text
05-lock-free-messaging/
├── README.md
├── CMakeLists.txt                  # Or Cargo.toml for a Rust implementation
├── include/                       # Public headers, if C++
├── src/                           # Production implementation
├── tests/                         # Unit, fuzz, and integration fixtures
├── benchmarks/                    # Reproducible benchmarks
├── examples/                      # Small examples and sample configs
├── scripts/                       # Build, replay, and measurement scripts
├── docs/                          # Protocol/design/experiment notes
└── results/                       # Generated results, not fabricated data
```

## Implementation roadmap

1. M1 — Implement SPSC queue and C++ memory-model rationale.
2. M2 — Write stress, wraparound, overflow and ordering tests.
3. M3 — Compare to a bounded mutex+condition-variable queue and standard containers.
4. M4 — Add batching and CPU-affinity experiments.
5. M5 — Optional MPSC design with formal invariants or model-based testing.

## Benchmarks and evidence to publish

- Producer-to-consumer one-way latency measured with synchronized methodology
- Throughput vs message size and queue capacity
- p50/p99/p99.9 latency under varying occupancy
- CPU consumption, spin time and cache-line invalidations

### Measured results

One run, `results/2026-11-24-apple-m5/` (raw CSV/JSON, environment and the
generated [summary](results/2026-11-24-apple-m5/summary.md)). Methodology:
[docs/benchmarking.md](docs/benchmarking.md).

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency), 128-byte cache lines, single socket |
| OS / kernel | macOS 26.5.1 (Darwin 25.5.0) |
| Compiler / version / flags | Apple clang 21.0.0, `-std=c++20 -O3 -DNDEBUG` |
| Dataset / event mix / sample count | Synthetic numbered messages. Throughput: 4,000,000 messages × 5 runs per configuration. Latency: 300,000 (100k/s), 3,000,000 (1M/s) and 5,000,000 (unpaced) samples |
| Warmup / affinity / governor | Latency: first 100,000 messages discarded. **No hard affinity** (macOS): QoS performance-core hint only. No governor control. On battery. **Machine was not idle: load average 7.4 at start, 17.3 at end** |
| Throughput | 64-byte messages, capacity 1024, median [min–max] of 5 runs: SPSC **52.5 M msgs/s** [49.2–55.1]; SPSC batch-of-32 **397 M msgs/s** [383–406]; mutex+condvar queue **21.7 M msgs/s** [21.0–22.0] |
| p50 / p99 / p99.9 latency | Paced at 1M msgs/s, capacity 1024: SPSC **125 ns / 219 µs / 5.2 ms**; mutex+condvar **2.3 µs / 239 µs / 2.1 ms**. Clock step is 41 ns. p99 and beyond are OS scheduling stalls on a loaded, unpinned machine, not queue behaviour |
| Raw results and reproduction command | `results/2026-11-24-apple-m5/`; `scripts/run_benchmarks.sh <label>` |

What the data supports, and what it does not:

- **SPSC vs mutex queue.** At 64-byte messages the SPSC queue moved 2.4× the
  messages of the condvar queue at capacity 1024 and 2.4× at capacity 64
  (25.9 vs 10.9 M msgs/s); median handoff latency at 1M msgs/s was 125 ns vs
  2.3 µs. The gap is not uniform: in several configurations the mutex queues
  were within noise of, or ahead of, the one-at-a-time SPSC path (e.g. 8-byte
  messages at capacity 65536). See the full table.
- **Batching is the consistent win.** One cursor publication per 32 messages
  gave 1.7–8.6× the one-at-a-time SPSC throughput in eight of the nine
  configurations (71× in the ninth, where the one-at-a-time run was
  abnormally slow), mostly with tight run-to-run ranges.
- **Cache-line padding did *not* show a consistent throughput benefit here.**
  The deliberately unpadded variant was faster than the padded queue in
  several configurations (e.g. 69.2 vs 52.5 M msgs/s at 64 bytes/capacity
  1024) and slower in others (51.8 vs 253.7 at 64 bytes/capacity 65536). The
  retries-per-message columns show the padded runs mostly had a consumer that
  outpaced the producer and kept finding the queue empty; my untested
  hypothesis is that false sharing throttles that consumer and lets messages
  accumulate, which reduces per-message cursor traffic. Separating that effect
  from scheduler noise needs pinned cores and `perf c2c` on Linux
  (`scripts/perf_c2c.sh`), which has not been run.
- **MPSC with one producer** was slower than SPSC in six of nine configurations, as
  expected from the CAS and per-slot sequence number; it has not been
  benchmarked with multiple producers.
- **CPU.** The spinning variants use ~1.0 core per thread regardless of load;
  the condvar queue used less (0.53–0.83 of a core per thread at 64-byte
  messages).
- **Not measured:** cache-line invalidation counts (no `perf` on macOS),
  same-core vs cross-core vs cross-socket placement (no hard affinity on this
  machine), multi-producer MPSC throughput, anything on x86 or Linux.

## Correctness and failure-mode tests

- Exactly one producer and consumer observe in-order monotonically numbered messages.
- Wraparound after many multiples of ring capacity preserves ordering.
- Full and empty outcomes are correct and do not overwrite unread data.
- ThreadSanitizer and high-iteration stress runs report no races (note sanitizer limitations).
- Compare against a reference queue under the same message mix.

Run compiler warnings, address/undefined-behavior sanitizers where applicable, and repeatability tests. Use ThreadSanitizer for concurrent code where supported. Add fuzz/property tests for all binary inputs.

See [docs/design.md](docs/design.md) §11 for which test covers which item.
Status on macOS/arm64: all six CTest targets pass in Release, under
ASan+UBSan, and under TSan (`scripts/run_sanitizers.sh`); sample output is in
[docs/demo.md](docs/demo.md). The suite found one real defect during
development — the MPSC sequence scheme is unsound at capacity 1 — which is now
rejected by the constructor and reproduced by the model checker. Coverage-guided
fuzzing (`-DLFM_LIBFUZZER=ON`) is wired up but has not been run: Apple clang
ships no libFuzzer, so only the deterministic random driver has been exercised.

## Local development

For a C++ implementation, a good default is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For Rust components, use `cargo test`, `cargo clippy` and `cargo bench` with the appropriate benchmark harness. **These commands are scaffolding guidance:** add the named build/test configuration as you implement the project.

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset — [docs/design.md](docs/design.md) §1
- [x] Unit, integration, negative-case and replay/stress tests — `tests/` (6 CTest targets)
- [x] Reproducible benchmark script and machine/environment metadata — `scripts/run_benchmarks.sh`, `results/*/environment.md`
- [x] At least one measured performance tradeoff, with raw results — batching, SPSC vs mutex, padding (see above); raw CSV in `results/`
- [x] Architecture diagram and explicit assumptions/limitations — [docs/design.md](docs/design.md) §2, §10
- [x] Short video, terminal demo or sample output showing the complete workflow — [docs/demo.md](docs/demo.md)
- [ ] CI that builds, runs tests and checks formatting/linting — `.github/workflows/ci.yml` is written (GCC/Clang on Linux, macOS, sanitizers, clang-format) but **has never run**; formatting was checked locally with clang-format 19.1.7
- [ ] Benchmarks on an idle, pinned Linux machine, including `perf c2c` and same-core/cross-core placement

## CV bullet (use **only after implementation**, replacing placeholders)

> Implemented a bounded C++20 SPSC ring buffer with acquire/release synchronization and cache-line isolation; benchmarked **[measured]** messages/sec and **[measured]** p99 handoff latency versus a mutex queue.

Placeholders are left in deliberately. The only numbers available so far come
from a loaded, unpinned laptop, where p99 is scheduler noise; fill this in from
a clean pinned run. From the current data the defensible statement is about
medians: ~52 M msgs/s (64-byte messages) and 125 ns median handoff, versus
~22 M msgs/s and 2.3 µs for a mutex+condvar queue on the same machine.

## Caveats / non-goals

- Avoid publishing nanosecond benchmarks without topology, clock method and sample counts.
- Do not say “wait-free” unless bounded completion is actually proven.
- C++ atomics are required; `volatile` is not a synchronization primitive.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- Official protocol specifications for whichever feed/order-entry version you implement
- *Trading and Exchanges* — Larry Harris (market structure)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
- Linux `perf` documentation, compiler documentation, and C++ memory-model references
