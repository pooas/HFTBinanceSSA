#!/usr/bin/env python3
"""
replay_server.py

Protocol-compatible Binance mock server for local offline backtesting.

Reads tests/data/hft_tick_data_sample.csv and serves:
  - REST  GET /api/v3/klines          -> Binance kline history (used by python-macro warmup)
  - WS    /ws/btcusdt@aggTrade        -> live aggTrade stream  (used by java-app)
  - WS    /ws/btcusdt@kline_1m        -> live 1m kline stream  (used by python-macro)

All JSON shapes match the real Binance payloads so the existing Java/C++/Python
consumers require zero live-path code changes.
"""

import os
import asyncio
import csv
import json
import logging
from datetime import datetime, timezone
from aiohttp import web

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
CSV_PATH = os.environ.get("CSV_PATH", "/data/ticks.csv")
MOCK_PORT = int(os.environ.get("MOCK_PORT", "8080"))
REPLAY_SPEED = float(os.environ.get("REPLAY_SPEED", "0"))
REPLAY_LOOP = os.environ.get("REPLAY_LOOP", "false").lower() in ("1", "true", "yes")

# ---------------------------------------------------------------------------
# CSV ingestion
# ---------------------------------------------------------------------------
def load_ticks(path: str):
    """Load the CSV and return a list of ticks compatible with Binance aggTrade."""
    ticks = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ts_str = row["timestamp"].strip().strip('"')
            dt = datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S.%f")
            #ts_ms = int(dt.replace(tzinfo=datetime.timezone.utc).timestamp() * 1000)
            ts_ms = int(dt.replace(tzinfo=timezone.utc).timestamp() * 1000)
            ticks.append({
                "p": float(row["price"]),
                "q": float(row["volume"]),
                "T": ts_ms,
                "m": False,
            })
    log.info(f"Loaded {len(ticks)} ticks from {path}")
    return ticks


def build_candles(ticks, interval_ms: int = 60_000):
    """Aggregate ticks into Binance-style 1m candles."""
    buckets = {}
    for t in ticks:
        open_time = (t["T"] // interval_ms) * interval_ms
        close_time = open_time + interval_ms - 1
        if open_time not in buckets:
            buckets[open_time] = {
                "t": open_time,
                "T": close_time,
                "o": t["p"],
                "h": t["p"],
                "l": t["p"],
                "c": t["p"],
                "v": 0.0,
                "n": 0,
            }
        c = buckets[open_time]
        c["h"] = max(c["h"], t["p"])
        c["l"] = min(c["l"], t["p"])
        c["c"] = t["p"]
        c["v"] += t["q"]
        c["n"] += 1

    candles = sorted(buckets.values(), key=lambda x: x["t"])
    log.info(f"Aggregated {len(candles)} candles")
    return candles


TICKS = load_ticks(CSV_PATH)
CANDLES = build_candles(TICKS)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def sleep_for_elapsed(last_ts: int, current_ts: int):
    """Return a sleep duration honoring REPLAY_SPEED, or 0 for max speed."""
    if REPLAY_SPEED <= 0 or last_ts is None:
        return 0.0
    delta_s = (current_ts - last_ts) / 1000.0
    return max(0.0, delta_s / REPLAY_SPEED)


# ---------------------------------------------------------------------------
# REST: historical klines (used by python-macro warmup)
# ---------------------------------------------------------------------------
async def handle_klines(request: web.Request):
    """Binance /api/v3/klines compatible endpoint."""
    symbol = request.query.get("symbol", "BTCUSDT")
    interval = request.query.get("interval", "1m")
    limit = int(request.query.get("limit", "500"))
    end_time = request.query.get("endTime")

    end_ms = int(end_time) if end_time else None

    filtered = [c for c in CANDLES if (end_ms is None or c["t"] <= end_ms)]
    result = filtered[-limit:]

    # Binance kline array format:
    # [openTime, open, high, low, close, volume, closeTime, quoteVolume,
    #  numTrades, takerBuyBase, takerBuyQuote, ignore]
    out = []
    for c in result:
        out.append([
            c["t"],
            str(c["o"]),
            str(c["h"]),
            str(c["l"]),
            str(c["c"]),
            str(c["v"]),
            c["T"],
            "0.0",
            c["n"],
            "0.0",
            "0.0",
            "0",
        ])

    log.info(f"REST /api/v3/klines symbol={symbol} interval={interval} "
             f"limit={limit} endTime={end_ms} -> returning {len(out)} candles")
    return web.json_response(out)


# ---------------------------------------------------------------------------
# WS: aggTrade stream (used by java-app)
# ---------------------------------------------------------------------------
async def handle_aggtrade_ws(request: web.Request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    log.info("WS /ws/btcusdt@aggTrade client connected")

    try:
        while True:
            last_ts = None
            for tick in TICKS:
                msg = {
                    "p": str(tick["p"]),
                    "q": str(tick["q"]),
                    "T": tick["T"],
                    "m": tick["m"],
                }
                await ws.send_str(json.dumps(msg))

                delay = sleep_for_elapsed(last_ts, tick["T"])
                if delay > 0:
                    await asyncio.sleep(delay)
                last_ts = tick["T"]

            if not REPLAY_LOOP:
                log.info("WS /ws/btcusdt@aggTrade replay finished; idling")
                while not ws.closed:
                    await asyncio.sleep(60)
                break
            log.info("WS /ws/btcusdt@aggTrade looping replay")
    except asyncio.CancelledError:
        pass
    except Exception as e:
        log.warning(f"WS /ws/btcusdt@aggTrade error: {e}")
    finally:
        await ws.close()
    return ws


# ---------------------------------------------------------------------------
# WS: kline stream (used by python-macro)
# ---------------------------------------------------------------------------
async def handle_kline_ws(request: web.Request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    log.info("WS /ws/btcusdt@kline_1m client connected")

    try:
        while True:
            last_ts = None
            for c in CANDLES:
                msg = {
                    "e": "kline",
                    "E": c["T"],
                    "s": "BTCUSDT",
                    "k": {
                        "t": c["t"],
                        "T": c["T"],
                        "s": "BTCUSDT",
                        "i": "1m",
                        "f": 0,
                        "L": 0,
                        "o": str(c["o"]),
                        "c": str(c["c"]),
                        "h": str(c["h"]),
                        "l": str(c["l"]),
                        "v": str(c["v"]),
                        "n": c["n"],
                        "x": True,
                        "q": "0.0",
                        "V": "0.0",
                        "Q": "0.0",
                    },
                }
                await ws.send_str(json.dumps(msg))

                delay = sleep_for_elapsed(last_ts, c["t"])
                if delay > 0:
                    await asyncio.sleep(delay)
                last_ts = c["t"]

            if not REPLAY_LOOP:
                log.info("WS /ws/btcusdt@kline_1m replay finished; idling")
                while not ws.closed:
                    await asyncio.sleep(60)
                break
            log.info("WS /ws/btcusdt@kline_1m looping replay")
    except asyncio.CancelledError:
        pass
    except Exception as e:
        log.warning(f"WS /ws/btcusdt@kline_1m error: {e}")
    finally:
        await ws.close()
    return ws


# ---------------------------------------------------------------------------
# Application bootstrap
# ---------------------------------------------------------------------------
def main():
    app = web.Application()
    app.router.add_get("/api/v3/klines", handle_klines)
    app.router.add_get("/ws/btcusdt@aggTrade", handle_aggtrade_ws)
    app.router.add_get("/ws/btcusdt@kline_1m", handle_kline_ws)

    log.info(f"Replay mock server starting on 0.0.0.0:{MOCK_PORT}")
    log.info(f"REPLAY_SPEED={REPLAY_SPEED}, REPLAY_LOOP={REPLAY_LOOP}")
    web.run_app(app, host="0.0.0.0", port=MOCK_PORT)


if __name__ == "__main__":
    main()
