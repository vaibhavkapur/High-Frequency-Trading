"""Post-process one run directory (summary.json, fills.csv, equity.csv).

Writes fills.parquet / equity.parquet next to the CSVs and, if matplotlib is
available, equity.png. Prints a few numbers recomputed independently from
the fill tape as a cross-check on the C++ accounting.

    python -m hftbt_tools.report results/sample_mm
"""
import argparse
import json
import os

import pandas as pd


def load(run_dir):
    with open(os.path.join(run_dir, "summary.json")) as f:
        summary = json.load(f)
    fills = pd.read_csv(os.path.join(run_dir, "fills.csv"))
    equity = pd.read_csv(os.path.join(run_dir, "equity.csv"))
    return summary, fills, equity


def recompute(fills, equity):
    """Metrics derived only from the output tables."""
    out = {"fills": int(len(fills)), "qty_filled": int(fills["qty"].sum()) if len(fills) else 0}
    signed = fills["qty"].where(fills["side"] == "B", -fills["qty"])
    out["final_positions"] = {k: int(v) for k, v in signed.groupby(fills["symbol"]).sum().items()}
    out["fees"] = float(fills["fee"].sum())
    out["maker_fills"] = int((fills["liquidity"] == "maker").sum())
    eq = equity["equity"]
    out["pnl_net"] = float(eq.iloc[-1]) if len(eq) else 0.0
    out["max_drawdown"] = float((eq.cummax() - eq).max()) if len(eq) else 0.0
    return out


def write_outputs(run_dir, fills, equity, plot=True):
    fills.to_parquet(os.path.join(run_dir, "fills.parquet"), index=False)
    equity.to_parquet(os.path.join(run_dir, "equity.parquet"), index=False)
    if not plot or equity.empty:
        return None
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return None
    t = (equity["ts_ns"] - equity["ts_ns"].iloc[0]) * 1e-9
    fig, (a, b) = plt.subplots(2, 1, sharex=True, figsize=(9, 5))
    a.plot(t, equity["equity"], lw=1)
    a.set_ylabel("equity (mark-to-market)")
    a.grid(alpha=0.3)
    b.step(t, equity["net_position"], where="post", lw=1, color="tab:orange")
    b.set_ylabel("net position (lots)")
    b.set_xlabel("simulated seconds")
    b.grid(alpha=0.3)
    fig.suptitle(os.path.basename(os.path.normpath(run_dir)))
    fig.tight_layout()
    path = os.path.join(run_dir, "equity.png")
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args(argv)
    summary, fills, equity = load(args.run_dir)
    mine = recompute(fills, equity)
    png = write_outputs(args.run_dir, fills, equity, plot=not args.no_plot)
    print("%-22s %14s %14s" % ("metric", "engine", "recomputed"))
    ok = True
    for key in ("fills", "qty_filled", "maker_fills", "fees", "pnl_net", "max_drawdown"):
        a, b = summary[key], mine[key]
        match = abs(a - b) <= 1e-6 * max(1.0, abs(a))
        ok &= match
        print("%-22s %14.6g %14.6g %s" % (key, a, b, "" if match else "<-- MISMATCH"))
    pos_ok = {k: v for k, v in summary["final_positions"].items() if v} == \
             {k: v for k, v in mine["final_positions"].items() if v}
    ok &= pos_ok
    print("%-22s %29s" % ("final_positions", "match" if pos_ok else "MISMATCH"))
    if png:
        print("wrote", png)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
