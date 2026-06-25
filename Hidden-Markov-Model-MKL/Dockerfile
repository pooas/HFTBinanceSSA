# استفاده از اوبونتو ۲۲.۰۴ به عنوان پایه
FROM ubuntu:22.04

# جلوگیری از درخواست‌های تعاملی در زمان نصب
ENV DEBIAN_FRONTEND=noninteractive

# 🌟 نصب ابزارهای پایه، پیش‌نیازهای اینتل و کتابخانه‌های شبکه
# توجه: libboost-all-dev حذف شد و libasio-dev (نسخه مستقل و سبک) جایگزین شد
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    wget \
    gnupg \
    libzmq3-dev \
    libssl-dev \
    libasio-dev \
    libwebsocketpp-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# افزودن مخزن رسمی اینتل برای MKL
RUN wget https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB && \
    apt-key add GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB && \
    echo "deb https://apt.repos.intel.com/oneapi all main" | tee /etc/apt/sources.list.d/oneAPI.list && \
    apt-get update && \
    apt-get install -y intel-oneapi-mkl-devel-2024.0

# تنظیم متغیرهای محیطی برای MKL
ENV LD_LIBRARY_PATH=/opt/intel/oneapi/mkl/latest/lib/intel64:$LD_LIBRARY_PATH
ENV MKLROOT=/opt/intel/oneapi/mkl/latest

WORKDIR /app
COPY . .

# بیلد پروژه به همراه پرچم‌های SSE برای جلوگیری از خطای Pragma
RUN mkdir build && cd build && \
    cmake -DCMAKE_CXX_FLAGS="-msse3 -mfpmath=sse" .. && \
    make

CMD ["./build/hmm_engine"]