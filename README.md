# High Frequency Trading

Ten C++20 projects explore how an order moves through an exchange, how market data reaches a trader, and how to measure execution strategies and system latency.

## Getting started

```bash
git clone https://github.com/vaibhavkapur/High-Frequency-Trading.git
cd High-Frequency-Trading/projects/01-matching-engine
```

To build the matching engine and run its tests, use a C++20 compiler and CMake 3.20 or later. CMake can download GoogleTest and Google Benchmark during configuration.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Each project builds independently. Run its README commands from its directory under `projects/`. Each project's documentation describes its inputs, benchmark environment, and how to reproduce its results.

## Projects

- [01 · Matching Engine](projects/01-matching-engine/) — Matches limit orders in price-time priority and compares two order book implementations on the same event stream.
- [02 · ITCH Feed Handler](projects/02-itch-feed-handler/) — Replays Nasdaq ITCH 5.0 files to reconstruct order books, quotes, and trades. The [implementation guide](projects/02-itch-feed-handler/IMPLEMENTATION.md) covers the decoder and replay tools.
- [03 · OUCH Order Gateway](projects/03-ouch-order-gateway/) — Sends orders to a simulated exchange over TCP and tracks acknowledgements, fills, and rejects.
- [04 · Event-Driven Backtester](projects/04-event-driven-backtester/) — Replays market events to test how queue position and latency affect simulated fills and strategy returns.
- [05 · Lock-Free Messaging](projects/05-lock-free-messaging/) — Benchmarks ring buffers for passing market data between producer and consumer threads.
- [06 · UDP Multicast Feed](projects/06-udp-multicast-feed/) — Distributes market data over multicast, detects sequence gaps, and recovers missing updates over TCP.
- [07 · Smart Order Router](projects/07-smart-order-router/) — Routes simulated orders across venues using liquidity, fees, quote age, latency, and risk limits.
- [08 · Market-Making Simulator](projects/08-market-making-simulator/) — Compares quoting strategies under inventory constraints and measures fills and adverse selection.
- [09 · Latency Performance Lab](projects/09-latency-performance-lab/) — Measures how memory layout, allocation, and concurrency affect latency in microbenchmarks and a matching engine.
- [10 · Cross-Exchange Arbitrage](projects/10-cross-exchange-arbitrage/) — Scans synthetic venue feeds for price differences and simulates whether trades remain profitable after fees and execution delay.
