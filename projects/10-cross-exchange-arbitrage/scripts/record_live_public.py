#!/usr/bin/env python3
"""
Public Market-Data Recorder (read-only, zero credentials).

Records live public REST snapshots / WebSocket order-book updates from Binance,
Coinbase, and OKX into xarb's JSONL envelope format so they can be replayed
deterministically offline via `xarb replay --feed <file.jsonl>`.

Note: Requires outbound internet access and `websockets` (`pip install websockets`)
when run interactively; never touches authenticated or order-entry endpoints.
"""
import argparse
import asyncio
import json
import time
import urllib.request


def make_envelope(venue: str, inst: int, kind: str, payload: str) -> str:
    return json.dumps({
        "recv_ns": time.time_ns(),
        "venue": venue,
        "inst": inst,
        "kind": kind,
        "payload": payload,
    }, separators=(",", ":"))


def fetch_binance_rest_snapshot(symbol: str = "BTCUSDT", limit: int = 100) -> str:
    url = f"https://api.binance.com/api/v3/depth?symbol={symbol}&limit={limit}"
    with urllib.request.urlopen(url, timeout=5) as resp:
        return resp.read().decode("utf-8")


async def record_live(out_path: str, duration_sec: int):
    try:
        import websockets  # type: ignore
    except ImportError:
        raise SystemExit("Install websockets first: pip install websockets")

    end_time = time.monotonic() + duration_sec
    lock = asyncio.Lock()

    with open(out_path, "w", encoding="utf-8") as fh:
        async def write_line(line: str):
            async with lock:
                fh.write(line + "\n")
                fh.flush()

        async def binance_task():
            url = "wss://stream.binance.com:9443/ws/btcusdt@depth@100ms"
            async with websockets.connect(url) as ws:
                await write_line(make_envelope("binance", 0, "reconnect", "live_start"))
                snap_fetched = False
                while time.monotonic() < end_time:
                    msg = await asyncio.wait_for(ws.recv(), timeout=5)
                    await write_line(make_envelope("binance", 0, "ws", msg))
                    if not snap_fetched:
                        snap = await asyncio.to_thread(fetch_binance_rest_snapshot, "BTCUSDT", 100)
                        await write_line(make_envelope("binance", 0, "rest_snapshot", snap))
                        snap_fetched = True

        async def okx_task():
            url = "wss://ws.okx.com:8443/ws/v5/public"
            async with websockets.connect(url) as ws:
                await ws.send(json.dumps({"op": "subscribe", "args": [{"channel": "books", "instId": "BTC-USDT"}]}))
                while time.monotonic() < end_time:
                    msg = await asyncio.wait_for(ws.recv(), timeout=5)
                    if '"action"' in msg:
                        await write_line(make_envelope("okx", 0, "ws", msg))

        await asyncio.gather(binance_task(), okx_task())


def main():
    p = argparse.ArgumentParser(description="Record public crypto L2 feeds to xarb JSONL")
    p.add_argument("--out", required=True)
    p.add_argument("--duration-sec", type=int, default=30)
    args = p.parse_args()
    asyncio.run(record_live(args.out, args.duration_sec))


if __name__ == "__main__":
    main()
