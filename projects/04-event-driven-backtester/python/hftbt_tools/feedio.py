"""Feed conversion between Parquet and the engine's CSV format.

The engine reads integer ticks/lots only. Real data usually has decimal
prices, so `parquet_to_csv` takes the tick size and refuses prices that are
not (to rounding) a whole number of ticks instead of silently rounding.

    python -m hftbt_tools.feedio to-csv  in.parquet out.csv [--tick-size 0.01]
    python -m hftbt_tools.feedio to-parquet in.csv out.parquet
"""
import argparse

import numpy as np
import pandas as pd

COLUMNS = ["ts_ns", "symbol", "type", "order_id", "side", "price", "qty"]
TYPES = set("ACDELT")


def read_csv(path):
    df = pd.read_csv(path, comment="#", dtype={"symbol": str, "type": str, "side": str},
                     keep_default_na=False, na_values=[])
    missing = [c for c in COLUMNS if c not in df.columns]
    if missing:
        raise ValueError("%s: missing columns %s" % (path, missing))
    for col in ("ts_ns", "order_id", "price", "qty"):
        df[col] = pd.to_numeric(df[col].replace("", 0)).astype("int64")
    return df[COLUMNS]


def validate(df):
    """Mirror of the checks the C++ loader applies; raises ValueError."""
    if not set(df["type"].unique()) <= TYPES:
        raise ValueError("unknown event types: %s" % sorted(set(df["type"].unique()) - TYPES))
    if (df["ts_ns"] < 0).any() or (df["ts_ns"].diff().dropna() < 0).any():
        raise ValueError("timestamps must be non-negative and non-decreasing")
    needs_side = df["type"].isin(["A", "L", "T"])
    if not df.loc[needs_side, "side"].isin(["B", "S"]).all():
        raise ValueError("A/L/T events need side B or S")
    if (df["qty"] < 0).any() or (df["price"] < 0).any():
        raise ValueError("price and qty must be non-negative")


def csv_to_parquet(src, dst):
    df = read_csv(src)
    validate(df)
    df.to_parquet(dst, index=False)
    return len(df)


def parquet_to_csv(src, dst, tick_size=None):
    df = pd.read_parquet(src)
    missing = [c for c in COLUMNS if c not in df.columns]
    if missing:
        raise ValueError("%s: missing columns %s" % (src, missing))
    df = df[COLUMNS].copy()
    if tick_size is not None:
        ticks = df["price"].astype("float64") / tick_size
        rounded = np.rint(ticks)
        if (np.abs(ticks - rounded) > 1e-6).any():
            raise ValueError("prices are not multiples of tick size %g" % tick_size)
        df["price"] = rounded.astype("int64")
    for col in ("ts_ns", "order_id", "price", "qty"):
        if not np.issubdtype(df[col].dtype, np.integer):
            raise ValueError("column %s must be integer (got %s); pass --tick-size for prices"
                             % (col, df[col].dtype))
    df["side"] = df["side"].fillna("")
    df = df.sort_values("ts_ns", kind="stable")
    validate(df)
    df.to_csv(dst, index=False)
    return len(df)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("to-csv")
    a.add_argument("src")
    a.add_argument("dst")
    a.add_argument("--tick-size", type=float, default=None)
    b = sub.add_parser("to-parquet")
    b.add_argument("src")
    b.add_argument("dst")
    args = ap.parse_args(argv)
    if args.cmd == "to-csv":
        n = parquet_to_csv(args.src, args.dst, args.tick_size)
    else:
        n = csv_to_parquet(args.src, args.dst)
    print("wrote %d events to %s" % (n, args.dst))


if __name__ == "__main__":
    main()
