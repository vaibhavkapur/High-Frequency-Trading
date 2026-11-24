# Explicit Assumptions, Limitations, and Bias Directions

This system is an educational paper-execution and market-data engineering testbed. Every simulation makes modeling assumptions; below is an explicit inventory of our simplifications and the direction of bias each introduces.

| Assumption / Simplification | What Is Modeled | Direction of Bias vs. Live Production |
|---|---|---|
| **Synthetic market-data generator** | Realistic wire schemas (Binance `U/u`, Coinbase `sequence`, OKX `seqId/prevSeqId` + CRC32) with Brownian mid-price, fleeting (5–15 ms) and persistent (50–110 ms) dislocations, fee-trapped crosses, and injected faults. | **Optimistic** on dislocation frequency & size compared to mature BTC/USDT liquidity; real cross-exchange dislocations are rarer and heavily contested. |
| **No market impact or queue contention on paper fills** | Paper legs walk displayed truth-book levels at `t_arrival` without permanently removing liquidity from future recorded book snapshots. | **Optimistic** if multiple consecutive signals hit the same persistent level before cooldown expires (partially mitigated by per-route cooldown and arrival-time truth replay). |
| **Static taker fee tiers** | Fixed per-venue taker fees (Binance 10 bps, Coinbase 12 bps, OKX 8 bps). | **Neutral / Conservative** for high-volume VIP tiers, **Optimistic** for retail tiers with higher spot fees. |
| **Pre-funded inventory, optional static rebalance delay** | Pre-funded quote/base balances per venue; optional transfer delay (e.g. 600 s) + withdrawal fee. No borrow/margin funding rates or short-selling costs. | **Optimistic** regarding capital efficiency / cost of holding inventory (directional BTC inventory exposure is marked-to-market, not hedged via perpetuals). |
| **No hidden / iceberg liquidity** | Only displayed L2 depth is considered executable. | **Conservative** on fill size when hidden liquidity sits inside the spread. |
| **Single-host clock in replay** | Exchange timestamps are tracked, while staleness is evaluated against monotonic local receive timestamps. | **Neutral** in replay; live deployments must additionally monitor NTP/PTP drift and exchange-to-local clock skew. |
| **Zero live order entry / credentials** | Strictly paper-only; no API keys, signing, rate-limit token buckets, or authenticated WebSocket/REST endpoints. | **Intentional safety boundary** — prevents accidental live trading and isolates the engineering focus to market-data normalization, risk modeling, and latency sensitivity. |
| **Unhedged one-legged MTM valuation** | When a Limit-IOC leg misses, the residual base inventory is valued at current cross-venue mid-price at trade completion rather than immediately crossing the spread to flatten. | **Optimistic** on one-legged loss magnitude: actively flattening a one-legged fill would incur an additional half-spread + taker fee + adverse selection cost. |

## Non-Goals

- Never interpret paper P&L on synthetic feeds as a live profitability estimate.
- Cross-exchange arbitrage is **never risk-free**: latency-induced one-legged fills, inventory drift, exchange outages, withdrawal halts, and counterparty/settlement risk are fundamental risks demonstrated directly by the latency sweeps in [results_report.md](results_report.md).
