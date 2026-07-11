# Migration Architecture Document: Binance → Hyperliquid

## 1. Objective

Migrate the high-frequency trading signal stack from **Binance** to **Hyperliquid Testnet** while enforcing the following separation of concerns:

- **Java + C++** → ultra-low-latency **tick-data** path only (trades + L2 order-book updates).
- **Python** → quantitative analysis on **1-minute candle (OHLCV)** data only, producing the `regime` signal consumed by execution.

No production code changes are included in this Phase-1 document. This document is submitted for explicit approval before Phase 2 begins.

---

## 2. Current-State Architecture (Binance)

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                                  BINANCE                                     │
│  aggTrade WS @ btcusdt@aggTrade        kline WS @ btcusdt@kline_1m           │
│         │                                       │                            │
│         ▼                                       ▼                            │
│  ┌──────────────┐                        ┌──────────────┐                   │
│  │  Java App    │◄──── ZMQ REGIME ───────│  cpp-hmm     │  candle-based HMM │
│  │  LMAX        │                        │  (C++)       │                   │
│  │  Disruptor   │◄──── ZMQ MACRO ────────│  python-macro│  candle-based     │
│  │              │                        │  (Python)    │  macro trend      │
│  │ writes ticks │                        └──────────────┘                   │
│  │ to ClickHouse│                                                           │
│  └──────┬───────┘                                                           │
│         │                                                                    │
│         ▼                                                                    │
│  ┌──────────────┐    ZMQ 5557 (SG frames)    ┌──────────────┐               │
│  │ ClickHouse   │◄───────────────────────────│  cpp-sg-dsp  │               │
│  │ hft_market_  │    ZMQ 5558 (SSA frames)   │  (C++)       │               │
│  │ data         │◄───────────────────────────│  cpp-ssa     │               │
│  └──────────────┘                            └──────────────┘               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Binance integrations identified

| Component | File | Binance endpoint | Data used | Concern violation? |
|---|---|---|---|---|
| Java tick ingress | `java-app/src/main/java/HftRegimeDetection.java` | `wss://stream.binance.com:9443/ws/btcusdt@aggTrade` | trade price `p`, qty `q`, time `T` | None |
| C++ HMM | `Hidden-Markov-Model-MKL/main.cpp` | `https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&limit=500`<br>`wss://stream.binance.com:9443/ws/btcusdt@kline_1m` | 1m close prices → log realized variance → HMM regime | **Yes** — C++ consumes candles |
| Python macro | `python-macro/macro_engine.py` | `https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&limit=...`<br>`wss://stream.binance.com:9443/ws/btcusdt@kline_1m` | 1m OHLCV → spectral smoother / macro trend | None |

### 2.2 Inter-process contracts already in place

These contracts **must be preserved** during migration so Java does not need to change its ZMQ parsing logic.

| Publisher | Port | Message format | Consumer field |
|---|---|---|---|
| cpp-hmm / future python-hmm | `5555` | `REGIME\|{state},{p0},{p1},{p2}` | `currentHmmRegime`, `currentProbTrend=p1`, `currentProbCrisis=p2` |
| python-macro | `5556` | `MACRO_TREND\|{value},{slope}` | `macroL1Value`, `macroL1Slope` |
| cpp-sg-dsp | `5557` | 52-byte little-endian binary frame | `sgSmoothed`, `sgSlope`, `sgAccel`, `sgSideway` |
| cpp-ssa-engine | `5558` | 68-byte little-endian binary frame | `ssaSmoothed`, `ssaSlope`, `ssaAccel`, `ssaMacroTrend`, etc. |

The ClickHouse table `default.hft_market_data` is already exchange-agnostic (no Binance-specific columns).

---

## 3. Target-State Architecture (Hyperliquid Testnet)

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                               HYPERLIQUID TESTNET                            │
│                                                                              │
│  trades WS ───────┐                                                          │
│  wss://api.hyperliquid-testnet.xyz/ws      ┌──────────────┐                 │
│                    │                       │  Java App    │                 │
│  l2Book WS ────────┼──────────────────────►│  LMAX        │                 │
│  (fast/snapshot)   │                       │  Disruptor   │                 │
│                    │                       │              │                 │
│                    │                       │ writes ticks │                 │
│                    │                       │ to ClickHouse│                 │
│                    │                       └──────┬───────┘                 │
│                    │                              │                         │
│  candle WS/REST ───┴───────────┐                  ▼                         │
│  api.hyperliquid-testnet.xyz   │          ┌──────────────┐                  │
│                                │          │ ClickHouse   │                  │
│                                │          │ hft_market_  │                  │
│                                │          │ data         │                  │
│                                │          └──────┬───────┘                  │
│                                │                 │                          │
│                                ▼                 ▼                          │
│                       ┌──────────────┐  ┌──────────────┐                    │
│                       │ python-signal│  │  cpp-sg-dsp  │ ZMQ 5557           │
│                       │ (Python)     │  │  (C++)       │                    │
│                       │  · HMM regime│  └──────────────┘                    │
│                       │  · Macro     │  ┌──────────────┐                    │
│                       │    trend     │  │  cpp-ssa     │ ZMQ 5558           │
│                       └──────┬───────┘  └──────────────┘                    │
│                              │                                              │
│                              │ ZMQ 5555 (REGIME)                            │
│                              │ ZMQ 5556 (MACRO_TREND)                       │
│                              ▼                                              │
│                       ┌──────────────┐                                      │
│                       │  Java App    │ uses regime/macro in tick handler   │
│                       └──────────────┘                                      │
│                                                                              │
│                       ┌─────────────────────────────────────┐               │
│                       │ python-execution (Phase 3)          │               │
│                       │  · polls ClickHouse regime          │               │
│                       │  · places orders via HL SDK         │               │
│                       │  · Agent EIP-712 ECDSA signing      │               │
│                       └─────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Key architectural decisions

1. **Retire `cpp-hmm`**. The C++ HMM node consumes 1m candles, which violates the "C++ only tick data" rule. Its function moves to a Python service.
2. **Consolidate Python quant signals** into a single container (`python-signal`). It subscribes to Hyperliquid candles, runs (a) the HMM regime model and (b) the existing macro-trend model, and publishes both ZMQ streams that Java already expects.
3. **Keep `cpp-sg-dsp` and `cpp-ssa-engine` unchanged** except for their ClickHouse dependency. They already read ticks from ClickHouse and emit binary ZMQ frames.
4. **Add a new standalone `python-execution` script** in Phase 3. It reads the regime from ClickHouse and places orders through the official `hyperliquid-python-sdk` using the provided Agent wallet.

---

## 4. Endpoint Mapping: Binance → Hyperliquid

### 4.1 Tick data (Java)

| Binance | Hyperliquid Testnet | Channel / request | Notes |
|---|---|---|---|
| `wss://stream.binance.com:9443/ws/btcusdt@aggTrade` | `wss://api.hyperliquid-testnet.xyz/ws` | `{"method":"subscribe","subscription":{"type":"trades","coin":"BTC"}}` | Batched array of trades per block. Each element has `px`, `sz`, `time`, `side`. |
| *(none in current code)* | `wss://api.hyperliquid-testnet.xyz/ws` | `{"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC","fast":true}}` | Snapshot-style L2 book. Top-of-book can be used to enrich the tick event or compute a micro-price. |

**Trade message shape (Hyperliquid)**

```json
{
  "channel": "trades",
  "data": [
    {
      "coin": "BTC",
      "side": "A",
      "px": "97250.5",
      "sz": "0.1234",
      "hash": "0x...",
      "time": 1783755000123,
      "tid": 123456789,
      "users": ["0x...", "0x..."]
    }
  ]
}
```

Java mapping:
- `event.price = Double.parseDouble(trade.px)`
- `event.volume = Double.parseDouble(trade.sz)`  (optionally signed by side: negative for `"A"` asks/sells)
- `event.timestamp = trade.time` (ms since epoch)

**L2 book message shape**

```json
{
  "channel": "l2Book",
  "data": {
    "coin": "BTC",
    "levels": [
      [{"px":"97250.0","sz":"1.5","n":3}, ...],
      [{"px":"97251.0","sz":"0.8","n":2}, ...]
    ],
    "time": 1783755000456
  }
}
```

The first element of `levels[0]` is the best bid, the first element of `levels[1]` is the best ask.

### 4.2 Candle data (Python)

| Binance | Hyperliquid Testnet | Notes |
|---|---|---|
| `https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&limit=...` | `POST https://api.hyperliquid-testnet.xyz/info` with body `{"type":"candleSnapshot","req":{"coin":"BTC","interval":"1m","startTime":<ms>,"endTime":<ms>}}` | Max 5000 candles per call; paginate with `startTime`. |
| `wss://stream.binance.com:9443/ws/btcusdt@kline_1m` | `wss://api.hyperliquid-testnet.xyz/ws` with subscription `{"method":"subscribe","subscription":{"type":"candle","coin":"BTC","interval":"1m"}}` | No `is_final` flag; finalize a candle when the next message has a larger `t` (open time). |

**Candle snapshot response**

```json
[
  {
    "t": 1783754940000,
    "T": 1783754999999,
    "s": "BTC",
    "i": "1m",
    "o": "97200.0",
    "c": "97250.5",
    "h": "97300.0",
    "l": "97190.0",
    "v": "12.3456",
    "n": 87
  }
]
```

Mapping to internal OHLCV:
- `open = o`, `high = h`, `low = l`, `close = c`, `volume = v`, `time = t`.

### 4.3 Symbol mapping

| Exchange | BTC perpetual | Notes |
|---|---|---|
| Binance | `BTCUSDT` | spot-margin perpetual |
| Hyperliquid | `BTC` | HyperCore perp coin name. Spot pairs use a different notation; the trading target is the BTC perpetual. |

---

## 5. Component-Level Migration Plan

### 5.1 Java tick ingress (`java-app`)

**Current:** `BinanceProducer extends WebSocketClient` connects to Binance aggTrade stream and parses `p`, `q`, `T`.

**Proposed change:**
- Rename / refactor to `HyperliquidTickProducer`.
- Connect to `wss://api.hyperliquid-testnet.xyz/ws`.
- On `onOpen`, send two subscription messages:
  1. `trades` for `BTC`.
  2. `l2Book` for `BTC` with `"fast":true`.
- On each incoming message, route by `channel`:
  - `trades`: iterate `data` array; publish one `TickEvent` per trade.
  - `l2Book`: store the latest top-of-book snapshot (optional) and/or publish a synthetic mid-price tick.
- Preserve the existing `TickEvent` schema and Disruptor wiring.
- Continue writing to ClickHouse via `ClickHouseBatchHandler`; `cpp-sg-dsp` and `cpp-ssa-engine` depend on this table.

**Dependencies to add:** none. `Java-WebSocket` and `gson` are already in `pom.xml`.

### 5.2 Python quant signal service (`python-signal`)

**Current:** `python-macro` consumes Binance 1m candles and publishes `MACRO_TREND|...` only. `cpp-hmm` consumes candles and publishes `REGIME|...`.

**Proposed change:**
- Replace the `cpp-hmm` container with a Python service that subscribes to Hyperliquid candles.
- The service runs two modules in the same process:
  1. **`hmm_regime_model`** — re-implements the C++ Gaussian HMM in Python (using `hmmlearn` or a hand-rolled Baum-Welch equivalent). Inputs: log realized variance computed from 1m close prices. Output: 3-state regime (0=calm, 1=trend, 2=crisis) and state probabilities.
  2. **`macro_trend_model`** — existing `CausalSpectralDispatcher` + `AdaptiveConstrainedSmoother` + `OnlineBayesianRegimeTracker`.
- The service publishes on two ZMQ sockets that mirror the old contracts:
  - Port `5555`: `REGIME|{state},{p0},{p1},{p2}`
  - Port `5556`: `MACRO_TREND|{value},{slope}`
- This keeps the Java subscriber untouched.

**Recommended approach:** keep the existing `python-macro` directory, add `hmm_model.py`, and rename the service image to `python-signal` in `docker-compose.yml`.

### 5.3 C++ DSP engines (`cpp-sg-dsp`, `cpp-ssa-engine`)

**Current:** poll ClickHouse `hft_market_data` for new ticks and emit ZMQ frames.

**Proposed change:** no functional changes. These engines are already exchange-agnostic because they consume the local ClickHouse table. The only dependency is that Java continues to write ticks. The ZMQ frame contracts (5557, 5558) remain unchanged.

**Build impact:** the `Hidden-Markov-Model-MKL` build stage in `Dockerfile.cpp-consolidated` should be removed because `cpp-hmm` is retired. The `hmm-runtime` target in `docker-compose.yml` should also be removed.

### 5.4 ClickHouse schema

**Current:** `default.hft_market_data` stores ticks, regime diagnostics, Kalman state, HMM state, etc.

**Proposed change:** no schema migration is required for Phase 2. The table is exchange-agnostic. Optional future enhancement: add `best_bid`, `best_ask`, `side` columns if L2 enrichment is desired.

### 5.5 Docker Compose / infrastructure

Changes to `docker-compose.yml`:

| Service | Action | Reason |
|---|---|---|
| `cpp-hmm` | Remove | Replaced by Python HMM. |
| `python-macro` | Rename to `python-signal` and add `HL_WS_URL`, `HL_REST_URL`, `HL_COIN`, `HL_INTERVAL` env vars. | Single quant signal source. |
| `java-app` | Replace `ZMQ_HOST=cpp-hmm` references / env vars with `python-signal` host. Or simply point `ZMQ_HOST` / `MACRO_ZMQ_HOST` to the new service name. | Java still expects REGIME on 5555 and MACRO_TREND on 5556. |
| `cpp-sg-dsp`, `cpp-ssa` | No change. | Tick-source agnostic. |
| New `python-execution` | Add in Phase 3. | Standalone execution script. |

---

## 6. Authentication & Order Execution

### 6.1 Current state

The repository currently **has no order execution or authentication code**. There is no Binance HMAC-SHA256 implementation to remove.

### 6.2 Target state (Phase 2 / Phase 3)

- Use the official **`hyperliquid-python-sdk`**.
- Network: `constants.TESTNET_API_URL`.
- Authentication mode: **Agent wallet** (L1 EIP-712 ECDSA signing).
- Credentials provided for Phase 3:
  - Main wallet: `0xA01Ad76F975bbc31ac2Ad9a0a0Dda8c91D06b07D`
  - Agent address: `0x660D2D066cbE772F8e523D83d22d4C45b83185a1`
  - Agent secret key: `0x9d22f7b52a16de417553f0dff9c0d6e50292634b47da9a25db434a3f4b25953d`

The Python execution script will:
1. Initialize `Info` and `Exchange` from the SDK with the Agent key.
2. Poll ClickHouse for the latest `hmm_regime` (or `regime`) value.
3. Maintain a target position:
   - `regime == 1` → target long.
   - `regime == -1` → target short.
4. Query current position via `info.user_state(main_wallet)` and compute the delta between current and target size.
5. Place an aggressive limit order (crossing the spread) for the delta, using the SDK's internal EIP-712 signing.
6. Poll order status via `orderStatus` and handle fills, rejects, and retries.

**Security note:** in production the Agent secret key must be injected via Docker secret / environment variable and never committed. For the Phase-3 testnet script the values may be passed as env vars.

---

## 7. Data Flow (Step-by-Step)

1. **Java** connects to Hyperliquid `trades` (+ optional `l2Book`) WebSocket.
2. Each parsed trade becomes a `TickEvent` published to the LMAX Disruptor.
3. `SsaProcessingHandler` fuses the event with the latest async ZMQ values:
   - `REGIME` from `python-signal` (port 5555).
   - `MACRO_TREND` from `python-signal` (port 5556).
   - SG frame from `cpp-sg-dsp` (port 5557).
   - SSA frame from `cpp-ssa-engine` (port 5558).
4. `ClickHouseBatchHandler` writes the enriched tick to `default.hft_market_data`.
5. `cpp-sg-dsp` and `cpp-ssa-engine` poll the table and publish DSP frames (steps 3–4 form a feedback loop).
6. `python-signal` subscribes to Hyperliquid 1m candles, updates HMM and macro models, and publishes regime / macro streams.
7. `python-execution` polls ClickHouse for the latest regime and sends orders through the Hyperliquid SDK.

---

## 8. Phase Rollout

### Phase 1 — Architecture Mapping & Approval (THIS DOCUMENT)
- [x] Scan repository.
- [x] Identify all Binance integrations.
- [x] Map tick data to Java, candle data to Python.
- [x] Propose Hyperliquid endpoint replacements.
- [ ] Obtain explicit approval before writing code.

### Phase 2 — Core Migration (starts after approval)
1. Replace Binance endpoints in Java with Hyperliquid `trades`/`l2Book` WebSocket.
2. Replace Binance endpoints in Python with Hyperliquid `candle` REST/WebSocket.
3. Move HMM logic from `cpp-hmm` to Python and retire the C++ HMM container.
4. Update `docker-compose.yml` and `Dockerfile.cpp-consolidated`.
5. Add Hyperliquid environment variables (no execution yet).
6. Build and smoke-test the signal path end-to-end.

### Phase 3 — Python Execution Script & ClickHouse Integration (starts after Phase 2 approval)
1. Create `python-execution/` service / script.
2. Connect to ClickHouse and poll `regime`/`hmm_regime`.
3. Integrate `hyperliquid-python-sdk` with the provided Agent credentials.
4. Implement buy/long and sell/short logic with aggressive limit orders.
5. Add order-status polling, error handling, retry/backoff, and logging.
6. Test on Hyperliquid Testnet with small size.

---

## 9. Open Questions / Request for Approval

Please confirm the following so Phase 2 can begin:

1. **Asset:** Is the target coin `BTC` on Hyperliquid perps, or do you want `ETH`/another coin?
2. **Python service consolidation:** Do you approve replacing `cpp-hmm` and keeping a single `python-signal` container that publishes both `REGIME` and `MACRO_TREND`?
3. **L2 enrichment:** Should Java subscribe to `l2Book` only for diagnostics, or should the top-of-book mid-price be injected as an additional tick stream?
4. **HMM library:** Should the Python HMM use `hmmlearn` (simpler, pip-installable) or a hand-rolled Baum-Welch to exactly match the C++ implementation?
5. **Execution model:** For `python-execution`, do you want the script to fully flip position (long ↔ short) on every regime change, or to scale into a fixed target size?

**Awaiting your approval to proceed to Phase 2.**
