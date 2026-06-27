FROM python:3.10-slim

# جلوگیری از بافر شدن لاگ‌های پایتون در داکر
ENV PYTHONUNBUFFERED=1

WORKDIR /app

# نصب پکیج‌های بیلد برای cvxpy در لینوکس
RUN apt-get update && apt-get install -y \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY macro_engine.py .

CMD ["python", "macro_engine.py"]