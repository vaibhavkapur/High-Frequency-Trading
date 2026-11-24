# Sample session

Captured output of the complete workflow on the machine described in
`results/2026-11-24-apple-m5/environment.md`. Regenerate by running the commands shown.

## Build and test

```text
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
$ ctest --test-dir build --output-on-failure
Test project ./build
    Start 1: spsc_basic_test
1/6 Test #1: spsc_basic_test ..................   Passed    0.04 sec
    Start 2: spsc_stress_test
2/6 Test #2: spsc_stress_test .................   Passed    1.60 sec
    Start 3: mpsc_test
3/6 Test #3: mpsc_test ........................   Passed    1.02 sec
    Start 4: mpsc_model_check
4/6 Test #4: mpsc_model_check .................   Passed    0.16 sec
    Start 5: endpoints_test
5/6 Test #5: endpoints_test ...................   Passed    0.97 sec
    Start 6: fuzz_ops
6/6 Test #6: fuzz_ops .........................   Passed    0.18 sec

100% tests passed out of 6

Total Test time (real) =   3.98 sec
```

## MPSC model check

```text
$ build/tests/mpsc_model_check
mpsc_model_check
  producers=1 items=4 capacity=2: 98 states, 1 terminal, 0 violations
  producers=2 items=4 capacity=2: 10097 states, 1 terminal, 0 violations
  producers=2 items=3 capacity=2: 4463 states, 1 terminal, 0 violations
  producers=2 items=3 capacity=4: 10633 states, 1 terminal, 0 violations
  producers=3 items=2 capacity=2: 55216 states, 1 terminal, 0 violations
  producers=3 items=2 capacity=4: 154427 states, 1 terminal, 0 violations
  [must fail] producers=2 items=2 capacity=2: 5115 states, 8 violation(s), e.g. I1: producer overwrote a slot holding an unread value
  [must fail] producers=1 items=2 capacity=1: 39 states, 8 violation(s), e.g. I1: producer overwrote a slot holding an unread value
OK
```

## Example: backpressure vs drop

```text
$ build/examples/market_data_pipeline
synthetic quotes=2000000, queue capacity=1024, slow path every 64 quotes

== policy: backpressure (lossless, feed waits) ==
feed:     offered=2000000 pushed=2000000 dropped=0 full_events=34305 wait_spins=6252184
strategy: received=2000000 sequence_gaps=0 empty_events=13 in_order=yes
  AAA quotes=500492   volume=150084100  vwap=9551.51 last=9084
  BBB quotes=499252   volume=149778600  vwap=20082.45 last=19985
  CCC quotes=500577   volume=150201300  vwap=30253.03 last=30546
  DDD quotes=499679   volume=149971200  vwap=40580.04 last=40631
accounting OK: pushed == received, pushed + dropped == offered, gaps == dropped

== policy: drop on full (feed never waits) ==
feed:     offered=2000000 pushed=168102 dropped=1831898 full_events=1831898 wait_spins=0
strategy: received=168102 sequence_gaps=1831898 empty_events=3 in_order=yes
  AAA quotes=42193    volume=12625400   vwap=9550.80 last=9079
  BBB quotes=42153    volume=12673400   vwap=20081.08 last=19981
  CCC quotes=41842    volume=12553000   vwap=30254.67 last=30545
  DDD quotes=41914    volume=12567400   vwap=40581.38 last=40632
accounting OK: pushed == received, pushed + dropped == offered, gaps == dropped
```

## Sanitizers

```text
$ scripts/run_sanitizers.sh
=== asan (-fsanitize=address,undefined) ===
100% tests passed out of 6
=== tsan (-fsanitize=thread) ===
100% tests passed out of 6
sanitizer runs passed
```

## Benchmarks

`scripts/run_benchmarks.sh <label>` — output is in `results/<label>/summary.md`.
