#!/usr/bin/env python3
"""Verify project-specific acceptance criteria against raw benchmark JSON."""

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify NUMA engine acceptance criteria")
    parser.add_argument("--benchmark-json", required=True, help="Path to raw_benchmark_results.json")
    args = parser.parse_args()

    path = Path(args.benchmark_json)
    if not path.is_file():
        print(f"FAIL: benchmark report missing: {path}", file=sys.stderr)
        return 1

    data = json.loads(path.read_text(encoding="utf-8"))
    failures = []

    if data.get("schema_version") != "1.0.0":
        failures.append(f"Unexpected schema_version: {data.get('schema_version')}")

    if not data.get("functional_equivalence_passed", False):
        failures.append("functional_equivalence_passed is not True")

    if not data.get("zero_lost_events", False):
        failures.append("zero_lost_events is not True")

    topo = data.get("topology", {})
    is_multi_numa = bool(topo.get("is_multi_numa", False))
    scenarios = {s["scenario_name"]: s for s in data.get("scenarios", [])}

    required_scenarios = [
        "baseline_mutex_unpinned",
        "spsc_default_unpinned",
        "spsc_local_pinned",
        "spsc_first_touch_local",
        "spsc_remote_node",
        "spsc_interleaved",
    ]
    for name in required_scenarios:
        if name not in scenarios:
            failures.append(f"Missing required scenario: {name}")

    # Check repeated runs and latency/throughput reporting for executed scenarios
    for name, s in scenarios.items():
        status = s.get("status")
        reps = int(s.get("repetitions", 0))
        if status in ("EXECUTED", "FALLBACK_SINGLE_NODE"):
            if reps < 2:
                failures.append(f"Scenario {name} has < 2 repetitions ({reps})")
            if float(s.get("mean_throughput_eps", 0)) <= 0:
                failures.append(f"Scenario {name} missing positive throughput")
            if int(s.get("total_lost_events", -1)) != 0:
                failures.append(f"Scenario {name} lost events: {s.get('total_lost_events')}")
            if not s.get("all_digests_match_baseline", False):
                failures.append(f"Scenario {name} digest mismatch vs baseline")
            for run in s.get("runs", []):
                e2e = run.get("e2e_latency", {})
                if "p99_ns" not in e2e or "p99_9_ns" not in e2e:
                    failures.append(f"Scenario {name} missing p99/p99.9 latency metrics")
        elif status in ("UNAVAILABLE_SINGLE_NODE", "UNAVAILABLE_PLATFORM"):
            if reps != 0:
                failures.append(f"Unavailable scenario {name} should have 0 repetitions")
        else:
            failures.append(f"Scenario {name} has unexpected status {status}")

    # On single-node hardware, remote & interleaved MUST be labeled unavailable or fallback
    if not is_multi_numa:
        for name in ("spsc_remote_node", "spsc_interleaved"):
            if name in scenarios:
                st = scenarios[name].get("status")
                if st not in ("UNAVAILABLE_SINGLE_NODE", "FALLBACK_SINGLE_NODE", "UNAVAILABLE_PLATFORM"):
                    failures.append(
                        f"Single-node host MUST label {name} as unavailable/fallback, got {st}"
                    )

    if failures:
        print("ACCEPTANCE CHECKS FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("ACCEPTANCE CHECKS PASSED:")
    print(f"  - Report ID: {data.get('report_id')}")
    print(f"  - Host: {topo.get('os_name')} {topo.get('arch')} ({topo.get('numa_node_count')} NUMA node(s))")
    print(f"  - Functional equivalence: PASS (digest=0x{data.get('baseline_digest')})")
    print("  - Zero lost events: PASS")
    for name in required_scenarios:
        s = scenarios[name]
        print(
            f"  - {name:24s} | status={s['status']:23s} | reps={s['repetitions']} | "
            f"mean_eps={s['mean_throughput_eps']:12.1f} | p99={s['median_p99_ns']:8.1f} ns | "
            f"p99.9={s['median_p99_9_ns']:8.1f} ns"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
