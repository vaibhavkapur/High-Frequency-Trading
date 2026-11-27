"""Chronological offline evaluation: short-horizon price response, adverse selection, and OOS statistics.

Strictly separated from the causal C++20 FeatureEngine so forward-looking target labels
(Delta midprice over horizon h) can never leak into feature calculation.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    import polars as pl  # type: ignore

    HAS_POLARS = True
except ImportError:  # pragma: no cover - verified explicitly in acceptance report
    pl = None
    HAS_POLARS = False


SCHEMA_VERSION = "mmae.v1"


@dataclass
class EvaluationConfig:
    horizons_ms: List[int] = field(default_factory=lambda: [50, 100, 250, 500])
    train_fraction: float = 0.70
    ridge_lambda: float = 1e-4
    stale_threshold_ns: int = 1_000_000_000
    tick_size: float = 0.01
    prefer_polars: bool = True


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _parse_opt_float(val: Any) -> Optional[float]:
    if val is None:
        return None
    if isinstance(val, (int, float)):
        f = float(val)
        return f if math.isfinite(f) else None
    s = str(val).strip()
    if not s:
        return None
    f = float(s)
    return f if math.isfinite(f) else None


def _parse_int(val: Any, default: int = 0) -> int:
    if val is None:
        return default
    if isinstance(val, int):
        return val
    s = str(val).strip()
    return int(s) if s else default


def _load_feature_rows(features_csv: Path, prefer_polars: bool) -> Tuple[List[Dict[str, Any]], str]:
    if prefer_polars and HAS_POLARS:
        df = pl.read_csv(str(features_csv))
        raw_rows = df.to_dicts()
        backend = f"polars-{pl.__version__}"
    else:
        with features_csv.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            raw_rows = list(reader)
        backend = "python-stdlib"

    rows: List[Dict[str, Any]] = []
    for r in raw_rows:
        rows.append(
            {
                "seq_num": _parse_int(r["seq_num"]),
                "event_ts_ns": _parse_int(r["event_ts_ns"]),
                "event_type": str(r["event_type"]),
                "book_valid": _parse_int(r["book_valid"]) == 1,
                "is_crossed": _parse_int(r["is_crossed"]) == 1,
                "is_locked": _parse_int(r["is_locked"]) == 1,
                "is_stale": _parse_int(r["is_stale"]) == 1,
                "gap_active": _parse_int(r["gap_active"]) == 1,
                "gap_detected_here": _parse_int(r["gap_detected_here"]) == 1,
                "mid_price": _parse_opt_float(r["mid_price"]),
                "spread_ticks": _parse_opt_float(r["spread_ticks"]),
                "spread_bps": _parse_opt_float(r["spread_bps"]),
                "microprice_offset_ticks": _parse_opt_float(r["microprice_offset_ticks"]),
                "obi_l1": _parse_opt_float(r["obi_l1"]),
                "obi_k": _parse_opt_float(r["obi_k"]),
                "ofi_event": _parse_opt_float(r["ofi_event"]) or 0.0,
                "ofi_window": _parse_opt_float(r["ofi_window"]) or 0.0,
                "is_trade": _parse_int(r["is_trade"]) == 1,
                "trade_price": _parse_opt_float(r["trade_price"]),
                "trade_qty": _parse_int(r["trade_qty"]),
                "trade_sign": _parse_int(r["trade_sign"], default=0),
                "classification_reason": str(r["classification_reason"]),
                "is_ambiguous": _parse_int(r["is_ambiguous"]) == 1,
                "rolling_trade_flow_imbalance": _parse_opt_float(r["rolling_trade_flow_imbalance"]),
            }
        )
    return rows, backend


def _solve_linear_system(a: List[List[float]], b: List[float]) -> Optional[List[float]]:
    n = len(b)
    mat = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(mat[r][col]))
        if abs(mat[pivot][col]) < 1e-12:
            return None
        mat[col], mat[pivot] = mat[pivot], mat[col]
        div = mat[col][col]
        for j in range(col, n + 1):
            mat[col][j] /= div
        for r in range(n):
            if r != col:
                factor = mat[r][col]
                if factor != 0.0:
                    for j in range(col, n + 1):
                        mat[r][j] -= factor * mat[col][j]
    return [mat[i][n] for i in range(n)]


def _fit_ridge_ols(xs: List[List[float]], ys: List[float], ridge_lambda: float) -> Optional[List[float]]:
    if not xs or len(xs) != len(ys):
        return None
    p = len(xs[0])
    xtx = [[0.0] * p for _ in range(p)]
    xty = [0.0] * p
    for x, y in zip(xs, ys):
        for i in range(p):
            xi = x[i]
            xty[i] += xi * y
            for j in range(i, p):
                xtx[i][j] += xi * x[j]
    for i in range(p):
        for j in range(i):
            xtx[i][j] = xtx[j][i]
        if i > 0:  # do not penalize intercept
            xtx[i][i] += ridge_lambda
    return _solve_linear_system(xtx, xty)


def _pearson(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    n = len(xs)
    if n < 2:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    dx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    dy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if dx == 0.0 or dy == 0.0:
        return None
    return num / (dx * dy)


def _average_ranks(vals: Sequence[float]) -> List[float]:
    n = len(vals)
    indexed = sorted(enumerate(vals), key=lambda t: t[1])
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and indexed[j + 1][1] == indexed[i][1]:
            j += 1
        avg_rank = 0.5 * (i + j) + 1.0
        for k in range(i, j + 1):
            ranks[indexed[k][0]] = avg_rank
        i = j + 1
    return ranks


def _spearman(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    if len(xs) < 2:
        return None
    return _pearson(_average_ranks(xs), _average_ranks(ys))


def _compute_forward_mids(
    rows: List[Dict[str, Any]],
    horizon_ns: int,
    stale_threshold_ns: int,
) -> List[Optional[float]]:
    """Compute forward midprice m(t_i + h) via as-of lookup, excluding gap-contaminated windows."""
    n = len(rows)
    if n == 0:
        return []

    max_ts = rows[-1]["event_ts_ns"]

    # Prefix count of gap-flagged events so we can test gap-free (i, j] in O(1).
    gap_prefix = [0] * (n + 1)
    for idx, r in enumerate(rows):
        gap_prefix[idx + 1] = gap_prefix[idx] + (1 if (r["gap_active"] or r["gap_detected_here"]) else 0)

    # Track last valid-book midprice index at or before any position j.
    last_valid_idx = [-1] * n
    cur_valid = -1
    for idx, r in enumerate(rows):
        if r["book_valid"] and r["mid_price"] is not None:
            cur_valid = idx
        last_valid_idx[idx] = cur_valid

    forward_mids: List[Optional[float]] = [None] * n
    j = 0
    for i, r in enumerate(rows):
        target_ts = r["event_ts_ns"] + horizon_ns
        if target_ts > max_ts:
            break
        while j + 1 < n and rows[j + 1]["event_ts_ns"] <= target_ts:
            j += 1

        # Reject if any gap occurred in [i, j].
        if gap_prefix[j + 1] - gap_prefix[i] > 0:
            continue

        lv = last_valid_idx[j]
        if lv < i:
            continue
        if target_ts - rows[lv]["event_ts_ns"] > stale_threshold_ns:
            continue
        forward_mids[i] = rows[lv]["mid_price"]

    return forward_mids


def evaluate_features(
    features_csv: Path,
    diagnostics_json: Optional[Path] = None,
    config: Optional[EvaluationConfig] = None,
) -> Dict[str, Any]:
    cfg = config or EvaluationConfig()
    rows, backend = _load_feature_rows(features_csv, cfg.prefer_polars)

    diag: Dict[str, Any] = {}
    if diagnostics_json is not None and diagnostics_json.exists():
        diag = json.loads(diagnostics_json.read_text(encoding="utf-8"))
        if "config" in diag:
            cfg.tick_size = float(diag["config"].get("tick_size", cfg.tick_size))
            cfg.stale_threshold_ns = int(diag["config"].get("stale_threshold_ns", cfg.stale_threshold_ns))

    n_rows = len(rows)
    valid_book_rows = sum(1 for r in rows if r["book_valid"])
    gap_rows = sum(1 for r in rows if r["gap_active"] or r["gap_detected_here"])
    stale_rows = sum(1 for r in rows if r["is_stale"])
    crossed_rows = sum(1 for r in rows if r["is_crossed"])
    locked_rows = sum(1 for r in rows if r["is_locked"])

    trade_rows = [r for r in rows if r["is_trade"]]
    classified_trades = [r for r in trade_rows if not r["is_ambiguous"] and r["trade_sign"] in (-1, 1)]
    ambiguous_trades = [r for r in trade_rows if r["is_ambiguous"]]

    # Chronological IS/OOS split with embargo = max(horizons_ms).
    max_horizon_ns = max(cfg.horizons_ms) * 1_000_000 if cfg.horizons_ms else 0
    if n_rows > 0:
        split_idx = max(0, min(n_rows - 1, int(n_rows * cfg.train_fraction) - 1))
        train_end_ts = rows[split_idx]["event_ts_ns"]
        test_start_ts = train_end_ts + max_horizon_ns
    else:
        split_idx = 0
        train_end_ts = 0
        test_start_ts = 0

    price_response_by_horizon: Dict[str, Any] = {}
    adverse_selection_by_horizon: Dict[str, Any] = {}
    oos_predictive_by_horizon: Dict[str, Any] = {}

    for h_ms in cfg.horizons_ms:
        h_key = f"{h_ms}ms"
        h_ns = h_ms * 1_000_000
        fwd_mids = _compute_forward_mids(rows, h_ns, cfg.stale_threshold_ns)

        # 1. Price response & adverse selection on classified non-gap valid-book trades.
        responses_ticks: List[float] = []
        responses_bps: List[float] = []
        eff_spreads_ticks: List[float] = []
        real_spreads_ticks: List[float] = []
        adv_sel_ticks: List[float] = []
        max_identity_residual = 0.0
        buy_responses_ticks: List[float] = []
        sell_responses_ticks: List[float] = []

        for i, r in enumerate(rows):
            if not (r["is_trade"] and r["book_valid"] and not r["is_ambiguous"] and r["trade_sign"] in (-1, 1)):
                continue
            mid0 = r["mid_price"]
            mid1 = fwd_mids[i]
            tpx = r["trade_price"]
            if mid0 is None or mid1 is None or tpx is None or mid0 <= 0.0:
                continue

            eps = float(r["trade_sign"])
            dm_ticks = (mid1 - mid0) / cfg.tick_size
            resp_ticks = eps * dm_ticks
            resp_bps = eps * ((mid1 - mid0) / mid0) * 10000.0
            eff_ticks = eps * ((tpx - mid0) / cfg.tick_size)
            real_ticks = eps * ((tpx - mid1) / cfg.tick_size)
            as_ticks = resp_ticks
            residual = abs(eff_ticks - (real_ticks + as_ticks))
            if residual > max_identity_residual:
                max_identity_residual = residual

            responses_ticks.append(resp_ticks)
            responses_bps.append(resp_bps)
            eff_spreads_ticks.append(eff_ticks)
            real_spreads_ticks.append(real_ticks)
            adv_sel_ticks.append(as_ticks)
            if r["trade_sign"] == 1:
                buy_responses_ticks.append(resp_ticks)
            else:
                sell_responses_ticks.append(resp_ticks)

        n_eval_trades = len(responses_ticks)
        mean_fn = lambda xs: (sum(xs) / len(xs)) if xs else None
        std_fn = (
            lambda xs: math.sqrt(sum((x - sum(xs) / len(xs)) ** 2 for x in xs) / (len(xs) - 1))
            if len(xs) > 1
            else None
        )

        mean_resp_ticks = mean_fn(responses_ticks)
        std_resp_ticks = std_fn(responses_ticks)
        se_resp_ticks = (
            std_resp_ticks / math.sqrt(n_eval_trades)
            if (std_resp_ticks is not None and n_eval_trades > 0)
            else None
        )

        price_response_by_horizon[h_key] = {
            "horizon_ms": h_ms,
            "n_classified_trades_evaluated": n_eval_trades,
            "trade_horizon_coverage": (n_eval_trades / len(classified_trades)) if classified_trades else 0.0,
            "mean_response_ticks": mean_resp_ticks,
            "std_response_ticks": std_resp_ticks,
            "se_response_ticks": se_resp_ticks,
            "mean_response_bps": mean_fn(responses_bps),
            "buy_mean_response_ticks": mean_fn(buy_responses_ticks),
            "sell_mean_response_ticks": mean_fn(sell_responses_ticks),
        }

        adverse_selection_by_horizon[h_key] = {
            "horizon_ms": h_ms,
            "n_trades": n_eval_trades,
            "mean_effective_half_spread_ticks": mean_fn(eff_spreads_ticks),
            "mean_realized_half_spread_ticks": mean_fn(real_spreads_ticks),
            "mean_adverse_selection_ticks": mean_fn(adv_sel_ticks),
            "adverse_selection_share_of_eff_spread": (
                (mean_fn(adv_sel_ticks) / mean_fn(eff_spreads_ticks))
                if (
                    mean_fn(adv_sel_ticks) is not None
                    and mean_fn(eff_spreads_ticks) is not None
                    and abs(mean_fn(eff_spreads_ticks)) > 1e-12
                )
                else None
            ),
            "max_decomposition_identity_residual": max_identity_residual,
        }

        # 2. Chronological IS/OOS linear prediction from strictly causal features.
        raw_train: List[Tuple[List[float], float]] = []
        raw_test: List[Tuple[List[float], float]] = []
        embargo_excluded = 0

        for i, r in enumerate(rows):
            if not r["book_valid"]:
                continue
            mid0 = r["mid_price"]
            mid1 = fwd_mids[i]
            if mid0 is None or mid1 is None:
                continue
            if r["obi_l1"] is None or r["obi_k"] is None or r["microprice_offset_ticks"] is None:
                continue

            tfi = r["rolling_trade_flow_imbalance"] if r["rolling_trade_flow_imbalance"] is not None else 0.0
            feats = [
                r["obi_l1"],
                r["obi_k"],
                r["ofi_window"],
                tfi,
                r["microprice_offset_ticks"],
            ]
            target_ticks = (mid1 - mid0) / cfg.tick_size

            ts = r["event_ts_ns"]
            if ts <= train_end_ts:
                # Ensure target timestamp also stays <= train_end_ts + 0?
                # No: embargo shifts test_start_ts = train_end_ts + max_horizon_ns,
                # so IS target at ts + h_ns <= train_end_ts + max_horizon_ns == test_start_ts (no overlap with OOS).
                raw_train.append((feats, target_ticks))
            elif ts < test_start_ts:
                embargo_excluded += 1
            else:
                raw_test.append((feats, target_ticks))

        feature_names = ["obi_l1", "obi_k", "ofi_window", "rolling_trade_flow_imbalance", "microprice_offset_ticks"]
        if len(raw_train) >= 10 and len(raw_test) >= 5:
            p = len(feature_names)
            # Standardize features strictly using IN-SAMPLE mean and std only (no OOS leakage).
            means = [sum(x[j] for x, _ in raw_train) / len(raw_train) for j in range(p)]
            stds = []
            for j in range(p):
                var = sum((x[j] - means[j]) ** 2 for x, _ in raw_train) / max(1, len(raw_train) - 1)
                s = math.sqrt(var)
                stds.append(s if s > 1e-12 else 1.0)

            x_train = [[1.0] + [(x[j] - means[j]) / stds[j] for j in range(p)] for x, _ in raw_train]
            y_train = [y for _, y in raw_train]
            x_test = [[1.0] + [(x[j] - means[j]) / stds[j] for j in range(p)] for x, _ in raw_test]
            y_test = [y for _, y in raw_test]

            weights = _fit_ridge_ols(x_train, y_train, cfg.ridge_lambda)
            if weights is not None:
                preds = [sum(w * v for w, v in zip(weights, row)) for row in x_test]
                mse_model = sum((yh - y) ** 2 for yh, y in zip(preds, y_test)) / len(y_test)
                mae_model = sum(abs(yh - y) for yh, y in zip(preds, y_test)) / len(y_test)
                mse_zero = sum(y ** 2 for y in y_test) / len(y_test)
                mae_zero = sum(abs(y) for y in y_test) / len(y_test)
                mean_y_test = sum(y_test) / len(y_test)
                sst_test = sum((y - mean_y_test) ** 2 for y in y_test)
                sse_model = sum((yh - y) ** 2 for yh, y in zip(preds, y_test))

                r2_vs_zero = (1.0 - (mse_model / mse_zero)) if mse_zero > 1e-12 else None
                r2_vs_mean = (1.0 - (sse_model / sst_test)) if sst_test > 1e-12 else None

                nz_pairs = [(yh, y) for yh, y in zip(preds, y_test) if abs(y) > 1e-12]
                dir_acc = (
                    sum(1 for yh, y in nz_pairs if (yh > 0 and y > 0) or (yh < 0 and y < 0)) / len(nz_pairs)
                    if nz_pairs
                    else None
                )

                oos_predictive_by_horizon[h_key] = {
                    "horizon_ms": h_ms,
                    "n_train": len(raw_train),
                    "n_embargo_excluded": embargo_excluded,
                    "n_test": len(raw_test),
                    "event_horizon_coverage": (len(raw_train) + embargo_excluded + len(raw_test)) / max(1, n_rows),
                    "oos_r2_vs_zero_baseline": r2_vs_zero,
                    "oos_r2_vs_test_mean": r2_vs_mean,
                    "oos_pearson_r": _pearson(preds, y_test),
                    "oos_spearman_ic": _spearman(preds, y_test),
                    "oos_directional_accuracy_nonzero": dir_acc,
                    "oos_rmse_ticks": math.sqrt(mse_model),
                    "oos_mae_ticks": mae_model,
                    "naive_zero_rmse_ticks": math.sqrt(mse_zero),
                    "naive_zero_mae_ticks": mae_zero,
                    "standardized_coefficients": {
                        "intercept": weights[0],
                        **{name: weights[idx + 1] for idx, name in enumerate(feature_names)},
                    },
                }
            else:
                oos_predictive_by_horizon[h_key] = {
                    "horizon_ms": h_ms,
                    "n_train": len(raw_train),
                    "n_embargo_excluded": embargo_excluded,
                    "n_test": len(raw_test),
                    "status": "singular_design_matrix",
                }
        else:
            oos_predictive_by_horizon[h_key] = {
                "horizon_ms": h_ms,
                "n_train": len(raw_train),
                "n_embargo_excluded": embargo_excluded,
                "n_test": len(raw_test),
                "status": "insufficient_samples",
            }

    return {
        "schema_version": SCHEMA_VERSION,
        "backend": backend,
        "polars_installed": HAS_POLARS,
        "features_csv": str(features_csv),
        "features_sha256": sha256_file(features_csv),
        "config": {
            "horizons_ms": cfg.horizons_ms,
            "train_fraction": cfg.train_fraction,
            "ridge_lambda": cfg.ridge_lambda,
            "stale_threshold_ns": cfg.stale_threshold_ns,
            "tick_size": cfg.tick_size,
        },
        "split": {
            "train_fraction": cfg.train_fraction,
            "train_end_ts_ns": train_end_ts,
            "embargo_ns": max_horizon_ns,
            "test_start_ts_ns": test_start_ts,
        },
        "coverage": {
            "total_feature_rows": n_rows,
            "valid_book_rows": valid_book_rows,
            "valid_book_ratio": (valid_book_rows / n_rows) if n_rows else 0.0,
            "gap_flagged_rows": gap_rows,
            "stale_rows": stale_rows,
            "crossed_rows": crossed_rows,
            "locked_rows": locked_rows,
            "total_trades": len(trade_rows),
            "classified_trades": len(classified_trades),
            "ambiguous_trades": len(ambiguous_trades),
            "classified_trade_coverage": (len(classified_trades) / len(trade_rows)) if trade_rows else 0.0,
            "ambiguous_trade_ratio": (len(ambiguous_trades) / len(trade_rows)) if trade_rows else 0.0,
        },
        "replay_gaps": diag.get("gaps", []),
        "price_response_by_horizon": price_response_by_horizon,
        "adverse_selection_by_horizon": adverse_selection_by_horizon,
        "oos_predictive_by_horizon": oos_predictive_by_horizon,
    }
