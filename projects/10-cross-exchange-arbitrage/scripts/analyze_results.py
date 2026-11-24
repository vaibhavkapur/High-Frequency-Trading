#!/usr/bin/env python3
"""
Analyze cross-exchange arbitrage replay audit outputs (SQLite / CSV / JSON)
and verify accounting consistency, signal invalidation breakdown, and latency sweep curves.
"""
import argparse
import csv
import json
import os
import sqlite3
from collections import Counter


def analyze_run(run_dir: str) -> dict:
    summary_path = os.path.join(run_dir, "summary.json")
    signals_path = os.path.join(run_dir, "signals.csv")
    trades_path = os.path.join(run_dir, "trades.csv")
    sqlite_path = os.path.join(run_dir, "audit.sqlite")

    with open(summary_path, "r", encoding="utf-8") as f:
        summary = json.load(f)

    # Cross-check signals.csv
    sig_status_counts = Counter()
    gross_spreads = []
    net_edges_actionable = []
    with open(signals_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            sig_status_counts[row["status"]] += 1
            gross_spreads.append(float(row["gross_spread_bps"]))
            if row["status"] == "actionable":
                net_edges_actionable.append(float(row["net_edge_bps"]))

    # Cross-check trades.csv
    outcome_counts = Counter()
    pnl_sum = 0.0
    fees_sum = 0.0
    slippage_sum = 0.0
    trade_rows = 0
    with open(trades_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            trade_rows += 1
            outcome_counts[row["outcome"]] += 1
            pnl_sum += float(row["total_pnl_quote"])
            fees_sum += float(row["buy_fee_quote"]) + float(row["sell_fee_quote"])
            slippage_sum += float(row["slippage_vs_exp_quote"])

    # Cross-check against SQLite database if present
    sqlite_ok = None
    if os.path.exists(sqlite_path):
        conn = sqlite3.connect(sqlite_path)
        cur = conn.cursor()
        db_sig_cnt = cur.execute("SELECT COUNT(*) FROM signals").fetchone()[0]
        db_trd_cnt = cur.execute("SELECT COUNT(*) FROM trades").fetchone()[0]
        conn.close()
        sqlite_ok = (db_sig_cnt == sum(sig_status_counts.values()) and db_trd_cnt == trade_rows)

    csv_vs_json_pnl_diff = abs(pnl_sum - summary["paper"]["net_simulated_pnl_quote"])
    assert csv_vs_json_pnl_diff < 1e-2, f"P&L mismatch CSV ({pnl_sum}) vs JSON ({summary['paper']['net_simulated_pnl_quote']})"

    return {
        "scenario": summary["scenario"],
        "run_hash": summary["run_hash"],
        "total_signals": sum(sig_status_counts.values()),
        "signal_breakdown": dict(sig_status_counts),
        "mean_gross_bps_all_crosses": (sum(gross_spreads) / len(gross_spreads)) if gross_spreads else 0.0,
        "mean_net_bps_actionable": (sum(net_edges_actionable) / len(net_edges_actionable)) if net_edges_actionable else 0.0,
        "trade_outcomes": dict(outcome_counts),
        "net_pnl_quote": pnl_sum,
        "total_fees_quote": fees_sum,
        "total_slippage_vs_expected_quote": slippage_sum,
        "sqlite_verified": sqlite_ok,
        "accounting_invariants_ok": summary["paper"]["accounting_invariants_ok"],
    }


def main():
    parser = argparse.ArgumentParser(description="Analyze xarb replay outputs")
    parser.add_argument("--run-dir", required=True, help="Directory with summary.json, signals.csv, trades.csv")
    parser.add_argument("--sweep-csv", help="Optional latency sweep CSV to summarize")
    args = parser.parse_args()

    report = analyze_run(args.run_dir)
    print(json.dumps(report, indent=2))

    if args.sweep_csv and os.path.exists(args.sweep_csv):
        print("\nLatency Sweep Summary:")
        print(f"{'Latency(ms)':>11} | {'Signals':>7} | {'Profit':>6} | {'1-Leg':>5} | {'Missed':>6} | {'Fill%':>7} | {'FP%':>7} | {'Net PnL':>10}")
        print("-" * 82)
        with open(args.sweep_csv, "r", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                print(
                    f"{float(row['order_latency_ms']):11.1f} | "
                    f"{int(row['actionable_signals']):7d} | "
                    f"{int(row['both_profitable']):6d} | "
                    f"{int(row['one_legged_unhedged']):5d} | "
                    f"{int(row['both_missed']):6d} | "
                    f"{float(row['fill_rate']) * 100:6.2f}% | "
                    f"{float(row['false_positive_rate']) * 100:6.2f}% | "
                    f"{float(row['net_pnl_quote']):10.2f}"
                )


if __name__ == "__main__":
    main()
