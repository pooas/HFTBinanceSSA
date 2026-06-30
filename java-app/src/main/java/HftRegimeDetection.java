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
        
        public double ssaTrendSlope;      
        public double sidewayScore;      
        public double trendStrength;     
    }

    // =========================================================================
    // 🌟 CAUSAL (ONE-SIDED) SAVITZKY-GOLAY FILTER
    // =========================================================================
    public static class CausalSavitzkyGolay {
        private final double[][][] valueCoeffs;  
        private final double[][][] slopeCoeffs;  
        private final int maxWindow;

        public CausalSavitzkyGolay(int maxWindow) {
            this.maxWindow = maxWindow;
            this.valueCoeffs = new double[maxWindow + 1][4][];
            this.slopeCoeffs = new double[maxWindow + 1][4][];

            for (int w = 3; w <= maxWindow; w++) {
                for (int d = 1; d <= Math.min(3, w - 1); d++) {
                    computeAndStore(w, d);
                }
            }
        }

        private void computeAndStore(int N, int deg) {
            int p = deg;
            double[][] A = new double[N][p + 1];
            for (int i = 0; i < N; i++) {
                double x = 1.0;
                for (int j = 0; j <= p; j++) { A[i][j] = x; x *= i; }
            }

            double[][] AtA = new double[p + 1][p + 1];
            for (int i = 0; i <= p; i++)
                for (int j = 0; j <= p; j++) {
                    double s = 0;
                    for (int k = 0; k < N; k++) s += A[k][i] * A[k][j];
                    AtA[i][j] = s;
                }

            double[][] inv = invert(AtA, p + 1);

            double[][] M = new double[p + 1][N];
            for (int i = 0; i <= p; i++)
                for (int j = 0; j < N; j++) {
                    double s = 0;
                    for (int k = 0; k <= p; k++) s += inv[i][k] * A[j][k];
                    M[i][j] = s;
                }

            double[] xp = new double[p + 1];
            double x = 1.0;
            for (int j = 0; j <= p; j++) { xp[j] = x; x *= (N - 1); }

            double[] vc = new double[N];
            for (int i = 0; i < N; i++) {
                double s = 0;
                for (int j = 0; j <= p; j++) s += xp[j] * M[j][i];
                vc[i] = s;
            }

            double[] sc = new double[N];
            for (int i = 0; i < N; i++) {
                double s = 0;
                for (int j = 1; j <= p; j++) s += j * Math.pow(N - 1, j - 1) * M[j][i];
                sc[i] = s;
            }

            valueCoeffs[N][deg] = vc;
            slopeCoeffs[N][deg] = sc;
        }

        private double[][] invert(double[][] m, int n) {
            double[][] a = new double[n][2 * n];
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) a[i][j] = m[i][j];
                a[i][n + i] = 1.0;
            }
            for (int i = 0; i < n; i++) {
                int piv = i;
                for (int k = i + 1; k < n; k++)
                    if (Math.abs(a[k][i]) > Math.abs(a[piv][i])) piv = k;
                if (piv != i) { double[] t = a[i]; a[i] = a[piv]; a[piv] = t; }
                double pv = a[i][i];
                if (Math.abs(pv) < 1e-15) continue;
                for (int j = 0; j < 2 * n; j++) a[i][j] /= pv;
                for (int k = 0; k < n; k++) {
                    if (k != i) {
                        double f = a[k][i];
                        for (int j = 0; j < 2 * n; j++) a[k][j] -= f * a[i][j];
                    }
                }
            }
            double[][] r = new double[n][n];
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++) r[i][j] = a[i][n + j];
            return r;
        }

        public double filterValue(double[] data, int window, int degree) {
            if (window > maxWindow) window = maxWindow;
            if (window <= degree) window = degree + 1;
            if (valueCoeffs[window][degree] == null) computeAndStore(window, degree);
            double[] c = valueCoeffs[window][degree];
            double r = 0;
            int n = data.length;
            int off = n - window;
            if (off < 0) { window = n; off = 0; c = valueCoeffs[window][degree]; }
            if (c == null) return data[n - 1];
            for (int i = 0; i < window; i++) r += c[i] * data[off + i];
            return r;
        }

        public double filterSlope(double[] data, int window, int degree) {
            if (window > maxWindow) window = maxWindow;
            if (window <= degree) window = degree + 1;
            if (slopeCoeffs[window][degree] == null) computeAndStore(window, degree);
            double[] c = slopeCoeffs[window][degree];
            double r = 0;
            int n = data.length;
            int off = n - window;
            if (off < 0) { window = n; off = 0; c = slopeCoeffs[window][degree]; }
            if (c == null) return 0.0;
            for (int i = 0; i < window; i++) r += c[i] * data[off + i];
            return r;
        }
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
        
        private double emaTrendSlope = 0.0;
        private double emaSidewayScore = 0.0;
        
        private int lastV2Regime = 0;

        // 🌟 افزایش سایز بافر برای ایجاد موج بسیار سنگین و پایدار
        private final CausalSavitzkyGolay sgFilter = new CausalSavitzkyGolay(250);
        private static final int SG_BUF_SIZE = 400;
        private final double[] sgPc0Buffer = new double[SG_BUF_SIZE];
        private int sgBufHead = 0;
        private int sgBufCount = 0;
        
        // 🌟 به جای پنجره متغیر که نویز ایجاد می‌کرد، از پنجره ثابت و سنگین استفاده می‌کنیم
        private final int FIXED_SG_WINDOW = 150; 
        private double lastSgTrend = 0.0;
        private double lastSgSlope = 0.0;

        public static class ChaosMath {
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
        
        private double[] hankelizeRank1(double sigma, double[] u, double[] v, int L, int K) {
            int N = L + K - 1;
            double[] series = new double[N];
            for (int k = 0; k < N; k++) {
                double sum = 0.0;
                int cnt = 0;
                int iMin = Math.max(0, k - K + 1);
                int iMax = Math.min(L - 1, k);
                for (int i = iMin; i <= iMax; i++) {
                    int j = k - i;
                    if (j >= 0 && j < K) {
                        sum += sigma * u[i] * v[j];
                        cnt++;
                    }
                }
                series[k] = (cnt > 0) ? (sum / cnt) : 0.0;
            }
            return series;
        }
        
        private double computeTrendSlope(double[] series) {
            int n = series.length;
            if (n < 2) return 0.0;
            double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
            for (int i = 0; i < n; i++) {
                sumX += i;
                sumY += series[i];
                sumXY += (double) i * series[i];
                sumX2 += (double) i * i;
            }
            double denom = (double) n * sumX2 - sumX * sumX;
            if (Math.abs(denom) < 1e-12) return 0.0;
            return ((double) n * sumXY - sumX * sumY) / denom;
        }
        
        private double computeSidewayScore(double[] sigmas, int numSingularValues) {
            if (numSingularValues < 4) return 0.0;
            double totalEnergy = 0.0;
            for (int i = 0; i < numSingularValues; i++) {
                totalEnergy += sigmas[i] * sigmas[i];
            }
            if (totalEnergy < 1e-12) return 1.0;
            
            double trendEnergy = sigmas[0] * sigmas[0];
            double oscillatoryEnergy = 0.0;
            for (int i = 1; i + 1 < numSingularValues; i += 2) {
                double pairRatio = Math.min(sigmas[i], sigmas[i + 1]) / 
                                   Math.max(sigmas[i], sigmas[i + 1] + 1e-12);
                if (pairRatio > 0.85) {
                    oscillatoryEnergy += sigmas[i] * sigmas[i] + sigmas[i + 1] * sigmas[i + 1];
                }
            }
            
            double oscRatio = oscillatoryEnergy / totalEnergy;
            double trendRatio = trendEnergy / totalEnergy;
            
            return Math.max(0.0, Math.min(1.0, oscRatio * (1.0 - trendRatio)));
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
                double rawTrendSlope = 0.0;
                double rawSidewayScore = 0.0;

                if (frobeniusSq < 1e-10) {
                    rawPc0 = mean;
                    rawEvr = 100.0; 
                    rawGapFactor = 0.0;
                    rawTrendSlope = 0.0;
                    rawSidewayScore = 1.0; 
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
                    
                    double[] u0 = new double[L];
                    double[] v0 = new double[K];
                    for (int i = 0; i < L; i++) u0[i] = U.get(i, maxIndex);
                    for (int j = 0; j < K; j++) v0[j] = V.get(j, maxIndex);
                    
                    double[] trendSeries = hankelizeRank1(sigma0, u0, v0, L, K);
                    
                    for (int i = 0; i < trendSeries.length; i++) {
                        trendSeries[i] += mean;
                    }
                    
                    rawTrendSlope = computeTrendSlope(trendSeries);
                    rawPc0 = trendSeries[trendSeries.length - 1];
                    rawSidewayScore = computeSidewayScore(sigmas, numSingularValues);
                    
                    rawEvr = Math.min((sigma0 * sigma0) / frobeniusSq, 1.0) * 100.0;
                    double gapRatio = sigma0 / Math.max(sigma1, 1e-9);
                    rawGapFactor = 1.0 / Math.max(1.0, gapRatio);
                }

                double alphaMetrics = 0.05;  
                double alphaSlope = 0.1;
                double alphaSideway = 0.05;
                
                if (emaTrendSlope == 0.0 && rawTrendSlope != 0.0) {
                    emaTrendSlope = rawTrendSlope;
                } else {
                    double adaptiveSlopeAlpha = alphaSlope * (1.0 - 0.5 * emaSidewayScore);
                    emaTrendSlope = adaptiveSlopeAlpha * rawTrendSlope + (1.0 - adaptiveSlopeAlpha) * emaTrendSlope;
                }
                
                if (emaSidewayScore == 0.0) {
                    emaSidewayScore = rawSidewayScore;
                } else {
                    emaSidewayScore = alphaSideway * rawSidewayScore + (1.0 - alphaSideway) * emaSidewayScore;
                }
                
                if (emaPc0 == 0.0) {
                    emaPc0 = rawPc0;
                    emaEvr = rawEvr;
                    emaGapFactor = rawGapFactor;
                } else {
                    emaEvr = alphaMetrics * rawEvr + (1.0 - alphaMetrics) * emaEvr;
                    emaGapFactor = alphaMetrics * rawGapFactor + (1.0 - alphaMetrics) * emaGapFactor;
                    
                    double evrFactor = Math.min(emaEvr / 100.0, 1.0);
                    double baseAlpha = 0.01 + 0.15 * Math.pow(evrFactor, 2);
                    double sidewayPenalty = 1.0 - 0.7 * emaSidewayScore; 
                    double adaptiveAlpha = baseAlpha * Math.max(0.3, sidewayPenalty);

                    double rawMicroSlope = rawPc0 - lastEmaPc0;
                    boolean isMacroBullish = projectedMacroSlope >= 0;
                    boolean isMicroBullish = rawMicroSlope >= 0;

                    if (projectedMacroSlope != 0.0 && (isMacroBullish == isMicroBullish)) {
                        adaptiveAlpha = Math.min(1.0, adaptiveAlpha * 2.0); 
                    }

                    emaPc0 = adaptiveAlpha * rawPc0 + (1.0 - adaptiveAlpha) * emaPc0;
                }

                // 🌟 وارد کردن دیتا به بافر سنگین DSP
                sgPc0Buffer[sgBufHead] = rawPc0;
                sgBufHead = (sgBufHead + 1) % SG_BUF_SIZE;
                if (sgBufCount < SG_BUF_SIZE) sgBufCount++;

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

                event.vress = noiseStdDev;
                event.pc0 = emaPc0;
                event.evr = emaEvr;
                event.eigenGap = emaGapFactor;
                event.ssaTrendSlope = emaTrendSlope;
                event.sidewayScore = emaSidewayScore;
                
                double trendPower = (emaEvr / 100.0) * emaGapFactor * emaProbTrend * (1.0 - emaSidewayScore);
                event.trendStrength = Math.max(0.0, Math.min(1.0, trendPower * 2.0));

                // =========================================================================
                // 🌟 THE ULTIMATE GAME CHANGER: MACRO-GRAVITATIONAL STAIRCASE (MGS)
                // خط زرد دیگر از طریق Offset یا پرش‌های قیمتی نقاشی نمی‌شود. 
                // این یک ترند مرکزی خالص است که به وسیله شیب کلان (Python) قفل می‌شود.
                // =========================================================================
                
                // --- STEP 1: HEAVY ANCHOR (تولید موج صاف مرکزی) ---
                int sgW = Math.min(sgBufCount, FIXED_SG_WINDOW); // پنجره کاملا ثابت و بدون پرش
                int sgDeg = 2; // درجه ثابت چندجمله‌ای

                double sgTrend, sgSlope;
                if (sgW >= 5) {
                    double[] sgData = new double[sgW];
                    for (int i = 0; i < sgW; i++) {
                        sgData[i] = sgPc0Buffer[(sgBufHead - sgW + i + SG_BUF_SIZE) % SG_BUF_SIZE];
                    }
                    sgTrend = sgFilter.filterValue(sgData, sgW, sgDeg);
                    sgSlope = sgFilter.filterSlope(sgData, sgW, sgDeg);
                } else {
                    sgTrend = emaPc0;
                    sgSlope = emaTrendSlope;
                }

                if (lastSgTrend == 0.0) { lastSgTrend = sgTrend; lastSgSlope = sgSlope; }
                
                // فیلتر نهایی بی‌نهایت نرم برای از بین بردن هرگونه اثر لبه‌ای (Edge Effects)
                sgTrend = 0.90 * lastSgTrend + 0.10 * sgTrend;
                sgSlope = 0.90 * lastSgSlope + 0.10 * sgSlope;
                lastSgTrend = sgTrend;
                lastSgSlope = sgSlope;
                
                if (lastValue2 == 0.0) {
                    lastValue2 = sgTrend;
                    lastV2Regime = (projectedMacroSlope >= 0) ? 1 : -1;
                }

                // --- STEP 2: MACRO GRAVITY (جهت مجاز حرکت خط) ---
                double macroGravity = 0.0;
                if (Math.abs(projectedMacroSlope) > 1e-8) {
                    macroGravity = Math.signum(projectedMacroSlope);
                }

                // --- STEP 3: KINEMATIC ONE-WAY RATCHET (قفل یک‌طرفه گام‌ها) ---
                double diff = sgTrend - lastValue2;
                double newValue2 = lastValue2;
                
                // سایز گام کوانتومی: خط تا زمانی که ترند به اندازه نویز جابجا نشود فریز می‌ماند
                double quantumStep = Math.max(event.vress * (0.5 + emaProbCrisis), 1e-6);
                
                // مکانیزم خروج اضطراری (Emergency Snap): 
                // فقط زمانی رخ می‌دهد که قیمت به طرز وحشتناکی (بیشتر از ۵ برابر نویز) خط را بشکند
                double emergencyThreshold = event.vress * 5.0;
                boolean emergencyBullBreak = (event.price > lastValue2 + emergencyThreshold) && (emaTrendSlope > event.vress * 0.1);
                boolean emergencyBearBreak = (event.price < lastValue2 - emergencyThreshold) && (emaTrendSlope < -event.vress * 0.1);

                if (emergencyBullBreak) {
                    // ریست نرم به سمت لنگر مرکزی
                    newValue2 = sgTrend - event.vress; 
                } else if (emergencyBearBreak) {
                    // ریست نرم به سمت لنگر مرکزی
                    newValue2 = sgTrend + event.vress; 
                } else {
                    // --- مکانیزم اصلی چرخ‌دنده (بدون پرش، فقط حرکت در جهت کلان) ---
                    if (Math.abs(diff) >= quantumStep) {
                        if (macroGravity > 0) {
                            // روند کلان صعودی: خط زرد فقط اجازه دارد بالا برود (کف‌سازی)
                            if (diff > 0) {
                                newValue2 = lastValue2 + diff * 0.15; // دنبال کردن نرم
                            } 
                            // اگر diff منفی بود خط کاملا فریز می‌ماند (پله افقی می‌سازد)
                            
                        } else if (macroGravity < 0) {
                            // روند کلان نزولی: خط زرد فقط اجازه دارد پایین بیاید (سقف‌سازی)
                            if (diff < 0) {
                                newValue2 = lastValue2 + diff * 0.15; // دنبال کردن نرم
                            }
                            // اگر diff مثبت بود خط کاملا فریز می‌ماند (پله افقی می‌سازد)
                            
                        } else {
                            // رژیم سایدوی کلان: اصطکاک شدید
                            if (Math.abs(diff) > event.vress * 3.0) {
                                newValue2 = lastValue2 + diff * 0.05; // نشت بسیار کند
                            }
                        }
                    }
                }

                // ثبت مقدار نهایی
                event.value2 = newValue2;
                event.ssaTrend = sgTrend; // (دیتا صرفا جهت تحلیل در دیتابیس)
                
                // رژیم نهایی صرفا بر اساس شیب کلان قفل می‌شود تا پرش رنگی نداشته باشیم
                event.regime = (macroGravity > 0) ? 1 : (macroGravity < 0 ? -1 : 0);
                currentMarketRegime = event.regime;

                event.hmmRegime = currentHmmRegime;
                event.hmmProbTrend = emaProbTrend;
                event.hmmProbCrisis = emaProbCrisis;

                isRatchetFrozen = (newValue2 == lastValue2);

                // آپدیت حافظه برای تیک بعدی
                lastV2Regime = event.regime;
                lastEmaPc0 = emaPc0;
                lastValue2 = newValue2;
                lastTrendSlope = emaTrendSlope;
                
                // =========================================================================
                
                event.momentumSignal = event.price - event.pc0;
                double CRISIS_THRESHOLD = 0.40; 
                event.crisisCapActive = (emaProbCrisis > CRISIS_THRESHOLD) ? 1 : 0;
                
                double TREND_P_STAR = 0.70 + 0.15 * emaSidewayScore; 
                double weight = 0.0;

                if (event.crisisCapActive == 0 && emaProbTrend >= TREND_P_STAR) {
                    weight = Math.min(1.0, (emaProbTrend - TREND_P_STAR) / (1.0 - TREND_P_STAR));
                }
                
                weight *= event.trendStrength;
                
                event.regimeWeight = weight;
                event.gatedMomentum = event.momentumSignal * event.regimeWeight;

                double MAX_POSITION = 1.0; 
                event.positionSize = (event.crisisCapActive == 1) ? 0.0 : (MAX_POSITION * event.regimeWeight);
                event.dynamicStopLoss = event.vress * 3.0;

                if (sequence % 500 == 0) {
                    System.out.printf("\n[DEBUG] Price: %.2f | S-Score: %.3f | SG-Slope: %+.6f | V2: %.2f | Regime: %d\n", 
                                      event.price, emaSidewayScore, sgSlope, event.value2, event.regime);
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
                
                sgPc0Buffer[sgBufHead] = event.price;
                sgBufHead = (sgBufHead + 1) % SG_BUF_SIZE;
                if (sgBufCount < SG_BUF_SIZE) sgBufCount++;

                lastSgTrend = 0.0;
                lastSgSlope = 0.0;
                
                event.ssaTrendSlope = 0.0;
                event.sidewayScore = 1.0;
                event.trendStrength = 0.0;
                lastEmaPc0 = event.price;
                lastValue2 = event.price;
            }

            // --- Lyapunov computation ---
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
        // 🌟 کاهش سایز بچ برای آپدیت ۵ برابر سریع‌تر گرافانا / Clickhouse
        private final int batchSizeThreshold = 200; 
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

        Disruptor<TickEvent> disruptor = new Disruptor<>(TickEvent::new, 1024, DaemonThreadFactory.INSTANCE, ProducerType.SINGLE, new BusySpinWaitStrategy());
        disruptor.handleEventsWith(new SsaProcessingHandler()).then(new ClickHouseBatchHandler());
        RingBuffer<TickEvent> ringBuffer = disruptor.start();
        new BinanceProducer(new URI("wss://stream.binance.com:9443/ws/btcusdt@aggTrade"), ringBuffer).connectBlocking(); 
        Thread.currentThread().join();
    }
}