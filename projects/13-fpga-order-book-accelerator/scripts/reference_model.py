#!/usr/bin/env python3
"""Independent Python software reference model for FPGA Order Book Accelerator.

Implements the exact fpga-ob.v1 contract from docs/contracts.md using plain
Python dictionaries with zero shared code with RTL or C++20 implementations.
Can verify JSON fixtures, binary streams, or be imported by cocotb tests.
"""

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

SCHEMA_VERSION = 1
WIRE_STRUCT = struct.Struct("<IIIIBBBB")  # 20-byte fixed-width frame

OP_MAP = {"NOP": 0, "ADD": 1, "CANCEL": 2, "EXECUTE": 3}
SIDE_MAP = {"BID": 0, "ASK": 1}

STATUS_OK = "STATUS_OK"
ERR_UNSUPPORTED_SCHEMA = "ERR_UNSUPPORTED_SCHEMA"
ERR_UNSUPPORTED_FLAGS = "ERR_UNSUPPORTED_FLAGS"
ERR_UNSUPPORTED_OPCODE = "ERR_UNSUPPORTED_OPCODE"
ERR_INVALID_SIDE = "ERR_INVALID_SIDE"
ERR_PRICE_OOB = "ERR_PRICE_OOB"
ERR_ZERO_QTY = "ERR_ZERO_QTY"
ERR_FULL_CAPACITY = "ERR_FULL_CAPACITY"
ERR_OVERFLOW_QTY = "ERR_OVERFLOW_QTY"
ERR_OVERFLOW_ORDERS = "ERR_OVERFLOW_ORDERS"
ERR_INVALID_CANCEL = "ERR_INVALID_CANCEL"
ERR_INVALID_EXECUTE = "ERR_INVALID_EXECUTE"


@dataclass
class Event:
    seq_no: int
    timestamp_ns: int
    price: int
    qty: int
    opcode: int
    side: int
    flags: int = 0
    schema_ver: int = SCHEMA_VERSION

    def pack_wire(self) -> bytes:
        return WIRE_STRUCT.pack(
            self.seq_no,
            self.timestamp_ns,
            self.price,
            self.qty,
            self.opcode,
            self.side,
            self.flags,
            self.schema_ver,
        )

    def pack_sv_int(self) -> int:
        # Matches order_book_pkg::event_pkt_t 160-bit MSB-to-LSB layout
        return (
            ((self.seq_no & 0xFFFFFFFF) << 128)
            | ((self.timestamp_ns & 0xFFFFFFFF) << 96)
            | ((self.price & 0xFFFFFFFF) << 64)
            | ((self.qty & 0xFFFFFFFF) << 32)
            | ((self.opcode & 0xFF) << 24)
            | ((self.side & 0xFF) << 16)
            | ((self.flags & 0xFF) << 8)
            | (self.schema_ver & 0xFF)
        )


class PythonReferenceBook:
    def __init__(self, price_base: int = 10000, num_levels: int = 64, max_active_levels: int = 32):
        self.price_base = price_base
        self.num_levels = num_levels
        self.max_active_levels = max_active_levels
        self.bids: Dict[int, List[int]] = {}  # price -> [qty, orders]
        self.asks: Dict[int, List[int]] = {}

    def reset(self) -> None:
        self.bids.clear()
        self.asks.clear()

    def _apply_one(self, ev: Event) -> str:
        if ev.schema_ver != SCHEMA_VERSION:
            return ERR_UNSUPPORTED_SCHEMA
        if ev.flags != 0:
            return ERR_UNSUPPORTED_FLAGS
        if ev.opcode not in (0, 1, 2, 3):
            return ERR_UNSUPPORTED_OPCODE
        if ev.opcode == 0:
            return STATUS_OK
        if ev.side not in (0, 1):
            return ERR_INVALID_SIDE
        if ev.price < self.price_base or ev.price >= self.price_base + self.num_levels:
            return ERR_PRICE_OOB
        if ev.qty == 0:
            return ERR_ZERO_QTY

        book = self.bids if ev.side == 0 else self.asks
        if ev.opcode == 1:  # ADD
            if ev.price not in book:
                if len(book) >= self.max_active_levels:
                    return ERR_FULL_CAPACITY
                book[ev.price] = [ev.qty, 1]
                return STATUS_OK
            cur_q, cur_o = book[ev.price]
            if cur_q + ev.qty > 0xFFFFFFFF:
                return ERR_OVERFLOW_QTY
            if cur_o == 0xFFFF:
                return ERR_OVERFLOW_ORDERS
            book[ev.price] = [cur_q + ev.qty, cur_o + 1]
            return STATUS_OK
        elif ev.opcode == 2:  # CANCEL
            if ev.price not in book or ev.qty > book[ev.price][0]:
                return ERR_INVALID_CANCEL
            rem = book[ev.price][0] - ev.qty
            if rem == 0:
                del book[ev.price]
            else:
                book[ev.price][0] = rem
                book[ev.price][1] = max(1, book[ev.price][1] - 1)
            return STATUS_OK
        elif ev.opcode == 3:  # EXECUTE
            if ev.price not in book or ev.qty > book[ev.price][0]:
                return ERR_INVALID_EXECUTE
            rem = book[ev.price][0] - ev.qty
            if rem == 0:
                del book[ev.price]
            else:
                book[ev.price][0] = rem
            return STATUS_OK
        return ERR_UNSUPPORTED_OPCODE

    def _snapshot(
        self, seq0: int, seq1: int, lane1_valid: bool, st0: str, st1: str, ts_ns: int
    ) -> dict:
        if self.bids:
            bb_p = max(self.bids.keys())
            bb_q, bb_o = self.bids[bb_p]
            bb_v = True
        else:
            bb_v, bb_p, bb_q, bb_o = False, 0, 0, 0

        if self.asks:
            ba_p = min(self.asks.keys())
            ba_q, ba_o = self.asks[ba_p]
            ba_v = True
        else:
            ba_v, ba_p, ba_q, ba_o = False, 0, 0, 0

        spread = (ba_p - bb_p) if (bb_v and ba_v) else 0
        crossed = bool(bb_v and ba_v and bb_p >= ba_p)
        return {
            "schema_ver": SCHEMA_VERSION,
            "lane1_valid": lane1_valid,
            "status_0": st0,
            "status_1": st1,
            "seq_no_0": seq0,
            "seq_no_1": seq1 if lane1_valid else 0,
            "timestamp_ns": ts_ns,
            "best_bid_valid": bb_v,
            "best_bid_price": bb_p,
            "best_bid_qty": bb_q,
            "best_bid_orders": bb_o,
            "best_ask_valid": ba_v,
            "best_ask_price": ba_p,
            "best_ask_qty": ba_q,
            "best_ask_orders": ba_o,
            "spread": spread,
            "crossed": crossed,
            "active_bid_levels": len(self.bids),
            "active_ask_levels": len(self.asks),
        }

    def apply_single(self, ev: Event) -> dict:
        st0 = self._apply_one(ev)
        return self._snapshot(ev.seq_no, 0, False, st0, STATUS_OK, ev.timestamp_ns)

    def apply_pair(self, ev0: Event, ev1: Event) -> dict:
        st0 = self._apply_one(ev0)
        st1 = self._apply_one(ev1)
        return self._snapshot(ev0.seq_no, ev1.seq_no, True, st0, st1, ev1.timestamp_ns)


def load_fixture(path: str) -> Tuple[dict, List[Event]]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    cfg = data.get("config", {"price_base": 10000, "num_levels": 64, "max_active_levels": 32})
    events: List[Event] = []
    for raw in data["events"]:
        op = OP_MAP[raw["opcode"]] if isinstance(raw["opcode"], str) else int(raw["opcode"])
        side = SIDE_MAP[raw["side"]] if isinstance(raw["side"], str) else int(raw["side"])
        events.append(
            Event(
                seq_no=int(raw["seq_no"]),
                timestamp_ns=int(raw["timestamp_ns"]),
                price=int(raw["price"]),
                qty=int(raw["qty"]),
                opcode=op,
                side=side,
                flags=int(raw.get("flags", 0)),
                schema_ver=int(raw.get("schema_ver", SCHEMA_VERSION)),
            )
        )
    return cfg, events


def main() -> int:
    parser = argparse.ArgumentParser(description="Independent Python reference verifier")
    parser.add_argument("--fixture", required=True, help="Input events JSON fixture")
    parser.add_argument("--expected", help="Optional expected outputs JSON fixture")
    args = parser.parse_args()

    cfg, events = load_fixture(args.fixture)
    ref = PythonReferenceBook(
        price_base=cfg["price_base"],
        num_levels=cfg["num_levels"],
        max_active_levels=cfg["max_active_levels"],
    )
    outputs = [ref.apply_single(ev) for ev in events]

    if args.expected:
        with open(args.expected, "r", encoding="utf-8") as f:
            exp_list = json.load(f)["expected"]
        if len(exp_list) != len(outputs):
            print(f"FAIL: length mismatch expected={len(exp_list)} got={len(outputs)}", file=sys.stderr)
            return 1
        for idx, (exp, got) in enumerate(zip(exp_list, outputs)):
            for k, v in exp.items():
                if got.get(k) != v:
                    print(
                        f"FAIL at index {idx} field '{k}': expected={v} got={got.get(k)}",
                        file=sys.stderr,
                    )
                    return 1
        print(f"PASS: Python independent reference matched {len(outputs)} expected outputs.")
    else:
        print(json.dumps({"outputs": outputs}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
