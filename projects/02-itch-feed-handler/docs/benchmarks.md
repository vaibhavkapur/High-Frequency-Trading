# Benchmark methodology

Raw results live in `results/<run>/`:

- `bench.jsonl` - one JSON object per configuration, including every timed pass
- `environment.txt` - hardware, OS, compiler, power source, exact command
- `benchmarks.md` - tables generated from `bench.jsonl` by `scripts/bench_report.py`

Reproduce with:

```bash
scripts/run_benchmarks.sh                       # 5M messages, 500 symbols, 15 passes
MESSAGES=20000000 RUNS=21 scripts/run_benchmarks.sh
TASKSET='taskset -c 3' scripts/run_benchmarks.sh   # Linux: pin to a core
```

## What is measured

`itch_bench` runs one stage per process so memory and allocation figures belong to
a single configuration:

| Mode | Work per message |
|---|---|
| `frame` | read the 2-byte length prefix, touch the payload's first byte |
| `decode` | framing + full decode into a typed struct (parser only) |
| `book-flat` | framing + decode + book update + trade statistics, `FlatOrderIndex` |
| `book-std` | the same with `StdOrderIndex` (`std::unordered_map`) |

Subtracting rows separates parser cost from book-update cost.

- **Input** is memory-mapped; one untimed warm-up pass faults it in.
- **Throughput:** N timed passes over the whole file. Each pass builds a fresh book.
  The timer covers the message loop only - not the end-of-pass digest or tearing the
  book down. The median pass is reported, with the fastest and slowest pass beside it.
- **Every pass must produce the same message count and book digest** as the warm-up,
  or the benchmark aborts.
- **Dead-code elimination:** decoded messages are passed through an empty
  `asm volatile` barrier so the compiler must materialise every field.
- **Per-message latency:** a separate pass reads `steady_clock` before and after each
  message. It is separate because the clock reads themselves slow the loop down.
- **Allocations:** global `operator new` is replaced in the benchmark binary and
  counted during the cold warm-up pass (which includes all container growth).
- **Peak RSS:** `getrusage` after the warm-up pass. It includes the resident pages of
  the mapped input file; the report prints the input size next to it.

## Caveats that apply to the published run

Read these before quoting any number.

1. **Synthetic data, adversarial locality.** The input comes from `itch_gen`, not a
   Nasdaq file. The generator picks the order to cancel/execute/replace uniformly at
   random from *all* live orders. Real order flow overwhelmingly touches recently
   added orders near the top of the book, so real data should hit the caches far more
   often than this does. Treat the book-update figures as describing this workload,
   not Nasdaq's.
2. **A laptop on battery, no core pinning.** macOS has no thread-affinity API. The
   benchmark requests `QOS_CLASS_USER_INTERACTIVE`, the supported way to prefer
   performance cores, but the scheduler still decides. The machine was not otherwise
   quiesced.
3. **The book stages are noisy on this machine.** Framing and decode passes agree to
   within a few percent. Book passes do not: in the published run the slowest pass is
   up to ~3.6x the median (see the min/max columns). Re-running the suite changed the
   book figures materially.
4. **The latency timer is coarse.** `steady_clock` on this machine advances in ~41 ns
   steps, so parser-only latencies (about 12 ns per message by throughput) read as 0 or
   41-42 ns. Percentiles for the decode stage are at the timer floor and say little;
   only the book-stage percentiles are meaningfully above it, and each still contains
   two clock reads. The `max` column is dominated by page faults and container growth
   in a cold pass, and by preemption.
5. **Each pass rebuilds the book from empty**, so growth of the order pool, level
   vectors and hash table is inside the timed region. For the add-heavy profile, which
   ends with 4.2 million live orders, a large share of the time is the OS providing
   fresh memory.

## The measured trade-off: order index

Hypothesis: a flat open-addressing table should beat `std::unordered_map` for the
order index, because it avoids one heap allocation and one pointer chase per order.

What the published run shows (`results/2026-11-24-apple-m5/benchmarks.md`):

- **Allocations: clear and stable.** About 1,400 allocations per million messages with
  the flat index (all of it vector growth) against 520,000-920,000 with
  `unordered_map` (one node per live order ever added). This figure is deterministic.
- **Time, balanced and churn profiles: flat is faster.** 2.3x and 1.7x by median pass,
  2.1x and 1.7x by fastest pass.
- **Time, add-heavy profile: not faster - `unordered_map` won** (0.75x by median,
  0.91x by fastest pass). A plausible but unverified explanation: the flat table has
  to double and rehash repeatedly on its way to 8M slots while the pool grows
  alongside it.
- **Memory: flat uses more at peak** (863 MB vs 731 MB RSS on add-heavy), because the
  table is kept at most 70% full and doubling briefly holds both old and new tables.

How much to trust it: the direction on balanced and churn held in every run made
during development, but the size of the gap did not, and the add-heavy comparison
changed sign between runs. The supportable conclusion is "flat removes essentially
all per-order allocation and is faster on mixed and churn workloads on this machine;
on a pure build-up workload growth cost dominates and the advantage disappears".
Pre-sizing the table from an expected order count is the obvious next experiment and
has not been done.

## What would make these numbers publishable

- A Linux host with `isolcpus`, a pinned core, the `performance` governor, and
  `perf stat` counters (cache misses, branch misses) alongside the timings.
- A licensed Nasdaq sample file, to replace the synthetic locality assumption.
- A higher-resolution timestamp source (e.g. `rdtsc` on x86) for per-message latency.
