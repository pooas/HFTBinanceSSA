# Base toolchain + all dependencies for both nodes
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 🌟 1. نصب پکیج‌های استاندارد (بدون cppzmq-dev)
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    wget \
    gnupg \
    libzmq3-dev \
    libczmq-dev \
    libssl-dev \
    libasio-dev \
    libwebsocketpp-dev \
    nlohmann-json3-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# 🌟 2. دانلود مستقیم هدرهای C++ برای ZeroMQ (حل مشکل Exit Code 100)
RUN wget https://raw.githubusercontent.com/zeromq/cppzmq/master/zmq.hpp -O /usr/local/include/zmq.hpp && \
    wget https://raw.githubusercontent.com/zeromq/cppzmq/master/zmq_addon.hpp -O /usr/local/include/zmq_addon.hpp

WORKDIR /app

# کپی کردن سورس‌کدها
COPY . .

# بیلد کردن پروژه
RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native" .. && \
    make -j$(nproc)

# مرحله اجرای نهایی (سبک‌سازی ایمیج)
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libzmq3-dev \
    libczmq-dev \
    libssl-dev \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# کپی کردن فایل اجرایی از مرحله بیلد
# (نام فایل اجرایی را با توجه به تنظیمات CMakeLists خودتان جایگزین کنید، مثلاً sg_dsp_node)
COPY --from=builder /app/build/sg_dsp_node .

CMD ["./sg_dsp_node"]