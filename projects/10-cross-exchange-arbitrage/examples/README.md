# Examples & Sample Workflows

Run the complete end-to-end workflow (generate -> convert to binary -> replay with SQLite/CSV audit -> latency sweep):

```bash
./scripts/demo.sh
```

Sample output captured from a real run is stored in [docs/sample_output.txt](../docs/sample_output.txt).

## Individual CLI Examples

```bash
# 1. Generate a deterministic 3-venue feed (Binance + Coinbase + OKX) with injected faults
./build/xarb generate --out examples/sample_feed.jsonl --steps 2500 --seed 20261124

# 2. Convert JSONL feed to CRC32-validated binary framing
./build/xarb convert --in examples/sample_feed.jsonl --out examples/sample_feed.xarb --to binary

# 3. Inspect a feed
./build/xarb inspect --feed examples/sample_feed.xarb

# 4. Replay with 5ms order latency, Limit-IOC, and export CSV/JSON/SQLite audit logs
./build/xarb replay \
  --feed examples/sample_feed.xarb \
  --scenario demo_5ms \
  --order-latency-ms 5 \
  --limit-ioc 1 \
  --out-dir results/demo

# 5. Run a deterministic latency sweep
./build/xarb sweep \
  --feed examples/sample_feed.xarb \
  --latencies-ms 0,2,5,15,50 \
  --out-csv results/demo_sweep.csv

# 6. Cross-check audit CSV/JSON/SQLite outputs with Python
python3 scripts/analyze_results.py --run-dir results/demo --sweep-csv results/demo_sweep.csv
```

## Optional Live Public Market-Data Recorder (Read-Only)

When running on a machine with outbound internet access (and `pip install websockets`), you can record live public Binance/OKX order-book updates into the same JSONL format (zero API keys, zero order submission):

```bash
python3 scripts/record_live_public.py --out examples/live_capture.jsonl --duration-sec 30
./build/xarb replay --feed examples/live_capture.jsonl --order-latency-ms 10 --out-dir results/live_replay
```
