#!/bin/zsh
# run_local.sh — macOS local dual-mode launcher for the HFT stack
#
# Forces linux/amd64 emulation so the x86_64-optimized C++ DSP binaries
# (Intel MKL, -msse3) produce bit-identical math on Apple Silicon.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

# Force x86_64 emulation for absolute math parity with the production server.
export DOCKER_DEFAULT_PLATFORM=linux/amd64

echo "====================================="
echo " HFT Local Launcher"
echo "====================================="
echo "  1) LIVE  — Binance WS / REST"
echo "  2) REPLAY — tests/data/hft_tick_data_sample.csv"
echo "====================================="

printf "Select Data Mode [1]: "
read -r choice
choice=${choice:-1}

case "$choice" in
  1|LIVE|live|"")
    export DATA_MODE=LIVE
    export BINANCE_AGGTRADE_WS_URL="wss://stream.binance.com:9443/ws/btcusdt@aggTrade"
    export BINANCE_KLINE_WS_URL="wss://stream.binance.com:9443/ws/btcusdt@kline_1m"
    export BINANCE_REST_URL="https://api.binance.com"
    export COMPOSE_PROFILES=""
    echo "🚀 LIVE mode selected"
    ;;
  2|REPLAY|replay)
    export DATA_MODE=REPLAY
    export BINANCE_AGGTRADE_WS_URL="ws://replay-mock:8080/ws/btcusdt@aggTrade"
    export BINANCE_KLINE_WS_URL="ws://replay-mock:8080/ws/btcusdt@kline_1m"
    export BINANCE_REST_URL="http://replay-mock:8080"
    export COMPOSE_PROFILES="replay"
    # Loop the CSV so late-starting consumers (java-app) can connect and still
    # replay from the beginning, and so DSP feedback loops (SG → Java → CH → SG)
    # have time to warm up and cross-influence subsequent rows.
    export REPLAY_LOOP=${REPLAY_LOOP:-true}
    echo "📼 REPLAY mode selected"
    echo "   CSV source: tests/data/hft_tick_data_sample.csv"
    ;;
  *)
    echo "❌ Invalid choice: $choice"
    exit 1
    ;;
esac

# Optional backtest throttle:
#   REPLAY_SPEED=0   -> max speed
#   REPLAY_SPEED=1.0 -> wall-clock according to CSV timestamps
export REPLAY_SPEED=${REPLAY_SPEED:-0}

echo "Starting Docker Compose (platform: $DOCKER_DEFAULT_PLATFORM)..."
# فلگ پروفایل از اینجا حذف شد، داکر به صورت خودکار از COMPOSE_PROFILES می‌خواند
docker compose up -d --build

echo ""
echo "✅ Stack started in $DATA_MODE mode"
echo "   Grafana         : http://localhost:3000"
echo "   ClickHouse HTTP : http://localhost:8123"