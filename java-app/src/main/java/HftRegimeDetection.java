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
    
    public static volatile int currentHmmRegime = 0; 
    public static volatile double currentProbTrend = 0.0;  
    public static volatile double currentProbCrisis = 0.0; 
    
    // 🌟 متغیرهای فرمانده (پایتون) - مجهز به سیستم فیزیک سینماتیک
    public static volatile double macroL1Value = 0.0;
    public static volatile double macroL1Slope = 0.0;
    public static volatile double macroL1Accel = 0.0;
    public static volatile double projectedMacroSlope = 0.0;

    public static class TickEvent {
        public double price;
        public double volume;
        public long timestamp;
        public double ssaTrend; 
        public long ingressNanoTime;
        public double lambda;
        public boolean isFrozen;
        public int regime;
        
        public int hmmRegime;
        public double hmmProbTrend;   
        public double hmmProbCrisis;  
        
        public double pc0;         
        public double evr;         
        public double bandUpper;   
        public double bandLower;
        
        public double vress;
        public double eigenGap;

        public double value2;
        public double domCycle;

        public double momentumSignal;
        public double regimeWeight;
        public double gatedMomentum;
        public double positionSize;
        public double dynamicStopLoss;
        public int crisisCapActive;
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
        
        private final int RESIDUAL_WINDOW = 100;
        private final double[] residualHistory = new double[RESIDUAL_WINDOW];
        private int residualHead = 0;
        private boolean residualFull = false;

        private double currentLambda = 0.0;
        private boolean currentRegimeShiftAlert = false;
        private final double[] lambdaHistory = new double[20];
        private int lambdaHead = 0;
        private boolean lambdaBufferFull = false;
        private int currentTau = 2;
        private int currentM = 3;

        private int currentMarketRegime = 1;
        private double smoothedDistance = 0.0;

        private double emaPc0 = 0.0;
        private double emaEvr = 0.0;
        private double emaGapFactor = 0.0;

        private double emaProbTrend = 0.0;
        private double emaProbCrisis = 0.0;
        private boolean probInitialized = false;

        private double lastEmaPc0 = 0.0;
        private double lastValue2 = 0.0;
        
        // 🌟 متغیرهای سیستم KMT و State Machine جدید
        private double smoothedVelocity = 0.0; 
        private int strictMicroTrend = 0; 
        
        private boolean isSidewaysState = true;
        private boolean isCrisisState = false;
        private long lastTimestamp = 0;

        public static class ChaosMath {
            public static int calculateAMI(double[] data, int maxTau, int bins) {
                return Math.max(1, maxTau / 2); 
            }
            public static int calculateFNN(double[] data, int tau, int maxM, double rTol) {
                return maxM;
            }
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            priceHistory[head] = event.price;
            double domCycle = mesaStrategy.updateAndGetCycle(event.price);
            
            event.domCycle = domCycle;
            boolean isRatchetFrozen = false;
            
            // =========================================================================
            // 🌟 1. T-EMA (Time-Weighted Smoothing) for HMM Probabilities
            // تبدیل پرش‌های 1 دقیقه‌ای C++ به منحنی‌های نرم و پیوسته بر اساس زمان
            // =========================================================================
            if (lastTimestamp == 0) lastTimestamp = event.timestamp;
            long dt = Math.max(1, event.timestamp - lastTimestamp);
            lastTimestamp = event.timestamp;
            
            double tauMs = 15000.0; // ثابت زمانی 15 ثانیه برای هموارسازی نرم
            double alphaProb = 1.0 - Math.exp(-dt / tauMs);

            if (!probInitialized) {
                emaProbTrend = currentProbTrend;
                emaProbCrisis = currentProbCrisis;
                probInitialized = true;
            } else {
                emaProbTrend += alphaProb * (currentProbTrend - emaProbTrend);
                emaProbCrisis += alphaProb * (currentProbCrisis - emaProbCrisis);
            }
            
            int L = Math.max(4, (int) Math.round(domCycle / 2.0));
            int N_ssa = L * 2;
            
            if (count >= N_ssa - 1) {
                // --- استخراج ماتریس و محاسبه SVD ---
                int K = N_ssa - L + 1;
                double[] data = new double[N_ssa];
                for (int i = 0; i < N_ssa; i++) {
                    data[i] = priceHistory[(head - N_ssa + 1 + i + MAX_CAPACITY) % MAX_CAPACITY];
                }
                
                double mean = 0.0;
                for (int i = 0; i < N_ssa; i++) mean += data[i];
                mean /= N_ssa;

                double frobeniusSq = 0.0;
                SimpleMatrix X = new SimpleMatrix(L, K);
                for (int j = 0; j < K; j++) {
                    for (int i = 0; i < L; i++) {
                        double val = data[j + i] - mean;
                        X.set(i, j, val);
                        frobeniusSq += val * val; 
                    }
                }
                
                double rawPc0, rawEvr, rawGapFactor;

                if (frobeniusSq < 1e-10) {
                    rawPc0 = mean;
                    rawEvr = 100.0; 
                    rawGapFactor = 0.0;
                } else {
                    SimpleSVD<SimpleMatrix> svd = X.svd();
                    SimpleMatrix U = svd.getU();
                    SimpleMatrix V = svd.getV();
                    SimpleMatrix W = svd.getW();
                    
                    int numSingularValues = Math.min(L, K);
                    double sigma0 = -1.0;
                    int maxIndex = 0;
                    
                    double[] sigmas = new double[numSingularValues];
                    for (int c = 0; c < numSingularValues; c++) {
                        double s = Math.abs(W.get(c, c));
                        sigmas[c] = s;
                        if (s > sigma0) {
                            sigma0 = s;
                            maxIndex = c;
                        }
                    }
                    
                    double sigma1 = 0.0;
                    for (int c = 0; c < numSingularValues; c++) {
                        if (c != maxIndex && sigmas[c] > sigma1) {
                            sigma1 = sigmas[c];
                        }
                    }
                    
                    rawPc0 = mean + (sigma0 * U.get(L - 1, maxIndex) * V.get(K - 1, maxIndex));
                    rawEvr = Math.min((sigma0 * sigma0) / frobeniusSq, 1.0) * 100.0;
                    double gapRatio = sigma0 / Math.max(sigma1, 1e-9);
                    rawGapFactor = 1.0 / Math.max(1.0, gapRatio);
                }

                double alphaMetrics = 0.05;  
                
                if (emaPc0 == 0.0) {
                    emaPc0 = rawPc0;
                    emaEvr = rawEvr;
                    emaGapFactor = rawGapFactor;
                } else {
                    emaEvr = alphaMetrics * rawEvr + (1.0 - alphaMetrics) * emaEvr;
                    emaGapFactor = alphaMetrics * rawGapFactor + (1.0 - alphaMetrics) * emaGapFactor;
                    
                    double evrFactor = Math.min(emaEvr / 100.0, 1.0);
                    double adaptiveAlpha = 0.01 + 0.15 * Math.pow(evrFactor, 2);

                    emaPc0 = adaptiveAlpha * rawPc0 + (1.0 - adaptiveAlpha) * emaPc0;
                }

                // محاسبه واریانس نویز (Vress)
                double currentResidual = event.price - emaPc0;
                residualHistory[residualHead] = currentResidual;
                residualHead = (residualHead + 1) % RESIDUAL_WINDOW;
                if (residualHead == 0) residualFull = true;

                double noiseStdDev = 0.0;
                int activeResCount = residualFull ? RESIDUAL_WINDOW : residualHead;
                
                if (activeResCount > 1) {
                    double resMean = 0;
                    for (int i = 0; i < activeResCount; i++) resMean += residualHistory[i];
                    resMean /= activeResCount;
                    double resVar = 0;
                    for (int i = 0; i < activeResCount; i++) {
                        double diff = residualHistory[i] - resMean;
                        resVar += diff * diff;
                    }
                    noiseStdDev = Math.sqrt(resVar / (activeResCount - 1));
                } else if (activeResCount == 1) {
                    noiseStdDev = Math.abs(residualHistory[0]);
                }

                double evrFactor = Math.min(emaEvr / 100.0, 1.0); 
                double rawDistance = noiseStdDev * Math.max(1.0, 1.0 + 4.0 * (1.0 - evrFactor) + 2.0 * emaGapFactor); 
                
                if (smoothedDistance == 0.0) smoothedDistance = rawDistance;
                smoothedDistance = 0.05 * rawDistance + 0.95 * smoothedDistance;

                event.pc0 = emaPc0;
                event.evr = emaEvr;
                event.vress = noiseStdDev;
                event.eigenGap = emaGapFactor;
                
                event.hmmRegime = currentHmmRegime;
                event.hmmProbTrend = emaProbTrend;
                event.hmmProbCrisis = emaProbCrisis;

                event.bandUpper = emaPc0 + smoothedDistance;
                event.bandLower = emaPc0 - smoothedDistance;

                // =========================================================================
                // 🌟 2. Micro-Macro Fusion (MMF) & Schmitt Trigger
                // =========================================================================
                
                // ترکیب احتمال روند ماکرو (C++) با قدرت روند میکرو (SSA EVR)
                // اگر کندل ۱ دقیقه‌ای صعودی باشد اما تیک‌ها نویزی شوند (EVR پایین)، احتمال روند فورا کاهش می‌یابد
                double normEvr = Math.min(emaEvr / 75.0, 1.0); 
                double fusedTrendProb = emaProbTrend * normEvr;

                // اشمیت تریگر (Hysteresis) برای حذف قطعی پرش بین رژیم‌ها
                if (isSidewaysState) {
                    if (fusedTrendProb > 0.55) isSidewaysState = false; // خروج از سایدوی (نیاز به قدرت بالا)
                } else {
                    if (fusedTrendProb < 0.35) isSidewaysState = true;  // بازگشت به سایدوی (نیاز به افت شدید)
                }

                if (isCrisisState) {
                    if (emaProbCrisis < 0.30) isCrisisState = false;
                } else {
                    if (emaProbCrisis > 0.50) isCrisisState = true;
                }

                boolean isSideways = isSidewaysState;

                // =========================================================================
                // 🌟 THE GAME CHANGER: KINEMATIC MAGNETIC TAPE (KMT) + MG-SSA
                // =========================================================================
                
                if (lastValue2 == 0.0) lastValue2 = emaPc0;

                double pc0Velocity = emaPc0 - lastEmaPc0;
                smoothedVelocity = 0.1 * pc0Velocity + 0.9 * smoothedVelocity;

                // ۳. محاسبه میدان دافعه (Dynamic Repulsion) - اکنون با Fused Prob آپدیت می‌شود
                double repulsionForce = noiseStdDev * Math.max(1.5, 1.0 + (emaEvr / 50.0)) * Math.pow(fusedTrendProb + 0.5, 2);
                
                double macroBias = (macroL1Value != 0.0) ? (event.price - macroL1Value) : 0.0;
                boolean strongMacroBear = (projectedMacroSlope < 0) && (macroBias < 0);
                boolean strongMacroBull = (projectedMacroSlope > 0) && (macroBias > 0);

                if (strictMicroTrend == 1 && event.price < lastValue2 - noiseStdDev && !strongMacroBull) {
                    strictMicroTrend = -1; 
                } else if (strictMicroTrend == -1 && event.price > lastValue2 + noiseStdDev && !strongMacroBear) {
                    strictMicroTrend = 1;  
                } else if (strictMicroTrend == 0) {
                    strictMicroTrend = (smoothedVelocity >= 0) ? 1 : -1;
                }

                double targetLine;
                if (isSideways) {
                    targetLine = event.price; 
                } else {
                    if (strictMicroTrend == 1 || strongMacroBull) {
                        targetLine = emaPc0 - repulsionForce; 
                    } else {
                        targetLine = emaPc0 + repulsionForce;
                    }
                }

                double smoothAlpha = isSideways ? 0.30 : 0.03; 
                double proposedLine = lastValue2 + smoothAlpha * (targetLine - lastValue2);

                isRatchetFrozen = false;
                if (!isSideways) {
                    if (strictMicroTrend == 1 || strongMacroBull) { 
                        if (proposedLine < lastValue2) {
                            proposedLine = lastValue2; 
                            isRatchetFrozen = true;
                        }
                    } else if (strictMicroTrend == -1 || strongMacroBear) { 
                        if (proposedLine > lastValue2) {
                            proposedLine = lastValue2; 
                            isRatchetFrozen = true;
                        }
                    }
                }

                event.value2 = proposedLine;
                event.ssaTrend = proposedLine; 
                event.regime = strictMicroTrend;
                
                lastEmaPc0 = emaPc0;
                lastValue2 = event.value2;
                
                // =========================================================================
                
                event.momentumSignal = event.price - event.value2; 
                event.crisisCapActive = isCrisisState ? 1 : 0; // استفاده از State فیلتر شده
                
                double TREND_P_STAR = 0.70;
                // استفاده از فیوژن برای تعیین وزن سایز پوزیشن به صورت نرم
                event.regimeWeight = (event.crisisCapActive == 0 && fusedTrendProb >= TREND_P_STAR) ? 
                                     Math.min(1.0, (fusedTrendProb - TREND_P_STAR) / (1.0 - TREND_P_STAR)) : 0.0;
                
                event.gatedMomentum = event.momentumSignal * event.regimeWeight;
                double MAX_POSITION = 1.0; 
                event.positionSize = (event.crisisCapActive == 1) ? 0.0 : (MAX_POSITION * event.regimeWeight);
                event.dynamicStopLoss = event.vress * 3.0;

                if (sequence % 500 == 0) {
                    System.out.printf("\n[DEBUG] Price: %.2f | isSideways: %b | FusedProb: %.2f | Val2: %.2f | Frozen: %b\n", 
                                      event.price, isSideways, fusedTrendProb, event.value2, isRatchetFrozen);
                }
                
            } else {
                event.pc0 = event.price;
                event.evr = 0.0;
                event.bandUpper = event.price;
                event.bandLower = event.price;
                event.ssaTrend = event.price;
                event.regime = currentMarketRegime;
                event.vress = 0.0;
                event.eigenGap = 0.0;
                event.hmmRegime = currentHmmRegime;
                event.hmmProbTrend = emaProbTrend;
                event.hmmProbCrisis = emaProbCrisis;
                
                event.momentumSignal = 0.0;
                event.regimeWeight = 0.0;
                event.gatedMomentum = 0.0;
                event.positionSize = 0.0;
                event.dynamicStopLoss = 0.0;
                event.crisisCapActive = 0;
                
                event.value2 = event.price;
                lastEmaPc0 = event.price;
                lastValue2 = event.price;
            }

            // --- محاسبه لیاپانوف ---
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
            event.isFrozen = currentRegimeShiftAlert || isRatchetFrozen;

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
            String host = System.getenv("CLICKHOUSE_HOST");
            if (host == null || host.trim().isEmpty()) host = "clickhouse"; 
            String user = System.getenv("CLICKHOUSE_USER");
            if (user == null || user.trim().isEmpty()) user = "default";
            String password = System.getenv("CLICKHOUSE_PASSWORD");
            if (password == null) password = ""; 
            
            String url = "jdbc:ch://" + host + ":8123/default?compress=0";
            
            int retries = 10;
            while (retries > 0) {
                try {
                    this.connection = DriverManager.getConnection(url, user, password);
                    String sql = "INSERT INTO hft_market_data (timestamp, sequence, price, volume, ssa_trend, lambda, is_frozen, regime, band_upper, band_lower, pc0, evr, vress, eigen_gap, hmm_regime, hmm_prob_trend, hmm_prob_crisis, value2, dom_cycle, momentum_signal, regime_weight, gated_momentum, position_size, dynamic_stop_loss, crisis_cap_active) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
                    this.statement = connection.prepareStatement(sql);
                    System.out.println("✅ Successfully connected to ClickHouse!");
                    break;
                } catch (SQLException e) {
                    retries--;
                    System.err.println("⏳ Waiting for ClickHouse... Retries left: " + retries);
                    try { Thread.sleep(3000); } catch (InterruptedException ie) {}
                    if (retries == 0) {
                        System.err.println("\n🔴 CRITICAL: ClickHouse Failed to connect!");
                        System.exit(1); 
                    }
                }
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
                statement.setInt(8, event.regime);
                statement.setDouble(9, event.bandUpper);
                statement.setDouble(10, event.bandLower);
                statement.setDouble(11, event.pc0);
                statement.setDouble(12, event.evr);
                statement.setDouble(13, event.vress);
                statement.setDouble(14, event.eigenGap);
                statement.setInt(15, event.hmmRegime);
                statement.setDouble(16, event.hmmProbTrend);
                statement.setDouble(17, event.hmmProbCrisis);
                statement.setDouble(18, event.value2);
                statement.setDouble(19, event.domCycle);
                statement.setDouble(20, event.momentumSignal);
                statement.setDouble(21, event.regimeWeight);
                statement.setDouble(22, event.gatedMomentum);
                statement.setDouble(23, event.positionSize);
                statement.setDouble(24, event.dynamicStopLoss);
                statement.setInt(25, event.crisisCapActive);
                
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
        @Override public void onOpen(ServerHandshake handshakedata) {
            System.out.println("✅ Connected to Binance WebSocket (Tick Stream).");
        }
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
                    event.ingressNanoTime = System.nanoTime(); 
                } finally { ringBuffer.publish(sequence); }
            } catch (Throwable e) {}
        }
        @Override public void onClose(int code, String reason, boolean remote) {
            try { Thread.sleep(5000); this.reconnect(); } catch (InterruptedException e) {}
        }
        @Override public void onError(Exception ex) {}
    }

    public static void main(String[] args) throws Exception {
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
                
                System.out.println("🔗 C++ HMM Subscriber active on " + address);
                
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

        // 🌟 شنونده هوش ماکرو (پایتون) مجهز به محاسبه شتاب
        Thread zmqMacroThread = new Thread(() -> {
            try (ZContext context = new ZContext()) {
                ZMQ.Socket subscriber = context.createSocket(SocketType.SUB);
                String zmqHost = System.getenv("MACRO_ZMQ_HOST");
                if (zmqHost == null || zmqHost.trim().isEmpty()) zmqHost = "localhost";
                String zmqPort = System.getenv("MACRO_ZMQ_PORT");
                if (zmqPort == null || zmqPort.trim().isEmpty()) zmqPort = "5556";
                
                String address = "tcp://" + zmqHost + ":" + zmqPort;
                subscriber.connect(address);
                subscriber.subscribe(new byte[0]); 
                
                System.out.println("🔗 Python Macro-L1 Subscriber active on " + address);
                
                while (!Thread.currentThread().isInterrupted()) {
                    try {
                        String msg = subscriber.recvStr();
                        if (msg != null && msg.startsWith("MACRO_TREND|")) {
                            String[] parts = msg.substring(12).trim().split(",");
                            if (parts.length >= 2) {
                                macroL1Value = Double.parseDouble(parts[0]);
                                double newSlope = Double.parseDouble(parts[1]);
                                
                                // محاسبه شتاب و پیش‌بینی تیلور (شتاب + سرعت)
                                if (macroL1Slope != 0.0) {
                                    macroL1Accel = newSlope - macroL1Slope;
                                }
                                macroL1Slope = newSlope;
                                projectedMacroSlope = macroL1Slope + macroL1Accel;
                            }
                        }
                    } catch (Exception ex) { }
                }
            } catch (Exception e) {}
        });
        zmqMacroThread.setDaemon(true);
        zmqMacroThread.start();

        System.out.println("HFT Regime Detection System Started...");
        
        Disruptor<TickEvent> disruptor = new Disruptor<>(TickEvent::new, 1024, DaemonThreadFactory.INSTANCE, ProducerType.SINGLE, new BusySpinWaitStrategy());
        disruptor.handleEventsWith(new SsaProcessingHandler()).then(new ClickHouseBatchHandler());
        RingBuffer<TickEvent> ringBuffer = disruptor.start();
        
        new BinanceProducer(new URI("wss://stream.binance.com:9443/ws/btcusdt@aggTrade"), ringBuffer).connectBlocking(); 
        
        // جلوگیری از بسته شدن برنامه
        Thread.currentThread().join();
    }

    // =========================================================================
    // Mock Classes for standalone compilation consistency
    // =========================================================================
    public static class QuantDSP {
        public static class MesaStrategyMEE {
            public MesaStrategyMEE(double v1, double v2, int v3, int v4, int v5) {}
            public double updateAndGetCycle(double price) { return 15.0; } // Mock value
        }
        public static class LyapunovEstimator {
            public static double calculateRigorousLLE(double[] data, int m, int tau, int tau2, int v) { return 0.02; } // Mock value
        }
    }
}