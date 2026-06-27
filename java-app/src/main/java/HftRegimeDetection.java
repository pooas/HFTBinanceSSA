import com.lmax.disruptor.BusySpinWaitStrategy;
import com.lmax.disruptor.EventHandler;
import com.lmax.disruptor.RingBuffer;
import com.lmax.disruptor.dsl.Disruptor;
import com.lmax.disruptor.dsl.ProducerType;
import com.lmax.disruptor.util.DaemonThreadFactory;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.ejml.simple.SimpleMatrix;
import org.ejml.simple.SimpleSVD;
import org.zeromq.SocketType;
import org.zeromq.ZContext;
import org.zeromq.ZMQ;

import java.net.URI;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.SQLException;

public class HftRegimeDetection {
    
    // متغیرهای رژیم دریافتی از C++ (مستقل از محاسبات SSA)
    public static volatile int currentHmmRegime = 0; 
    public static volatile double currentProbTrend = 0.0;  
    public static volatile double currentProbCrisis = 0.0; 
    
    public static class TickEvent {
        public double price;
        public double volume;
        public long timestamp;
        
        // مقادیر دریافتی از C++
        public int hmmRegime;
        public double hmmProbTrend;   
        public double hmmProbCrisis;  
        
        // خروجی‌های موتور SSA
        public double ssaTrend; 
        public double ssaSlope;
        public double value2;
        public double dominantCycle;
        public int isFrozen;
    }

    public static class SsaProcessingHandler implements EventHandler<TickEvent> {
        
        private final int MAX_CAPACITY = 2000;
        private final double[] priceHistory = new double[MAX_CAPACITY];
        private final double[] v2RawHistory = new double[MAX_CAPACITY];
        private final double[] vossHistory = new double[MAX_CAPACITY];
        private int head = 0;
        private int count = 0;

        // کلاچ پردازشی (Throttle) برای جلوگیری از انفجار CPU در تیک‌دیتا
        private long lastSvdTime = 0;
        private double lastSvdPrice = 0.0;
        
        // حافظه برای فریز کردن سیگنال
        private double lastSsaTrend = 0.0;
        private double prevSsaTrend = 0.0;
        private double lastValue2 = 0.0;
        private double currentDominantCycle = 20.0;
        
        // ==========================================
        // فیلترهای DSP (High-Performance O(1))
        // ==========================================
        private double hp_1 = 0, hp_2 = 0;
        private double p_1 = 0, p_2 = 0;
        private double filt_1 = 0, filt_2 = 0;
        private int barCount = 0;

        private double applySuperSmoother(double price, double period) {
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2 * a1 * Math.cos(1.414 * Math.PI / period);
            double c2 = b1;
            double c3 = -a1 * a1;
            double c1 = 1 - c2 - c3;

            double filt = c1 * (price + p_1) / 2.0 + c2 * filt_1 + c3 * filt_2;
            
            p_2 = p_1; p_1 = price;
            filt_2 = filt_1; filt_1 = filt;
            return filt;
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            barCount++;
            double currentPrice = event.price;
            
            // شبیه‌سازی استخراج چرخه (یک Homodyne ساده شده برای حفظ سرعت)
            // در حالت واقعی می‌توان فیلتر MEE را اضافه کرد، اما برای تیک‌دیتا معمولا ثابت فرض می‌شود یا کند آپدیت می‌شود
            currentDominantCycle = 20.0; 

            priceHistory[head] = currentPrice;
            
            // تولید Value2 خام (استفاده از SuperSmoother به عنوان پایه)
            double v2Raw = applySuperSmoother(currentPrice, 10.0);
            v2RawHistory[head] = v2Raw;

            // =======================================================
            // 🌟 موتور SSA و استخراج PC1 با استفاده از EJML SVD
            // =======================================================
            long now = System.currentTimeMillis();
            
            // کلاچ پردازشی: فقط اگر 500 میلی‌ثانیه گذشته باشد یا قیمت 0.5 دلار جابجا شده باشد ماتریس حل می‌شود
            if (count > 50 && (now - lastSvdTime > 500 || Math.abs(currentPrice - lastSvdPrice) >= 0.5)) {
                
                int L = Math.max(4, (int) Math.round(currentDominantCycle / 2.0));
                int N_ssa = L * 2;
                int K = N_ssa - L + 1;
                
                SimpleMatrix X = new SimpleMatrix(L, K);
                for (int j = 0; j < K; j++) {
                    for (int i = 0; i < L; i++) {
                        int idx = (head - N_ssa + 1 + i + j + MAX_CAPACITY) % MAX_CAPACITY;
                        X.set(i, j, priceHistory[idx]);
                    }
                }
                
                try {
                    SimpleSVD<SimpleMatrix> svd = X.svd();
                    SimpleMatrix W = svd.getW();
                    SimpleMatrix U = svd.getU();
                    SimpleMatrix V = svd.getV();
                    
                    // پیدا کردن بزرگترین مقدار ویژه
                    double maxSigma = -1.0;
                    int maxIdx = 0;
                    int minDim = Math.min(L, K);
                    for (int i = 0; i < minDim; i++) {
                        double s = Math.abs(W.get(i, i));
                        if (s > maxSigma) { maxSigma = s; maxIdx = i; }
                    }
                    
                    // استخراج نقطه پایانی بُعد اول (PC1 Pure Trend)
                    prevSsaTrend = lastSsaTrend;
                    lastSsaTrend = maxSigma * U.get(L - 1, maxIdx) * V.get(K - 1, maxIdx);
                    
                } catch (Exception e) {
                    // در صورت سینگولار شدن ماتریس، مقدار قبلی حفظ می‌شود
                }
                
                lastSvdTime = now;
                lastSvdPrice = currentPrice;
            }

            double currentSsaSlope = lastSsaTrend - prevSsaTrend;

            // =======================================================
            // 🌟 فیلتر پیش‌بین Voss
            // =======================================================
            double delayFloat = Math.max(2.0, currentDominantCycle / 8.0);
            int delayInt = (int) delayFloat;
            double delayFrac = delayFloat - delayInt;

            double vossRaw = v2Raw;
            if (count > delayInt + 1) {
                int idx1 = (head - delayInt + MAX_CAPACITY) % MAX_CAPACITY;
                int idx2 = (head - delayInt - 1 + MAX_CAPACITY) % MAX_CAPACITY;
                
                double pastVal = (1 - delayFrac) * v2RawHistory[idx1] + delayFrac * v2RawHistory[idx2];
                vossRaw = 3.5 * v2Raw - 2.5 * pastVal;
            }
            vossHistory[head] = vossRaw;

            // میانگین متحرک 3 دوره‌ای روی خروجی Voss
            double vossSmoothed = vossRaw;
            if (count > 2) {
                int idx1 = (head - 1 + MAX_CAPACITY) % MAX_CAPACITY;
                int idx2 = (head - 2 + MAX_CAPACITY) % MAX_CAPACITY;
                vossSmoothed = (vossRaw + vossHistory[idx1] + vossHistory[idx2]) / 3.0;
            }

            // =======================================================
            // 🌟 کلید قطع و وصل کوانتومی (Freeze Logic)
            // =======================================================
            int isFrozen = 0;
            double finalValue2 = 0.0;
            
            if (currentSsaSlope < 0) {
                // استخوان‌بندی بازار نزولی است -> سیگنال فریز می‌شود
                finalValue2 = lastValue2; 
                isFrozen = 1;
            } else {
                // بازار صعودی است -> سیگنال آزادانه حرکت می‌کند
                finalValue2 = vossSmoothed;
                lastValue2 = vossSmoothed;
                isFrozen = 0;
            }

            // =======================================================
            // مقداردهی Event برای ارسال به دیتابیس
            // =======================================================
            event.ssaTrend = lastSsaTrend;
            event.ssaSlope = currentSsaSlope;
            event.value2 = finalValue2;
            event.isFrozen = isFrozen;
            event.dominantCycle = currentDominantCycle;
            
            // رژیم‌های C++ که کاملا ایزوله هستند
            event.hmmRegime = currentHmmRegime;
            event.hmmProbTrend = currentProbTrend;
            event.hmmProbCrisis = currentProbCrisis;

            head = (head + 1) % MAX_CAPACITY;
            if (count < MAX_CAPACITY) count++;
        }
    }

    public static class ClickHouseBatchHandler implements EventHandler<TickEvent> {
        private Connection connection;
        private PreparedStatement statement;
        private final int batchSizeThreshold = 500;
        private int currentBatchSize = 0;

        public ClickHouseBatchHandler() {
            try {
                String host = System.getenv("CLICKHOUSE_HOST");
                if (host == null || host.trim().isEmpty()) host = "clickhouse"; 
                String user = System.getenv("CLICKHOUSE_USER");
                if (user == null || user.trim().isEmpty()) user = "default";
                String password = System.getenv("CLICKHOUSE_PASSWORD");
                if (password == null) password = ""; 
                
                String url = "jdbc:ch://" + host + ":8123/default?compress=0";
                this.connection = DriverManager.getConnection(url, user, password);
                
                String sql = "INSERT INTO hft_market_data (timestamp, price, volume, ssa_trend, is_frozen, hmm_regime, hmm_prob_trend, hmm_prob_crisis, value2, dom_cycle) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
                this.statement = connection.prepareStatement(sql);
            } catch (SQLException e) {
                System.err.println("\n🔴 CRITICAL: ClickHouse Failed: " + e.getMessage());
                System.exit(1); 
            }
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            if (statement == null) return;
            try {
                statement.setTimestamp(1, new java.sql.Timestamp(event.timestamp));
                statement.setDouble(2, event.price);
                statement.setDouble(3, event.volume);
                statement.setDouble(4, event.ssaTrend);
                statement.setInt(5, event.isFrozen);
                statement.setInt(6, event.hmmRegime);
                statement.setDouble(7, event.hmmProbTrend);
                statement.setDouble(8, event.hmmProbCrisis);
                statement.setDouble(9, event.value2);
                statement.setDouble(10, event.dominantCycle);
                
                statement.addBatch();
                currentBatchSize++;
                if (currentBatchSize >= batchSizeThreshold || endOfBatch) flush();
            } catch (SQLException e) {}
        }

        private void flush() {
            if (currentBatchSize == 0) return;
            try { statement.executeBatch(); currentBatchSize = 0; } 
            catch (SQLException e) { currentBatchSize = 0; }
        }
    }

    public static class BinanceProducer extends WebSocketClient {
        private final RingBuffer<TickEvent> ringBuffer;
        public BinanceProducer(URI serverUri, RingBuffer<TickEvent> ringBuffer) {
            super(serverUri); 
            this.ringBuffer = ringBuffer;
            this.setConnectionLostTimeout(0); 
        }
        @Override public void onOpen(ServerHandshake handshakedata) {}
        @Override public void onMessage(String message) {
            try {
                JsonObject json = JsonParser.parseString(message).getAsJsonObject();
                if (!json.has("p")) return; 
                long sequence = ringBuffer.next();
                try {
                    TickEvent event = ringBuffer.get(sequence);
                    event.price = json.get("p").getAsDouble();
                    event.volume = json.get("q").getAsDouble();
                    event.timestamp = json.get("T").getAsLong();
                } finally { ringBuffer.publish(sequence); }
            } catch (Throwable e) {}
        }
        @Override public void onClose(int code, String reason, boolean remote) {
            try { Thread.sleep(5000); this.reconnect(); } catch (InterruptedException e) {}
        }
        @Override public void onError(Exception ex) {}
    }

    public static void main(String[] args) throws Exception {
        
        // پردازش مستقل پیام‌های ZeroMQ از C++
        Thread zmqThread = new Thread(() -> {
            try (ZContext context = new ZContext()) {
                ZMQ.Socket subscriber = context.createSocket(SocketType.SUB);
                String zmqHost = System.getenv("ZMQ_HOST");
                if (zmqHost == null || zmqHost.trim().isEmpty()) zmqHost = "localhost";
                String zmqPort = System.getenv("ZMQ_PORT");
                if (zmqPort == null || zmqPort.trim().isEmpty()) zmqPort = "5555";
                
                String address = "tcp://" + zmqHost + ":" + zmqPort;
                subscriber.connect(address);
                subscriber.subscribe(new byte[0]); 
                
                System.out.println("🔗 C++ Regime Subscriber active on " + address);
                
                while (!Thread.currentThread().isInterrupted()) {
                    try {
                        String msg = subscriber.recvStr();
                        if (msg != null && msg.startsWith("REGIME|")) {
                            String[] parts = msg.substring(7).trim().split(",");
                            if (parts.length >= 4) {
                                currentHmmRegime = Integer.parseInt(parts[0]);
                                currentProbTrend = Double.parseDouble(parts[2]);
                                currentProbCrisis = Double.parseDouble(parts[3]);
                            }
                        }
                    } catch (Exception ex) { }
                }
            } catch (Exception e) {}
        });
        zmqThread.setDaemon(true);
        zmqThread.start();

        Disruptor<TickEvent> disruptor = new Disruptor<>(TickEvent::new, 2048, DaemonThreadFactory.INSTANCE, ProducerType.SINGLE, new BusySpinWaitStrategy());
        disruptor.handleEventsWith(new SsaProcessingHandler()).then(new ClickHouseBatchHandler());
        RingBuffer<TickEvent> ringBuffer = disruptor.start();
        new BinanceProducer(new URI("wss://stream.binance.com:9443/ws/btcusdt@aggTrade"), ringBuffer).connectBlocking(); 
        Thread.currentThread().join();
    }
}