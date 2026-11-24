#!/usr/bin/env python3
"""Independent reference implementation of the ITCH 5.0 replay pipeline.

This is the "second parser" used to cross-check the C++ implementation. It
shares no code with it: message layouts are restated here as struct format
strings, and the book is a plain dict-of-dicts. It is slow and simple on
purpose. Given the same input file it must produce byte-identical CSVs to
`itch_replay` for:

    --bbo-csv       best bid/offer, one row per change
    --trades-csv    one row per E/C/P/Q trade report
    --snapshot-csv  final top-N levels per symbol
    --counts-csv    decoded message counts by type

Policies mirrored from the C++ implementation (docs/design.md):
  * payload length must equal the specification length, else the message is
    rejected; unknown type bytes are rejected; side must be 'B' or 'S'
  * duplicate order reference on add -> rejected
  * unknown order reference -> ignored
  * execute/cancel beyond remaining shares -> clamped
  * replace removes the original even if the new reference is a duplicate
"""
import argparse
import struct
import sys

# Body layouts after the 11-byte common header
# (type:1, stock locate:2, tracking number:2, timestamp:6).
BODY = {
    b"S": ">c",
    b"R": ">8sccIcc2scccccIc",
    b"H": ">8scc4s",
    b"Y": ">8sc",
    b"L": ">4s8sccc",
    b"V": ">QQQ",
    b"W": ">c",
    b"K": ">8sIcI",
    b"J": ">8sIIII",
    b"h": ">8scc",
    b"A": ">QcI8sI",
    b"F": ">QcI8sI4s",
    b"E": ">QIQ",
    b"C": ">QIQcI",
    b"X": ">QI",
    b"D": ">Q",
    b"U": ">QQII",
    b"P": ">QcI8sIQ",
    b"Q": ">Q8sIQc",
    b"B": ">Q",
    b"I": ">QQc8sIIIcc",
    b"N": ">8sc",
    b"O": ">8scIIIQII",
}
HEADER_LEN = 11
LENGTH = {t: HEADER_LEN + struct.calcsize(fmt) for t, fmt in BODY.items()}
STRUCT = {t: struct.Struct(fmt) for t, fmt in BODY.items()}


def price(p):
    return "%d.%04d" % (p // 10000, p % 10000)


class Book:
    """All symbols. levels[(locate, side)] maps price -> [qty, order_count]."""

    def __init__(self):
        self.orders = {}  # ref -> [locate, side, price, qty]
        self.levels = {}
        self.symbols = {}

    def add(self, ref, locate, side, qty, px):
        if ref in self.orders:
            return False
        self.orders[ref] = [locate, side, px, qty]
        lv = self.levels.setdefault((locate, side), {}).setdefault(px, [0, 0])
        lv[0] += qty
        lv[1] += 1
        return True

    def _drop(self, ref):
        locate, side, px, qty = self.orders.pop(ref)
        side_levels = self.levels[(locate, side)]
        lv = side_levels[px]
        lv[0] -= qty
        lv[1] -= 1
        if lv[1] == 0:
            del side_levels[px]
        return locate, side, px, qty

    def reduce(self, ref, qty):
        """Returns (locate, side, price, applied_qty) or None if unknown."""
        o = self.orders.get(ref)
        if o is None:
            return None
        locate, side, px, have = o
        applied = min(qty, have)
        if applied == have:
            self._drop(ref)
        else:
            o[3] -= applied
            self.levels[(locate, side)][px][0] -= applied
        return locate, side, px, applied

    def remove(self, ref):
        if ref not in self.orders:
            return None
        return self._drop(ref)

    def replace(self, old, new, qty, px):
        """Returns the locate whose book changed, or None if `old` is unknown."""
        gone = self.remove(old)
        if gone is None:
            return None
        locate, side = gone[0], gone[1]
        self.add(new, locate, side, qty, px)
        return locate

    def sorted_levels(self, locate, side):
        lv = self.levels.get((locate, side), {})
        return sorted(lv.items(), reverse=(side == b"B"))

    def bbo(self, locate):
        bids = self.levels.get((locate, b"B"))
        asks = self.levels.get((locate, b"S"))
        bid = max(bids) if bids else None
        ask = min(asks) if asks else None
        return (
            bid,
            bids[bid][0] if bid is not None else 0,
            ask,
            asks[ask][0] if ask is not None else 0,
        )


def replay(data, depth, bbo_out, trades_out, snapshot_out, counts_out):
    book = Book()
    counts = {}
    last_bbo = {}
    empty_bbo = (None, 0, None, 0)
    pos = 0
    n = len(data)

    def symbol(locate):
        return book.symbols.get(locate, "")

    def emit_trade(ts, locate, kind, px, qty, match, aggressor, printable):
        if trades_out:
            trades_out.write(
                "%d,%d,%s,%s,%s,%d,%d,%s,%s\n"
                % (ts, locate, symbol(locate), kind, price(px), qty, match, aggressor, printable)
            )

    if bbo_out:
        bbo_out.write("timestamp,locate,symbol,bid_px,bid_qty,ask_px,ask_qty\n")
    if trades_out:
        trades_out.write("timestamp,locate,symbol,kind,price,qty,match,aggressor,printable\n")

    while n - pos >= 2:
        (length,) = struct.unpack_from(">H", data, pos)
        if length == 0:
            pos += 2
            continue
        if n - pos - 2 < length:
            break  # truncated payload: stop, as the C++ reader does
        start = pos + 2
        pos = start + length
        mtype = data[start : start + 1]
        if LENGTH.get(mtype) != length:
            continue  # unknown type, truncated or oversized
        locate, _tracking, ts_hi, ts_lo = struct.unpack_from(">HHHI", data, start + 1)
        ts = (ts_hi << 32) | ts_lo
        f = STRUCT[mtype].unpack_from(data, start + HEADER_LEN)
        if mtype in (b"A", b"F", b"P") and f[1] not in (b"B", b"S"):
            continue  # invalid side
        counts[mtype] = counts.get(mtype, 0) + 1

        touched = None
        if mtype == b"R":
            book.symbols[locate] = f[0].decode("latin-1").rstrip(" ")
        elif mtype in (b"A", b"F"):
            ref, side, shares, stock, px = f[0], f[1], f[2], f[3], f[4]
            if book.add(ref, locate, side, shares, px):
                touched = locate
                book.symbols.setdefault(locate, stock.decode("latin-1").rstrip(" "))
        elif mtype == b"E":
            r = book.reduce(f[0], f[1])
            if r is not None:
                touched = r[0]
                emit_trade(ts, r[0], "E", r[2], r[3], f[2], "S" if r[1] == b"B" else "B", "Y")
        elif mtype == b"C":
            r = book.reduce(f[0], f[1])
            if r is not None:
                touched = r[0]
                printable = "N" if f[3] == b"N" else "Y"
                emit_trade(ts, r[0], "C", f[4], r[3], f[2], "S" if r[1] == b"B" else "B", printable)
        elif mtype == b"X":
            r = book.reduce(f[0], f[1])
            if r is not None:
                touched = r[0]
        elif mtype == b"D":
            r = book.remove(f[0])
            if r is not None:
                touched = r[0]
        elif mtype == b"U":
            touched = book.replace(f[0], f[1], f[2], f[3])
        elif mtype == b"P":
            emit_trade(ts, locate, "P", f[4], f[2], f[5], "", "Y")
        elif mtype == b"Q":
            emit_trade(ts, locate, "Q", f[2], f[0], f[3], "", "Y")

        if touched is not None:
            now = book.bbo(touched)
            if now != last_bbo.get(touched, empty_bbo):
                last_bbo[touched] = now
                if bbo_out:
                    bid, bid_qty, ask, ask_qty = now
                    bbo_out.write(
                        "%d,%d,%s,%s,%d,%s,%d\n"
                        % (
                            ts,
                            touched,
                            symbol(touched),
                            price(bid) if bid is not None else "",
                            bid_qty,
                            price(ask) if ask is not None else "",
                            ask_qty,
                        )
                    )

    if snapshot_out:
        snapshot_out.write("locate,symbol,side,level,price,qty,orders\n")
        for locate in sorted({k[0] for k in book.levels}):
            for side in (b"B", b"S"):
                for i, (px, (qty, orders)) in enumerate(book.sorted_levels(locate, side)[:depth]):
                    snapshot_out.write(
                        "%d,%s,%s,%d,%s,%d,%d\n"
                        % (locate, symbol(locate), side.decode(), i, price(px), qty, orders)
                    )

    if counts_out:
        counts_out.write("type,count\n")
        for mtype in sorted(counts):
            counts_out.write("%s,%d\n" % (mtype.decode("latin-1"), counts[mtype]))

    return counts, book


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--bbo-csv")
    ap.add_argument("--trades-csv")
    ap.add_argument("--snapshot-csv")
    ap.add_argument("--counts-csv")
    ap.add_argument("--depth", type=int, default=5)
    args = ap.parse_args()

    with open(args.file, "rb") as fh:
        data = fh.read()

    outs = [
        open(p, "w", newline="\n", encoding="latin-1") if p else None
        for p in (args.bbo_csv, args.trades_csv, args.snapshot_csv, args.counts_csv)
    ]
    try:
        counts, book = replay(data, args.depth, *outs)
    finally:
        for o in outs:
            if o:
                o.close()
    print(
        "reference: %d messages decoded, %d live orders"
        % (sum(counts.values()), len(book.orders)),
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
