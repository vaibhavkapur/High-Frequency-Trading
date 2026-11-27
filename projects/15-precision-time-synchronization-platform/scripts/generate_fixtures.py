#!/usr/bin/env python3
"""Deterministic fixture generator for Precision Time Synchronization Platform (ptsp.v1).

Generates valid, invalid, boundary, and fault-injection fixtures using a fixed PRNG seed,
along with an independent Python expected-result reference file for verification.
"""

from __future__ import annotations

import json
import os
import random
from pathlib import Path
from typing import Dict, List, Tuple

SCHEMA_VERSION = "ptsp.v1"
BASE_EPOCH_NS = 1_700_000_000_000_000_000  # 2023-11-14T22:13:20Z (synthetic lab epoch)


def write_sync_csv(path: Path, rows: List[Tuple]) -> None:
    lines = [
        f"# schema_version={SCHEMA_VERSION}",
        "seq,epoch_id,reference_domain,local_domain,reference_clock_id,local_clock_id,"
        "capture_method,error_source,t1_ns,t2_ns,t3_ns,t4_ns,true_offset_ns,true_drift_ppb,true_asymmetry_ns",
    ]
    for r in rows:
        lines.append(",".join(str(x) for x in r))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_packets_csv(path: Path, rows: List[Tuple]) -> None:
    lines = [
        f"# schema_version={SCHEMA_VERSION}",
        "packet_id,sequence_id,source_id,symbol,"
        "event_ns,event_unc_ns,event_domain,event_role,event_capture,event_error_source,event_clock_id,"
        "rx_ns,rx_unc_ns,rx_domain,rx_role,rx_capture,rx_error_source,rx_clock_id",
    ]
    for r in rows:
        lines.append(",".join(str(x) for x in r))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_cfg(path: Path, kv: Dict[str, object]) -> None:
    lines = [f"# Deterministic scenario config ({SCHEMA_VERSION})", f"schema_version={SCHEMA_VERSION}"]
    for k, v in kv.items():
        lines.append(f"{k}={v}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def make_sync_row(
    seq: int,
    epoch_id: int,
    t1_ns: int,
    true_offset_ns: int,
    true_drift_ppb: float,
    forward_delay_ns: int,
    reverse_delay_ns: int,
    turnaround_ns: int = 1_000,
    ref_dom: str = "TAI",
    loc_dom: str = "MONOTONIC_RAW",
) -> Tuple:
    # Two-way PTP equations:
    # t2 = t1 + d_f + offset(t1)
    # t3 = t2 + turnaround
    # t4 = t3 - offset(t1) + d_r
    t2_ns = t1_ns + forward_delay_ns + true_offset_ns
    t3_ns = t2_ns + turnaround_ns
    t4_ns = t3_ns - true_offset_ns + reverse_delay_ns
    asym_ns = (forward_delay_ns - reverse_delay_ns) // 2
    return (
        seq,
        epoch_id,
        ref_dom,
        loc_dom,
        "gm-tai-0",
        "local-osc-0",
        "SYNTHETIC_FIXTURE",
        "SYNTHETIC_FIXTURE",
        t1_ns,
        t2_ns,
        t3_ns,
        t4_ns,
        true_offset_ns,
        f"{true_drift_ppb:.3f}",
        asym_ns,
    )


def make_packet_row(
    packet_id: int,
    seq_id: int,
    source_id: str,
    symbol: str,
    event_ns: int,
    event_unc_ns: int,
    true_offset_ns: int,
    transit_ns: int,
    rx_unc_ns: int = 25,
    ev_dom: str = "TAI",
    rx_dom: str = "MONOTONIC_RAW",
    ev_role: str = "EVENT_TIME",
    rx_role: str = "RECEIVE_TIME",
) -> Tuple:
    rx_ns = event_ns + transit_ns + true_offset_ns
    return (
        packet_id,
        seq_id,
        source_id,
        symbol,
        event_ns,
        event_unc_ns,
        ev_dom,
        ev_role,
        "SYNTHETIC_FIXTURE",
        "SYNTHETIC_FIXTURE",
        f"venue-{source_id}",
        rx_ns,
        rx_unc_ns,
        rx_dom,
        rx_role,
        "SYNTHETIC_FIXTURE",
        "SYNTHETIC_FIXTURE",
        "local-osc-0",
    )


def generate_all(fixtures_dir: Path) -> None:
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261127)

    # 1) reference_drift: clean linear drift (150 ppb), low symmetric jitter, clean ordered packets
    ref_sync = []
    ref_pkts = []
    base_offset = 12_500
    drift_ppb = 150.0
    for i in range(60):
        t1 = BASE_EPOCH_NS + i * 1_000_000_000
        offset = int(round(base_offset + drift_ppb * i))
        noise_f = rng.randint(-12, 12)
        noise_r = rng.randint(-12, 12)
        ref_sync.append(make_sync_row(i + 1, 0, t1, offset, drift_ppb, 800 + noise_f, 800 + noise_r))

    for j in range(30):
        ev_t = BASE_EPOCH_NS + (10 + j) * 1_000_000_000 + 250_000
        offset = int(round(base_offset + drift_ppb * (10 + j + 0.00025)))
        ref_pkts.append(make_packet_row(j + 1, j + 1, "feed-a", "SYNTH-EQ1", ev_t, 20, offset, 1_500))

    write_sync_csv(fixtures_dir / "reference_drift_sync.csv", ref_sync)
    write_packets_csv(fixtures_dir / "reference_drift_packets.csv", ref_pkts)
    write_cfg(
        fixtures_dir / "reference_drift.cfg",
        {
            "scenario_name": "reference_drift",
            "seed": 20261127,
            "sync_fixture_path": "reference_drift_sync.csv",
            "packet_fixture_path": "reference_drift_packets.csv",
            "window_size": 32,
            "max_skew_ns": 5000,
            "reorder_window_ns": 2000,
            "max_allowed_offset_error_ns": 100,
            "max_allowed_drift_error_ppb": 15.0,
        },
    )

    # Independent Python reference calculation on reference_drift_sync.csv
    raw_offsets = [((r[9] - r[8]) - (r[11] - r[10])) // 2 for r in ref_sync]
    true_offsets = [r[12] for r in ref_sync]
    raw_rtts = [(r[11] - r[8]) - (r[10] - r[9]) for r in ref_sync]
    expected_ref = {
        "schema_version": SCHEMA_VERSION,
        "scenario_name": "reference_drift",
        "sync_sample_count": len(ref_sync),
        "packet_count": len(ref_pkts),
        "true_initial_offset_ns": base_offset,
        "true_drift_ppb": drift_ppb,
        "true_final_offset_ns": true_offsets[-1],
        "python_raw_offset_mae_ns": round(
            sum(abs(a - b) for a, b in zip(raw_offsets, true_offsets)) / len(ref_sync), 4
        ),
        "python_min_rtt_ns": min(raw_rtts),
        "expected_orderer_state": "HEALTHY",
        "expected_packets_emitted": len(ref_pkts),
    }
    (fixtures_dir / "expected_reference.json").write_text(
        json.dumps(expected_ref, indent=2) + "\n", encoding="utf-8"
    )

    # 2) clock_step_and_outliers: +25,000 ns clock step at seq=30 + single-packet +18,000 ns delay outliers
    step_sync = []
    step_pkts = []
    for i in range(70):
        t1 = BASE_EPOCH_NS + i * 1_000_000_000
        step_jump = 25_000 if i >= 30 else 0
        offset = int(round(5_000 + step_jump + 80.0 * i))
        df = 750 + rng.randint(-10, 10)
        dr = 750 + rng.randint(-10, 10)
        if i in (12, 22, 48, 58):
            df += 18_000  # Single-packet forward queuing spike (9 us offset bias if unfiltered!)
        step_sync.append(make_sync_row(i + 1, 0, t1, offset, 80.0, df, dr))

    for j in range(25):
        sec = 35 + j
        ev_t = BASE_EPOCH_NS + sec * 1_000_000_000 + 100_000
        offset = int(round(5_000 + 25_000 + 80.0 * sec))
        step_pkts.append(make_packet_row(j + 1, j + 1, "feed-a", "SYNTH-EQ1", ev_t, 20, offset, 1_400))

    write_sync_csv(fixtures_dir / "clock_step_and_outliers_sync.csv", step_sync)
    write_packets_csv(fixtures_dir / "clock_step_and_outliers_packets.csv", step_pkts)
    write_cfg(
        fixtures_dir / "clock_step_and_outliers.cfg",
        {
            "scenario_name": "clock_step_and_outliers",
            "seed": 20261127,
            "sync_fixture_path": "clock_step_and_outliers_sync.csv",
            "packet_fixture_path": "clock_step_and_outliers_packets.csv",
            "window_size": 24,
            "step_threshold_ns": 5000,
            "step_confirm_count": 2,
            "max_skew_ns": 15000,
            "reorder_window_ns": 2000,
            "max_allowed_offset_error_ns": 150,
            "max_allowed_drift_error_ppb": 30.0,
        },
    )

    # 3) packet_asymmetry: periodic severe asymmetric congestion spikes on forward path
    asym_sync = []
    asym_pkts = []
    for i in range(60):
        t1 = BASE_EPOCH_NS + i * 1_000_000_000
        offset = int(round(-8_000 + 60.0 * i))
        df = 600 + rng.randint(-8, 8)
        dr = 600 + rng.randint(-8, 8)
        if i >= 10 and (i % 7 == 0):
            df += 12_000  # Heavy asymmetric forward burst
        asym_sync.append(make_sync_row(i + 1, 0, t1, offset, 60.0, df, dr))

    for j in range(20):
        sec = 15 + j
        ev_t = BASE_EPOCH_NS + sec * 1_000_000_000 + 500_000
        offset = int(round(-8_000 + 60.0 * sec))
        asym_pkts.append(make_packet_row(j + 1, j + 1, "feed-a", "SYNTH-FX1", ev_t, 20, offset, 1_600))

    write_sync_csv(fixtures_dir / "packet_asymmetry_sync.csv", asym_sync)
    write_packets_csv(fixtures_dir / "packet_asymmetry_packets.csv", asym_pkts)
    write_cfg(
        fixtures_dir / "packet_asymmetry.cfg",
        {
            "scenario_name": "packet_asymmetry",
            "seed": 20261127,
            "sync_fixture_path": "packet_asymmetry_sync.csv",
            "packet_fixture_path": "packet_asymmetry_packets.csv",
            "window_size": 24,
            "max_skew_ns": 12000,
            "reorder_window_ns": 2000,
            "max_allowed_offset_error_ns": 150,
            "max_allowed_drift_error_ppb": 30.0,
        },
    )

    # 4) clock_restart: host restarts at seq=25, epoch increments 0->1 and local clock resets by -500 ms
    rst_sync = []
    rst_pkts = []
    for i in range(55):
        t1 = BASE_EPOCH_NS + i * 1_000_000_000
        if i < 25:
            epoch_id = 0
            offset = int(round(4_000 + 100.0 * i))
            drift = 100.0
        else:
            epoch_id = 1
            offset = int(round(-500_000_000 + 120.0 * (i - 25)))
            drift = 120.0
        df = 700 + rng.randint(-10, 10)
        dr = 700 + rng.randint(-10, 10)
        rst_sync.append(make_sync_row(i + 1, epoch_id, t1, offset, drift, df, dr))

    for j in range(15):
        sec = 32 + j
        ev_t = BASE_EPOCH_NS + sec * 1_000_000_000 + 200_000
        offset = int(round(-500_000_000 + 120.0 * (sec - 25)))
        rst_pkts.append(make_packet_row(j + 1, j + 1, "feed-a", "SYNTH-EQ2", ev_t, 20, offset, 1_300))

    write_sync_csv(fixtures_dir / "clock_restart_sync.csv", rst_sync)
    write_packets_csv(fixtures_dir / "clock_restart_packets.csv", rst_pkts)
    write_cfg(
        fixtures_dir / "clock_restart.cfg",
        {
            "scenario_name": "clock_restart",
            "seed": 20261127,
            "sync_fixture_path": "clock_restart_sync.csv",
            "packet_fixture_path": "clock_restart_packets.csv",
            "window_size": 24,
            "max_skew_ns": 5000,
            "reorder_window_ns": 2000,
            "max_allowed_offset_error_ns": 150,
            "max_allowed_drift_error_ppb": 30.0,
        },
    )

    # 5) out_of_order_stream: packets arrive out of event-time order within bounded reorder window
    ooo_sync = ref_sync[:30]
    ooo_pkts = []
    base_ev = BASE_EPOCH_NS + 15 * 1_000_000_000
    # Arrival order swaps pairs (events are spaced by 500 ns, reorder window = 2,000 ns, event_unc = 20 ns -> disjoint intervals!)
    arrival_order = [0, 2, 1, 3, 5, 4, 6, 8, 7, 9, 11, 10, 12, 13, 14, 15]
    for arrival_idx, logical_idx in enumerate(arrival_order):
        ev_t = base_ev + logical_idx * 500
        offset = int(round(base_offset + drift_ppb * 15))
        src = "feed-a" if (logical_idx % 2 == 0) else "feed-b"
        ooo_pkts.append(
            make_packet_row(
                packet_id=100 + logical_idx,
                seq_id=logical_idx + 1,
                source_id=src,
                symbol="SYNTH-EQ1",
                event_ns=ev_t,
                event_unc_ns=20,
                true_offset_ns=offset,
                transit_ns=1_200 + (arrival_idx % 3) * 100,
            )
        )

    write_sync_csv(fixtures_dir / "out_of_order_stream_sync.csv", ooo_sync)
    write_packets_csv(fixtures_dir / "out_of_order_stream_packets.csv", ooo_pkts)
    write_cfg(
        fixtures_dir / "out_of_order_stream.cfg",
        {
            "scenario_name": "out_of_order_stream",
            "seed": 20261127,
            "sync_fixture_path": "out_of_order_stream_sync.csv",
            "packet_fixture_path": "out_of_order_stream_packets.csv",
            "window_size": 24,
            "max_skew_ns": 5000,
            "reorder_window_ns": 2000,
            "fail_closed_on_ambiguity": "true",
        },
    )

    # 6) skew_violation_fail_closed: packet #6 has injected -25,000 ns causal skew violation -> halts fail-closed
    skew_sync = ref_sync[:30]
    skew_pkts = []
    for j in range(10):
        ev_t = BASE_EPOCH_NS + 15 * 1_000_000_000 + j * 10_000
        offset = int(round(base_offset + drift_ppb * 15))
        transit = -25_000 if j == 5 else 1_200  # Receive time precedes event time by 25 us -> causal violation!
        skew_pkts.append(
            make_packet_row(j + 1, j + 1, "feed-a", "SYNTH-EQ1", ev_t, 20, offset, transit)
        )

    write_sync_csv(fixtures_dir / "skew_violation_fail_closed_sync.csv", skew_sync)
    write_packets_csv(fixtures_dir / "skew_violation_fail_closed_packets.csv", skew_pkts)
    write_cfg(
        fixtures_dir / "skew_violation_fail_closed.cfg",
        {
            "scenario_name": "skew_violation_fail_closed",
            "seed": 20261127,
            "sync_fixture_path": "skew_violation_fail_closed_sync.csv",
            "packet_fixture_path": "skew_violation_fail_closed_packets.csv",
            "window_size": 24,
            "max_skew_ns": 5000,
            "reorder_window_ns": 2000,
            "max_causal_slack_ns": 200,
        },
    )

    # 7) Invalid & boundary fixtures for contract validation tests
    write_sync_csv(
        fixtures_dir / "invalid_negative_rtt_sync.csv",
        [
            # t4 - t1 = 100, t3 - t2 = 500 -> negative RTT (-400 ns)
            (1, 0, "TAI", "MONOTONIC_RAW", "gm-0", "loc-0", "SYNTHETIC_FIXTURE", "SYNTHETIC_FIXTURE",
             BASE_EPOCH_NS, BASE_EPOCH_NS + 1000, BASE_EPOCH_NS + 1500, BASE_EPOCH_NS + 100, "", "", "")
        ],
    )
    write_packets_csv(
        fixtures_dir / "invalid_mixed_domain_packets.csv",
        [
            make_packet_row(1, 1, "feed-a", "SYNTH-EQ1", BASE_EPOCH_NS + 10_000, 20, 0, 1000, ev_dom="TAI"),
            make_packet_row(2, 2, "feed-b", "SYNTH-EQ1", BASE_EPOCH_NS + 20_000, 20, 0, 1000, ev_dom="UTC"),
        ],
    )
    write_packets_csv(
        fixtures_dir / "boundary_zero_uncertainty_packets.csv",
        [
            make_packet_row(1, 1, "feed-a", "SYNTH-EQ1", BASE_EPOCH_NS + 10_000, 0, 0, 0, rx_unc_ns=0),
        ],
    )
    (fixtures_dir / "invalid_schema.cfg").write_text(
        "schema_version=ptsp.v999\nscenario_name=bad\n", encoding="utf-8"
    )
    (fixtures_dir / "invalid_unknown_key.cfg").write_text(
        f"schema_version={SCHEMA_VERSION}\nnot_a_real_key=123\n", encoding="utf-8"
    )

    print(f"Generated deterministic fixtures in {fixtures_dir}")


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    generate_all(root / "fixtures")
