#!/usr/bin/env python3
"""Inject one deliberate bug at a time and check that the C++ test suite fails.

    python scripts/mutation_check.py            # needs an existing ./build

Each mutant is a (file, original text, replacement) triple. A mutant that
survives is either a gap in the tests or an equivalent mutant; the one known
equivalent is marked. Sources are restored (and rebuilt) afterwards.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ENG_H, ENG, EXCH = "include/hftbt/engine.hpp", "src/engine.cpp", "src/exchange.cpp"

MUTANTS = [
    ("market-data delay ignored", ENG,
     "const Timestamp at = md_path_.deliver_at(e.ts);", "const Timestamp at = e.ts;"),
    ("order-path delay ignored", ENG,
     "push(order_path_.deliver_at(now_ + cfg_.latency.compute_delay_ns), kPrioOrderEntry, req);",
     "push(now_, kPrioOrderEntry, req);"),
    ("response delay ignored", ENG,
     "push(response_path_.deliver_at(now_), kPrioResponse, r);", "push(now_, kPrioResponse, r);"),
    ("strategy sees the truth book", ENG_H,
     "const OrderBook& book(InstrumentId i) const override { return observed_[i]; }",
     "const OrderBook& book(InstrumentId i) const override { return exchange_.book(i); }"),
    ("orders matched before same-timestamp feed events", ENG_H,
     "        kPrioFeed = 0,\n        kPrioOrderEntry = 1,",
     "        kPrioFeed = 1,\n        kPrioOrderEntry = 0,"),
    ("jitter may reorder a path", "include/hftbt/latency.hpp",
     "        last_ = std::max(last_, t);\n        return last_;", "        return t;"),
    ("L3 queue priority ignored", EXCH,
     "if (seq < o.join_seq) { // the executed order was ahead of us", "if (false) {"),
    ("L2 queue ahead not consumed", EXCH, "eligible = qty - o.ahead;", "eligible = qty;"),
    ("one trade fills every eligible order in full", EXCH, "own_filled += fill;", ""),
    ("proportional model behaves as optimistic", EXCH,
     "if (base > 0) o.ahead -= cancelled * std::min(o.ahead, base) / base;",
     "o.ahead -= cancelled;"),
    ("crossing-quote flag ignored", EXCH,
     "        if (cfg_.fill_on_crossing_quote)\n            match_passive(st, e.instrument, opposite(r.side), r.price, r.qty, 0,",
     "        if (true)\n            match_passive(st, e.instrument, opposite(r.side), r.price, r.qty, 0,"),
    ("post-only not enforced", EXCH,
     "if (req.post_only && opp.best() && crosses(opp.best()->price)) {", "if (false) {"),
    ("taker slippage ignored", EXCH,
     "r.price = std::max<Price>(1, lvl.price + sign(req.side) * cfg_.taker_slippage_ticks);",
     "r.price = lvl.price;"),
    ("displayed liquidity can be taken twice", EXCH, "lvl.consumed += take;", ""),
    ("cancel leaves the order resting", EXCH,
     "    orders.erase(o);\n    live_.erase(it);", "    live_.erase(it);"),
    ("fee sign flipped", "src/portfolio.cpp",
     "a.cash -= static_cast<double>(signed_qty) * px * tick_value_ + fee;",
     "a.cash -= static_cast<double>(signed_qty) * px * tick_value_ - fee;"),
    ("position limit not checked", "src/oms.cpp",
     "if (worst > risk_.max_position) return RejectReason::RiskMaxPosition;", ""),
    ("markout sign flipped", ENG,
     "const double m = sign(mo->side) * (*mid - static_cast<double>(mo->price));",
     "const double m = sign(mo->side) * (static_cast<double>(mo->price) - *mid);"),
    # Equivalent: neither the strategy nor the exchange can observe which of
    # these two happens first within one nanosecond.
    ("EQUIVALENT: observed market data before order arrival at a tie", ENG_H,
     "        kPrioOrderEntry = 1,\n        kPrioMdObserved = 2,",
     "        kPrioOrderEntry = 2,\n        kPrioMdObserved = 1,"),
]


def build():
    return subprocess.run(["cmake", "--build", "build", "--target", "hftbt_tests"],
                          cwd=ROOT, capture_output=True, text=True).returncode == 0


def main():
    survived = 0
    for name, rel, old, new in MUTANTS:
        path = os.path.join(ROOT, rel)
        with open(path) as f:
            src = f.read()
        if src.count(old) != 1:
            print("STALE     %s (pattern not found exactly once in %s)" % (name, rel))
            survived += 1
            continue
        try:
            with open(path, "w") as f:
                f.write(src.replace(old, new))
            if not build():
                print("NO BUILD  %s" % name)
                continue
            try:
                res = subprocess.run([os.path.join(ROOT, "build", "hftbt_tests"),
                                      "--gtest_brief=1"], capture_output=True, text=True,
                                     timeout=300)
                failed = sorted({l.split("]")[1].split("(")[0].strip()
                                 for l in res.stdout.splitlines()
                                 if l.startswith("[  FAILED  ]") and "." in l})
                caught = res.returncode != 0
            except subprocess.TimeoutExpired:
                failed, caught = ["(timeout)"], True
            if caught:
                print("caught    %s  <- %s%s" % (name, failed[0] if failed else "test binary aborted",
                                                 " +%d more" % (len(failed) - 1) if len(failed) > 1 else ""))
            else:
                print("SURVIVED  %s" % name)
                survived += not name.startswith("EQUIVALENT")
        finally:
            with open(path, "w") as f:  # rewrite (not move) so the build sees a new mtime
                f.write(src)
    build()
    print("\n%d mutants, %d unexpected survivors" % (len(MUTANTS), survived))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
