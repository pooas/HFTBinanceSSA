#!/bin/bash

# توقف در صورت ارور
set -e

echo "➡️ Stopping and cleaning up previous containers..."
if [ -d "HFTBinanceSSA" ]; then
  cd HFTBinanceSSA
  sudo docker compose down -v || true
  cd ..
  sudo rm -rf HFTBinanceSSA
fi

echo "➡️ Cloning the repository..."
git clone https://pooas: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@github.com/pooas/HFTBinanceSSA.git

cd HFTBinanceSSA

git pull
# ==========================================
# بخش جدید: نصب آفلاین و مستقیم پلاگین کلیک‌هاوس
# ==========================================
echo "➡️ Setting up ClickHouse plugin directly (bypassing Git)..."
mkdir -p grafana-plugins
cd grafana-plugins

# پاک کردن نسخه احتمالی قبلی که با گیت آمده است
sudo rm -rf grafana-clickhouse-datasource

# نصب پیش‌نیازهای دانلود
sudo apt update -y
sudo apt install -y unzip wget

# دانلود مستقیم نسخه Linux AMD64 از گیت‌هاب گرافانا
sudo wget https://github.com/grafana/clickhouse-datasource/releases/download/v4.3.1/grafana-clickhouse-datasource-4.3.1.linux_amd64.zip

# اکسترکت کردن فایل
sudo unzip grafana-clickhouse-datasource-4.3.1.linux_amd64.zip -d grafana-clickhouse-datasource

# تنظیم مالکیت پوشه برای یوزر گرافانا (شناسه ۴۷۲)
sudo chown -R 472:472 grafana-clickhouse-datasource

# پاک کردن فایل زیپ اضافه
sudo rm grafana-clickhouse-datasource-4.3.1.linux_amd64.zip

# بازگشت به پوشه اصلی پروژه
cd ..
# ==========================================

echo "➡️ Checking and configuring Docker..."
sudo apt update -y
sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc || true
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt update -y
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable docker
sudo systemctl start docker



echo "➡️ Running docker compose..."
sudo docker compose up -d --build

echo "✅ Deployment completed successfully! Grafana and ClickHouse are ready."