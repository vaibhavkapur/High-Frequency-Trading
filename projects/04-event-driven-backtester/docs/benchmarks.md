# Benchmarks

Measured on one laptop, on synthetic input. Raw output is in `results/benchmarks/`; every number
below is copied from those files. Re-run with:

```bash
scripts/run_benchmarks.sh 2000000 7      # events per scenario, repetitions
```

## Environment

| Field | Value |
|---|---|
| CPU / cores / topology | Apple M5, 10 cores (4 performance + 6 efficiency), 16 GB |
| OS / kernel | macOS 26.5.1, Darwin 25.5.0 arm64 |
| Compiler / version / flags | Apple clang 21.0.0, `-O3 -DNDEBUG -std=c++20` (CMake Release) |
| Dataset / event mix / sample count | `generate_l3`, seed 42, 2,000,000 L3 events per scenario (about 42% add, 36% cancel/delete, 22% execute), generated in-process |
| Warmup / affinity / governor | No warmup; 7 repetitions, best and median reported. Not pinned (macOS exposes no affinity API). On battery power. |
| Throughput | 23.9 M dispatched events/s (9.8 M feed events/s) for replay + market-making strategy; 30.2 M/s with a no-op strategy |
| p50 / p99 / p99.9 latency | 42 / 84 / 166 ns per dispatched event (market maker). Quantised: the clock tick on this machine is 41 ns |
| Raw results and reproduction command | `results/benchmarks/{environment.txt,replay.csv,scale.csv}`; `scripts/run_benchmarks.sh 2000000 7` |

Source revision: the directory was not a git checkout when this was run, so there is no commit
hash to cite. Re-run after the first commit to pin one.

### How to read the numbers

- **Dispatched events** counts everything the scheduler processes: each feed event is dispatched
  once at the exchange and once more when the strategy observes it, plus order, report, timer,
  markout and sample events. "Feed events/s" is the more conservative figure.
- Timed region is `Engine::run()` only. Feed generation, file I/O and report writing are excluded.
- Per-event latency comes from a separate pass that wraps every `Engine::step()` in two clock
  reads, which adds overhead and is quantised to the 41 ns tick. Treat p50 as "at most one or two
  ticks"; the tail percentiles are the informative part. `max_ns` in the CSV is dominated by OS
  preemption and allocator growth, not by the engine.
- **Noise floor:** three scenarios have identical configurations (`strategy/market_maker`,
  `md_queue/fifo/delay_50000`, `instruments/1`) and measured 23.88, 23.52 and 23.55 M/s — a spread
  of about 1.5%. Differences of that size elsewhere in the tables mean nothing.
- An earlier run launched as a background job was roughly 2× slower across the board with one
  median 16× worse than its best, presumably from being scheduled on efficiency cores. That run was
  discarded; run the script in the foreground on an otherwise idle machine.

## Replay throughput by strategy

| Strategy | Dispatched events | Fills | Best wall time | Dispatched/s | Feed events/s | p50 / p99 / p99.9 |
|---|---|---|---|---|---|---|
| no-op | 4,000,840 | 0 | 0.1325 s | 30.2 M | 15.1 M | 41 / 84 / 125 ns |
| market maker | 4,898,943 | 36,121 | 0.2051 s | 23.9 M | 9.8 M | 42 / 84 / 166 ns |
| imbalance taker | 4,141,122 | 35,016 | 0.1509 s | 27.4 M | 13.3 M | 42 / 84 / 125 ns |

## Measured tradeoff: where delayed market data waits

Every feed event has to be delivered to the strategy later. The general solution is to push it into
the scheduler's binary heap. Because each latency path is monotone, a plain FIFO merged with the
heap gives the same order (`Scheduler.FifoAndHeapMarketDataQueuesAreEquivalent` checks the event
hashes are equal). Market-making strategy, one instrument; the number of events in flight grows
with the delay (mean inter-event gap is 50 µs):

| Market-data delay | Events in flight (approx.) | FIFO | Heap | Heap cost |
|---|---|---|---|---|
| 0 | 0–1 | 28.7 M/s | 26.1 M/s | −9% |
| 50 µs | ~1 | 23.5 M/s | 21.4 M/s | −9% |
| 5 ms | ~100 | 22.1 M/s | 16.8 M/s | −24% |
| 500 ms | ~10,000 | 22.9 M/s | 13.2 M/s | −42% |

The FIFO is flat in the delay; the heap pays `O(log n)` per event on a structure that no longer
fits the smallest caches, and its p99.9 rises from 166 ns to 416 ns. The price of the FIFO is a
restriction: jitter may never reorder a path. That restriction is also the more realistic model of
a TCP session, so the FIFO is the default and the heap remains as `run.md_queue = heap`, mainly as
an independent implementation to test against.

## Sensitivity to the number of instruments

Same 2,000,000 events spread uniformly over N instruments, market maker quoting all of them.

| Instruments | Dispatched/s | Relative | p99 / p99.9 |
|---|---|---|---|
| 1 | 23.5 M | 1.00 | 84 / 167 ns |
| 4 | 22.0 M | 0.94 | 84 / 208 ns |
| 16 | 20.0 M | 0.85 | 125 / 292 ns |
| 64 | 17.7 M | 0.75 | 125 / 209 ns |
| 256 | 15.5 M | 0.66 | 125 / 292 ns |

Per-event work does not depend on N (instruments are indexed directly). The decline tracks working-
set size: each instrument has two books, two L3 order maps and strategy state, and consecutive
events land on different instruments.

## Sensitivity to the number of active simulated orders

A ladder of K resting orders (K/2 per side, one per tick from one tick behind the touch), sized so
they collect partial fills but never complete; `open_orders_at_end` in the CSV confirms K orders
stayed live.

| Active orders | Dispatched/s | Relative | p99 / p99.9 |
|---|---|---|---|
| 0 | 28.1 M | 1.00 | 84 / 125 ns |
| 2 | 26.8 M | 0.95 | 84 / 125 ns |
| 16 | 26.7 M | 0.95 | 84 / 125 ns |
| 128 | 21.9 M | 0.78 | 125 / 167 ns |
| 1,024 | 11.4 M | 0.40 | 417 / 542 ns |
| 8,192 | 10.5 M | 0.37 | 459 / 1,291 ns |

Simulated orders per side live in a vector sorted by price, and a market event scans the orders
priced at or through the event's price. A handful of quotes costs about 5%. Cost then grows with
how many of our orders the market has moved *into* — which is why 8,192 is barely worse than 1,024:
the price wandered a few hundred ticks, so the extra orders were never reached. A strategy that
keeps thousands of orders inside the traded range would want a price-indexed structure here; the
baselines keep two.

## Simulation length and memory

One process per size (peak RSS is per-process), market maker, each size run twice in-process and
the two event hashes compared.

| Feed events | Dispatched | Best wall time | Dispatched/s | Peak RSS | Reproducible |
|---|---|---|---|---|---|
| 1,000,000 | 2,449,699 | 0.113 s | 21.7 M | 40 MB | yes |
| 4,000,000 | 9,801,345 | 0.445 s | 22.0 M | 155 MB | yes |
| 16,000,000 | 39,214,194 | 1.670 s | 23.5 M | 613 MB | yes |
| 64,000,000 | 156,872,524 | 6.928 s | 22.6 M | 1,857 MB (see note) | yes |

Throughput is flat from 1 M to 64 M events. Memory is dominated by the feed held in RAM at 40 bytes
per event (38–40 MB per million measured up to 16 M). At 64 M the feed alone is 2,441 MiB, more than
the reported peak: macOS excludes compressed pages from `ru_maxrss`, so that figure understates.
64 M events is the largest run tested, not a limit of the engine; the practical ceiling is RAM,
since the loader does not stream.

## Not measured

- Linux, x86, pinned cores, or a fixed-frequency governor.
- CSV / binary load time.
- Real market data: event mix and book shape differ from the synthetic flow, and both affect
  throughput.
