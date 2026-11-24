#!/usr/bin/env python3
"""Turns one results/<timestamp>/ directory into a Markdown summary.

Every number comes straight from gateway_bench.jsonl / codec_bench.csv in that
directory; nothing is estimated or filled in.
"""
import csv
import json
import sys
from pathlib import Path


def us(ns):
    return f"{ns / 1000:.1f}"


def main():
    out = Path(sys.argv[1])
    runs = [json.loads(line) for line in (out / "gateway_bench.jsonl").read_text().splitlines() if line]
    print(f"# Benchmark results: {out.name}\n")
    print("Localhost only: gateway and exchange simulator in one process, loopback TCP.")
    print("These figures describe this machine and this simulator, not a real venue.\n")
    print("## Environment\n")
    print("```text")
    print((out / "environment.txt").read_text().rstrip())
    print("```\n")

    print("## Submit-to-ack round trip and throughput\n")
    print("| Run | Window | Spin | NODELAY | Journal | Requests/s | p50 us | p90 us | p99 us | p99.9 us | max us | Gateway CPU % | Queue depth mean / max |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in runs:
        if r["fault_rate"] > 0 or r["label"].startswith("faults"):
            continue
        print(
            f"| {r['label']} | {r['window']} | {r['spin']} | {r['nodelay']} | {r['journal']} | "
            f"{r['requests_per_s']:.0f} | {us(r['rtt_p50_ns'])} | {us(r['rtt_p90_ns'])} | "
            f"{us(r['rtt_p99_ns'])} | {us(r['rtt_p999_ns'])} | {us(r['rtt_max_ns'])} | "
            f"{r['gateway_cpu_pct']:.1f} | {r['queue_depth_mean']:.2f} / {r['queue_depth_max']} |"
        )

    print("\n## Behaviour under injected faults (simulated)\n")
    print("| Run | Fault rate | Requests/s | p50 us | p99 us | p99.9 us | Disconnects | Duplicates | Reorders | Delays | Uncertain | Resends | Duplicates dropped | Mismatches vs exchange |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in runs:
        if not r["label"].startswith("faults"):
            continue
        print(
            f"| {r['label']} | {r['fault_rate'] * 100:.0f}% | {r['requests_per_s']:.0f} | "
            f"{us(r['rtt_p50_ns'])} | {us(r['rtt_p99_ns'])} | {us(r['rtt_p999_ns'])} | "
            f"{r['injected_disconnects']} | {r['injected_duplicates']} | {r['injected_reorders']} | "
            f"{r['injected_delays']} | {r['uncertain']} | {r['resends']} | "
            f"{r['duplicates_dropped']} | {r['mismatches']} |"
        )

    print("\n## Codec only\n")
    print("| Operation | Bytes | min ns/op | median ns/op |")
    print("|---|---|---|---|")
    with open(out / "codec_bench.csv") as f:
        for row in csv.DictReader(f):
            print(
                f"| {row['operation']} | {row['bytes']} | "
                f"{float(row['min_ns_per_op']):.2f} | {float(row['median_ns_per_op']):.2f} |"
            )

    samples = sum(r["rtt_samples"] for r in runs)
    print(f"\n{len(runs)} gateway runs, {samples} RTT samples in total. Raw samples: `rtt_<run>.csv`.")
    print("Per-run records: `gateway_bench.jsonl`. Console output: `gateway_bench.log`, `codec_bench.log`.")
    bad = [r["label"] for r in runs if not r["consistent"]]
    print(f"\nRuns whose final order state disagreed with the exchange: {', '.join(bad) if bad else 'none'}.")


if __name__ == "__main__":
    main()
