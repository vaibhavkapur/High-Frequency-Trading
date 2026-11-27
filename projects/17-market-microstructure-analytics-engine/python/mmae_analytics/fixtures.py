"""Deterministic fixture and synthetic signed-flow stream generator."""

from __future__ import annotations

import csv
import random
from pathlib import Path
from typing import Dict, List, Tuple


CSV_HEADER = [
    "event_ts_ns",
    "recv_ts_ns",
    "seq_num",
    "symbol",
    "event_type",
    "side",
    "price_ticks",
    "qty",
    "order_id",
]


def generate_synthetic_signed_flow_csv(
    output_path: Path,
    seed: int = 42,
    n_steps: int = 1200,
    symbol: str = "SYNTH_FLOW",
    initial_mid_ticks: int = 10000,
    inject_gaps: bool = True,
) -> Dict[str, int]:
    """Generate a deterministic L3 + TRADE event stream with known persistent signed flow.

    Latent informed flow regime psi in {-1, +1} persists across steps:
    - Order book imbalance (OBI) and OFI tilt in the direction of psi before trades arrive.
    - Aggressive trades arrive predominantly in the direction of psi (at ask for +1, at bid for -1),
      with ~8% explicit midpoint prints to exercise trade-sign ambiguity.
    - Short-horizon midprice shifts causally in the direction of psi, creating measurable
      positive price response R(h) > 0, positive adverse selection AS(h) > 0, and OOS signal.
    - Optional sequence gaps (followed by L2_SNAPSHOT_CLEAR recovery) test gap flagging.
    """
    rng = random.Random(seed)
    ts = 1_700_000_000_000_000_000
    seq = 1
    next_oid = 1

    mid = initial_mid_ticks
    half_spread = 1  # 2-tick spread (mid-1 bid, mid+1 ask) so exact midpoint trades exist!
    psi = 1  # latent signed flow pressure (+1 buy regime, -1 sell regime)

    rows: List[List[object]] = []
    # Track resting L3 orders: order_id -> (side, price, qty)
    active_bids: Dict[int, Tuple[int, int]] = {}
    active_asks: Dict[int, Tuple[int, int]] = {}

    def emit(ev_type: str, side: str, px: int, qty: int, oid: int, dt_ns: int) -> None:
        nonlocal ts, seq
        ts += dt_ns
        rows.append([ts, ts + 250, seq, symbol, ev_type, side, px, qty, oid])
        seq += 1

    def rebuild_book_snapshot(clear_first: bool) -> None:
        nonlocal next_oid
        if clear_first:
            emit("L2_SNAPSHOT_CLEAR", "UNKNOWN", 0, 0, 0, 2_000_000)
            active_bids.clear()
            active_asks.clear()
        bid_base = 260 if psi > 0 else 110
        ask_base = 110 if psi > 0 else 260
        for lvl in range(3):
            bpx = mid - half_spread - lvl
            apx = mid + half_spread + lvl
            bqty = bid_base + rng.randint(10, 60)
            aqty = ask_base + rng.randint(10, 60)
            oid_b = next_oid
            next_oid += 1
            active_bids[oid_b] = (bpx, bqty)
            emit("L3_ADD", "BID", bpx, bqty, oid_b, 500_000)

            oid_a = next_oid
            next_oid += 1
            active_asks[oid_a] = (apx, aqty)
            emit("L3_ADD", "ASK", apx, aqty, oid_a, 500_000)

    rebuild_book_snapshot(clear_first=False)

    gap_steps = {int(n_steps * 0.35), int(n_steps * 0.78)} if inject_gaps else set()
    injected_gaps = 0
    total_trades = 0
    midpoint_trades = 0

    for step in range(n_steps):
        # Regime switching with strong persistence (stays in regime ~18 steps on average).
        if rng.random() < 0.055:
            psi = -psi

        if step in gap_steps:
            # Skip 5 sequence numbers to create an explicit detectable gap.
            seq += 5
            injected_gaps += 1
            # Emit one trade during active gap (must be flagged GAP_ACTIVE / AMBIGUOUS).
            emit("TRADE", "UNKNOWN", mid + half_spread, 40, 0, 5_000_000)
            total_trades += 1
            # Recover cleanly via snapshot clear + L3 rebuild.
            rebuild_book_snapshot(clear_first=True)
            continue

        # 1. Causal quote tilt / liquidity replenishment aligned with latent flow psi.
        best_bid = mid - half_spread
        best_ask = mid + half_spread

        # Add passive liquidity skewed toward the strong side.
        if psi > 0:
            add_side, add_px, add_qty = "BID", best_bid, rng.randint(90, 220)
        else:
            add_side, add_px, add_qty = "ASK", best_ask, rng.randint(90, 220)

        oid = next_oid
        next_oid += 1
        if add_side == "BID":
            active_bids[oid] = (add_px, add_qty)
        else:
            active_asks[oid] = (add_px, add_qty)
        emit("L3_ADD", add_side, add_px, add_qty, oid, 8_000_000)

        # Cancel an older order on the weak side to keep bounded active order count and drive OFI.
        weak_map = active_asks if psi > 0 else active_bids
        weak_side = "ASK" if psi > 0 else "BID"
        if len(weak_map) > 5:
            old_oid = next(iter(weak_map.keys()))
            weak_map.pop(old_oid)
            emit("L3_CANCEL", weak_side, 0, 0, old_oid, 6_000_000)

        # Keep strong side bounded too.
        strong_map = active_bids if psi > 0 else active_asks
        strong_side = "BID" if psi > 0 else "ASK"
        if len(strong_map) > 10:
            old_oid = next(iter(strong_map.keys()))
            strong_map.pop(old_oid)
            emit("L3_CANCEL", strong_side, 0, 0, old_oid, 4_000_000)

        # 2. Emit trade print: 8% midpoint (explicit ambiguity), else 80% aligned with psi.
        u = rng.random()
        t_qty = rng.randint(20, 120)
        if u < 0.08:
            t_px = mid  # exact midpoint
            midpoint_trades += 1
        else:
            aggressor = psi if rng.random() < 0.80 else -psi
            t_px = best_ask if aggressor > 0 else best_bid
        emit("TRADE", "UNKNOWN", t_px, t_qty, 0, 10_000_000)
        total_trades += 1

        # 3. Execute against a resting L3 order periodically to generate queue metrics.
        target_map = active_asks if psi > 0 else active_bids
        target_side = "ASK" if psi > 0 else "BID"
        if target_map and rng.random() < 0.35:
            exec_oid = next(iter(target_map.keys()))
            epx, eqty = target_map[exec_oid]
            fill = min(eqty, rng.randint(10, 50))
            if fill >= eqty:
                target_map.pop(exec_oid)
            else:
                target_map[exec_oid] = (epx, eqty - fill)
            emit("L3_EXECUTE", target_side, epx, fill, exec_oid, 5_000_000)

        # 4. Short-horizon price impact: midprice shifts causally in direction of psi.
        if rng.random() < 0.28:
            old_mid = mid
            step_dir = psi if rng.random() < 0.85 else -psi
            mid = max(1000, mid + step_dir)
            if mid != old_mid:
                # Re-center quotes cleanly around new mid without crossing.
                rebuild_book_snapshot(clear_first=True)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(CSV_HEADER)
        w.writerows(rows)

    return {
        "seed": seed,
        "total_rows": len(rows),
        "total_trades": total_trades,
        "midpoint_trades": midpoint_trades,
        "injected_gaps": injected_gaps,
    }
