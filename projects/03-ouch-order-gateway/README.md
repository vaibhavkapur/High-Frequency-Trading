# OUCH-Style Order Gateway & Exchange Simulator

> Order-entry transport, exchange acknowledgements, risk checks, and complete order state machines.

**Project type:** Portfolio / educational systems project  
**Stack:** C++20, POSIX TCP sockets, CMake, GoogleTest  
**Status:** Implemented and tested on macOS/arm64. The Linux build and the CI workflow are written but have not been run yet. Benchmark numbers below are loopback measurements from one laptop.

A C++20 order gateway that speaks an OUCH 4.2-style binary protocol over a SoupBinTCP 3.00-style session, plus a local exchange simulator with a matching core and fault injection to test it against. It is not a certified exchange integration and it never connects anywhere except the loopback interface.

## What it does

- **Order entry, cancel and replace** with client tokens and exchange order reference numbers.
- **Accepted, rejected, executed, canceled and replaced** responses, each applied idempotently.
- **Per-order state machine** with partial fills, duplicate detection and an explicit `Reconciling` state for orders whose fate is unknown.
- **Session handling**: login, heartbeats both ways, timeout detection, reconnect with backoff, sequenced replay, controlled logout.
- **Pre-trade risk**: maximum quantity, maximum notional, price bands, worst-case position limits.
- **Append-only journal**, written before the wire, from which a restarted gateway rebuilds its state.
- **Fault injection** in the simulator: delayed acknowledgements, disconnects before and after processing, duplicate responses, out-of-order delivery, silent server.

## Architecture

```text
 caller ──> RiskEngine ──> OrderManager ──> EventLog ──> codec ──> SoupBinTCP session ══ TCP ══╗
   ^         (refuse)      (state machine)  (journal)   (encode)                              ║
   │                                                                                          v
   └── listener <── OrderManager <── EventLog <── codec <── FrameParser <══ TCP ══ exchange simulator
                    (idempotent apply)            (decode)                         ├ fault injector
                                                                                   ├ OUCH handler
                                                                                   ├ matching core
                                                                                   └ sequenced log per account
```

The wire codec, the network session, the order state machine and the simulator are separate pieces. The state machine does no I/O and takes time as an argument, so its entire state is a function of the journal. See [docs/design.md](docs/design.md) for the state diagram, the reconciliation rules and the limitations, and [docs/protocol.md](docs/protocol.md) for the supported message subset, byte layouts and every deliberate simplification.

### Reconnect without blind resends

When a connection drops with requests unanswered, the gateway does not guess. Unanswered orders become `Reconciling` and keep counting as exposure. On the next login the gateway asks the exchange to replay the sequenced stream from the last message it saw, and re-sends each uncertain request **with its original token**. The exchange ignores a token it has already seen, so each request takes effect at most once whether or not the first copy arrived. This is the recovery model the OUCH specification describes ("All Inbound Messages may be repeated benignly"). An alternative `Hold` policy sends nothing until the owner decides.

## Quick start

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is fetched at configure time unless it is already installed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ouch_demo
```

`ouch_demo` starts the simulator in-process and walks through order entry, a partial fill, replace, cancel, risk rejects, an exchange reject, two reconnect scenarios, duplicate responses and a restart from the journal. Its output from this machine is saved in [docs/sample-output.txt](docs/sample-output.txt).

Other binaries:

| Binary | Purpose |
|---|---|
| `ouch_exchange_sim --port 9100 [--fault-rate 0.01]` | Stand-alone simulator, for a separate client or a packet capture (`scripts/capture.sh`) |
| `ouch_journal_dump <file>` | Prints a gateway journal and the order state it implies |
| `codec_bench`, `gateway_bench` | Benchmarks (see below) |

Minimal use of the library:

```cpp
ouch::GatewayConfig cfg;
cfg.port = 9100;
cfg.journal_path = "gateway.journal";
ouch::Gateway gw(cfg, ouch::RiskLimits{});
gw.risk().set_reference_price(ouch::Symbol("AAPL"), 1'500'000);   // $150.0000
gw.set_listener([](const ouch::OrderUpdate& u) { /* state changed */ });
gw.connect();
while (!gw.active()) gw.poll(1);

ouch::NewOrder o;
o.side = 'B';
o.shares = 100;
o.stock = ouch::Symbol("AAPL");
o.price = 1'500'000;
auto r = gw.submit(o);            // risk-checked, journaled, sent
for (;;) gw.poll(1);              // drives I/O, heartbeats, reconnects
```

## Repository layout

```text
include/ouch/          Public headers: wire, soupbin, order_manager, risk, event_log, socket, gateway
include/ouch/sim/      Exchange simulator: matching_engine, exchange (sessions, faults)
src/                   Implementation
tests/                 GoogleTest unit, property and TCP integration tests; tests/fuzz libFuzzer targets
benchmarks/            codec_bench, gateway_bench
examples/              demo, stand-alone simulator, journal dump
scripts/               run_benchmarks.sh, summarize_results.py, test_all.sh, check_format.sh, capture.sh
docs/                  protocol.md, design.md, sample-output.txt
results/               Raw benchmark output and machine metadata, one directory per run
```

## Tests

125 GoogleTest cases, all passing on this machine in three configurations: Release, AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer.

| Suite | Cases | What it covers |
|---|---|---|
| `WireFixture`, `Wire` | 16 | Hand-written byte fixtures for every message, sizes, malformed input, validation |
| `SoupBin`, `FrameParser` | 11 | Framing, login fixtures, byte-at-a-time reassembly, error handling |
| `Oms` | 35 | State machine: fills, cancels, replaces, races, duplicates, reordering, disconnects |
| `Risk`, `Matching`, `EventLog` | 22 | Limits and boundaries, price-time priority, journal round trip and replay |
| `Property` | 6 | Seeded random tests: codec round trip, chunking invariance, OMS invariants under garbage, idempotence under duplication and replay |
| `Integration` | 27 | Real gateway against the simulator over loopback TCP, with scripted faults and journal recovery |
| `FaultStorm` | 8 seeds | Random workload with random faults, compared against exchange ground truth |

Soak-testing the fault storm under ThreadSanitizer found two real state-machine bugs that the first passing run had missed (a late `Replaced` undoing a cancel, and a cancel the exchange never answers). Both are fixed and have regression tests; the storm then ran more than 130 consecutive ThreadSanitizer iterations (over 1,000 seeded runs) and 150 Release iterations without a failure, where it had previously failed within 12 to 41.

Three libFuzzer targets (`tests/fuzz`) cover the decoders, the frame parser and the state machine. They build with `-DOUCH_BUILD_FUZZERS=ON` on a clang that ships libFuzzer. Apple clang does not, so **they have not been run**; the `Property` suite is the randomized coverage that has.

Run everything, including the sanitizer builds, with `scripts/test_all.sh`.

The README's required failure-mode tests map to:

| Requirement | Test |
|---|---|
| Wire fixtures round-trip to identical bytes | `WireFixture.*`, `Property.ArbitraryBytes...` |
| Partial fills stay live, quantities consistent | `Oms.PartialFillsStayLiveAndAccountingIsConsistent`, `Integration.FullLifecycleOverTcp` |
| Cancel racing a fill ends in a valid state | `Oms.CancelRacing*`, `Integration.CancelRacingFillResolvesToValidFinalState`, `Integration.CancelBeatsPartialFill` |
| Duplicate ACK/execution not double counted | `Oms.Duplicate*`, `Integration.DuplicateResponsesAreNotDoubleCounted`, `Property.OmsIsIdempotent...` |
| Disconnect leaves an explicit reconciliation state | `Oms.DisconnectMakesInFlightRequestsUncertain`, `Integration.LostOrderEntersReconciling...`, `Integration.AcceptedButUnacknowledged...` |

`FaultStorm` runs thousands of random orders, cancels and replaces against a simulator injecting all four fault types at 3% each, then requires every order's open and executed quantity to equal the exchange's own record.

## Benchmarks

All numbers below are from one run of `scripts/run_benchmarks.sh 200000` on 2026-11-24. Raw data: [results/20261124T020804Z](results/20261124T020804Z/SUMMARY.md). Gateway and simulator share one process and talk over loopback TCP, so these describe this laptop and this simulator, not a venue.

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency) |
| OS / kernel | macOS 26.5.1, Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0, `-O3 -DNDEBUG -std=c++20` |
| Dataset / event mix / sample count | 200,000 synthetic orders sent per run. Of every 50 workload slots, 44 are alternating buys and sells at one price (which trade against each other), 5 are passive orders canceled on acknowledgement, and 1 is an oversized order refused by risk and never sent. 180,000 RTT samples per fault-free run. |
| Warmup / affinity / governor | First 20,000 orders discarded. No pinning (macOS has none), no governor control, **running on battery**. One run per configuration, no repeats. |
| Throughput | 61,154 requests/s with one request in flight; 408,649 requests/s with 64 in flight |
| p50 / p99 / p99.9 latency | Submit-to-ack, one in flight: 15.3 / 25.7 / 59.9 us (max 4.8 ms) |
| Raw results and reproduction command | `results/20261124T020804Z/`, `scripts/run_benchmarks.sh 200000` |

**Submit-to-ack** is measured from just before the request is journaled and written to the socket until its Accepted message has been read, decoded and applied. It includes both loopback hops, the simulator's matching and both threads' wake-ups.

| Run | In flight | Requests/s | p50 us | p99 us | p99.9 us | Gateway thread CPU |
|---|---|---|---|---|---|---|
| Baseline (blocking poll, NODELAY, no journal) | 1 | 61,154 | 15.3 | 25.7 | 59.9 | 36.6% |
| Busy-poll | 1 | 59,031 | 15.8 | 28.2 | 65.9 | 44.0% |
| Nagle on | 1 | 60,673 | 15.3 | 29.9 | 78.2 | 36.3% |
| Journal, buffered | 1 | 60,737 | 15.1 | 25.5 | 64.3 | 38.5% |
| Journal, flushed per record | 1 | 44,051 | 17.1 | 30.0 | 93.4 | 54.9% |
| NODELAY | 8 | 237,791 | 21.8 | 71.0 | 437.8 | 74.5% |
| Nagle on | 8 | 254,919 | 27.3 | 46.4 | 94.5 | 60.3% |
| Throughput | 64 | 408,649 | 81.9 | 191.8 | 1,503.0 | 96.9% |
| Open loop, 20,000 orders/s | up to 896 | 22,041 | 62.1 | 464.6 | 16,313.2 | 16.4% |

What the measurements show, and what they do not:

- **Journal durability costs throughput more than latency.** Flushing every record to the OS before sending cut closed-loop throughput by 28% (61,154 to 44,051 requests/s) and raised p50 by 1.8 us. The buffered journal was indistinguishable from no journal.
- **Pipelining trades latency for throughput.** Going from 1 to 64 requests in flight raised throughput 6.7x and p50 from 15.3 to 81.9 us, because acknowledgements queue behind each other.
- **Busy-polling did not help here.** Latency was no better and the gateway thread still spent over half its time off-CPU. The cause was not investigated; contention with the simulator thread on the shared loopback socket is a guess, not a finding.
- **Nagle's algorithm made no clear difference** on macOS loopback. The two window-8 runs differ in opposite directions at p50 and p99.9, and with one run each that is within what run-to-run noise could produce.
- Maximum latencies of 4 to 53 ms appear in every run and were not investigated. With no core pinning and a laptop on battery, scheduler preemption is the likely source.

**Under injected faults** (simulated, each of disconnect / duplicate / reorder / delay drawn independently at the stated rate, 16 requests in flight):

| Fault rate | Requests/s | p50 us | p99 us | Disconnects | Requests made uncertain | Resends | Duplicates dropped | Orders disagreeing with exchange |
|---|---|---|---|---|---|---|---|---|
| 0% | 379,104 | 30.5 | 63.2 | 0 | 0 | 0 | 0 | 0 of 200,000 |
| 1% | 19,638 | 129.5 | 3,100.9 | 2,363 | 33,500 | 37,808 | 3,748 | 0 of 200,000 |
| 5% | 7,774 | 1,476.8 | 7,666.7 | 14,173 | 132,602 | 226,768 | 17,932 | 0 of 200,000 |

Throughput collapses under faults because every disconnect costs a reconnect and a resend of everything in flight, and injected delays are 1 ms each. The point of these runs is the last column: after tens of thousands of dropped connections and duplicated or reordered messages, every order's open and executed quantity matched the exchange's record.

**Codec only** (no sockets; whole-loop timing over 5,000,000 operations, median of 9 loops): encode 0.8 to 2.8 ns and decode 0.7 to 2.7 ns per message depending on size; SoupBinTCP reassembly plus decode of an Executed message 2.6 ns. At these sizes the codec is a rounding error next to a ~15 us loopback round trip.

Not measured: Linux, `perf` profiles, pinned cores, real network links, more than one client, or repeated runs for confidence intervals.

Reproduce with `scripts/run_benchmarks.sh`. Each run writes raw samples, per-run JSON, console logs and machine metadata to a new directory under `results/`.

## Definition of done

- [x] Working implementation with a documented protocol subset ([docs/protocol.md](docs/protocol.md))
- [x] Unit, integration, negative-case and replay/stress tests
- [x] Reproducible benchmark script and machine/environment metadata
- [x] Measured performance tradeoffs with raw results
- [x] Architecture diagram and explicit assumptions/limitations ([docs/design.md](docs/design.md))
- [x] Sample output showing the complete workflow ([docs/sample-output.txt](docs/sample-output.txt))
- [ ] CI that builds, runs tests and checks formatting. The workflow exists (`.github/workflows/ci.yml`) but has never executed: this directory is not yet a git repository.

## CV bullet (use only after you have reproduced the numbers yourself)

> Designed a C++20 OUCH-style binary order gateway and exchange simulator with order lifecycle tracking, pre-trade risk, reconnect reconciliation and fault injection; measured **[your measured value]** p99 submit-to-ACK latency on loopback.

## Caveats / non-goals

- A TCP server that parses OUCH-shaped messages is not an exchange-certified gateway. Layouts were checked against the published OUCH 4.2 document; behaviour was checked only against this project's own simulator.
- Exactly-once execution across a disconnect is not achievable. The gateway gives at-most-once effect per token, and only because the exchange deduplicates tokens.
- Loopback latency says little about real networks, kernels tuned for trading, or a real matching engine.
- Never point this at a live venue. The simulator binds 127.0.0.1 and the gateway has no production safeguards.

This is an educational portfolio system. It is not a certified exchange integration, investment advice, or authorization to connect to a real trading venue.

## Further reading

- NASDAQ OUCH 4.2 and SoupBinTCP 3.00 specifications (nasdaqtrader.com)
- *Trading and Exchanges*, Larry Harris
- *Systems Performance*, Brendan Gregg
