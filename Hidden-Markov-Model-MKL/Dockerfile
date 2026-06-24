# استفاده از ایمیج رسمی اینتل که شامل ابزارهای OneAPI و MKL است
FROM intel/oneapi-basekit:devel-ubuntu22.04

# جلوگیری از توقف نصب در مراحل تعاملی اوبونتو
ENV DEBIAN_FRONTEND=noninteractive

# نصب پیش‌نیازهای حیاتی: CMake, ZeroMQ و OpenSSL
# نصب پیش‌نیازهای حیاتی و شبکه‌ای
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libzmq3-dev \
    libssl-dev \
    libboost-all-dev \
    libwebsocketpp-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
# تعیین پوشه کاری داخل کانتینر
WORKDIR /app

# کپی کردن تمام سورس‌کدهای C++ به داخل کانتینر
COPY . /app/

# ساخت پوشه build، اجرای CMake و کامپایل کدها با استفاده از تمام هسته‌های پردازشی
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# اجرای فایل کامپایل‌شده نهایی
CMD ["./build/hmm_engine"]