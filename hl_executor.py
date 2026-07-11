#!/usr/bin/env python3
from __future__ import annotations

"""
hl_executor.py

Hyperliquid TESTNET execution engine for the HFT regime signal.

Behavior:
  - Polls the local ClickHouse table `default.hft_market_data` for the latest
    `regime` value (1 = Long, -1 = Short).
  - Sizes the target BTC perpetual position as 80% of account equity * 10x leverage.
  - On a regime change, sends a SINGLE Immediate-Or-Cancel limit order that flips
    the current position to the new target (e.g. Short 1 BTC -> Long 1 BTC is a
    Buy order for 2 BTC).
  - Prints high-precision latency benchmarks for every execution path.

Dependencies:
    pip install hyperliquid-python-sdk clickhouse-connect eth-account

Security note:
  The credentials below are injected as constants because this is a testnet script.
  In production, load them from environment variables or Docker secrets.
"""

import os
import time
import traceback

import eth_account
from eth_account.signers.local import LocalAccount

from hyperliquid.exchange import Exchange
from hyperliquid.info import Info
from hyperliquid.utils import constants

# ClickHouse driver
import clickhouse_connect

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
MAIN_WALLET: str = "0xA01Ad76F975bbc31ac2Ad9a0a0Dda8c91D06b07D"
AGENT_ADDRESS: str = "0x660D2D066cbE772F8e523D83d22d4C45b83185a1"
AGENT_SECRET_KEY: str = "0x9d22f7b52a16de417553f0dff9c0d6e50292634b47da9a25db434a3f4b25953d"

COIN: str = "BTC"
LEVERAGE: int = 10
EQUITY_PCT: float = 0.80
SLIPPAGE: float = 0.01          # 1% aggressive buffer
SIZE_DECIMALS: int = 4          # requested target-size precision
PRICE_SIG_FIGS: int = 5         # mirror SDK market-order price rounding
MIN_ORDER_SIZE: float = 1e-9
POLL_INTERVAL_S: float = 0.5

CLICKHOUSE_HOST: str = os.getenv("CLICKHOUSE_HOST", "localhost")
CLICKHOUSE_PORT: int = int(os.getenv("CLICKHOUSE_PORT", "8123"))
CLICKHOUSE_USER: str = os.getenv("CLICKHOUSE_USER", "default")
CLICKHOUSE_PASSWORD: str = os.getenv("CLICKHOUSE_PASSWORD", "hft123")
CLICKHOUSE_DATABASE: str = os.getenv("CLICKHOUSE_DATABASE", "default")


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
def get_clickhouse_client() -> clickhouse_connect.driver.Client:
    """Return a ClickHouse HTTP client pointing at the local instance."""
    return clickhouse_connect.get_client(
        host=CLICKHOUSE_HOST,
        port=CLICKHOUSE_PORT,
        username=CLICKHOUSE_USER,
        password=CLICKHOUSE_PASSWORD,
        database=CLICKHOUSE_DATABASE,
    )


def get_latest_regime(client: clickhouse_connect.driver.Client) -> int | None:
    """Fetch the most recent `regime` value from hft_market_data."""
    result = client.query(
        "SELECT regime FROM hft_market_data ORDER BY timestamp DESC LIMIT 1"
    )
    if result.result_rows:
        value = result.result_rows[0][0]
        return int(value) if value is not None else None
    return None


def get_mark_price(info: Info) -> float:
    """Return the BTC mark price from Hyperliquid's meta + asset contexts."""
    meta, ctxs = info.meta_and_asset_ctxs()
    universe = meta.get("universe", [])
    for idx, asset in enumerate(universe):
        if asset.get("name") == COIN:
            mark_px = ctxs[idx].get("markPx")
            if mark_px is None:
                raise RuntimeError(f"markPx missing for {COIN}")
            return float(mark_px)
    raise RuntimeError(f"{COIN} not found in meta_and_asset_ctxs")


def get_current_btc_position(info: Info, address: str) -> float:
    """Return signed BTC position size (+ long, - short, 0 flat)."""
    user_state = info.user_state(address)
    positions = user_state.get("assetPositions", [])
    for pos in positions:
        p = pos.get("position", {})
        if p.get("coin") == COIN:
            return float(p.get("szi", "0"))
    return 0.0


def round_size(sz: float) -> float:
    """Round a BTC size to the configured decimal places."""
    return round(sz, SIZE_DECIMALS)


def round_price(px: float, info: Info) -> float:
    """
    Round a limit price the same way the SDK rounds market-order prices:
    5 significant figures, then truncated to 6 - szDecimals decimals.
    """
    asset = info.name_to_asset(COIN)
    sz_decimals = info.asset_to_sz_decimals[asset]
    price_decimals = max(0, 6 - sz_decimals)
    px = float(f"{px:.{PRICE_SIG_FIGS}g}")
    return round(px, price_decimals)


def execute_flip(
    agent_exchange: Exchange,
    info: Info,
    regime: int,
) -> tuple[float, float, dict] | None:
    """
    Compute the flip order and submit it to Hyperliquid.

    Returns (t_api_start, t_api_done, order_result) on an attempted order,
    or None if no trade is required.
    """
    account_value = float(info.user_state(MAIN_WALLET)["marginSummary"]["accountValue"])
    if account_value <= 0:
        print(f"[EXEC] accountValue is {account_value}; skipping trade.")
        return None

    mark_price = get_mark_price(info)
    notional_target = account_value * EQUITY_PCT * LEVERAGE
    target_btc_size = round_size(notional_target / mark_price)

    # regime (+1 / -1) gives the direction of the target position
    target_btc_size *= regime

    current_size = get_current_btc_position(info, MAIN_WALLET)
    order_size = round_size(target_btc_size - current_size)

    if abs(order_size) < MIN_ORDER_SIZE:
        print(
            f"[EXEC] No flip needed. current={current_size:.4f} "
            f"target={target_btc_size:.4f} order={order_size:.4f}"
        )
        return None

    is_buy = order_size > 0
    abs_size = abs(order_size)

    # Aggressive limit: +1% for buys, -1% for sells
    limit_price = mark_price * (1 + SLIPPAGE if is_buy else 1 - SLIPPAGE)
    limit_price = round_price(limit_price, info)

    side_str = "BUY" if is_buy else "SELL"
    print(
        f"[EXEC] Flipping position: current={current_size:.4f} "
        f"target={target_btc_size:.4f} order={abs_size:.4f} {side_str} "
        f"@ limit={limit_price:.2f} (mark={mark_price:.2f})"
    )

    t_api_start = time.perf_counter()
    result = agent_exchange.order(
        COIN,
        is_buy,
        abs_size,
        limit_price,
        {"limit": {"tif": "Ioc"}},
    )
    t_api_done = time.perf_counter()

    if result.get("status") == "ok":
        statuses = result.get("response", {}).get("data", {}).get("statuses", [])
        for status in statuses:
            if "filled" in status:
                filled = status["filled"]
                print(
                    f"[EXEC] FILLED oid={filled.get('oid')} "
                    f"totalSz={filled.get('totalSz')} avgPx={filled.get('avgPx')}"
                )
            elif "error" in status:
                print(f"[EXEC] ORDER REJECTED: {status['error']}")
            else:
                print(f"[EXEC] STATUS: {status}")
    else:
        print(f"[EXEC] API ERROR: {result}")

    return t_api_start, t_api_done, result


def print_latency(t_start: float, t_db_done: float, t_api_start: float, t_api_done: float) -> None:
    """Print the four required latency measurements in milliseconds."""
    db_ms = (t_db_done - t_start) * 1000.0
    prep_ms = (t_api_start - t_db_done) * 1000.0
    api_ms = (t_api_done - t_api_start) * 1000.0
    total_ms = (t_api_done - t_start) * 1000.0
    print(
        f"[LATENCY] DB={db_ms:7.3f}ms | Prep={prep_ms:7.3f}ms | "
        f"HL_API={api_ms:7.3f}ms | Total={total_ms:7.3f}ms"
    )


# -----------------------------------------------------------------------------
# Main loop
# -----------------------------------------------------------------------------
def main() -> None:
    print("[INIT] Deriving agent account from provided secret key...")
    agent_account: LocalAccount = eth_account.Account.from_key(AGENT_SECRET_KEY)
    if agent_account.address.lower() != AGENT_ADDRESS.lower():
        raise ValueError(
            f"Agent secret key maps to {agent_account.address}, "
            f"but AGENT_ADDRESS is {AGENT_ADDRESS}"
        )

    print(f"[INIT] Main wallet:  {MAIN_WALLET}")
    print(f"[INIT] Agent wallet: {agent_account.address}")
    print(f"[INIT] Hyperliquid API: {constants.TESTNET_API_URL}")

    info = Info(constants.TESTNET_API_URL, skip_ws=True)
    agent_exchange = Exchange(
        agent_account,
        constants.TESTNET_API_URL,
        account_address=MAIN_WALLET,
    )

    print(f"[INIT] Setting {COIN} leverage to {LEVERAGE}x cross margin...")
    try:
        lev_result = agent_exchange.update_leverage(LEVERAGE, COIN)
        print(f"[INIT] Leverage update: {lev_result}")
    except Exception as e:
        print(f"[WARN] update_leverage failed (continuing): {e}")

    print("[INIT] Connecting to ClickHouse...")
    ch_client = get_clickhouse_client()
    print("[INIT] Executor ready. Polling for regime changes...")

    last_regime: int | None = None
    while True:
        t_start = time.perf_counter()
        try:
            regime = get_latest_regime(ch_client)
        except Exception as e:
            print(f"[WARN] ClickHouse query failed: {e}")
            traceback.print_exc()
            time.sleep(POLL_INTERVAL_S)
            continue
        t_db_done = time.perf_counter()

        if regime is None:
            print("[WARN] No regime rows in ClickHouse yet; retrying...")
            time.sleep(POLL_INTERVAL_S)
            continue

        if regime == last_regime:
            time.sleep(POLL_INTERVAL_S)
            continue

        if regime not in (1, -1):
            print(f"[WARN] Ignoring unexpected regime value: {regime}")
            time.sleep(POLL_INTERVAL_S)
            continue

        regime_name = "LONG" if regime == 1 else "SHORT"
        print(f"[SIGNAL] Regime changed -> {regime_name} ({regime})")
        last_regime = regime

        try:
            flip_result = execute_flip(agent_exchange, info, regime)
        except Exception as e:
            print(f"[ERROR] Failed to execute flip: {e}")
            traceback.print_exc()
            time.sleep(POLL_INTERVAL_S)
            continue

        if flip_result is not None:
            t_api_start, t_api_done, _ = flip_result
            print_latency(t_start, t_db_done, t_api_start, t_api_done)

        time.sleep(POLL_INTERVAL_S)


if __name__ == "__main__":
    main()
