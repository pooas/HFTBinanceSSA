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
        private double lastLogicalDistanceLine = 0.0;
        private double smoothedDistance = 0.0;

        private double emaPc0 = 0.0;
        private double emaEvr = 0.0;
        private double emaGapFactor = 0.0;

        private double emaProbTrend = 0.0;
        private double emaProbCrisis = 0.0;
        private boolean probInitialized = false;

        private double lastEmaPc0 = 0.0;
        private double lastValue2 = 0.0;

        public static class ChaosMath {
            public static int calculateAMI(double[] data, int maxTau, int bins) {
                // (Implementation remains unchanged for brevity)
                return Math.max(1, maxTau / 2); 
            }

            public static int calculateFNN(double[] data, int tau, int maxM, double rTol) {
                // (Implementation remains unchanged for brevity)
                return maxM;
            }
        }

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            priceHistory[head] = event.price;
            double domCycle = mesaStrategy.updateAndGetCycle(event.price);
            
            event.domCycle = domCycle;
            boolean isRatchetFrozen = false;
            
            if (!probInitialized) {
                emaProbTrend = currentProbTrend;
                emaProbCrisis = currentProbCrisis;
                probInitialized = true;
            } else {
                emaProbTrend = 0.05 * currentProbTrend + 0.95 * emaProbTrend;
                emaProbCrisis = 0.05 * currentProbCrisis + 0.95 * emaProbCrisis;
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
                }

                double evrFactor = Math.min(emaEvr / 100.0, 1.0); 
                double rawDistance = noiseStdDev * Math.max(1.0, 1.0 + 4.0 * (1.0 - evrFactor) + 2.0 * emaGapFactor); 
                
                if (smoothedDistance == 0.0) smoothedDistance = rawDistance;
                smoothedDistance = 0.05 * rawDistance + 0.95 * smoothedDistance;

                event.pc0 = emaPc0;
                event.evr = emaEvr;
                event.vress = noiseStdDev;
                event.eigenGap = emaGapFactor;


                // =========================================================================
                // 🌟 THE 3-TIER MG-SSA ARCHITECTURE (طبق Blueprint درخواست شده)
                // =========================================================================
                
                // مقداردهی اولیه برای تیک‌های نخستین
                if (lastValue2 == 0.0) lastValue2 = emaPc0;

                // --- لایه ۱: هموارسازی تطبیقی برای رفتار پله‌ای (Adaptive Staircase Smoothing) ---
                // آلفا بر اساس چرخه غالب، نویز جاری و احتمال بحران مدل‌سازی می‌شود
                double sigmaDomCycle = Math.max(0.1, Math.min(10.0 / Math.max(event.domCycle, 1.0), 1.0));
                double sigmaNoise = Math.min(noiseStdDev / Math.max(smoothedDistance, 1e-5), 1.0);
                double hCrisis = Math.max(0.0, 1.0 - emaProbCrisis); // در بحران آلفا کم می‌شود تا فریز شود
                
                double alphaT = sigmaDomCycle * (1.0 - 0.5 * sigmaNoise) * hCrisis;
                alphaT = Math.max(0.01, Math.min(alphaT, 1.0)); // محدودسازی آلفا بین 0.01 و 1
                
                // آپدیت بدون تاخیر (Zero-lag update): 
                double value2Raw = lastValue2 + alphaT * (emaPc0 - lastValue2);

                // --- لایه ۲: تابع دروازه (Trend vs. Sideways Gating Function) ---
                double normEvr = Math.min(emaEvr / 100.0, 1.0);
                double normGap = Math.min(emaGapFactor, 1.0);
                
                // Option A: Multiplicative gating
                double gT = normEvr * normGap * emaProbTrend; 
                
                // سوئیچ نرم بین خط مسطح (lastValue2) و خط خام (value2Raw)
                double value2Gated = (gT * value2Raw) + ((1.0 - gT) * lastValue2);

                // --- لایه ۳: همگام‌سازی فیزیک ماکرو-میکرو (Macro Ratchet Effect) ---
                double deltaXC = value2Gated - lastValue2;
                event.value2 = value2Gated;
                isRatchetFrozen = false;

                if (macroL1Value != 0.0) {
                    // D(t) = sgn(macro_slope) * sgn(delta_x_c)
                    double dM = Math.signum(projectedMacroSlope) * Math.signum(deltaXC);
                    
                    if (dM < 0) {
                        // حرکت خلاف جهت کلان! -> اعمال ضامن مکث (Flatline)
                        event.value2 = lastValue2; 
                        isRatchetFrozen = true;
                    } else if (dM > 0) {
                        // هم‌سو با جهت کلان -> کشش نرم به سمت خط کلان (Gravity Projection)
                        double macroGravityBeta = 0.05 * gT; // کشش فقط در زمان روندهای قوی
                        event.value2 = value2Gated + macroGravityBeta * (macroL1Value - lastValue2);
                    }
                }
                
                lastEmaPc0 = emaPc0;
                lastValue2 = event.value2;

                // =========================================================================

                event.momentumSignal = event.price - event.value2; // اکنون مومنتوم براساس Value2 محاسبه می‌شود
                event.crisisCapActive = (emaProbCrisis > 0.40) ? 1 : 0;
                
                double TREND_P_STAR = 0.70;
                event.regimeWeight = (event.crisisCapActive == 0 && emaProbTrend >= TREND_P_STAR) ? 
                                     Math.min(1.0, (emaProbTrend - TREND_P_STAR) / (1.0 - TREND_P_STAR)) : 0.0;
                
                event.gatedMomentum = event.momentumSignal * event.regimeWeight;
                event.positionSize = (event.crisisCapActive == 1) ? 0.0 : (1.0 * event.regimeWeight);
                event.dynamicStopLoss = event.vress * 3.0;

                if (sequence % 500 == 0) {
                    System.out.printf("\n[DEBUG] Price: %.2f | Gating(gT): %.3f | Alpha(aT): %.3f | Val2: %.2f | Frozen: %b\n", 
                                      event.price, gT, alphaT, event.value2, isRatchetFrozen);
                }
                
            } else {
                // (Initialization block skipped for brevity, keeps defaults)
                event.value2 = event.price;
                lastEmaPc0 = event.price;
                lastValue2 = event.price;
            }

            // --- محاسبه لیاپانوف ---
            ssaTrendBuffer[ssaHead] = event.value2; // اکنون به جای ssaTrend روی value2 محاسبه می‌کنیم
            ssaHead = (ssaHead + 1) % LLE_WINDOW;
            if (ssaHead == 0) ssaBufferFull = true;

            if (ssaBufferFull && sequence % 10 == 0) {
                double[] flatBuffer = new double[LLE_WINDOW];
                for (int i = 0; i < LLE_WINDOW; i++) flatBuffer[i] = ssaTrendBuffer[(ssaHead + i) % LLE_WINDOW];

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

            event.lambda = currentLambda;
            event.isFrozen = currentRegimeShiftAlert || isRatchetFrozen;

            head = (head + 1) % MAX_CAPACITY;
            if (count < MAX_CAPACITY) count++;
        }
    }

    public static class ClickHouseBatchHandler implements EventHandler<TickEvent> {
        // (ClickHouse block unchanged - writes event.value2 properly)
        private Connection connection;
        private PreparedStatement statement;
        private final int batchSizeThreshold = 1000;
        private int currentBatchSize = 0;

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
            // (DB Insert stub)
        }
    }

    public static class BinanceProducer extends WebSocketClient {
        // (Websocket block unchanged)
        private final RingBuffer<TickEvent> ringBuffer;
        public BinanceProducer(URI serverUri, RingBuffer<TickEvent> ringBuffer) { super(serverUri); this.ringBuffer = ringBuffer; }
        @Override public void onOpen(ServerHandshake handshakedata) {}
        @Override public void onMessage(String message) {}
        @Override public void onClose(int code, String reason, boolean remote) {}
        @Override public void onError(Exception ex) {}
    }

    public static void main(String[] args) throws Exception {
        // (ZMQ connections and Disruptor initialization unchanged)
        System.out.println("HFT Regime Detection System Started...");
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