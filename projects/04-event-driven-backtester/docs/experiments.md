# Experiments

Strategy metrics under different latency, fee and queue-position assumptions. All input is
**synthetic, uninformed order flow** (see [assumptions.md](assumptions.md)): the point is to show
the simulator reacts to its assumptions in a defensible direction, not to evaluate the strategies.

```bash
python scripts/run_experiments.py --sessions 5 --events 400000
```

Five sessions (generator seeds 1–5), 400,000 L3 events each, about 40 simulated seconds per
session, one instrument, tick value 1. Raw per-session rows are in `results/experiments/*.csv`;
tables below are means over the five sessions unless a range is shown. Defaults: maker −0.1 bp,
taker 0.3 bp, market maker half-spread 2 ticks / 10 lots / ±50 lots, imbalance threshold 0.6.

## 1. Latency

The same delay is applied to market data, order entry and responses.

**Market maker**

| Latency per path | Net P&L (min … max over sessions) | Fills | Fill ratio | Cancels too late | Maker markout @100 ms |
|---|---|---|---|---|---|
| 0 | 102,186 (99,238 … 104,951) | 11,511 | 0.152 | 8,793 | +0.97 ticks |
| 10 µs | 100,715 (97,065 … 103,437) | 11,279 | 0.151 | 8,703 | +0.98 |
| 100 µs | 78,538 (74,018 … 80,937) | 8,698 | 0.141 | 7,255 | +1.00 |
| 1 ms | 22,870 (17,486 … 24,899) | 3,414 | 0.139 | 3,842 | +0.90 |
| 10 ms | −4,420 (−8,854 … −381) | 387 | 0.105 | 1,045 | −0.17 |
| 100 ms | 4,814 (−148 … 9,998) | 12 | 0.032 | 152 | +1.25 |

P&L falls monotonically from 0 to 10 ms and changes sign: a slower quoter requotes less often,
earns the spread on fewer fills, and at 10 ms its markout turns negative — the fills it still gets
are disproportionately the ones where the market had already moved (stale quotes). At 100 ms the
strategy barely trades (12 fills per session) and the result is noise, as the range shows.

"Cancels too late" is the cancel/fill race in numbers: even at zero latency the order may be filled
by an event carrying the same timestamp as the one that triggered the cancel.

**Imbalance taker**

| Latency per path | Net P&L (min … max) | Fills | Fill ratio | Taker markout @100 ms |
|---|---|---|---|---|
| 0 | −38,540 (−40,898 … −35,007) | 10,251 | 0.720 | −0.60 ticks |
| 100 µs | −34,143 (−36,245 … −30,292) | 9,916 | 0.704 | −0.53 |
| 1 ms | −15,249 (−17,923 … −12,269) | 5,591 | 0.594 | −0.37 |
| 10 ms | −4,465 (−8,035 … −1,461) | 810 | 0.508 | −0.98 |
| 100 ms | −814 (−4,165 … 3,551) | 81 | 0.473 | −2.32 |

It loses at every latency, as it must: top-of-book imbalance carries no information in flow where
trade direction is an independent coin flip, so the strategy pays spread and fees for nothing.
"Less negative with more latency" is purely fewer trades. The fill ratio drops with latency
because the quote an IOC order was aimed at is more often gone when it arrives.

## 2. Fees

At 100 µs latency. Neither baseline looks at fees, so behaviour (and gross P&L) is identical and
only the fee line moves — a check that fees are wired into accounting and nothing else.

| Maker / taker (bp) | Market maker net (gross 71,165) | Imbalance net (gross −20,739) |
|---|---|---|
| −0.2 / 0.1 | 85,911 | −25,207 |
| −0.1 / 0.3 | 78,538 | −34,143 |
| 0.0 / 0.5 | 71,165 | −43,078 |
| 0.2 / 1.0 | 56,418 | −65,418 |

## 3. Queue-position model

The same five sessions, once as exact L3 and once collapsed to L2 (`hftbt convert --to-l2`), where
the position of each cancellation in the queue has to be assumed. Market maker, 100 µs latency.

| Feed | Queue model | Fills | Lots filled | Net P&L |
|---|---|---|---|---|
| L3 | exact | 8,698 | 73,904 | 78,538 |
| L2 | conservative | 8,636 | 73,198 | 77,443 |
| L2 | proportional | 8,755 | 73,908 | 78,444 |
| L2 | optimistic | 8,828 | 74,700 | 79,913 |

The ordering is the one the models are named for, and the exact L3 answer falls between
conservative and optimistic. The spread is small here (about ±1% of lots filled) because this
market maker mostly quotes inside or at a thin touch, where there is little queue to be uncertain
about. A strategy that joins deep queues would see a far larger gap, and with real L2 data there is
no L3 column to tell you which model is closer.

## 4. Parameter sweep, in-sample vs out-of-sample

`hftbt_tools.sweep` runs every grid point on the first half of a session (in-sample) and again on
the second half (out-of-sample, books warmed up, strategy starting flat), ranks on in-sample P&L
only, and reports how the winner did out of sample. Grid: half-spread {1,2,3,4} × inventory skew
{0, 0.05, 0.2} × requote threshold {1,2} = 24 trials per session.

| Session | Best in-sample P&L | Same parameters out-of-sample | OOS rank of IS winner | Median OOS P&L over grid | Spearman(IS, OOS) |
|---|---|---|---|---|---|
| 1 | 50,449 | 53,337 | 1 / 24 | 27,658 | 0.954 |
| 2 | 48,752 | 51,011 | 1 / 24 | 25,009 | 0.981 |
| 3 | 52,210 | 50,197 | 1 / 24 | 24,276 | 0.898 |
| 4 | 52,020 | 52,845 | 1 / 24 | 28,102 | 0.982 |
| 5 | 53,625 | 51,065 | 1 / 24 | 26,590 | 0.992 |

In-sample rank predicts out-of-sample rank almost perfectly. That is what a **stationary generator**
produces — both halves are draws from the same process — and it is the only thing this table
establishes: the split, the warm-up and the ranking work. Real markets are not stationary; the same
tool on real sessions is expected to show a visibly lower rank correlation and an in-sample winner
that is not the out-of-sample winner. The safeguards the tooling enforces regardless of data:

- parameters are selected on in-sample data only, and both numbers are always printed side by side;
- the number of trials is printed next to the in-sample maximum, because the best of N trials is
  biased upward by construction;
- the out-of-sample half replays the whole file so its books are correct, but the strategy receives
  nothing before the split (`run.trade_start_ts`) and starts with no position;
- unknown configuration keys are an error, so a mistyped parameter cannot silently turn a sweep
  into N copies of the default.
