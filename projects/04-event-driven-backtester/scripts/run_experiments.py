#!/usr/bin/env python3
"""Sensitivity experiments on synthetic sessions. Writes results/experiments/*.csv.

    python scripts/run_experiments.py [--bin build/hftbt] [--sessions 5] [--events 400000]

Sessions are synthetic L3 flow from `hftbt gen` with seeds 1..N. They are
regenerated on every run (deterministically) and not stored in git. See
docs/experiments.md for how to read the output, and docs/assumptions.md for
why none of it is evidence about real markets.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))

import pandas as pd  # noqa: E402

from hftbt_tools import sweep  # noqa: E402

STRATEGIES = {
    "market_maker": {"strategy.name": "market_maker", "strategy.half_spread_ticks": 2,
                     "strategy.order_qty": 10, "strategy.max_position": 50,
                     "strategy.skew_ticks_per_lot": 0.05},
    "imbalance": {"strategy.name": "imbalance", "strategy.threshold": 0.6,
                  "strategy.order_qty": 5, "strategy.max_position": 20,
                  "strategy.cooldown_ns": 1000000, "strategy.max_hold_ns": 2000000000},
}
BASE = {"run.sample_interval_ns": 100000000,
        "run.markout_horizons_ns": "1000000,100000000,1000000000",
        "fees.maker_bps": -0.1, "fees.taker_bps": 0.3,
        "risk.max_order_qty": 100, "risk.max_position": 100, "risk.max_open_orders": 10}
KEEP = ["pnl_net", "pnl_gross", "fees", "fills", "maker_fills", "taker_fills", "qty_filled",
        "fill_ratio", "orders_sent", "cancels_sent", "cancel_rejects", "turnover",
        "max_drawdown", "avg_gross_exposure", "event_hash"]


def latency(ns):
    return {"latency.md_delay_ns": ns, "latency.order_delay_ns": ns,
            "latency.response_delay_ns": ns}


def run(binary, overrides):
    cmd = [binary, "run", "--json"]
    for k, v in overrides.items():
        cmd += ["--set", "%s=%s" % (k, v)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(res.stderr)
    s = json.loads(res.stdout)
    row = {k: s[k] for k in KEEP}
    for m in s["markouts"]:
        ms = m["horizon_ns"] // 1000000
        row["maker_markout_%dms" % ms] = m["maker_avg_ticks"]
        row["taker_markout_%dms" % ms] = m["taker_avg_ticks"]
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "hftbt"))
    ap.add_argument("--sessions", type=int, default=5)
    ap.add_argument("--events", type=int, default=400000)
    ap.add_argument("--out", default=os.path.join(ROOT, "results", "experiments"))
    args = ap.parse_args()
    data_dir = os.path.join(ROOT, "build", "experiment-data")
    os.makedirs(data_dir, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)

    sessions = {}
    for seed in range(1, args.sessions + 1):
        l3 = os.path.join(data_dir, "session%d_l3.bin" % seed)
        l2 = os.path.join(data_dir, "session%d_l2.bin" % seed)
        subprocess.run([args.bin, "gen", "--out", l3, "--events", str(args.events), "--seed",
                        str(seed), "--gap-ns", "100000"], check=True, capture_output=True)
        subprocess.run([args.bin, "convert", l3, l2, "--to-l2"], check=True, capture_output=True)
        sessions[seed] = (l3, l2)

    def base(strategy, seed, l2=False):
        o = dict(BASE)
        o.update(STRATEGIES[strategy])
        o["run.data"] = sessions[seed][1 if l2 else 0]
        return o

    # 1. Latency: the same delay on all three paths.
    rows = []
    for strategy in STRATEGIES:
        for ns in (0, 10000, 100000, 1000000, 10000000, 100000000):
            for seed in sessions:
                o = base(strategy, seed)
                o.update(latency(ns))
                rows.append(dict(strategy=strategy, latency_ns=ns, session=seed, **run(args.bin, o)))
    lat = pd.DataFrame(rows)
    lat.to_csv(os.path.join(args.out, "latency.csv"), index=False)

    # 2. Fees at 100 us latency. Behaviour is fee-independent, so only costs move.
    rows = []
    for strategy in STRATEGIES:
        for maker, taker in ((-0.2, 0.1), (-0.1, 0.3), (0.0, 0.5), (0.2, 1.0)):
            for seed in sessions:
                o = base(strategy, seed)
                o.update(latency(100000))
                o.update({"fees.maker_bps": maker, "fees.taker_bps": taker})
                rows.append(dict(strategy=strategy, maker_bps=maker, taker_bps=taker, session=seed,
                                 **run(args.bin, o)))
    fees = pd.DataFrame(rows)
    fees.to_csv(os.path.join(args.out, "fees.csv"), index=False)

    # 3. Queue model: exact L3 vs the same flow reduced to L2.
    rows = []
    for feed, model in (("l3", "exact"), ("l2", "conservative"), ("l2", "proportional"),
                        ("l2", "optimistic")):
        for seed in sessions:
            o = base("market_maker", seed, l2=(feed == "l2"))
            o.update(latency(100000))
            if feed == "l2":
                o["exchange.queue_model"] = model
            rows.append(dict(feed=feed, queue_model=model, session=seed, **run(args.bin, o)))
    queue = pd.DataFrame(rows)
    queue.to_csv(os.path.join(args.out, "queue_model.csv"), index=False)

    # 4. Parameter sweep with in-sample / out-of-sample split, per session.
    grid = sweep.parse_grid(["strategy.half_spread_ticks=1,2,3,4",
                             "strategy.skew_ticks_per_lot=0,0.05,0.2",
                             "strategy.requote_threshold_ticks=1,2"])
    verdicts, tables = [], []
    for seed in sessions:
        cfg = os.path.join(data_dir, "sweep%d.ini" % seed)
        o = base("market_maker", seed)
        o.update(latency(100000))
        with open(cfg, "w") as f:
            for k, v in o.items():
                f.write("%s = %s\n" % (k, v))
        df, _ = sweep.sweep(args.bin, cfg, grid, split=0.5, jobs=4)
        df.insert(0, "session", seed)
        tables.append(df)
        verdicts.append(dict(session=seed, **sweep.verdict(df)))
    pd.concat(tables).to_csv(os.path.join(args.out, "sweep_is_oos.csv"), index=False)
    ver = pd.DataFrame(verdicts)
    ver.to_csv(os.path.join(args.out, "sweep_verdicts.csv"), index=False)

    pd.set_option("display.width", 200)
    agg = dict(pnl_net="mean", fills="mean", fill_ratio="mean", cancel_rejects="mean")
    print("== latency (mean over %d sessions)" % len(sessions))
    t = lat.groupby(["strategy", "latency_ns"]).agg(
        pnl_min=("pnl_net", "min"), pnl_max=("pnl_net", "max"),
        maker_markout_100ms=("maker_markout_100ms", "mean"),
        taker_markout_100ms=("taker_markout_100ms", "mean"), **{k: (k, v) for k, v in agg.items()})
    print(t.round(3).to_string())
    print("\n== fees")
    print(fees.groupby(["strategy", "maker_bps", "taker_bps"]).agg(
        pnl_net=("pnl_net", "mean"), pnl_gross=("pnl_gross", "mean"), fees=("fees", "mean"),
        fills=("fills", "mean")).round(2).to_string())
    print("\n== queue model")
    print(queue.groupby(["feed", "queue_model"], sort=False).agg(
        fills=("fills", "mean"), qty_filled=("qty_filled", "mean"),
        fill_ratio=("fill_ratio", "mean"), pnl_net=("pnl_net", "mean")).round(3).to_string())
    print("\n== in-sample vs out-of-sample (24 trials per session)")
    print(ver.round(3).to_string(index=False))
    with open(os.path.join(args.out, "README.md"), "w") as f:
        f.write("Produced by `python scripts/run_experiments.py --sessions %d --events %d`.\n\n"
                "Synthetic order flow (seeds 1..%d). See docs/experiments.md.\n"
                % (args.sessions, args.events, args.sessions))


if __name__ == "__main__":
    main()
