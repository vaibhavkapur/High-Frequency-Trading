"""Parameter sweep with an in-sample / out-of-sample split.

Every grid point is run twice on the same feed: once on the first part
(in-sample, `run.end_ts`), once on the rest (out-of-sample,
`run.trade_start_ts`, the book is warmed up but the strategy starts flat).
Parameters are ranked on in-sample P&L only. The report then shows what the
in-sample winner did out of sample, and how well in-sample rank predicted
out-of-sample rank over the whole grid (Spearman). With many trials the
in-sample maximum is biased upward by construction; the number of trials is
printed next to it for that reason.

    python -m hftbt_tools.sweep --bin build/hftbt --config examples/configs/market_maker.ini \\
        --grid strategy.half_spread_ticks=1,2,3 --grid latency.order_delay_ns=0,100000,1000000 \\
        --out results/sweep_mm
"""
import argparse
import itertools
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

import pandas as pd

METRICS = ["pnl_net", "pnl_gross", "fees", "fills", "fill_ratio", "turnover", "max_drawdown",
           "avg_gross_exposure"]


def parse_grid(specs):
    grid = []
    for spec in specs:
        key, _, values = spec.partition("=")
        if not key or not values:
            raise ValueError("--grid expects section.key=v1,v2,..., got %r" % spec)
        grid.append((key.strip(), [v.strip() for v in values.split(",") if v.strip()]))
    return grid


def run_once(binary, config, overrides):
    cmd = [binary, "run", "--config", config, "--json"]
    for k, v in overrides.items():
        cmd += ["--set", "%s=%s" % (k, v)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError("%s failed: %s" % (" ".join(cmd), res.stderr.strip()))
    return json.loads(res.stdout)


def config_value(config, key, overrides=None):
    """Read `section.key` from an .ini the same way the engine does."""
    if overrides and key in overrides:
        return overrides[key]
    section, value = "", None
    with open(config) as f:
        for line in f:
            line = line.split("#")[0].split(";")[0].strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
            elif "=" in line:
                k, _, v = line.partition("=")
                if (section + "." if section else "") + k.strip() == key:
                    value = v.strip()
    return value


def split_timestamp(binary, config, fraction):
    """Timestamp at `fraction` of the feed's span."""
    data = config_value(config, "run.data")
    if not data:
        raise ValueError("%s does not set run.data" % config)
    res = subprocess.run([binary, "info", data], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(res.stderr.strip())
    info = json.loads(res.stdout)
    return int(info["start_ts"] + fraction * (info["end_ts"] - info["start_ts"]))


def sweep(binary, config, grid, split=0.5, jobs=4):
    split_ts = split_timestamp(binary, config, split)
    keys = [k for k, _ in grid]
    points = [dict(zip(keys, combo)) for combo in itertools.product(*[v for _, v in grid])]

    def one(point):
        row = dict(point)
        segments = (("is", {"run.end_ts": split_ts}), ("oos", {"run.trade_start_ts": split_ts}))
        for name, extra in segments:
            overrides = dict(point)
            overrides.update(extra)
            overrides["run.out_dir"] = ""
            s = run_once(binary, config, overrides)
            for m in METRICS:
                row["%s_%s" % (name, m)] = s[m]
            row["%s_event_hash" % name] = s["event_hash"]
        return row

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        rows = list(pool.map(one, points))
    df = pd.DataFrame(rows)
    df["is_rank"] = df["is_pnl_net"].rank(ascending=False, method="min").astype(int)
    df["oos_rank"] = df["oos_pnl_net"].rank(ascending=False, method="min").astype(int)
    return df.sort_values("is_rank").reset_index(drop=True), split_ts


def verdict(df):
    best = df.iloc[0]
    n = len(df)
    # Spearman = Pearson on ranks (avoids a scipy dependency).
    rho = df["is_pnl_net"].rank().corr(df["oos_pnl_net"].rank()) if n > 2 else float("nan")
    return {
        "trials": n,
        "is_best_pnl": float(best["is_pnl_net"]),
        "oos_pnl_of_is_best": float(best["oos_pnl_net"]),
        "oos_rank_of_is_best": int(best["oos_rank"]),
        "oos_median_pnl": float(df["oos_pnl_net"].median()),
        "share_profitable_oos": float((df["oos_pnl_net"] > 0).mean()),
        "spearman_is_vs_oos": None if rho != rho else float(rho),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", default="build/hftbt")
    ap.add_argument("--config", required=True)
    ap.add_argument("--grid", action="append", default=[], required=True)
    ap.add_argument("--split", type=float, default=0.5, help="in-sample fraction of the span")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args(argv)

    df, split_ts = sweep(args.bin, args.config, parse_grid(args.grid), args.split, args.jobs)
    v = verdict(df)
    v["split_ts"] = split_ts
    os.makedirs(args.out, exist_ok=True)
    df.to_csv(os.path.join(args.out, "sweep.csv"), index=False)
    df.to_parquet(os.path.join(args.out, "sweep.parquet"), index=False)
    with open(os.path.join(args.out, "verdict.json"), "w") as f:
        json.dump(v, f, indent=2)

    keys = [k for k, _ in parse_grid(args.grid)]
    cols = keys + ["is_pnl_net", "oos_pnl_net", "is_rank", "oos_rank", "oos_fills"]
    with pd.option_context("display.width", 200, "display.max_rows", 200):
        print(df[cols].to_string(index=False))
    print()
    print("trials                      %d" % v["trials"])
    print("best in-sample P&L          %.2f  (maximum of %d trials: biased upward)"
          % (v["is_best_pnl"], v["trials"]))
    print("same parameters, OOS        %.2f  (rank %d of %d out of sample)"
          % (v["oos_pnl_of_is_best"], v["oos_rank_of_is_best"], v["trials"]))
    print("median OOS P&L over grid    %.2f" % v["oos_median_pnl"])
    if v["spearman_is_vs_oos"] is not None:
        print("Spearman(IS, OOS)           %.3f" % v["spearman_is_vs_oos"])
    print("wrote %s/{sweep.csv,sweep.parquet,verdict.json}" % args.out)


if __name__ == "__main__":
    main()
