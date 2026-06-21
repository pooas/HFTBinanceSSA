#!/bin/bash

# توقف خودکار اسکریپت در صورت بروز هرگونه ارور
set -e

echo "➡️ Starting deployment process..."

# ۱. کلون کردن ریپازیتوری با استفاده از توکن
echo "➡️ Cloning the repository..."
# در صورت وجود پوشه از قبل، آن را پاک می‌کند تا تداخل ایجاد نشود
rm -rf HFTBinanceSSA
git clone https://pooas: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@github.com/pooas/HFTBinanceSSA.git

# ورود به پوشه پروژه
cd HFTBinanceSSA

# ۲. نصب پیش‌نیازها و کلیدهای داکر
echo "➡️ Installing Docker prerequisites..."
sudo apt update -y
sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

# ۳. اضافه کردن سورس داکر به اوبونتو
echo "➡️ Adding Docker repository..."
sudo tee /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}")
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

# ۴. نصب پکیج‌های اصلی داکر
echo "➡️ Installing Docker packages..."
sudo apt update -y
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# استارت کردن و فعال‌سازی داکر برای اجرای خودکار بعد از ریبوت سرور
echo "➡️ Starting Docker service..."
sudo systemctl enable docker
sudo systemctl start docker

# ۵. اجرای کانتینرها
echo "➡️ Running docker compose..."
sudo docker compose up -d

echo "✅ Deployment completed successfully! HFT system is running."

docker logs -f hft_java_producer