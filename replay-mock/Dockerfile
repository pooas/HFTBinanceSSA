FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY replay_server.py .

EXPOSE 8080

ENV CSV_PATH=/data/ticks.csv
ENV MOCK_PORT=8080
ENV REPLAY_SPEED=0
ENV REPLAY_LOOP=false

CMD ["python", "replay_server.py"]
