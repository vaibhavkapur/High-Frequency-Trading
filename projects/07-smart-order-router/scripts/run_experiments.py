#!/usr/bin/env python3
"""Run controlled multi-policy backtest scenarios and sensitivity sweeps for `sor`."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
SOR_BIN = ROOT / "build" / "sor"
OUT_DIR = ROOT / "results" / "experiments"

SCENARIOS = [
    "fee_inversion",
    "stale_feed_trap",
    "latency_race_depletion",
    "venue_failover",
    "mixed_stress",
]


def run_sor(args: list[str]) -> Any:
    cmd = [str(SOR_BIN)] + args + ["--json"]
    proc = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return json.loads(proc.stdout)


def format_md_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join(["---"] * len(headers)) + "|",
    ]
    for r in rows:
        lines.append("| " + " | ".join(r) + " |")
    return "\n".join(lines)


def main() -> int:
    if not SOR_BIN.exists():
        print(f"Error: {SOR_BIN} not found. Build with CMake first.", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    orders = 300
    seed = 42

    # 1. Baseline vs Smart comparison across all 5 controlled scenarios.
    all_scenario_rows: list[dict[str, Any]] = []
    for sc in SCENARIOS:
        rows = run_sor(["--compare", "--scenario", sc, "--orders", str(orders), "--seed", str(seed)])
        all_scenario_rows.extend(rows)

    (OUT_DIR / "scenarios.json").write_text(json.dumps(all_scenario_rows, indent=2) + "\n")

    with (OUT_DIR / "scenarios.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "scenario",
            "policy",
            "fill_ratio",
            "gross_is_bps",
            "fee_cost_bps",
            "net_is_bps",
            "cancel_to_fill_ratio",
            "failovers_triggered",
            "median_parent_latency_us",
            "p99_parent_latency_us",
        ])
        for r in all_scenario_rows:
            writer.writerow([
                r["scenario"],
                r["policy"],
                f"{r['fill_ratio']:.4f}",
                f"{r['gross_is_bps']:.4f}",
                f"{r['fee_cost_bps']:.4f}",
                f"{r['net_is_bps']:.4f}",
                f"{r['cancel_to_fill_ratio']:.4f}",
                r["failovers_triggered"],
                f"{r['median_parent_latency_us']:.2f}",
                f"{r['p99_parent_latency_us']:.2f}",
            ])

    # 2. Sensitivity sweeps (staleness timeout, fee scale, latency scale, liquidity scale).
    sensitivity: dict[str, list[dict[str, Any]]] = {
        "staleness_timeout_ms": [],
        "fee_scale": [],
        "latency_scale": [],
        "liquidity_scale": [],
    }

    for stale_ms in [1.0, 2.0, 5.0, 100.0, 5000.0]:
        res = run_sor([
            "--simulate",
            "--scenario", "stale_feed_trap",
            "--policy", "smart_cost_aware",
            "--orders", str(orders),
            "--seed", str(seed),
            "--stale-timeout-ms", str(stale_ms),
        ])
        res["param_value"] = stale_ms
        sensitivity["staleness_timeout_ms"].append(res)

    for fee_scale in [0.5, 1.0, 1.5, 2.0]:
        naive = run_sor([
            "--simulate", "--scenario", "fee_inversion", "--policy", "naive_best_price",
            "--orders", str(orders), "--seed", str(seed), "--fee-scale", str(fee_scale),
        ])
        smart = run_sor([
            "--simulate", "--scenario", "fee_inversion", "--policy", "smart_cost_aware",
            "--orders", str(orders), "--seed", str(seed), "--fee-scale", str(fee_scale),
        ])
        sensitivity["fee_scale"].append({
            "param_value": fee_scale,
            "naive_net_is_bps": naive["net_is_bps"],
            "smart_net_is_bps": smart["net_is_bps"],
            "delta_bps": smart["net_is_bps"] - naive["net_is_bps"],
            "smart_fee_bps": smart["fee_cost_bps"],
            "naive_fee_bps": naive["fee_cost_bps"],
            "smart_fill_ratio": smart["fill_ratio"],
        })

    for lat_scale in [0.5, 1.0, 2.0, 4.0]:
        naive = run_sor([
            "--simulate", "--scenario", "latency_race_depletion", "--policy", "naive_best_price",
            "--orders", str(orders), "--seed", str(seed), "--latency-scale", str(lat_scale),
        ])
        smart = run_sor([
            "--simulate", "--scenario", "latency_race_depletion", "--policy", "smart_cost_aware",
            "--orders", str(orders), "--seed", str(seed), "--latency-scale", str(lat_scale),
        ])
        sensitivity["latency_scale"].append({
            "param_value": lat_scale,
            "naive_fill_ratio": naive["fill_ratio"],
            "smart_fill_ratio": smart["fill_ratio"],
            "naive_cxl_fill": naive["cancel_to_fill_ratio"],
            "smart_cxl_fill": smart["cancel_to_fill_ratio"],
            "naive_med_lat_us": naive["median_parent_latency_us"],
            "smart_med_lat_us": smart["median_parent_latency_us"],
        })

    for liq_scale in [0.4, 0.7, 1.0, 1.5]:
        res = run_sor([
            "--simulate", "--scenario", "mixed_stress", "--policy", "smart_cost_aware",
            "--orders", str(orders), "--seed", str(seed), "--liquidity-scale", str(liq_scale),
        ])
        res["param_value"] = liq_scale
        sensitivity["liquidity_scale"].append(res)

    (OUT_DIR / "sensitivity.json").write_text(json.dumps(sensitivity, indent=2) + "\n")

    # 3. Render Markdown summary report.
    md_parts: list[str] = [
        "# Multi-Venue SOR Experiment Results",
        "",
        f"Reproduction command: `python3 scripts/run_experiments.py` (orders={orders}, seed={seed})",
        "",
        "## Controlled Scenario Comparison",
        "",
    ]

    headers = [
        "Scenario", "Policy", "Fill %", "Gross IS (bps)", "Fee (bps)",
        "Net IS (bps)", "Cxl/Fill", "Failovers", "Med Lat (us)", "Venue Shares",
    ]
    rows_md: list[list[str]] = []
    for r in all_scenario_rows:
        vshares = ", ".join(f"{v['name']}:{v['fill_share']*100:.0f}%" for v in r["venues"])
        rows_md.append([
            r["scenario"],
            r["policy"],
            f"{r['fill_ratio']*100:.1f}%",
            f"{r['gross_is_bps']:.2f}",
            f"{r['fee_cost_bps']:.2f}",
            f"{r['net_is_bps']:.2f}",
            f"{r['cancel_to_fill_ratio']:.2f}",
            str(r["failovers_triggered"]),
            f"{r['median_parent_latency_us']:.1f}",
            vshares,
        ])
    md_parts.append(format_md_table(headers, rows_md))
    md_parts.append("")

    md_parts.append("## Sensitivity: Quote Staleness Timeout (`stale_feed_trap`)")
    md_parts.append("")
    md_parts.append(format_md_table(
        ["Timeout (ms)", "Fill %", "Net IS (bps)", "Cxl/Fill", "Med Lat (us)"],
        [
            [
                f"{r['param_value']:.1f}",
                f"{r['fill_ratio']*100:.1f}%",
                f"{r['net_is_bps']:.2f}",
                f"{r['cancel_to_fill_ratio']:.2f}",
                f"{r['median_parent_latency_us']:.1f}",
            ]
            for r in sensitivity["staleness_timeout_ms"]
        ],
    ))
    md_parts.append("")

    md_parts.append("## Sensitivity: Fee Scale (`fee_inversion`)")
    md_parts.append("")
    md_parts.append(format_md_table(
        ["Fee Scale", "Naive Net IS (bps)", "Smart Net IS (bps)", "Savings (bps)", "Naive Fee (bps)", "Smart Fee (bps)"],
        [
            [
                f"{r['param_value']:.1f}x",
                f"{r['naive_net_is_bps']:.2f}",
                f"{r['smart_net_is_bps']:.2f}",
                f"{-r['delta_bps']:.2f}",
                f"{r['naive_fee_bps']:.2f}",
                f"{r['smart_fee_bps']:.2f}",
            ]
            for r in sensitivity["fee_scale"]
        ],
    ))
    md_parts.append("")

    md_parts.append("## Sensitivity: Latency Scale (`latency_race_depletion`)")
    md_parts.append("")
    md_parts.append(format_md_table(
        ["Latency Scale", "Naive Fill%", "Smart Fill%", "Naive Cxl/Fill", "Smart Cxl/Fill", "Naive MedLat (us)", "Smart MedLat (us)"],
        [
            [
                f"{r['param_value']:.1f}x",
                f"{r['naive_fill_ratio']*100:.1f}%",
                f"{r['smart_fill_ratio']*100:.1f}%",
                f"{r['naive_cxl_fill']:.2f}",
                f"{r['smart_cxl_fill']:.2f}",
                f"{r['naive_med_lat_us']:.1f}",
                f"{r['smart_med_lat_us']:.1f}",
            ]
            for r in sensitivity["latency_scale"]
        ],
    ))
    md_parts.append("")

    md_parts.append("## Sensitivity: Displayed Liquidity Scale (`mixed_stress`, `smart_cost_aware`)")
    md_parts.append("")
    md_parts.append(format_md_table(
        ["Liquidity Scale", "Fill %", "Gross IS (bps)", "Net IS (bps)", "Cxl/Fill", "Med Lat (us)"],
        [
            [
                f"{r['param_value']:.1f}x",
                f"{r['fill_ratio']*100:.1f}%",
                f"{r['gross_is_bps']:.2f}",
                f"{r['net_is_bps']:.2f}",
                f"{r['cancel_to_fill_ratio']:.2f}",
                f"{r['median_parent_latency_us']:.1f}",
            ]
            for r in sensitivity["liquidity_scale"]
        ],
    ))
    md_parts.append("")

    summary_text = "\n".join(md_parts)
    (OUT_DIR / "summary.md").write_text(summary_text)
    print(summary_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
