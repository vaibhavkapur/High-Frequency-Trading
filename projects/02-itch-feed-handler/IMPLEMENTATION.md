# NASDAQ ITCH 5.0 Feed Handler - implementation status

`README.md` is the original project brief. This file describes what was built
against it, how to run it, and what the evidence does and does not show.

**Stack:** C++20, CMake, Catch2, Python 3 (independent reference implementation)  
**Status:** Implemented for file replay and tested. Benchmarks are measured on
synthetic data on one laptop; see [What is and is not established](#what-is-and-is-not-established).

## What it does

- Decodes all 23 message types of Nasdaq TotalView-ITCH 5.0 (spec revision of
  April 28, 2023) into typed structs, with bounds-checked, endian-safe parsing.
- Rebuilds per-symbol limit order books from order-level events: best bid/offer,
  spread, top-N depth, and per-level FIFO time priority.
- Replays length-prefixed ITCH files deterministically, optionally throttled to a
  message rate or to a multiple of feed time.
- Reports trades (displayed executions, hidden-order trades, crosses) with inferred
  aggressor side, signed volume, top-of-book imbalance and microprice.
- Counts, rather than crashes on, unknown message types, malformed payloads and
  inconsistent order flow.
- Writes BBO history, trade tape, depth snapshot and per-symbol summary as CSV.

## Architecture

```text
ITCH file / MoldUDP64 packet -> framing -> checked decoder -> typed events
                                                                  |
                                                                  v
                                             order-ID book builder (FeedHandler)
                                             order index -> order pool -> price levels
                                                                  |
                                              BBO / top-N depth / trades / statistics
                                                                  |
                                                             CSV outputs
```

Details, data structures and error policies: [docs/design.md](docs/design.md).  
Specification citation and supported-message matrix: [docs/protocol.md](docs/protocol.md).

## Build and test

Requires a C++20 compiler (tested: Apple clang 21), CMake 3.20+, Python 3.9+.
Catch2 v3 is used from the system if installed, otherwise fetched at configure time.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer build (what the tests are meant to be run under):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DITCH_SANITIZE=address,undefined -DITCH_WERROR=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

## Usage

```bash
# Generate a deterministic synthetic file (no licensed data needed)
./build/itch_gen --out sample.itch --messages 500000 --symbols 40 --seed 7

# Replay it: summary on the terminal, CSVs on disk
./build/itch_replay sample.itch --depth 5 --show 2 \
    --bbo-csv bbo.csv --trades-csv trades.csv \
    --snapshot-csv snapshot.csv --summary-csv summary.csv

# Throttle: 100k messages/second, or 60x feed time
./build/itch_replay sample.itch --rate 100000
./build/itch_replay sample.itch --speed 60

# Whole workflow in one go (generate, replay, show outputs, cross-check)
scripts/demo.sh
```

Sample output of `scripts/demo.sh` is in [docs/demo-output.txt](docs/demo-output.txt). Excerpt:

```text
messages          500086 (decoded 500086)
decode rejects    unknown_type=0 truncated=0 oversized=0 invalid_field=0 empty=0
book ops          adds=215401 reduces=84627 removes=129806 replaces=45352
book anomalies    duplicate_order=0 unknown_order=0 over_reduce=0 locate_mismatch=0
trades            60607 reports, 362 broken
bbo updates       23112
live orders       62927
book digest       4c670e861ec5a796

S00001 (locate 1)  spread 0.0200  imbalance -0.246  microprice 159.9375  volume 593873 (buy 188959 / sell 199514)
     bid qty   orders          bid | ask          orders   ask qty
       13556       31     159.9300 | 159.9500     36       22404
        8362       23     159.9200 | 159.9600     30       15733
```

To replay a real Nasdaq historical file, decompress it first (`gunzip`) and pass it
to `itch_replay`. Use only data you are licensed to access; none is included here.

A minimal use of the library API is in [examples/top_of_book.cpp](examples/top_of_book.cpp).

## Correctness evidence

`ctest` runs 70 entries: 56 Catch2 test cases (the book, index and fuzz cases run
once per order-index implementation, giving 69) plus the Python cross-check. All pass
in Release and under AddressSanitizer + UndefinedBehaviorSanitizer with `-Werror`.

| Requirement from the brief | Where it is tested |
|---|---|
| Endianness and offsets, hand-authored byte fixtures | `tests/test_endian.cpp`, `[fixture]` cases in `tests/test_decoder.cpp` (A, F, E, C, X, D, U, P, Q, R) |
| Reader consumes exactly the spec length | compile-time `static_assert` per message in `decoder.hpp` |
| Truncated/corrupt payload gives a bounded error | every truncation of every type, from exact-size heap buffers under ASan |
| Add -> partial execution -> cancel -> delete residual | `tests/test_book.cpp`, `tests/test_feed_handler.cpp` |
| Replace updates order ID and priority | `tests/test_book.cpp` (same price, new price, chains, unknown and duplicate ids) |
| Trades and crosses do not mutate the book | `tests/test_feed_handler.cpp` |
| Deterministic replay, identical snapshots across runs | digest equality across runs and across both index implementations |
| Property tests | decode/encode round trip for all types; book vs. a naive reference model; index vs. `unordered_map` |
| Fuzz-style tests | byte-flipped and truncated streams; 300k adversarial payloads per index |
| Second parser | `scripts/itch_ref.py`, see below |

**Cross-check against a second implementation.** `scripts/itch_ref.py` is a separate
Python implementation that shares no code with the C++ one (layouts restated as
`struct` formats, dict-based book). `scripts/crosscheck.py` runs both over the same
file and requires byte-identical BBO history, trade tape, depth snapshot and message
counts, for three synthetic profiles and for a copy with 5% of message bodies
corrupted and the tail cut mid-frame. It runs as part of `ctest`. To confirm the
check has teeth, five deliberate bugs were injected into the reference (unclamped
over-execution, reversed bid ordering, duplicate adds accepted, flipped aggressor
side, replace ignored); each one made the cross-check fail.

A libFuzzer target is provided (`tests/fuzz/fuzz_feed.cpp`, `-DITCH_BUILD_FUZZERS=ON`)
but has not been built or run: Apple's clang does not ship libFuzzer.

## Benchmarks

Methodology, caveats and the trade-off analysis: [docs/benchmarks.md](docs/benchmarks.md).  
Raw results and generated tables: [results/2026-11-24-apple-m5/](results/2026-11-24-apple-m5/).

```bash
scripts/run_benchmarks.sh
```

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency) |
| OS / kernel | macOS 26.5.1 (25F80), Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0 (clang-2100.1.1.101), `-O3 -DNDEBUG -std=c++20` |
| Dataset / event mix / sample count | Synthetic (`itch_gen`, seed 1), 500 symbols, 5,001,006 messages per profile; profiles `balanced`, `add-heavy`, `churn` |
| Warmup / affinity / governor | 1 untimed pass, then 15 timed passes; no pinning available on macOS, `QOS_CLASS_USER_INTERACTIVE` requested; on battery power |
| Throughput (balanced, median pass) | parser only: 79.1 M msgs/s (2,570 MB/s); decode + book: 10.4 M msgs/s (337 MB/s) |
| p50 / p99 / p99.9 latency (balanced, decode + book) | 84 / 375 / 1,875 ns, measured with a ~41 ns timer step |
| Raw results and reproduction command | `results/2026-11-24-apple-m5/bench.jsonl`; `MESSAGES=5000000 SYMBOLS=500 SEED=1 RUNS=15 scripts/run_benchmarks.sh` |

Source revision: none recorded, because the directory was not a git repository when
the run was made.

Median ns per message by stage (min-max across the 15 passes in brackets):

| Profile | Framing | + decode | + book (flat index) | + book (`unordered_map`) |
|---|---:|---:|---:|---:|
| balanced | 2.7 | 12.6 [11.8-13.3] | 96.3 [81.2-329.6] | 225.9 [172.2-817.2] |
| add-heavy | 2.6 | 13.1 [10.5-26.8] | 142.0 [108.0-298.5] | 106.7 [98.1-155.8] |
| churn | 2.6 | 11.5 [10.8-13.2] | 71.7 [65.2-76.8] | 122.6 [109.1-166.5] |

**Measured trade-off (order index).** The flat open-addressing index cuts allocations
from 520,000-920,000 per million messages to about 1,400, and was 2.3x (balanced) and
1.7x (churn) faster than `std::unordered_map`. On the add-heavy profile it was
*slower* (0.75x) and used more peak memory (863 MB vs 731 MB).

**How far to trust these.** Parser figures are stable to a few percent. Book figures
are not: passes within a run vary by up to ~3.6x, and repeated runs changed both the
size of the flat-index advantage and, for add-heavy, its sign. The synthetic generator
also touches live orders uniformly at random, which is harder on the caches than real
order flow. These are measurements of this workload on this laptop, not predictions
for Nasdaq data.

## What is and is not established

Established:

- Message layouts match the official specification, checked field by field.
- The C++ pipeline and an independent Python implementation agree exactly on
  synthetic and corrupted streams.
- Malformed input does not cause out-of-bounds access in the tested cases, under
  ASan/UBSan.
- Replay is deterministic.

Not established:

- **Behaviour on real Nasdaq data.** No licensed sample was available, so the
  pipeline has not been run on one. `scripts/crosscheck.py --file FILE` will run the
  cross-check on a real file.
- **Live connectivity.** Replay is from files. MoldUDP64 is a buffer parser with gap
  detection, exercised on hand-built packets only; there are no sockets.
- **CI.** `.github/workflows/ci.yml` is written (GCC and clang on Linux, clang on
  macOS, ASan/UBSan, fuzz smoke test, clang-format) but has never executed, and the
  code has not been compiled with GCC or on Linux.
- **Performance on tuned hardware.** See the benchmark caveats above.

Further limitations (trading-state handling, broken trades, Parquet, single trading
day) are listed in [docs/design.md](docs/design.md#assumptions-and-limitations).

## Repository layout

```text
include/itch/    library headers (decoder, framing, book, feed handler, pacing)
src/             mapped file, synthetic generator
tools/           itch_replay, itch_gen
tests/           Catch2 tests; tests/fuzz/ libFuzzer target
benchmarks/      itch_bench
examples/        minimal API example
scripts/         reference implementation, cross-check, benchmarks, demo, formatting
docs/            protocol matrix, design, benchmark methodology, demo output
results/         raw benchmark results
```

## Roadmap status

| Milestone | Status |
|---|---|
| M1 Read and validate a length-prefixed ITCH file | done (synthetic files; real sample not tested) |
| M2 Decode all order-related messages into typed events | done (all 23 message types) |
| M3 Per-symbol books and BBO | done |
| M4 Cross-check against a second parser and fixtures | done on synthetic and corrupted data |
| M5 Optimise allocations, publish benchmark methodology | pooled orders and flat index measured; pre-sizing not explored |

## Definition of done

- [x] Working implementation with a documented supported feature/protocol subset
- [x] Unit, integration, negative-case and replay/stress tests
- [x] Reproducible benchmark script and machine/environment metadata
- [x] At least one measured performance tradeoff, with raw results
- [x] Architecture diagram and explicit assumptions/limitations
- [x] Sample output showing the complete workflow (`docs/demo-output.txt`)
- [ ] CI that builds, runs tests and checks formatting/linting - workflow written, never run

## CV bullet

The brief's bullet is not ready to fill in: the throughput measured here comes from
synthetic data on a laptop and should be re-measured on real data first.
