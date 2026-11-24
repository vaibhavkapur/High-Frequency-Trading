# Benchmark methodology

How the numbers in `results/` are produced, what they mean, and what they do
not. Read this before quoting anything.

## Reproducing

```bash
scripts/run_benchmarks.sh my-label      # writes results/my-label/
```

That builds Release, records the environment (`environment.md`), runs the three
harnesses, stores their raw output (`throughput.csv`, `latency.csv`,
`micro.json`) and derives `summary.md` from those files with
`scripts/summarize.py`. Nothing in `summary.md` is typed by hand.

Linux, pinned to chosen cores:

```bash
LFM_BENCH_PLACEMENT="--producer-cpu 2 --consumer-cpu 4" scripts/run_benchmarks.sh pinned-2-4
```

Run it for (a) two hyperthreads of one core, (b) two cores sharing an L3,
(c) two sockets, and compare. Ideally isolate the cores (`isolcpus`,
`nohz_full`), set the `performance` governor, and disable turbo; record what
you did — `environment.md` captures governor and isolation settings
automatically.

## Harnesses

### `bench_throughput` — messages per second

One producer thread and one consumer thread move N numbered messages; wall time
is taken from the release of a start gate to the consumer receiving the last
message. Swept over queue variant × message size (8/64/256 bytes) × capacity.
Each configuration is run several times; every run is a CSV row. The summary
reports the **median and the min–max range**, not the best run.

Also recorded per run:

* per-thread CPU time (`CLOCK_THREAD_CPUTIME_ID`) → CPU consumption
* failed `try_push`/`try_pop` attempts → spin iterations per message
* the thread placement that actually took effect

The consumer checks sequence numbers, so a run that delivered anything out of
order aborts instead of reporting a number.

Variants: `spsc`, `spsc_unpadded` (cursors share a cache line), `spsc_batch`
(batches of 32, one cursor publication per batch), `mpsc` (one producer),
`mutex_spin` (mutex queue, non-blocking calls, busy-spin), `mutex_cv` (mutex
queue, blocking on condition variables), `deque_mutex` (`std::deque` + mutex).

### `bench_latency` — one-way handoff latency

* The producer stamps each message; the consumer reads the clock immediately
  after the pop. Both use the same process-local monotonic clock, so the two
  timestamps are directly comparable.
* **Paced** runs (100k and 1M msgs/s): the stamp is the *intended* send time
  `start + i / rate`, and the producer busy-waits until then. A stall of the
  producer or a full queue therefore inflates the measured latency of every
  message it delays, instead of hiding it (no coordinated omission).
* **Unpaced** run (rate 0): the producer runs flat out, the queue sits near
  full, and the figure is queueing delay at high occupancy — roughly
  `occupancy / throughput`, not a handoff time.
* Mean occupancy (sampled every 64th pop) is reported next to each row, which
  is what "latency under varying occupancy" refers to.
* Warm-up messages are discarded; every remaining sample is stored and sorted,
  so percentiles are exact order statistics over the stated sample count.

Known measurement error, reported with the data:

* **Clock step.** Measured at start-up. On Apple Silicon it is ~41 ns, so
  latencies are multiples of ~41 ns and anything near that size is
  quantisation, not signal.
* **Clock read cost.** Each sample includes the consumer's clock read (measured
  mean cost is recorded). No attempt is made to subtract it.
* **Tail = scheduler.** Without core isolation and hard pinning, p99.9 and max
  are dominated by OS preemption and (on Apple Silicon) P/E-core migration.
  They characterise the machine, not the queue. With intended-time stamping, a
  single 10 ms preemption at 1M msgs/s delays ~10,000 consecutive messages,
  which is enough to move p99.

### `bench_micro` — Google Benchmark

Single-threaded push+pop cost per message, and batch vs one-by-one. There is no
second thread, hence no cache-line transfer: this isolates instruction-path
cost. It flatters the mutex queue (an uncontended lock is a couple of atomic
operations) and says nothing about cross-core behaviour.

## Cache-line invalidations

Not measured in the published results: the machine used has no `perf`, and
macOS exposes no HITM-style counters to unprivileged tools.
`scripts/perf_c2c.sh` runs `perf stat` and `perf c2c` on Linux for the padded
and unpadded queues; until someone runs it, the only evidence for the padding
is indirect — the `spsc` vs `spsc_unpadded` throughput difference.

## Interpreting the published run

* It is one laptop, on battery or mains as recorded, with other processes
  running (load average is recorded at start and end). Treat differences
  smaller than the min–max spread as noise.
* macOS cannot pin threads. The QoS hint keeps both threads on performance
  cores most of the time but does not stop migration, and it cannot select
  *which* cores, so the "same-socket vs cross-core" comparison from the README
  cannot be made on this hardware. It needs a Linux box.
* Throughput with small messages is partly a memory-bandwidth and
  store-forwarding benchmark; with capacity far larger than the cache the ring
  itself misses. Do not extrapolate between message sizes.
* When the consumer is faster than the producer the queue runs empty and the
  consumer's cached cursor never helps (it reloads `tail` on every pop);
  when the producer is faster the queue runs full and the reverse holds. Which
  regime a run is in is visible in the retries-per-message columns.
