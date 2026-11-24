# NASDAQ ITCH 5.0 Market Data Feed Handler

> Decode binary order-level market data and reconstruct an exchange-style book.

**Project type:** Portfolio / educational systems project  
**Primary stack:** C++20, CMake, Python (validation), Linux perf, Catch2/GoogleTest  
**Status:** This README records the original project brief. The [implementation guide](IMPLEMENTATION.md) documents the file-replay decoder, build and test commands, and measured results on synthetic data.

## Why build this?

Parse historical TotalView-ITCH 5.0 data and rebuild order-level market state. Start with **file replay**; add transport after parser correctness is established.

**Hiring signal:** Demonstrates design tradeoffs, production-style correctness tests, and measurable engineering relevant to HFT software roles. A finished implementation with reproducible results is more persuasive than an ambitious design alone.

## Architecture

```text
ITCH file / framed transport -> framing -> binary decoder -> typed events
                                                        |
                                                        v
                                               order-ID book builder
                                                        |
                                               BBO / depth / trades
                                                        |
                                                 CSV/Parquet metrics
```

## Core requirements

- Big-endian decoding and bounds-safe parsing of message header/payloads
- Core order flow: add, execute, cancel, delete, replace, trade, and cross-trade where applicable
- Per-symbol order-ID book reconstruction, best bid/offer, spread, and top-N levels
- Replay from file with deterministic event ordering and basic throughput control
- Unknown-message and malformed-payload policies with counters
- Optional derived imbalance, microprice, and signed-trade statistics

## Key engineering decisions

- Version and cite the official **ITCH 5.0** specification in your repository; build an explicit supported-message matrix.
- ITCH payload records differ from network packet framing. Keep parsing of the message body separate from transport/framing (e.g. MoldUDP64 or file length prefixes).
- Do not cast arbitrary network bytes directly to packed C++ structs without addressing alignment, endianness and object lifetime.
- Track the proper namespace/lifetime of order reference numbers and maintain references through replace events.
- ITCH is a public market-data protocol, not a private order-entry interface.

## Suggested repository structure

```text
02-itch-feed-handler/
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

1. M1 — Read and validate a length-prefixed historical ITCH sample.
2. M2 — Decode all order-related message types into a typed event model.
3. M3 — Reconstruct per-symbol order books and BBO.
4. M4 — Cross-check selected states/trades against a second parser and known test fixtures.
5. M5 — Optimize allocations and publish benchmark methodology.

## Benchmarks and evidence to publish

- Input MB/sec and messages/sec by message mix
- Parser-only latency vs full decoder+book update latency
- p50/p99/p99.9 per-message processing time
- Peak RSS and allocations per million decoded events

### Benchmark reporting template

| Field | Value |
|---|---|
| CPU / cores / topology | TODO: record exact hardware |
| OS / kernel | TODO |
| Compiler / version / flags | TODO |
| Dataset / event mix / sample count | TODO |
| Warmup / affinity / governor | TODO |
| Throughput | TODO: measured value + units |
| p50 / p99 / p99.9 latency | TODO: measured values + units |
| Raw results and reproduction command | TODO: link |

Do not insert example numbers as if observed. Keep methodology, source code revision, and command-line parameters alongside any published measurements.

## Correctness and failure-mode tests

- Endianness and offset tests with hand-authored byte fixtures.
- Truncated or corrupted payload returns a bounded error instead of out-of-bounds access.
- Add -> partial execution -> cancel -> delete produces expected residual quantity.
- Replace correctly updates order ID and priority according to feed semantics.
- An entire deterministic replay produces matching symbol snapshots across runs.

Run compiler warnings, address/undefined-behavior sanitizers where applicable, and repeatability tests. Use ThreadSanitizer for concurrent code where supported. Add fuzz/property tests for all binary inputs.

## Local development

For a C++ implementation, a good default is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For Rust components, use `cargo test`, `cargo clippy` and `cargo bench` with the appropriate benchmark harness. **These commands are scaffolding guidance:** add the named build/test configuration as you implement the project.

## Definition of done

- [ ] Working implementation with a documented supported feature/protocol subset
- [ ] Unit, integration, negative-case and replay/stress tests
- [ ] Reproducible benchmark script and machine/environment metadata
- [ ] At least one measured performance tradeoff, with raw results
- [ ] Architecture diagram and explicit assumptions/limitations
- [ ] Short video, terminal demo or sample output showing the complete workflow
- [ ] CI that builds, runs tests and checks formatting/linting

## CV bullet (use **only after implementation**, replacing placeholders)

> Built a C++20 NASDAQ ITCH 5.0 decoder and order-book reconstruction pipeline with checked binary parsing and deterministic replay; processed **[measured]** messages/sec while maintaining BBO and depth for **[measured]** instruments.

## Caveats / non-goals

- Do not present historical file replay as evidence of live exchange network connectivity.
- Trade messages and auction/cross-related events need careful interpretation; do not mutate the displayed book for every trade record indiscriminately.
- Use only data you are licensed to access and redistribute.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- Official protocol specifications for whichever feed/order-entry version you implement
- *Trading and Exchanges* — Larry Harris (market structure)
- *Systems Performance* — Brendan Gregg (measurement and profiling)
- Linux `perf` documentation, compiler documentation, and C++ memory-model references
