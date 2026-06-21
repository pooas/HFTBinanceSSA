#!/bin/bash

# توقف در صورت ارور
set -e

echo "➡️ Stopping and cleaning up previous containers..."
# اگر پوشه از قبل وجود داشته باشد، کانتینرها و والیوم‌های آن را متوقف و پاک می‌کند
if [ -d "HFTBinanceSSA" ]; then
  cd HFTBinanceSSA
  sudo docker compose down -v || true
  cd ..
  sudo rm -rf HFTBinanceSSA
fi

echo "➡️ Cloning the repository..."
git clone https://pooas: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@github.com/pooas/HFTBinanceSSA.git

cd HFTBinanceSSA

# حل مشکل خوانده نشدن پلاگین گرافانا
if [ -d "grafana-plugins" ]; then
    echo "➡️ Fixing permissions for Grafana offline plugins..."
    # تغییر مالکیت پوشه به یوزر گرافانا (شناسه 472)
    sudo chown -R 472:472 grafana-plugins
    sudo chmod -R 775 grafana-plugins
fi

# نصب پیش‌نیازها و داکر (اگر نصب نباشند انجام می‌شود)
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

# استارت کردن پروژه
echo "➡️ Running docker compose..."
sudo docker compose up -d

echo "✅ Deployment completed successfully! Grafana should now load the plugin."