#!/usr/bin/env python3
"""Summarize raw hft_bench CSV and JSON artifacts into Markdown tables."""

import csv
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: summarize_benchmarks.py <results_dir>", file=sys.stderr)
        return 1

    out_dir = pathlib.Path(sys.argv[1])
    summary_csv = out_dir / "summary.csv"
    e2e_json = out_dir / "e2e_latency.json"
    env_json = out_dir / "environment.json"
    sha_txt = out_dir / "git_sha.txt"

    env = json.loads(env_json.read_text()) if env_json.exists() else {}
    e2e = json.loads(e2e_json.read_text()) if e2e_json.exists() else {}
    git_sha = sha_txt.read_text().strip() if sha_txt.exists() else "unknown"

    print("# Benchmark Summary\n")
    print(f"- **Git SHA:** `{git_sha}`")
    if env:
        print(
            f"- **Host:** `{env.get('cpu_model', 'unknown')}` (`{env.get('architecture', '')}`, "
            f"{env.get('logical_cores', '?')} logical cores), `{env.get('os_name', '')}`"
        )
        print(
            f"- **Compiler / Build:** `{env.get('compiler_id', '')} {env.get('compiler_version', '')}` "
            f"(`{env.get('build_type', '')}`, flags: `{env.get('cxx_flags', '')}`)"
        )
        print(f"- **Clock overhead:** `{env.get('clock_overhead_ns', '?')} ns`\n")

    if summary_csv.exists():
        print("## Stage & Comparative Benchmarks\n")
        print("| Suite | Benchmark | Variant | p50 (ns/op) | p99 (ns/op) | Mops/s | RepCV% |")
        print("|---|---|---|---:|---:|---:|---:|")
        with summary_csv.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                print(
                    f"| {row['suite']} | `{row['name']}` | `{row['variant']}` | "
                    f"{float(row['p50_ns']):.2f} | {float(row['p99_ns']):.2f} | "
                    f"{float(row['throughput_mops']):.2f} | {float(row['rep_p50_cv_pct']):.2f}% |"
                )
        print("")

    if e2e:
        print("## End-to-End Distribution Breakdown (Wall-Clock Monotonic ns)\n")
        print("| Metric | Samples | p50 (ns) | p95 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for key in ("tick_to_decision", "order_submit_to_ack", "order_submit_to_fill"):
            d = e2e.get(key)
            if not d:
                continue
            print(
                f"| `{key}` | {d['count']} | {d['p50_ns']:.1f} | {d['p95_ns']:.1f} | "
                f"{d['p99_ns']:.1f} | {d['p999_ns']:.1f} | {d['max_ns']:.1f} |"
            )
        print("")
        print(
            f"- **Pool vs Map Checksum Match:** `{e2e.get('pool_vs_map_checksum_match')}`\n"
            f"- **Direct vs SPSC Checksum Match:** `{e2e.get('direct_vs_spsc_checksum_match')}` "
            f"(drops: `{e2e.get('spsc_drops')}`)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
