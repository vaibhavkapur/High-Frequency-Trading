# Developer & Operator Runbook

## 1. Prerequisites & Clean Build

Required tools:
- C++20 compiler (`clang++` >= 15 or `g++` >= 11)
- `cmake` >= 3.20

Optional:
- `libzstd` development headers & library (auto-detected if present; safe to omit)

```bash
cd projects/16-order-book-compression-engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## 2. One-Command Full Validation

Runs clean Release build, all 5 CTest suites, the reproducible CLI demo, and the full compression/latency/random-access benchmark suite, writing raw evidence to `reports/`:

```bash
./scripts/run_all.sh
```

## 3. CLI Operations Walkthrough

### Reproducible Demo
```bash
./build/obce_cli demo --events 10000 --seed 20261127 --checkpoint 256 --schema 2
```

### Encode Canonical JSONL Fixture to Binary Stream
```bash
# Reference binary (fixed-width baseline)
./build/obce_cli encode fixtures/valid_l2_l3_v2.jsonl /tmp/sample_ref.obce --codec ref --schema 2

# Delta + Varint + Checkpoints
./build/obce_cli encode fixtures/valid_l2_l3_v2.jsonl /tmp/sample_delta.obce --codec delta --schema 2 --checkpoint 256
```

### Inspect Stream Header & Checkpoint Count
```bash
./build/obce_cli inspect /tmp/sample_delta.obce
```

### Random-Access Seek by Sequence ID
```bash
./build/obce_cli seek /tmp/sample_delta.obce 102
```

### Decode Back to JSONL
```bash
# Strict verification (fails on any CRC error or sequence gap)
./build/obce_cli decode /tmp/sample_delta.obce /tmp/decoded.jsonl --policy strict

# Gap-tolerant decode (preserves explicit encoded gaps, fails on CRC corruption)
./build/obce_cli decode /tmp/sample_delta.obce /tmp/decoded.jsonl --policy allow

# Disaster recovery mode (resyncs at next valid checkpoint if delta frames are missing/corrupt)
./build/obce_cli decode /tmp/damaged.obce /tmp/recovered.jsonl --policy resync
```

## 4. Diagnostic Counters Reference

Every encode/decode operation populates `DiagnosticCounters` (emitted as JSON by `obce_cli`):

| Counter | Meaning | Operational Action |
|---|---|---|
| `frames_processed` | Total frames encoded or decoded | Verify > 0 for non-empty streams |
| `checkpoint_frames` | Anchor checkpoint frames processed | Confirms periodic resync/seek points exist |
| `delta_frames` | Compressed delta frames processed | Ratio to checkpoints reflects `checkpoint_interval / delta_frame_events` |
| `sequence_gaps_detected` | Discontinuities where `seq_id > prev + 1` or missing delta frame | Investigate upstream feed packet drop; review `missing_events_estimated` |
| `crc_errors` | Frames failing Castagnoli CRC32C check | Indicates storage/network bit-flip or truncation; never trust corrupt frame |
| `corrupt_frames_skipped` | Unrecoverable frames bypassed in `ResyncAtCheckpoint` mode | Expected > 0 only during disaster recovery |
| `resync_recoveries` | Successful resynchronizations onto subsequent valid checkpoints | Confirms recovery resumed cleanly after frame loss |
| `schema_v1_upgrades` | Schema v1 events transparently decoded into v2 struct | Expected when reading legacy v1 archives |

## 5. Recovery Procedure: Missing or Corrupt Delta Frame

When `obce_cli decode ... --policy strict` fails with `CRC_MISMATCH` or `SEQUENCE_GAP`:
1. **Do not** force-decode delta bytes onto stale state (the decoder prevents this automatically).
2. Inspect the archive header and checkpoint table:
   ```bash
   ./build/obce_cli inspect <damaged.obce>
   ```
3. Re-run decode with `--policy resync`:
   ```bash
   ./build/obce_cli decode <damaged.obce> <recovered.jsonl> --policy resync
   ```
4. Review the emitted `diagnostics` JSON:
   - `resync_recoveries` indicates how many times decoding successfully re-anchored at a subsequent CRC-verified checkpoint.
   - `missing_events_estimated` bounds the maximum lost sequence window (at most `checkpoint_interval - 1` events per damaged checkpoint window).
5. If exact completeness is required, request retransmission of the missing sequence range `[last_good_seq + 1, resync_checkpoint_seq - 1]` from upstream archive storage.
