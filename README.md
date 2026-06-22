# HFTBinanceSSA

git add .
git commit -m "Add offline clickhouse plugin and update docker-compose"
git push
token:
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

Grafana dashboard sql:

SELECT 
    toStartOfInterval(timestamp, INTERVAL 1 second) AS time,
    avg(evr) AS trend_strength
FROM default.hft_market_data
WHERE $__timeFilter(timestamp) AND evr > 0 -- فیلتر کردن داده‌های قدیمی و صفر
GROUP BY time
ORDER BY time ASC