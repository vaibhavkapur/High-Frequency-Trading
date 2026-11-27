#!/usr/bin/env python3
"""Verifies raw benchmark outputs, fixture oracle match, sequence invariants, and builds comparison_summary.json."""

import json
import os
import sys


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    project_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    reports_dir = os.path.join(project_dir, "reports")
    fixtures_dir = os.path.join(project_dir, "fixtures")

    errors = []

    # 1. Verify deterministic fixture replay against independent expected oracle.
    oracle = load_json(os.path.join(fixtures_dir, "expected_reference_result.json"))
    replay = load_json(os.path.join(reports_dir, "fixture_replay_result.json"))
    if replay.get("execution_status") != "FIXTURE_VERIFICATION":
        errors.append(f"Unexpected fixture execution_status: {replay.get('execution_status')}")
    if replay.get("is_simulated") is not False:
        errors.append("Fixture replay must set is_simulated=false")

    exp_seq = oracle["expected_sequence"]
    act_seq = replay["sequence"]
    for k, v in exp_seq.items():
        if k == "expected_total_sent":
            if replay["throughput"]["actual_packets_sent"] != v:
                errors.append(f"Fixture {k}: expected {v}, got {replay['throughput']['actual_packets_sent']}")
        else:
            if act_seq.get(k) != v:
                errors.append(f"Fixture sequence.{k}: expected {v}, got {act_seq.get(k)}")

    exp_lat = oracle["expected_latency_ns"]
    act_lat = replay["latency_ns"]
    for k, v in exp_lat.items():
        if isinstance(v, float):
            if abs(float(act_lat.get(k, -1)) - v) > 0.01:
                errors.append(f"Fixture latency_ns.{k}: expected {v}, got {act_lat.get(k)}")
        else:
            if act_lat.get(k) != v:
                errors.append(f"Fixture latency_ns.{k}: expected {v}, got {act_lat.get(k)}")

    # 2. Verify all measured loopback benchmark reports.
    measured_files = [
        "baseline_loopback_10k_blocking.json",
        "baseline_loopback_10k_busy_poll.json",
        "baseline_loopback_50k_blocking.json",
        "baseline_loopback_50k_busy_poll.json",
        "burst_loopback_blocking.json",
        "burst_loopback_busy_poll.json",
    ]

    summary_rows = []
    for fname in measured_files:
        fpath = os.path.join(reports_dir, fname)
        data = load_json(fpath)
        if data.get("execution_status") != "MEASURED":
            errors.append(f"{fname}: expected execution_status=MEASURED, got {data.get('execution_status')}")
        if data.get("is_simulated") is not False:
            errors.append(f"{fname}: is_simulated must be false")
        seq = data["sequence"]
        if not seq.get("accounting_invariant_holds"):
            errors.append(f"{fname}: accounting_invariant_holds is false")
        if not seq.get("sequence_accounting_verified"):
            errors.append(f"{fname}: sequence_accounting_verified is false")
        lat = data["latency_ns"]
        if not (lat["min"] <= lat["p50"] <= lat["p95"] <= lat["p99"] <= lat["p99_9"] <= lat["max"]):
            errors.append(f"{fname}: latency quantiles are not monotonic")

        summary_rows.append({
            "report_file": fname,
            "scenario_name": data["scenario_name"],
            "backend": data["backend"],
            "execution_status": data["execution_status"],
            "is_simulated": data["is_simulated"],
            "packet_bytes": data["config"]["packet_bytes"],
            "target_pps": data["config"]["target_pps"],
            "pacing_mode": data["config"]["pacing_mode"],
            "actual_packets_sent": data["throughput"]["actual_packets_sent"],
            "measured_received": seq["measured_received"],
            "inferred_total_lost": seq["inferred_total_lost"],
            "loss_ratio": seq["loss_ratio"],
            "achieved_recv_pps": data["throughput"]["achieved_recv_pps"],
            "achieved_recv_mbps": data["throughput"]["achieved_recv_mbps"],
            "p50_ns": lat["p50"],
            "p99_ns": lat["p99"],
            "p99_9_ns": lat["p99_9"],
            "max_ns": lat["max"],
            "sequence_accounting_verified": seq["sequence_accounting_verified"],
        })

    # 3. Verify unrun Linux/bypass capability status files are explicitly labeled NOT_RUN (or AVAILABLE on Linux).
    unrun_rows = []
    for fname in ["recvmmsg_status.json", "af_xdp_status.json", "dpdk_status.json"]:
        data = load_json(os.path.join(reports_dir, fname))
        if data.get("is_simulated") is not False:
            errors.append(f"{fname}: is_simulated must be false")
        cap = data["capability"]
        unrun_rows.append({
            "report_file": fname,
            "backend": cap["backend"],
            "execution_status": data["execution_status"],
            "compiled": cap["compiled"],
            "os_supported": cap["os_supported"],
            "runtime_available": cap["runtime_available"],
            "reason_code": cap["reason_code"],
            "reason": cap["reason"],
        })

    # 4. Verify hot-path microbenchmark disclaimer and invariant.
    micro = load_json(os.path.join(reports_dir, "hotpath_microbench.json"))
    if micro.get("benchmark_type") != "DOMAIN_HOTPATH_MICROBENCH":
        errors.append("hotpath_microbench.json missing DOMAIN_HOTPATH_MICROBENCH label")
    if not micro.get("accounting_invariant_holds"):
        errors.append("hotpath_microbench.json sequence accounting invariant failed")

    comparison = {
        "schema_version": 1,
        "verified": len(errors) == 0,
        "errors": errors,
        "measured_paths": summary_rows,
        "unrun_or_gated_paths": unrun_rows,
        "domain_hotpath_microbench": {
            "report_file": "hotpath_microbench.json",
            "benchmark_type": micro["benchmark_type"],
            "iterations": micro["iterations"],
            "throughput_ops_per_sec": micro["throughput_ops_per_sec"],
            "p50_ns": micro["processing_latency_ns"]["p50"],
            "p99_ns": micro["processing_latency_ns"]["p99"],
            "p99_9_ns": micro["processing_latency_ns"]["p99_9"],
        },
    }

    summary_path = os.path.join(reports_dir, "comparison_summary.json")
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(comparison, f, indent=2)
        f.write("\n")

    if errors:
        print("ACCEPTANCE VERIFICATION FAILED:")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)

    print("ACCEPTANCE VERIFICATION PASSED (0 errors).")
    for row in summary_rows:
        print(
            f"  [MEASURED] {row['scenario_name']} ({row['backend']}): "
            f"recv={row['measured_received']}, lost={row['inferred_total_lost']}, "
            f"p50={row['p50_ns']}ns, p99={row['p99_ns']}ns, p99.9={row['p99_9_ns']}ns, "
            f"pps={row['achieved_recv_pps']:.1f}"
        )
    for row in unrun_rows:
        print(f"  [{row['execution_status']}] {row['backend']}: {row['reason_code']} - {row['reason']}")


if __name__ == "__main__":
    main()
