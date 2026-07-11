import pandas as pd
import numpy as np
import time
import threading
import requests
import cvxpy as cp
import warnings
import traceback
import asyncio
import websockets
import json
import zmq
from datetime import datetime

warnings.filterwarnings("ignore")

# =========================================================================
# 🌟 Setup ZeroMQ Publisher for Java
# =========================================================================
context = zmq.Context()
zmq_socket = context.socket(zmq.PUB)
zmq_socket.bind("tcp://*:5556")
print("🔗 Python Macro-L1 Publisher active on port 5556")

class OnlineBayesianRegimeTracker:
    def __init__(self):
        self.belief = np.array([0.0, 1.0, 0.0]) 
        self.transition_matrix = np.array([
            [0.85, 0.10, 0.05], 
            [0.15, 0.70, 0.15], 
            [0.05, 0.10, 0.85]  
        ])

    def _gaussian_pdf(self, x, mean, std):
        pdf_val = (1.0 / (std * np.sqrt(2 * np.pi))) * np.exp(-0.5 * ((x - mean) / std) ** 2)
        return max(pdf_val, 1e-10)

    def update(self, z_score, ssa_slope):
        prior = self.transition_matrix.T @ self.belief
        likelihood_s1 = self._gaussian_pdf(z_score, mean=-1.5, std=0.6)
        likelihood_s2 = self._gaussian_pdf(z_score, mean=0.0, std=0.5)
        likelihood_s3s4 = self._gaussian_pdf(z_score, mean=1.5, std=0.6)
        
        if ssa_slope < 0:
            likelihood_s1 *= 1.5
            likelihood_s3s4 *= 0.5
        elif ssa_slope > 0:
            likelihood_s3s4 *= 1.5
            likelihood_s1 *= 0.5
            
        likelihood = np.array([likelihood_s1, likelihood_s2, likelihood_s3s4])
        posterior = prior * likelihood
        
        if np.sum(posterior) == 0:
            self.belief = prior 
        else:
            self.belief = posterior / np.sum(posterior)
            
        most_likely_state = np.argmax(self.belief)
        
        if most_likely_state == 0: return -1, "S1"
        elif most_likely_state == 1: return 0, "S2"
        else: return 1, "S4" if z_score > 1.5 else "S3"

class AdaptiveConstrainedSmoother:
    def __init__(self, window_size=40):
        self.W = window_size

    def optimize_endpoint(self, y_raw, frozen_history, current_regime):
        if len(y_raw) < self.W: return y_raw[-1]

        y = y_raw[-self.W:]
        x = cp.Variable(self.W)

        if current_regime == "S4": lambda_val = 0.05
        elif current_regime == "S3": lambda_val = 0.5
        elif current_regime == "S2": lambda_val = 5.0
        elif current_regime == "S1": lambda_val = 50.0
        else: lambda_val = 1.0

        objective = cp.Minimize(cp.sum_squares(y - x) + lambda_val * cp.norm(cp.diff(x), 1))
        constraints = []
        
        if len(frozen_history) >= self.W:
            lock_point = self.W - 3
            frozen_array = np.array([d['value2'] for d in frozen_history[-self.W:]])
            constraints.append(x[:lock_point] == frozen_array[:lock_point])

        problem = cp.Problem(objective, constraints)
        try:
            solver = cp.GUROBI if cp.GUROBI in cp.installed_solvers() else cp.ECOS
            problem.solve(solver=solver, warm_start=True)
            return x.value[-1] if x.value is not None else y_raw[-1]
        except Exception:
            return y_raw[-1]

class CausalSpectralDispatcher:
    def __init__(self, window_size=120, trend_threshold=80):
        self.L = window_size
        self.trend_threshold = trend_threshold

    def fit_transform(self, data):
        N = len(data)
        if N < self.L * 2: return np.copy(data)

        K = N - self.L + 1
        X = np.column_stack([data[i:i+self.L] for i in range(K)])
        R = np.dot(X, X.T) / K

        eigenvalues, eigenvectors = np.linalg.eigh(R)
        idx = np.argsort(eigenvalues)[::-1]
        eigenvectors = eigenvectors[:, idx]

        w_trend = np.zeros(self.L)
        for i in range(self.L):
            u = eigenvectors[:, i]
            zero_crossings = np.sum(np.diff(np.sign(u)) != 0)
            period = float('inf') if zero_crossings == 0 else 2.0 * self.L / zero_crossings
            if period > self.trend_threshold:
                w_trend += u[-1] * u

        raw_trend = np.convolve(data, w_trend[::-1], mode='valid')
        trend_out = np.copy(data)
        trend_out[self.L - 1:] = raw_trend
        return trend_out

class HyperliquidQuantBot:
    def __init__(self, symbol="BTC", interval="15m", history_limit=3000):
        self.symbol = symbol.upper()
        self.interval = interval
        self.history_limit = history_limit
        
        self.df = pd.DataFrame()
        self.frozen_history = []
        self.lock = threading.Lock()
        
        self.dispatcher = CausalSpectralDispatcher(window_size=120)
        self.smoother = AdaptiveConstrainedSmoother(window_size=40)
        self.regime_tracker = OnlineBayesianRegimeTracker()
        
        self.load_historical_data()

    def load_historical_data(self):
        print(f"⏳ Fetching {self.history_limit} historical candles for {self.symbol}...")
        end_time = int(time.time() * 1000)
        start_time = end_time - self.history_limit * 60 * 1000

        body = {
            "type": "candleSnapshot",
            "req": {
                "coin": self.symbol,
                "interval": self.interval,
                "startTime": start_time,
                "endTime": end_time
            }
        }

        try:
            res = requests.post(
                "https://api.hyperliquid-testnet.xyz/info",
                json=body,
                timeout=10
            ).json()
            if not res or isinstance(res, dict):
                raise ValueError("Hyperliquid candleSnapshot returned non-array response")
        except Exception as e:
            print(f"❌ API Error: {e}")
            res = []

        all_klines = sorted(res, key=lambda k: k['t'])
        all_klines = all_klines[-self.history_limit:]
        df_list = [{'time': pd.to_datetime(k['t'], unit='ms'), 'open': float(k['o']), 'high': float(k['h']),
                    'low': float(k['l']), 'close': float(k['c']), 'volume': float(k['v'])} for k in all_klines]

        with self.lock:
            self.df = pd.DataFrame(df_list)
        print(f"✅ Loaded {len(self.df)} candles. Running Warmup...")

        for i in range(120, len(self.df)):
            self.calculate_metrics(self.df.iloc[:i+1])
        print("🚀 Warmup Complete! Macro Engine is Live.")

    def get_market_regime(self, ssa_slope, v2_hist):
        if len(v2_hist) < 5: return "S2"
        if ssa_slope > 0.5: return "S4"
        elif ssa_slope > 0: return "S3"
        elif ssa_slope > -0.5: return "S2"
        else: return "S1"

    def calculate_metrics(self, current_df):
        close_prices = current_df['close'].values.astype(float)
        
        macro_trend = self.dispatcher.fit_transform(close_prices)
        latest_macro_trend = macro_trend[-1]
        ssa_slope = latest_macro_trend - macro_trend[-2] if len(macro_trend) > 1 else 0
        
        v2_hist = [d['value2'] for d in self.frozen_history] if self.frozen_history else []
        regime = self.get_market_regime(ssa_slope, v2_hist)
        latest_value2 = self.smoother.optimize_endpoint(macro_trend, self.frozen_history, regime)
        
        # 🌟 محاسبه شیب ماکرو
        macro_l1_slope = latest_value2 - v2_hist[-1] if len(v2_hist) > 0 else 0.0

        row_data = {
            'time': current_df['time'].iloc[-1],
            'close': current_df['close'].iloc[-1],
            'ssa_trend': latest_macro_trend,
            'ssa_slope': ssa_slope,
            'regime': regime,
            'value2': latest_value2
        }
        
        self.frozen_history.append(row_data)
        if len(self.frozen_history) > 1000:
            self.frozen_history.pop(0)

        # 🌟 ارسال سیگنال ماکرو به جاوا (فقط در زمان بسته شدن کامل کندل)
        msg = f"MACRO_TREND|{latest_value2},{macro_l1_slope}"
        zmq_socket.send_string(msg)
        print(f"📡 ZMQ Sent -> L1_Value: {latest_value2:.2f} | L1_Slope: {macro_l1_slope:+.4f} | Regime: {regime}")

    async def run_hyperliquid_stream(self):
        ws_url = "wss://api.hyperliquid-testnet.xyz/ws"
        while True:
            try:
                async with websockets.connect(ws_url) as websocket:
                    print(f"🟢 WS Connected. Listening to {self.interval} Hyperliquid candles...")
                    sub = {
                        "method": "subscribe",
                        "subscription": {"type": "candle", "coin": self.symbol, "interval": self.interval}
                    }
                    await websocket.send(json.dumps(sub))
                    current_candle = None
                    while True:
                        msg = await websocket.recv()
                        data = json.loads(msg)
                        if data.get('channel') != 'candle':
                            continue
                        k = data['data']
                        t = k['t']
                        if current_candle is None:
                            current_candle = k
                            continue
                        if t == current_candle['t']:
                            current_candle = k
                            continue
                        if t > current_candle['t']:
                            c = float(current_candle['c'])
                            new_time = pd.to_datetime(current_candle['t'], unit='ms')
                            with self.lock:
                                new_row = pd.DataFrame([{
                                    'time': new_time,
                                    'open': float(current_candle['o']),
                                    'high': float(current_candle['h']),
                                    'low': float(current_candle['l']),
                                    'close': c,
                                    'volume': float(current_candle['v'])
                                }])
                                self.df = pd.concat([self.df, new_row], ignore_index=True)
                                if len(self.df) > 2000:
                                    self.df = self.df.iloc[-2000:].reset_index(drop=True)
                            self.calculate_metrics(self.df)
                            current_candle = k
            except Exception as e:
                print(f"🔴 Stream Error: {e}. Reconnecting...")
                await asyncio.sleep(3)

if __name__ == "__main__":
    bot = HyperliquidQuantBot(symbol="BTC", interval="1m", history_limit=3000)
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(bot.run_hyperliquid_stream())