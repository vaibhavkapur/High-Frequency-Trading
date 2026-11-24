#!/usr/bin/env python3
"""Analytics, accounting audit, and matplotlib figure generation for 08-market-making-simulator."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any

# Ensure matplotlib has a writable config directory in restricted environments.
os.environ.setdefault("MPLCONFIGDIR", "/tmp/mmsim_mplconfig")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def audit_run_directory(run_dir: Path, max_abs_position: int = 500) -> dict[str, Any]:
    """Independently audits accounting, position limits, and markout identities using pandas."""
    summary_path = run_dir / "summary.json"
    fills_path = run_dir / "fills.csv"
    equity_path = run_dir / "equity.csv"

    with summary_path.open("r", encoding="utf-8") as f:
        summary = json.load(f)

    eq_df = pd.read_csv(equity_path)
    fills_df = pd.read_csv(fills_path)

    tick_size = float(summary["tick_size_dollars"])

    # 1. Verify accounting reconciliation on every equity curve sample:
    #    cash + position * (mid_ticks * tick_size) == net_mtm_pnl (for initial_cash == 0)
    if not eq_df.empty:
        mark = eq_df["mid_ticks"] * tick_size
        recon_err = (eq_df["cash_dollars"] + eq_df["position"] * mark) - eq_df["net_mtm_pnl_dollars"]
        max_recon_err = float(np.max(np.abs(recon_err)))
        max_pos_seen = int(np.max(np.abs(eq_df["position"])))
    else:
        max_recon_err = 0.0
        max_pos_seen = 0

    # 2. Verify markout identity: captured_half_spread - adv_sel == realized_spread
    max_markout_err = 0.0
    if not fills_df.empty:
        for h in ("10ms", "100ms", "1s"):
            adv_col = f"adv_sel_{h}_ticks"
            real_col = f"realized_{h}_ticks"
            valid = fills_df[adv_col].notna() & fills_df[real_col].notna()
            if valid.any():
                diff = (
                    fills_df.loc[valid, "captured_half_spread_ticks"]
                    - fills_df.loc[valid, adv_col]
                    - fills_df.loc[valid, real_col]
                )
                max_markout_err = max(max_markout_err, float(np.max(np.abs(diff))))

    return {
        "run_dir": str(run_dir),
        "max_recon_err_dollars": max_recon_err,
        "max_pos_seen": max_pos_seen,
        "position_limit_ok": max_pos_seen <= max_abs_position,
        "max_markout_identity_err_ticks": max_markout_err,
        "fill_rows": int(len(fills_df)),
        "equity_rows": int(len(eq_df)),
    }


def plot_oos_comparison(sessions_df: pd.DataFrame, out_path: Path) -> None:
    test_df = sessions_df[sessions_df["split"] == "test"].copy()
    if test_df.empty:
        return

    sessions = list(dict.fromkeys(test_df["session_id"].tolist()))
    inv_df = test_df[test_df["strategy"] == "inventory_aware"].set_index("session_id").loc[sessions]
    naive_df = test_df[test_df["strategy"] == "naive_symmetric"].set_index("session_id").loc[sessions]

    x = np.arange(len(sessions))
    width = 0.36
    short_labels = [s.replace("test_", "").replace("_", "\n") for s in sessions]

    fig, axes = plt.subplots(1, 3, figsize=(14, 4.4), dpi=140)

    # Panel 1: Net MTM P&L
    axes[0].bar(x - width / 2, naive_df["net_mtm_pnl_dollars"], width, label="Naive Symmetric", color="#7f8c8d")
    axes[0].bar(x + width / 2, inv_df["net_mtm_pnl_dollars"], width, label="Inventory-Aware", color="#1f77b4")
    axes[0].axhline(0.0, color="black", linewidth=0.8)
    axes[0].set_title("Out-of-Sample Net MTM P&L ($)")
    axes[0].set_xticks(x, short_labels, fontsize=8)
    axes[0].set_ylabel("Dollars ($)")
    axes[0].legend(fontsize=8)
    axes[0].grid(axis="y", alpha=0.3)

    # Panel 2: Max Drawdown
    axes[1].bar(x - width / 2, naive_df["max_drawdown_dollars"], width, label="Naive Symmetric", color="#c0392b", alpha=0.75)
    axes[1].bar(x + width / 2, inv_df["max_drawdown_dollars"], width, label="Inventory-Aware", color="#27ae60", alpha=0.85)
    axes[1].set_title("Out-of-Sample Max Drawdown ($)")
    axes[1].set_xticks(x, short_labels, fontsize=8)
    axes[1].set_ylabel("Dollars ($)")
    axes[1].legend(fontsize=8)
    axes[1].grid(axis="y", alpha=0.3)

    # Panel 3: RMS Inventory Exposure
    axes[2].bar(x - width / 2, naive_df["rms_position"], width, label="Naive Symmetric", color="#d35400", alpha=0.75)
    axes[2].bar(x + width / 2, inv_df["rms_position"], width, label="Inventory-Aware", color="#2980b9", alpha=0.85)
    axes[2].set_title("Out-of-Sample RMS Inventory (sh)")
    axes[2].set_xticks(x, short_labels, fontsize=8)
    axes[2].set_ylabel("Shares")
    axes[2].legend(fontsize=8)
    axes[2].grid(axis="y", alpha=0.3)

    fig.suptitle("Untouched Out-of-Sample Test Sessions (S09-S12): Inventory-Aware vs Naive Symmetric", fontsize=11)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_equity_and_inventory(exp_dir: Path, session_id: str, out_path: Path) -> None:
    inv_eq = pd.read_csv(exp_dir / "runs" / f"{session_id}__inventory_aware" / "equity.csv")
    naive_eq = pd.read_csv(exp_dir / "runs" / f"{session_id}__naive_symmetric" / "equity.csv")

    fig, axes = plt.subplots(2, 1, figsize=(11, 6.2), sharex=True, dpi=140)

    axes[0].plot(naive_eq["timestamp_ns"] * 1e-9, naive_eq["net_mtm_pnl_dollars"],
                 label="Naive Symmetric (Net MTM)", color="#7f8c8d", linewidth=1.4)
    axes[0].plot(inv_eq["timestamp_ns"] * 1e-9, inv_eq["net_mtm_pnl_dollars"],
                 label="Inventory-Aware (Net MTM)", color="#1f77b4", linewidth=1.6)
    axes[0].axhline(0.0, color="black", linewidth=0.7, linestyle="--")
    axes[0].set_ylabel("Net MTM P&L ($)")
    axes[0].set_title(f"Intraday Trajectory on Out-of-Sample Session: {session_id}")
    axes[0].legend(fontsize=9)
    axes[0].grid(alpha=0.3)

    axes[1].plot(naive_eq["timestamp_ns"] * 1e-9, naive_eq["position"],
                 label="Naive Symmetric Position", color="#c0392b", alpha=0.8, linewidth=1.2)
    axes[1].plot(inv_eq["timestamp_ns"] * 1e-9, inv_eq["position"],
                 label="Inventory-Aware Position", color="#27ae60", linewidth=1.5)
    axes[1].axhline(0, color="black", linewidth=0.7, linestyle="--")
    axes[1].set_xlabel("Session Time (seconds)")
    axes[1].set_ylabel("Inventory (shares)")
    axes[1].legend(fontsize=9)
    axes[1].grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_adverse_selection(sessions_df: pd.DataFrame, out_path: Path) -> None:
    test_df = sessions_df[sessions_df["split"] == "test"]
    if test_df.empty:
        return

    horizons = ["10ms", "100ms", "1s"]
    x = np.arange(len(horizons))
    width = 0.35

    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.4), dpi=140)
    for strat, offset, color, label in [
        ("naive_symmetric", -width / 2, "#7f8c8d", "Naive Symmetric"),
        ("inventory_aware", width / 2, "#1f77b4", "Inventory-Aware"),
    ]:
        sub = test_df[test_df["strategy"] == strat]
        adv = [sub[f"adv_sel_{h}_ticks"].mean() for h in horizons]
        real = [sub[f"realized_{h}_ticks"].mean() for h in horizons]
        axes[0].bar(x + offset, adv, width, label=label, color=color)
        axes[1].bar(x + offset, real, width, label=label, color=color)

    axes[0].axhline(0.0, color="black", linewidth=0.8)
    axes[0].set_title("Mean Adverse Selection (ticks, lower is better)")
    axes[0].set_xticks(x, ["10 ms", "100 ms", "1 s"])
    axes[0].set_ylabel("Ticks")
    axes[0].legend(fontsize=9)
    axes[0].grid(axis="y", alpha=0.3)

    axes[1].axhline(0.0, color="black", linewidth=0.8)
    axes[1].set_title("Mean Realized Spread Retained (ticks, higher is better)")
    axes[1].set_xticks(x, ["10 ms", "100 ms", "1 s"])
    axes[1].set_ylabel("Ticks")
    axes[1].legend(fontsize=9)
    axes[1].grid(axis="y", alpha=0.3)

    fig.suptitle("Out-of-Sample Markout Decomposition Across Delay Horizons (S09-S12)", fontsize=11)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def plot_robustness_sweeps(exp_dir: Path, out_path: Path) -> None:
    spread_df = pd.read_csv(exp_dir / "sweep_spread.csv")
    lat_df = pd.read_csv(exp_dir / "sweep_latency.csv")
    fee_df = pd.read_csv(exp_dir / "sweep_fees.csv")
    fm_df = pd.read_csv(exp_dir / "sweep_fill_model.csv")

    fig, axes = plt.subplots(2, 2, figsize=(12, 8), dpi=140)

    # 1. Spread sensitivity
    ax = axes[0, 0]
    for strat, color in [("naive_symmetric", "#7f8c8d"), ("inventory_aware", "#1f77b4")]:
        sub = spread_df[spread_df["strategy"] == strat]
        ax.plot(sub["base_half_spread_ticks"], sub["net_mtm_pnl_dollars"], marker="o", label=strat, color=color)
    ax.axhline(0.0, color="black", linewidth=0.7, linestyle="--")
    ax.set_title("Sensitivity to Base Half-Spread")
    ax.set_xlabel("Base Half-Spread (ticks)")
    ax.set_ylabel("Net MTM P&L ($)")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # 2. Execution latency sensitivity
    ax = axes[0, 1]
    for strat, color in [("naive_symmetric", "#7f8c8d"), ("inventory_aware", "#1f77b4")]:
        sub = lat_df[lat_df["strategy"] == strat]
        ax.plot(sub["one_way_latency_us"], sub["net_mtm_pnl_dollars"], marker="o", label=strat, color=color)
    ax.set_xscale("symlog", linthresh=50)
    ax.axhline(0.0, color="black", linewidth=0.7, linestyle="--")
    ax.set_title("Sensitivity to Assumed Execution Latency")
    ax.set_xlabel("One-Way Order Latency (us, symlog)")
    ax.set_ylabel("Net MTM P&L ($)")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # 3. Maker fee sensitivity
    ax = axes[1, 0]
    for strat, color in [("naive_symmetric", "#7f8c8d"), ("inventory_aware", "#1f77b4")]:
        sub = fee_df[fee_df["strategy"] == strat]
        ax.plot(sub["maker_fee_bps"], sub["net_mtm_pnl_dollars"], marker="o", label=strat, color=color)
    ax.axhline(0.0, color="black", linewidth=0.7, linestyle="--")
    ax.axvline(0.0, color="gray", linewidth=0.7, linestyle=":")
    ax.set_title("Sensitivity to Maker Fee / Rebate Regime")
    ax.set_xlabel("Maker Fee (bps; negative = rebate)")
    ax.set_ylabel("Net MTM P&L ($)")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # 4. Fill model sensitivity (QueuePriority vs ProbabilisticTouch)
    ax = axes[1, 1]
    labels = [f"{r['fill_model']}\n({r['strategy']})" for _, r in fm_df.iterrows()]
    colors = ["#1f77b4" if s == "inventory_aware" else "#7f8c8d" for s in fm_df["strategy"]]
    ax.bar(np.arange(len(labels)), fm_df["net_mtm_pnl_dollars"], color=colors)
    ax.axhline(0.0, color="black", linewidth=0.7, linestyle="--")
    ax.set_title("Execution Assumption Sensitivity (Fill Model)")
    ax.set_xticks(np.arange(len(labels)), labels, fontsize=8)
    ax.set_ylabel("Net MTM P&L ($)")
    ax.grid(axis="y", alpha=0.3)

    fig.suptitle("Robustness Analysis: Spread, Latency, Fees, and Fill Model Assumptions", fontsize=12)
    fig.tight_layout()
    fig.savefig(out_path)
    plt.close(fig)


def generate_all_reports(exp_dir: Path, fig_dir: Path) -> dict[str, Any]:
    fig_dir.mkdir(parents=True, exist_ok=True)
    sessions_df = pd.read_csv(exp_dir / "sessions_summary.csv")

    # Run independent accounting & invariant audits on every recorded session run.
    audits = []
    runs_dir = exp_dir / "runs"
    if runs_dir.exists():
        for run_path in sorted(runs_dir.iterdir()):
            if run_path.is_dir() and (run_path / "summary.json").exists():
                audits.append(audit_run_directory(run_path))

    plot_oos_comparison(sessions_df, fig_dir / "oos_comparison.png")
    plot_adverse_selection(sessions_df, fig_dir / "adverse_selection_markouts.png")
    if (exp_dir / "runs" / "S10_test_toxic_trend__inventory_aware" / "equity.csv").exists():
        plot_equity_and_inventory(exp_dir, "S10_test_toxic_trend", fig_dir / "equity_and_inventory.png")
    if (exp_dir / "sweep_spread.csv").exists():
        plot_robustness_sweeps(exp_dir, fig_dir / "robustness_sweeps.png")

    audit_summary = {
        "audited_runs": len(audits),
        "max_reconciliation_error_dollars": max((a["max_recon_err_dollars"] for a in audits), default=0.0),
        "max_markout_identity_error_ticks": max((a["max_markout_identity_err_ticks"] for a in audits), default=0.0),
        "all_position_limits_respected": all(a["position_limit_ok"] for a in audits),
    }
    with (exp_dir / "audit_report.json").open("w", encoding="utf-8") as f:
        json.dump(audit_summary, f, indent=2)
    return audit_summary


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze simulator outputs and generate figures.")
    parser.add_argument("--exp-dir", type=Path, required=True)
    parser.add_argument("--fig-dir", type=Path, required=True)
    args = parser.parse_args()
    summary = generate_all_reports(args.exp_dir, args.fig_dir)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
