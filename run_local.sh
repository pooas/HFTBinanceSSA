#!/bin/bash
# run_setup_and_start.sh — Universal Auto-Bootstrap Launcher for the HFT stack
#
# Forces linux/amd64 emulation, auto-installs Docker/Dependencies,
# downloads offline plugins, and launches the stack on any new server.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

# ==========================================
# 1. Check for Root / Sudo privileges
# ==========================================
SUDO=''
if (( $EUID != 0 )); then
    SUDO='sudo'
fi

echo "====================================="
echo " 🛠️ System Pre-flight Checks..."
echo "====================================="

# ==========================================
# 2. Auto-Install Basic Dependencies (curl, unzip)
# ==========================================
if ! command -v curl &> /dev/null || ! command -v unzip &> /dev/null; then
    echo "📦 Installing required basic packages (curl, unzip)..."
    if command -v apt-get &> /dev/null; then
        $SUDO apt-get update -y && $SUDO apt-get install -y curl unzip
    elif command -v yum &> /dev/null; then
        $SUDO yum install -y curl unzip
    else
        echo "❌ Cannot find apt or yum. Please install curl and unzip manually."
        exit 1
    fi
    echo "✅ Basic packages installed."
fi

# ==========================================
# 3. Auto-Install Docker & Docker Compose
# ==========================================
if ! command -v docker &> /dev/null; then
    echo "🐳 Docker not found! Installing Docker automatically..."
    curl -fsSL https://get.docker.com -o get-docker.sh
    $SUDO sh get-docker.sh
    rm get-docker.sh
    $SUDO usermod -aG docker $USER || true
    echo "✅ Docker installed successfully."
    # Restart docker service just in case
    $SUDO systemctl start docker || true
    $SUDO systemctl enable docker || true
else
    echo "✅ Docker is already installed."
fi

if ! docker compose version &> /dev/null; then
    echo "🐳 Docker Compose plugin not found! Installing..."
    if command -v apt-get &> /dev/null; then
        $SUDO apt-get install -y docker-compose-plugin
    elif command -v yum &> /dev/null; then
        $SUDO yum install -y docker-compose-plugin
    fi
    echo "✅ Docker Compose plugin installed."
fi

# Fix Grafana Permissions (Crucial for new servers to prevent db locks)
echo "🔧 Setting Grafana directory permissions (User 472)..."
$SUDO chown -R 472:472 ./grafana 2>/dev/null || true

# ==========================================
# 4. HFT Stack Execution Logic
# ==========================================
export DOCKER_DEFAULT_PLATFORM=linux/amd64

echo ""
echo "====================================="
echo " 🚀 HFT Server Launcher"
echo "====================================="
echo "  1) LIVE   — Binance WS / REST"
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
    echo "🟢 LIVE mode selected"
    ;;
  2|REPLAY|replay)
    export DATA_MODE=REPLAY
    export BINANCE_AGGTRADE_WS_URL="ws://127.0.0.1:8080/ws/btcusdt@aggTrade"
    export BINANCE_KLINE_WS_URL="ws://127.0.0.1:8080/ws/btcusdt@kline_1m"
    export BINANCE_REST_URL="http://127.0.0.1:8080"
    export COMPOSE_PROFILES="replay"
    export REPLAY_LOOP=${REPLAY_LOOP:-true}
    echo "📼 REPLAY mode selected"
    echo "   CSV source: tests/data/hft_tick_data_sample.csv"
    ;;
  *)
    echo "❌ Invalid choice: $choice"
    exit 1
    ;;
esac

export REPLAY_SPEED=${REPLAY_SPEED:-0}

echo "Starting Docker Compose (platform: $DOCKER_DEFAULT_PLATFORM)..."
$SUDO docker compose up -d --build

# ==========================================
# 5. Auto-Install Grafana Plugin via Docker Exec
# ==========================================
echo ""
echo "⏳ Waiting 5 seconds for Grafana to initialize..."
sleep 5

echo "🔌 Installing ClickHouse plugin directly inside Grafana container..."
$SUDO docker exec -i hft_grafana sh -c "wget -qO /tmp/plugin.zip https://github.com/grafana/clickhouse-datasource/releases/download/v4.1.0/grafana-clickhouse-datasource-4.1.0.linux_amd64.zip && unzip -o /tmp/plugin.zip -d /var/lib/grafana/plugins/ && rm /tmp/plugin.zip && grafana cli plugins install grafana-clickhouse-datasource"

echo "🔄 Restarting Grafana to apply the plugin..."
$SUDO docker restart hft_grafana

echo ""
echo "✅ Stack started in $DATA_MODE mode"
echo "   Grafana         : http://127.0.0.1:3000 (Or your server's Public IP)"
echo "   ClickHouse HTTP : http://127.0.0.1:8123"
