"""End-to-end tests: the CLI as a user runs it, plus the Python tooling."""
import hashlib
import json
import os
import subprocess

import pandas as pd
import pytest

from hftbt_tools import feedio, report, sweep


def run(binary, *args, cwd=None):
    res = subprocess.run([binary] + list(args), capture_output=True, text=True, cwd=cwd)
    assert res.returncode == 0, res.stderr
    return res.stdout


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


@pytest.fixture(scope="module")
def feed(hftbt_bin, tmp_path_factory):
    path = str(tmp_path_factory.mktemp("feed") / "feed.csv")
    run(hftbt_bin, "gen", "--out", path, "--events", "30000", "--seed", "5", "--gap-ns", "200000")
    return path


@pytest.fixture(scope="module")
def config(feed, tmp_path_factory):
    path = str(tmp_path_factory.mktemp("cfg") / "mm.ini")
    with open(path, "w") as f:
        f.write("[run]\ndata = %s\nsample_interval_ns = 10000000\n"
                "markout_horizons_ns = 1000000\n"
                "[latency]\nmd_delay_ns = 50000\nmd_jitter_ns = 20000\norder_delay_ns = 100000\n"
                "order_jitter_ns = 50000\nresponse_delay_ns = 100000\n"
                "[fees]\nmaker_bps = -0.1\ntaker_bps = 0.3\n"
                "[strategy]\nname = market_maker\nhalf_spread_ticks = 1\n" % feed)
    return path


def test_generator_is_reproducible(hftbt_bin, feed, tmp_path):
    again = str(tmp_path / "again.csv")
    other = str(tmp_path / "other.csv")
    run(hftbt_bin, "gen", "--out", again, "--events", "30000", "--seed", "5", "--gap-ns", "200000")
    run(hftbt_bin, "gen", "--out", other, "--events", "30000", "--seed", "6", "--gap-ns", "200000")
    assert digest(feed) == digest(again)
    assert digest(feed) != digest(other)


def test_same_input_seed_config_gives_byte_identical_reports(hftbt_bin, config, tmp_path):
    outs = []
    for name in ("a", "b"):
        out = str(tmp_path / name)
        run(hftbt_bin, "run", "--config", config, "--set", "run.out_dir=" + out,
            "--set", "run.event_log=%s.log" % out)
        outs.append(out)
    for fname in ("summary.json", "fills.csv", "equity.csv"):
        assert digest(os.path.join(outs[0], fname)) == digest(os.path.join(outs[1], fname))
    assert digest(outs[0] + ".log") == digest(outs[1] + ".log")
    with open(os.path.join(outs[0], "summary.json")) as f:
        assert json.load(f)["fills"] > 0, "comparison is vacuous without fills"


def test_csv_and_binary_feeds_give_the_same_run(hftbt_bin, feed, config, tmp_path):
    binfeed = str(tmp_path / "feed.bin")
    run(hftbt_bin, "convert", feed, binfeed)
    a = run(hftbt_bin, "run", "--config", config, "--quiet")
    b = run(hftbt_bin, "run", "--config", config, "--quiet", "--set", "run.data=" + binfeed)
    assert a == b and len(a.strip()) == 16


def test_seed_changes_jittered_run_only(hftbt_bin, config):
    base = run(hftbt_bin, "run", "--config", config, "--quiet", "--set", "run.seed=1")
    assert base != run(hftbt_bin, "run", "--config", config, "--quiet", "--set", "run.seed=2")
    no_jitter = ["--set", "latency.md_jitter_ns=0", "--set", "latency.order_jitter_ns=0"]
    assert run(hftbt_bin, "run", "--config", config, "--quiet", "--set", "run.seed=1", *no_jitter) \
        == run(hftbt_bin, "run", "--config", config, "--quiet", "--set", "run.seed=2", *no_jitter)


def test_cli_rejects_bad_input(hftbt_bin, config, tmp_path):
    def fails(*args):
        res = subprocess.run([hftbt_bin] + list(args), capture_output=True, text=True)
        assert res.returncode != 0
        return res.stderr

    assert "unknown config key" in fails("run", "--config", config, "--set", "latency.md_delay=5")
    assert "not an integer" in fails("run", "--config", config, "--set", "latency.md_delay_ns=x")
    assert "unknown strategy" in fails("run", "--config", config, "--set", "strategy.name=zzz")
    assert "cannot open" in fails("run", "--config", config, "--set", "run.data=/nonexistent.csv")
    bad = tmp_path / "bad.csv"
    bad.write_text("ts_ns,symbol,type,order_id,side,price,qty\n5,X,A,1,B,100,1\n4,X,A,2,B,100,1\n")
    assert "goes backwards" in fails("run", "--config", config, "--set", "run.data=%s" % bad)
    garbage = tmp_path / "bad.bin"
    garbage.write_bytes(b"HFTBTFD1" + b"\xff" * 40)
    assert "error" in fails("run", "--config", config, "--set", "run.data=%s" % garbage)


def test_parquet_round_trip(feed, tmp_path):
    pq = str(tmp_path / "feed.parquet")
    back = str(tmp_path / "back.csv")
    n = feedio.csv_to_parquet(feed, pq)
    assert n == 30000
    assert feedio.parquet_to_csv(pq, back) == n
    pd.testing.assert_frame_equal(feedio.read_csv(feed), feedio.read_csv(back))


def test_parquet_decimal_prices_need_exact_ticks(tmp_path):
    df = pd.DataFrame({"ts_ns": [1, 2], "symbol": ["X", "X"], "type": ["A", "A"],
                       "order_id": [1, 2], "side": ["B", "S"], "price": [100.25, 100.50],
                       "qty": [5, 5]})
    pq = str(tmp_path / "dec.parquet")
    out = str(tmp_path / "dec.csv")
    df.to_parquet(pq, index=False)
    with pytest.raises(ValueError, match="must be integer"):
        feedio.parquet_to_csv(pq, out)
    with pytest.raises(ValueError, match="not multiples"):
        feedio.parquet_to_csv(pq, out, tick_size=0.1)
    assert feedio.parquet_to_csv(pq, out, tick_size=0.25) == 2
    assert list(feedio.read_csv(out)["price"]) == [401, 402]


def test_feed_validation_mirrors_engine():
    ok = pd.DataFrame({"ts_ns": [1, 2], "symbol": ["X", "X"], "type": ["A", "D"],
                       "order_id": [1, 1], "side": ["B", ""], "price": [100, 0], "qty": [5, 0]})
    feedio.validate(ok)
    for col, value, msg in (("ts_ns", [2, 1], "non-decreasing"), ("type", ["A", "Q"], "unknown"),
                            ("side", ["", ""], "need side"), ("qty", [-1, 0], "non-negative")):
        bad = ok.copy()
        bad[col] = value
        with pytest.raises(ValueError, match=msg):
            feedio.validate(bad)


def test_report_recomputes_engine_numbers(hftbt_bin, config, tmp_path):
    out = str(tmp_path / "run")
    run(hftbt_bin, "run", "--config", config, "--set", "run.out_dir=" + out)
    assert report.main([out, "--no-plot"]) == 0
    assert os.path.exists(os.path.join(out, "fills.parquet"))
    summary, fills, equity = report.load(out)
    mine = report.recompute(fills, equity)
    assert mine["qty_filled"] == summary["qty_filled"] > 0
    assert mine["pnl_net"] == pytest.approx(summary["pnl_net"])

    # The cross-check must be able to fail: corrupt one fill.
    fills.loc[0, "qty"] += 1
    fills.to_csv(os.path.join(out, "fills.csv"), index=False)
    assert report.main([out, "--no-plot"]) == 1


def test_sweep_splits_in_and_out_of_sample(hftbt_bin, config):
    grid = sweep.parse_grid(["strategy.half_spread_ticks=1,2", "latency.order_delay_ns=0,2000000"])
    df, split_ts = sweep.sweep(hftbt_bin, config, grid, split=0.5, jobs=2)
    assert len(df) == 4
    assert list(df["is_rank"]) == sorted(df["is_rank"])
    assert (df["is_fills"] > 0).all() and (df["oos_fills"] > 0).all()
    # Both halves are different simulations of the same grid point.
    assert (df["is_event_hash"] != df["oos_event_hash"]).all()
    # The parameters really reach the engine: latency changes the outcome.
    by_latency = df.groupby("latency.order_delay_ns")["is_event_hash"].agg(set)
    assert by_latency.iloc[0].isdisjoint(by_latency.iloc[1])
    v = sweep.verdict(df)
    assert v["trials"] == 4 and 1 <= v["oos_rank_of_is_best"] <= 4

    # In-sample + out-of-sample fills partition the full run's opportunity set:
    # the in-sample half must equal a plain run truncated at the split.
    point = {"strategy.half_spread_ticks": "1", "latency.order_delay_ns": "0"}
    full_is = sweep.run_once(hftbt_bin, config, dict(point, **{"run.end_ts": split_ts}))
    row = df[(df["strategy.half_spread_ticks"] == "1") & (df["latency.order_delay_ns"] == "0")]
    assert row["is_event_hash"].iloc[0] == full_is["event_hash"]


def test_sweep_grid_parser_rejects_garbage():
    with pytest.raises(ValueError):
        sweep.parse_grid(["no_equals"])
    assert sweep.parse_grid(["a.b=1, 2,3"]) == [("a.b", ["1", "2", "3"])]
