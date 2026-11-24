#!/usr/bin/env python3
"""Reproducible 12-session Train/Validation/Test study and robustness sweep harness."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import pandas as pd
from analytics import generate_all_reports

CLI = ROOT / "build" / "mmsim"
EXP_DIR = ROOT / "results" / "experiments"
DATA_DIR = EXP_DIR / "sessions"
RUNS_DIR = EXP_DIR / "runs"
FIG_DIR = ROOT / "results" / "figures"

# Strict chronological splits:
#   Train (S01-S04): used ONLY for selecting (risk_aversion, vol_spread_mult)
#   Val   (S05-S08): held out for sanity check (no tuning)
#   Test  (S09-S12): strictly untouched out-of-sample evaluation
SESSIONS = [
    # Train
    ("S01_train_mean_reverting", "train", "mean_reverting", 10101, 60.0),
    ("S02_train_toxic_trend",    "train", "toxic_trend",    10102, 60.0),
    ("S03_train_high_vol_burst", "train", "high_vol_burst", 10103, 60.0),
    ("S04_train_thin_book",      "train", "thin_book",      10104, 60.0),
    # Validation
    ("S05_val_mean_reverting",   "val",   "mean_reverting", 20201, 60.0),
    ("S06_val_toxic_trend",      "val",   "toxic_trend",    20202, 60.0),
    ("S07_val_high_vol_burst",   "val",   "high_vol_burst", 20203, 60.0),
    ("S08_val_thin_book",        "val",   "thin_book",      20204, 60.0),
    # Out-of-sample Test (never used for tuning)
    ("S09_test_mean_reverting",  "test",  "mean_reverting", 30301, 60.0),
    ("S10_test_toxic_trend",     "test",  "toxic_trend",    30302, 60.0),
    ("S11_test_high_vol_burst",  "test",  "high_vol_burst", 30303, 60.0),
    ("S12_test_thin_book",       "test",  "thin_book",      30304, 60.0),
]

# Realistic positive-fee regime (+0.20 bps maker fee, +1.50 bps taker fee) so spread capture
# must genuinely overcome both adverse selection AND transaction costs (not subsidized by rebates).
BASE_OVERRIDES = [
    "maker_fee_bps=0.20",
    "taker_fee_bps=1.50",
    "md_latency_us=100",
    "order_latency_us=150",
    "ack_latency_us=100",
    "max_abs_position=500",
    "max_order_size=100",
    "max_drawdown_dollars=1500.0",
]


def run_cli(args: list[str]) -> None:
    res = subprocess.run([str(CLI)] + args, check=False, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"Command failed: {args}\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")


def run_sim(feed_path: Path, cfg_path: Path, overrides: list[str], out_dir: Path) -> dict:
    cmd = ["simulate", "--input", str(feed_path), "--config", str(cfg_path), "--out-dir", str(out_dir), "--quiet"]
    for kv in overrides:
        cmd += ["--set", kv]
    run_cli(cmd)
    with (out_dir / "summary.json").open("r", encoding="utf-8") as f:
        return json.load(f)


def flatten_summary(meta: dict, s: dict) -> dict:
    row = dict(meta)
    for k in (
        "strategy",
        "fill_model",
        "total_market_events",
        "session_duration_sec",
        "quotes_submitted",
        "quotes_accepted",
        "quotes_risk_rejected",
        "cancels_submitted",
        "cancels_succeeded",
        "cancels_too_late",
        "cancel_races_filled",
        "fill_count",
        "total_filled_shares",
        "order_fill_rate",
        "share_fill_rate",
        "final_position",
        "max_abs_position",
        "mean_abs_position",
        "rms_position",
        "realized_pnl_dollars",
        "unrealized_pnl_dollars",
        "gross_mtm_pnl_dollars",
        "total_fees_dollars",
        "net_mtm_pnl_dollars",
        "max_drawdown_dollars",
        "reconciliation_error_dollars",
        "mean_captured_half_spread_ticks",
        "mean_captured_half_spread_bps",
        "drawdown_kill_triggered",
    ):
        row[k] = s[k]
    for m in s.get("markouts", []):
        ms = int(round(m["horizon_ms"]))
        lbl = "10ms" if ms == 10 else ("100ms" if ms == 100 else "1s")
        row[f"markout_n_{lbl}"] = m["valid_fills"]
        row[f"adv_sel_{lbl}_ticks"] = m["mean_adverse_selection_ticks"]
        row[f"realized_{lbl}_ticks"] = m["mean_realized_spread_ticks"]
    return row


def main() -> None:
    if not CLI.exists():
        raise FileNotFoundError(f"Build the C++ CLI first: {CLI}")

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    FIG_DIR.mkdir(parents=True, exist_ok=True)

    # 1. Generate all 12 deterministic binary replay sessions.
    print("[1/5] Generating 12 chronological market sessions (4 train / 4 val / 4 test)...")
    feed_paths: dict[str, Path] = {}
    for sid, _split, regime, seed, dur in SESSIONS:
        fpath = DATA_DIR / f"{sid}.mmbin"
        feed_paths[sid] = fpath
        run_cli([
            "generate",
            "--out", str(fpath),
            "--regime", regime,
            "--seed", str(seed),
            "--duration", str(dur),
        ])

    inv_cfg = ROOT / "examples" / "inventory_aware.cfg"
    naive_cfg = ROOT / "examples" / "naive_symmetric.cfg"

    # 2. Tune (risk_aversion, vol_spread_mult) strictly on Train (S01-S04).
    print("[2/5] Tuning Inventory-Aware parameters strictly on Train split (S01-S04)...")
    grid_rows = []
    best_score = -1e18
    best_params = (1.2, 0.40)

    for ra in (0.6, 1.2, 2.0):
        for vmult in (0.25, 0.45, 0.70):
            pnls = []
            dds = []
            rmss = []
            for sid, split, _reg, _seed, _dur in SESSIONS:
                if split != "train":
                    continue
                ov = BASE_OVERRIDES + [f"risk_aversion={ra}", f"vol_spread_mult={vmult}"]
                s = run_sim(feed_paths[sid], inv_cfg, ov, EXP_DIR / "tmp_tune" / f"{sid}_{ra}_{vmult}")
                pnls.append(s["net_mtm_pnl_dollars"])
                dds.append(s["max_drawdown_dollars"])
                rmss.append(s["rms_position"])
            mean_pnl = sum(pnls) / len(pnls)
            mean_dd = sum(dds) / len(dds)
            mean_rms = sum(rmss) / len(rmss)
            # Conservative risk-adjusted selection objective: net P&L penalized by drawdown & inventory.
            score = mean_pnl - 0.5 * mean_dd - 0.1 * mean_rms
            grid_rows.append({
                "risk_aversion": ra,
                "vol_spread_mult": vmult,
                "train_mean_net_pnl": mean_pnl,
                "train_mean_max_dd": mean_dd,
                "train_mean_rms_pos": mean_rms,
                "selection_score": score,
            })
            if score > best_score:
                best_score = score
                best_params = (ra, vmult)

    pd.DataFrame(grid_rows).to_csv(EXP_DIR / "train_grid_search.csv", index=False)
    best_ra, best_vmult = best_params
    print(f"    Selected on Train: risk_aversion={best_ra}, vol_spread_mult={best_vmult} (score={best_score:.2f})")

    # 3. Evaluate frozen strategies across all 12 sessions (Train / Val / Test).
    print("[3/5] Evaluating frozen Inventory-Aware and Naive Symmetric across all 12 sessions...")
    session_rows = []
    for sid, split, regime, seed, _dur in SESSIONS:
        meta_base = {"session_id": sid, "split": split, "regime": regime, "seed": seed}

        s_naive = run_sim(feed_paths[sid], naive_cfg, BASE_OVERRIDES, RUNS_DIR / f"{sid}__naive_symmetric")
        session_rows.append(flatten_summary(meta_base, s_naive))

        ov_inv = BASE_OVERRIDES + [f"risk_aversion={best_ra}", f"vol_spread_mult={best_vmult}"]
        s_inv = run_sim(feed_paths[sid], inv_cfg, ov_inv, RUNS_DIR / f"{sid}__inventory_aware")
        session_rows.append(flatten_summary(meta_base, s_inv))

    sessions_df = pd.DataFrame(session_rows)
    sessions_df.to_csv(EXP_DIR / "sessions_summary.csv", index=False)

    # 4. Robustness sweeps on Validation high_vol_burst session (S07) — keeps Test untouched for sweeps!
    print("[4/5] Running robustness sweeps (spread, latency, fees, fill model) on S07_val_high_vol_burst...")
    sweep_feed = feed_paths["S07_val_high_vol_burst"]
    inv_frozen = BASE_OVERRIDES + [f"risk_aversion={best_ra}", f"vol_spread_mult={best_vmult}"]

    # 4a. Base half-spread sweep
    spread_rows = []
    for hs in (0.5, 1.0, 1.5, 2.0, 3.0, 4.0):
        for strat, cfg, base_ov in [
            ("naive_symmetric", naive_cfg, BASE_OVERRIDES),
            ("inventory_aware", inv_cfg, inv_frozen),
        ]:
            ov = base_ov + [f"base_half_spread_ticks={hs}", f"min_half_spread_ticks={hs}"]
            s = run_sim(sweep_feed, cfg, ov, EXP_DIR / "sweeps" / f"spread_{strat}_{hs}")
            spread_rows.append(flatten_summary({"base_half_spread_ticks": hs}, s))
    pd.DataFrame(spread_rows).to_csv(EXP_DIR / "sweep_spread.csv", index=False)

    # 4b. Latency sweep (scaled MD + Order + Ack latency)
    lat_rows = []
    for lat_us in (0, 50, 250, 1000, 5000, 20000):
        for strat, cfg, base_ov in [
            ("naive_symmetric", naive_cfg, BASE_OVERRIDES),
            ("inventory_aware", inv_cfg, inv_frozen),
        ]:
            ov = base_ov + [
                f"md_latency_us={lat_us}",
                f"order_latency_us={lat_us}",
                f"ack_latency_us={lat_us}",
            ]
            s = run_sim(sweep_feed, cfg, ov, EXP_DIR / "sweeps" / f"lat_{strat}_{lat_us}")
            lat_rows.append(flatten_summary({"one_way_latency_us": lat_us}, s))
    pd.DataFrame(lat_rows).to_csv(EXP_DIR / "sweep_latency.csv", index=False)

    # 4c. Maker fee sweep
    fee_rows = []
    for fee_bps in (-0.30, -0.20, 0.0, 0.25, 0.50, 1.00):
        for strat, cfg, base_ov in [
            ("naive_symmetric", naive_cfg, BASE_OVERRIDES),
            ("inventory_aware", inv_cfg, inv_frozen),
        ]:
            ov = base_ov + [f"maker_fee_bps={fee_bps}"]
            s = run_sim(sweep_feed, cfg, ov, EXP_DIR / "sweeps" / f"fee_{strat}_{fee_bps}")
            fee_rows.append(flatten_summary({"maker_fee_bps": fee_bps}, s))
    pd.DataFrame(fee_rows).to_csv(EXP_DIR / "sweep_fees.csv", index=False)

    # 4d. Fill model sensitivity (QueuePriority vs ProbabilisticTouch)
    fm_rows = []
    for fm in ("queue_priority", "probabilistic_touch"):
        for strat, cfg, base_ov in [
            ("naive_symmetric", naive_cfg, BASE_OVERRIDES),
            ("inventory_aware", inv_cfg, inv_frozen),
        ]:
            ov = base_ov + [f"fill_model={fm}"]
            s = run_sim(sweep_feed, cfg, ov, EXP_DIR / "sweeps" / f"fm_{fm}_{strat}")
            fm_rows.append(flatten_summary({"fill_model_cfg": fm}, s))
    pd.DataFrame(fm_rows).to_csv(EXP_DIR / "sweep_fill_model.csv", index=False)

    # 5. Run pandas accounting audits and generate matplotlib figures.
    print("[5/5] Auditing accounting invariants and generating matplotlib figures...")
    audit = generate_all_reports(EXP_DIR, FIG_DIR)

    # Print concise Test split summary table.
    test_df = sessions_df[sessions_df["split"] == "test"]
    print("\n=== Out-of-Sample Test Split Summary (S09-S12) ===")
    cols = [
        "session_id",
        "strategy",
        "fill_count",
        "gross_mtm_pnl_dollars",
        "total_fees_dollars",
        "net_mtm_pnl_dollars",
        "max_drawdown_dollars",
        "rms_position",
        "adv_sel_100ms_ticks",
        "adv_sel_1s_ticks",
    ]
    print(test_df[cols].to_string(index=False))
    print("\nAudit summary:", json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
