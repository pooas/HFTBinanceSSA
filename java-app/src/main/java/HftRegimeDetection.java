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

import java.net.URI;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.Properties;

public class HftRegimeDetection {

    public static class TickEvent {
        public double price;
        public double volume;
        public long timestamp;
        public double ssaTrend; 
        public long ingressNanoTime;
        public double lambda;
        public boolean isFrozen;
        public double bandUpper;
        public double bandLower;
        // اضافه شدن فیلد رژیم بازار به دیتابیس (1 = صعودی، -1 = نزولی)
        public int regime; 
    }

    public static class SsaProcessingHandler implements EventHandler<TickEvent> {
        
        private final QuantDSP.MesaStrategyMEE mesaStrategy = new QuantDSP.MesaStrategyMEE(8.0, 330.0, 150, 5, 3);
        
        private final int MAX_CAPACITY = 1000;
        private final double[] priceHistory = new double[MAX_CAPACITY];
        private int head = 0;
        private int count = 0;

        private final int LLE_WINDOW = 500; 
        private final double[] ssaTrendBuffer = new double[LLE_WINDOW];
        private int ssaHead = 0;
        private boolean ssaBufferFull = false;
        
        private double currentLambda = 0.0;
        private boolean currentRegimeShiftAlert = false;
        private final double[] lambdaHistory = new double[20];
        private int lambdaHead = 0;
        private boolean lambdaBufferFull = false;
        private int currentTau = 2;
        private int currentM = 3;

        // 🌟 متغیر حافظه رژیم بازار (1: Bullish, -1: Bearish)
        private int currentMarketRegime = 1;
        // متغیری برای جلوگیری از حرکت رو به عقبِ خطِ فاصله در روندهای قوی (Trailing Logic)
        private double lastLogicalDistanceLine = 0.0;

        public static class ChaosMath {
            // ... (دقیقاً همان کدهای قبلی calculateAMI و calculateFNN بدون تغییر) ...
            public static int calculateAMI(double[] data, int maxTau, int bins) {
                int n = data.length;
                double[] ami = new double[maxTau + 1];
                double minVal = Double.MAX_VALUE;
                double maxVal = -Double.MAX_VALUE;
                for (double v : data) {
                    if (v < minVal) minVal = v;
                    if (v > maxVal) maxVal = v;
                }
                if (maxVal - minVal < 1e-6) return 1;
                for (int tau = 1; tau <= maxTau; tau++) {
                    int[][] joint = new int[bins][bins];
                    int[] marg1 = new int[bins];
                    int[] marg2 = new int[bins];
                    int validCount = n - tau;
                    for (int i = 0; i < validCount; i++) {
                        int b1 = (int) ((data[i] - minVal) / (maxVal - minVal) * (bins - 1));
                        int b2 = (int) ((data[i + tau] - minVal) / (maxVal - minVal) * (bins - 1));
                        b1 = Math.max(0, Math.min(bins - 1, b1));
                        b2 = Math.max(0, Math.min(bins - 1, b2));
                        joint[b1][b2]++; marg1[b1]++; marg2[b2]++;
                    }
                    double mutualInfo = 0.0;
                    for (int i = 0; i < bins; i++) {
                        for (int j = 0; j < bins; j++) {
                            if (joint[i][j] > 0) {
                                double pxy = (double) joint[i][j] / validCount;
                                double px = (double) marg1[i] / validCount;
                                double py = (double) marg2[j] / validCount;
                                mutualInfo += pxy * Math.log(pxy / (px * py));
                            }
                        }
                    }
                    ami[tau] = mutualInfo;
                }
                for (int tau = 2; tau < maxTau; tau++) {
                    if (ami[tau] < ami[tau - 1] && ami[tau] < ami[tau + 1]) return tau;
                }
                return Math.max(1, maxTau / 2); 
            }

            public static int calculateFNN(double[] data, int tau, int maxM, double rTol) {
                int n = data.length;
                if (n < 50) return 3;
                for (int m = 1; m <= maxM; m++) {
                    int falseNeighbors = 0; int totalNeighbors = 0;
                    int numVectors = n - m * tau;
                    if (numVectors < 10) return m;
                    for (int i = 0; i < numVectors; i++) {
                        double minDistSq = Double.MAX_VALUE; int nearestNeighbor = -1;
                        for (int j = 0; j < numVectors; j++) {
                            if (Math.abs(i - j) > tau) { 
                                double distSq = 0;
                                for (int d = 0; d < m; d++) {
                                    double diff = data[i + d * tau] - data[j + d * tau];
                                    distSq += diff * diff;
                                }
                                if (distSq > 1e-12 && distSq < minDistSq) {
                                    minDistSq = distSq; nearestNeighbor = j;
                                }
                            }
                        }
                        if (nearestNeighbor != -1) {
                            double rM = Math.sqrt(minDistSq);
                            double nextDiff = Math.abs(data[i + m * tau] - data[nearestNeighbor + m * tau]);
                            double ratio = nextDiff / Math.max(rM, 1e-10);
                            if (ratio > rTol) falseNeighbors++;
                            totalNeighbors++;
                        }
                    }
                    if (totalNeighbors > 0 && (double) falseNeighbors / totalNeighbors < 0.05) return m;
                }
                return maxM;
            }
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            priceHistory[head] = event.price;
            double domCycle = mesaStrategy.updateAndGetCycle(event.price);
            
            int L = Math.max(4, (int) Math.round(domCycle / 2.0));
            int N_ssa = L * 2;
            
            if (count >= N_ssa - 1) {
                int K = N_ssa - L + 1;
                double[] data = new double[N_ssa];
                for (int i = 0; i < N_ssa; i++) {
                    data[i] = priceHistory[(head - N_ssa + 1 + i + MAX_CAPACITY) % MAX_CAPACITY];
                }
                
                SimpleMatrix X = new SimpleMatrix(L, K);
                for (int j = 0; j < K; j++) {
                    for (int i = 0; i < L; i++) X.set(i, j, data[j + i]);
                }
                
                SimpleSVD<SimpleMatrix> svd = X.svd();
                SimpleMatrix U = svd.getU();
                SimpleMatrix V = svd.getV();
                
                // 1. استخراج ترند مرکزی بسیار نرم (PC0)
                double pc0 = svd.getW().get(0, 0) * (U.get(L - 1, 0) * V.get(K - 1, 0));

                // 2. محاسبه واریانس نویز (فاصله قیمت تا ترند مرکزی) بر اساس مقالات
                double noiseVariance = 0.0;
                for (int i = 0; i < N_ssa; i++) {
                    double diff = data[i] - pc0;
                    noiseVariance += diff * diff;
                }
                double noiseStdDev = Math.sqrt(noiseVariance / N_ssa);
                
                // 3. ایجاد فاصله منطقی (انرژی پسماند با ضریب 2 برای جلوگیری کامل از Whipsaw)
                double logicalDistance = noiseStdDev * 2.0; 
                double currentLineVal;

                // 4. ماشین وضعیت تغییر رژیم (Change-Point Regime Machine)
                if (currentMarketRegime == 1) { // رژیم صعودی فعلی
                    currentLineVal = pc0 - logicalDistance; // خط در نقش حمایت (پایین قیمت)
                    
                    // تشخیص تغییر رژیم به نزولی: شکست قطعی حمایت
                    if (event.price < currentLineVal) {
                        currentMarketRegime = -1; // تغییر رژیم!
                        currentLineVal = pc0 + logicalDistance; // پرش خط به بالا (مقاومت)
                    }
                } else { // رژیم نزولی فعلی
                    currentLineVal = pc0 + logicalDistance; // خط در نقش مقاومت (بالای قیمت)
                    
                    // تشخیص تغییر رژیم به صعودی: شکست قطعی مقاومت
                    if (event.price > currentLineVal) {
                        currentMarketRegime = 1; // تغییر رژیم!
                        currentLineVal = pc0 - logicalDistance; // پرش خط به پایین (حمایت)
                    }
                }

                event.ssaTrend = currentLineVal;
                event.regime = currentMarketRegime;
                
            } else {
                event.ssaTrend = event.price;
                event.regime = currentMarketRegime;
            }

            // --- بخش محاسبه لیاپانوف ---
            ssaTrendBuffer[ssaHead] = event.ssaTrend;
            ssaHead = (ssaHead + 1) % LLE_WINDOW;
            if (ssaHead == 0) ssaBufferFull = true;

            if (ssaBufferFull) {
                double[] flatBuffer = new double[LLE_WINDOW];
                for (int i = 0; i < LLE_WINDOW; i++) flatBuffer[i] = ssaTrendBuffer[(ssaHead + i) % LLE_WINDOW];

                if (sequence % 500 == 0) {
                    currentTau = ChaosMath.calculateAMI(flatBuffer, 30, 20); 
                    currentM = ChaosMath.calculateFNN(flatBuffer, currentTau, 6, 15.0); 
                }

                if (sequence % 10 == 0) {
                    currentLambda = QuantDSP.LyapunovEstimator.calculateRigorousLLE(
                            flatBuffer, currentM, currentTau, currentTau * 2, 5
                    );
                    
                    lambdaHistory[lambdaHead] = currentLambda;
                    lambdaHead = (lambdaHead + 1) % 20; 
                    if (lambdaHead == 0) lambdaBufferFull = true;

                    if (lambdaBufferFull) {
                        double avgLambda = 0;
                        for (double l : lambdaHistory) avgLambda += l;
                        avgLambda /= 20;
                        currentRegimeShiftAlert = (currentLambda > 0.05) && (currentLambda > avgLambda * 1.40);
                    }
                }
            }

            event.lambda = currentLambda;
            event.isFrozen = currentRegimeShiftAlert;

            head = (head + 1) % MAX_CAPACITY;
            if (count < MAX_CAPACITY) count++;
        }
    }

    public static class ClickHouseBatchHandler implements EventHandler<TickEvent> {
        private Connection connection;
        private PreparedStatement statement;
        private final int batchSizeThreshold = 1000;
        private int currentBatchSize = 0;

        public ClickHouseBatchHandler() {
            try {
                String url = "jdbc:ch://clickhouse:8123/default";
                
                // استفاده از Properties برای ارسال امن مشخصات
                Properties properties = new Properties();
                properties.setProperty("user", "default");
                properties.setProperty("password", "hft123");
                properties.setProperty("compress", "0");

                this.connection = DriverManager.getConnection(url, properties);

                String createTableSQL = "CREATE TABLE IF NOT EXISTS hft_market_data (" +
                        "timestamp DateTime64(3), " +
                        "sequence UInt64, " +
                        "price Float64, " +
                        "volume Float64, " +
                        "ssa_trend Float64, " +
                        "lambda Float64, " +
                        "is_frozen UInt8, " +
                        "regime Int8" +
                        ") ENGINE = MergeTree() ORDER BY (timestamp, sequence)";
                this.connection.createStatement().execute(createTableSQL);

                String sql = "INSERT INTO hft_market_data (timestamp, sequence, price, volume, ssa_trend, lambda, is_frozen, regime) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
                this.statement = connection.prepareStatement(sql);
                System.out.println("✅ ClickHouse Connection Established Successfully!");
                
            } catch (SQLException e) {
                System.err.println("🔴 CRITICAL: ClickHouse Connection Failed: " + e.getMessage());
                System.exit(1); 
            }
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            if (statement == null) return;
            try {
                statement.setTimestamp(1, new java.sql.Timestamp(event.timestamp));
                statement.setLong(2, sequence);
                statement.setDouble(3, event.price);
                statement.setDouble(4, event.volume);
                statement.setDouble(5, event.ssaTrend);
                statement.setDouble(6, event.lambda);
                statement.setInt(7, event.isFrozen ? 1 : 0);
                statement.setInt(8, event.regime); // ثبت رژیم در دیتابیس
                statement.addBatch();
                currentBatchSize++;
                if (currentBatchSize >= batchSizeThreshold || endOfBatch) flush();
            } catch (SQLException e) {
                System.err.println("⚠️ Error inserting tick: " + e.getMessage());
            }
        }

        private void flush() {
            if (currentBatchSize == 0) return;
            try {
                statement.executeBatch(); 
                currentBatchSize = 0;
            } catch (SQLException e) {
                System.err.println("🔴 Failed to flush batch: " + e.getMessage());
                currentBatchSize = 0; 
            }
        }
    }

    public static class BinanceProducer extends WebSocketClient {
        private final RingBuffer<TickEvent> ringBuffer;
        public BinanceProducer(URI serverUri, RingBuffer<TickEvent> ringBuffer) {
            super(serverUri); this.ringBuffer = ringBuffer;
        }
        @Override
        public void onOpen(ServerHandshake handshakedata) { System.out.println("🟢 Connected to Binance High-Frequency Stream!"); }
        @Override
        public void onMessage(String message) {
            try {
                JsonObject json = JsonParser.parseString(message).getAsJsonObject();
                if (!json.has("p")) return; 
                System.out.print("."); System.out.flush();
                
                long sequence = ringBuffer.next();
                try {
                    TickEvent event = ringBuffer.get(sequence);
                    event.price = json.get("p").getAsDouble();
                    event.volume = json.get("q").getAsDouble();
                    event.timestamp = json.get("T").getAsLong();
                    event.ingressNanoTime = System.nanoTime(); 
                } finally {
                    ringBuffer.publish(sequence); 
                }
            } catch (Exception e) {
                // چاپ خطاهای پنهان
                System.err.println("\n🔴 Error parsing or inserting tick: " + e.getMessage());
                e.printStackTrace();
            }
        }
        @Override public void onClose(int code, String reason, boolean remote) {}
        @Override public void onError(Exception ex) {}
    }

    public static void main(String[] args) throws Exception {
        Disruptor<TickEvent> disruptor = new Disruptor<>(TickEvent::new, 1024, DaemonThreadFactory.INSTANCE, ProducerType.SINGLE, new BusySpinWaitStrategy());
        disruptor.handleEventsWith(new SsaProcessingHandler()).then(new ClickHouseBatchHandler());
        RingBuffer<TickEvent> ringBuffer = disruptor.start();
        new BinanceProducer(new URI("wss://stream.binance.com:9443/ws/btcusdt@aggTrade"), ringBuffer).connectBlocking(); 
        Thread.currentThread().join();
    }
}