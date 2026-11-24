"""Pytest verification suite for simulator outputs, accounting reconciliation, and analytics."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

from analytics import audit_run_directory

EXP_DIR = ROOT / "results" / "experiments"
FIG_DIR = ROOT / "results" / "figures"


def test_all_twelve_sessions_present_and_split_cleanly() -> None:
    summary_csv = EXP_DIR / "sessions_summary.csv"
    assert summary_csv.exists(), "Run scripts/run_experiments.py before pytest"
    df = pd.read_csv(summary_csv)
    assert len(df) == 24  # 12 sessions x 2 strategies
    assert set(df["split"].unique()) == {"train", "val", "test"}
    for split in ("train", "val", "test"):
        assert len(df[df["split"] == split]) == 8  # 4 sessions x 2 strategies


def test_accounting_reconciliation_and_position_limits_on_all_runs() -> None:
    runs_dir = EXP_DIR / "runs"
    run_dirs = sorted(p for p in runs_dir.iterdir() if p.is_dir() and (p / "summary.json").exists())
    assert len(run_dirs) == 24
    for rdir in run_dirs:
        audit = audit_run_directory(rdir, max_abs_position=500)
        assert audit["max_recon_err_dollars"] < 1e-5, f"Recon mismatch in {rdir}: {audit}"
        assert audit["position_limit_ok"], f"Position limit breached in {rdir}: {audit}"
        assert audit["max_markout_identity_err_ticks"] < 1e-5, f"Markout identity err in {rdir}: {audit}"


def test_markout_timestamp_support_never_extrapolates() -> None:
    run_dir = EXP_DIR / "runs" / "S09_test_mean_reverting__inventory_aware"
    with (run_dir / "summary.json").open("r", encoding="utf-8") as f:
        s = json.load(f)
    fills = pd.read_csv(run_dir / "fills.csv")
    if fills.empty:
        pytest.skip("No fills recorded in session")

    end_ns = int(round(s["session_duration_sec"] * 1e9))
    for horizon_ns, col in [
        (10_000_000, "adv_sel_10ms_ticks"),
        (100_000_000, "adv_sel_100ms_ticks"),
        (1_000_000_000, "adv_sel_1s_ticks"),
    ]:
        unsupported = fills[fills["exchange_time_ns"] + horizon_ns > end_ns]
        if not unsupported.empty:
            assert unsupported[col].isna().all(), f"Extrapolated markout found for {col}"


def test_spread_sweep_monotonic_fill_volume_on_naive_baseline() -> None:
    spread_df = pd.read_csv(EXP_DIR / "sweep_spread.csv")
    naive = spread_df[spread_df["strategy"] == "naive_symmetric"].sort_values("base_half_spread_ticks")
    shares = naive["total_filled_shares"].to_numpy()
    # Wider spreads must reduce or maintain filled shares.
    assert np.all(np.diff(shares) <= 0), f"Non-monotonic fill shares across spreads: {shares}"


def test_benchmark_raw_results_and_figures_generated() -> None:
    bench_csv = ROOT / "results" / "benchmarks" / "bench_results.csv"
    assert bench_csv.exists()
    bdf = pd.read_csv(bench_csv)
    assert len(bdf) == 4
    assert (bdf["throughput_ev_per_sec"] > 0).all()

    for fname in (
        "oos_comparison.png",
        "equity_and_inventory.png",
        "adverse_selection_markouts.png",
        "robustness_sweeps.png",
    ):
        fpath = FIG_DIR / fname
        assert fpath.exists() and fpath.stat().st_size > 10_000, f"Missing/empty figure: {fpath}"
