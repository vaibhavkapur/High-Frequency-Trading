#!/usr/bin/env python3
"""Turns one results/<timestamp>/ directory into a Markdown summary on stdout.

Only the standard library is used. Every number printed comes from the JSON
files in the directory; nothing is estimated or filled in.
"""
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def fmt_rate(x):
    return f"{x / 1e6:.1f} M/s"


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: summarize.py results/<timestamp>")
    out = Path(sys.argv[1])

    print(f"# Benchmark summary: {out.name}\n")
    print("## Environment\n")
    print("```")
    print((out / "environment.txt").read_text().rstrip())
    print("```\n")

    runs = defaultdict(list)  # (label, book) -> [json]
    order = []
    for path in sorted((out / "latency").glob("*.json")):
        data = json.loads(path.read_text())
        key = (data["label"], data["book"])
        if data["label"] not in order:
            order.append(data["label"])
        runs[key].append(data)

    configs = out / "configs.txt"
    if configs.exists():  # keep the order the configurations were run in
        wanted = configs.read_text().split()
        order = [label for label in wanted if label in order] + [label for label in order if label not in wanted]

    if runs:
        any_run = next(iter(runs.values()))[0]
        clock = any_run["clock"]
        print("## Mixed workload (me_latency)\n")
        print(
            f"Clock granularity {clock['granularity_ns']} ns, clock read cost about "
            f"{clock['read_cost_ns']:.0f} ns. Latencies are per command and include one clock read; "
            "values below the granularity cannot be resolved. Throughput is measured in a separate pass "
            "without per-command clock reads. Each cell is the median-throughput run of the repetitions; "
            "the throughput range across repetitions is shown in brackets.\n"
        )
        print(
            "| Config | Book | Capacity/book | Resting orders | Levels (bid+ask) | Throughput [min-max] | "
            "p50 | p95 | p99 | p99.9 | max | Allocs/cmd |"
        )
        print("|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|")
        for label in order:
            for book in ("pool", "map"):
                reps = runs.get((label, book))
                if not reps:
                    continue
                reps = sorted(reps, key=lambda r: r["throughput_commands_per_sec"])
                med = reps[len(reps) // 2]
                lat = med["latency_ns"]["all"]
                end = med["end_state"]
                lo = reps[0]["throughput_commands_per_sec"]
                hi = reps[-1]["throughput_commands_per_sec"]
                print(
                    f"| {label} | {book} | {med['max_orders_per_book']:,} | {end['resting_orders']:,} | {end['bid_levels'] + end['ask_levels']:,} | "
                    f"{fmt_rate(med['throughput_commands_per_sec'])} [{lo / 1e6:.1f}-{hi / 1e6:.1f}] | "
                    f"{lat['p50']} | {lat['p95']} | {lat['p99']} | {lat['p99_9']} | {lat['max']:,} | "
                    f"{med['allocations_per_command']:.3f} |"
                )
        print()

        print("### Latency by command class (ns), median-throughput run\n")
        print("| Config | Book | Class | Count | p50 | p95 | p99 | p99.9 | max | mean |")
        print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|")
        for label in order:
            for book in ("pool", "map"):
                reps = runs.get((label, book))
                if not reps:
                    continue
                reps = sorted(reps, key=lambda r: r["throughput_commands_per_sec"])
                med = reps[len(reps) // 2]
                for cls, lat in med["latency_ns"].items():
                    if cls == "all" or lat["count"] == 0:
                        continue
                    print(
                        f"| {label} | {book} | {cls} | {lat['count']:,} | {lat['p50']} | {lat['p95']} | "
                        f"{lat['p99']} | {lat['p99_9']} | {lat['max']:,} | {lat['mean']:.1f} |"
                    )
        print()

        print("### Measured event mix (first repetition of each config)\n")
        print("| Config | Commands | Accepted | Trades | Canceled | Reduced | Replaced | Rejected |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|")
        for label in order:
            reps = runs.get((label, "pool")) or runs.get((label, "map"))
            r = reps[0]
            m = r["measured_mix"]
            print(
                f"| {label} | {r['measured_commands']:,} | {m['accepted']:,} | {m['trades']:,} | "
                f"{m['canceled']:,} | {m['reduced']:,} | {m['replaced']:,} | {m['rejected']:,} |"
            )
        print()

    micro = out / "microbench.json"
    if micro.exists():
        data = json.loads(micro.read_text())
        rows = defaultdict(dict)  # (op, arg) -> {book: ns per item}
        cvs = defaultdict(dict)  # (op, arg) -> {book: coefficient of variation across repetitions}
        ops = []
        for b in data["benchmarks"]:
            aggregate = b.get("aggregate_name") if b.get("run_type") == "aggregate" else None
            if aggregate not in (None, "median", "cv"):
                continue
            name = b["name"].rsplit("_", 1)[0] if aggregate else b["name"]
            op, rest = name.split("<", 1)
            book, arg = rest.split(">", 1)
            book = "pool" if "Pool" in book else "map"
            key = (op.replace("BM_", ""), arg.lstrip("/"))
            if key not in ops:
                ops.append(key)
            if aggregate == "cv":
                cvs[key][book] = b["items_per_second"]
            else:
                rows[key][book] = 1e9 / b["items_per_second"]
        print("## Single-operation microbenchmarks (me_bench, Google Benchmark)\n")
        print(
            "Mean nanoseconds per item over a timed batch (median of the repetitions when repeated). "
            "An item is one command, except for Sweep where it is one fill. "
            "The argument is the number of resting orders in the book (Sweep: orders/fills per taker). "
            "cv is the coefficient of variation of the rate across repetitions: a difference between the "
            "two books that is not well clear of it is not a result.\n"
        )
        print("| Operation | Book depth | map ns | map cv | pool ns | pool cv | map/pool |")
        print("|---|---:|---:|---:|---:|---:|---:|")

        def cv(key, book):
            value = cvs[key].get(book)
            return f"{value * 100:.0f}%" if value is not None else "n/a"

        for key in ops:
            m, p = rows[key].get("map"), rows[key].get("pool")
            ratio = f"{m / p:.2f}x" if m and p else ""
            print(f"| {key[0]} | {key[1]} | {m:.1f} | {cv(key, 'map')} | {p:.1f} | {cv(key, 'pool')} | {ratio} |")
        print()


if __name__ == "__main__":
    main()
