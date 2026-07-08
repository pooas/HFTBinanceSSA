import com.lmax.disruptor.EventHandler;
import com.lmax.disruptor.RingBuffer;
import com.lmax.disruptor.dsl.Disruptor;
import com.lmax.disruptor.dsl.ProducerType;
import com.lmax.disruptor.util.DaemonThreadFactory;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import org.zeromq.SocketType;
import org.zeromq.ZContext;
import org.zeromq.ZMQ;

import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.SQLException;
import java.util.concurrent.TimeUnit;
import com.lmax.disruptor.YieldingWaitStrategy;

public class HftRegimeDetection {

    public static volatile int currentHmmRegime = 0;
    public static volatile double currentProbTrend = 0.0;
    public static volatile double currentProbCrisis = 0.0;

    public static volatile double macroL1Value = 0.0;
    public static volatile double macroL1Slope = 0.0;
    public static volatile double macroL1Accel = 0.0;
    public static volatile double projectedMacroSlope = 0.0;

    // SG-DSP fields — populated by the SG ZMQ subscriber thread
    public static volatile double sgSmoothed = 0.0;
    public static volatile double sgSlope = 0.0;
    public static volatile double sgAccel = 0.0;
    public static volatile double sgSideway = 0.0;
    public static volatile int sgAdaptiveWindow = 0;
    public static volatile int sgPolyOrder = 0;
    public static volatile long sgTimestampNs = 0;
    private static final int SG_FRAME_MAGIC = 0x53474450;
    private static final int SG_FRAME_SIZE = 52;

    // SSA fields — populated by the SSA ZMQ subscriber thread
    public static volatile double ssaSmoothed = 0.0;
    public static volatile double ssaSlope = 0.0;
    public static volatile double ssaAccel = 0.0;
    public static volatile double ssaMacroTrend = 0.0;
    public static volatile int ssaLFast = 0;
    public static volatile int ssaLSlow = 0;
    public static volatile float ssaBlendWeight = 0.0f;
    public static volatile float ssaEvrFast = 0.0f;
    public static volatile float ssaEvrSlow = 0.0f;
    public static volatile float ssaEigenGap = 0.0f;
    public static volatile long ssaTimestampNs = 0;
    
    private static final int SSA_FRAME_MAGIC = 0x53534150; // "SSAP"
    private static final int SSA_FRAME_SIZE = 68;          // 68 bytes frame

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

        // Phase-3 Sensor-Fusion output — the zero-lag fused trend line.
        public double zeroLagTrend;
        // v1.2.0 Kalman diagnostics — written to ClickHouse so Grafana can
        // visualise the filter's real-time adaptation (K_k, R_k, Q_k).
        public double kalmanGain;
        public double kalmanR;
        public double kalmanQ;
        // Per-tick snapshots of async ZMQ values, captured in SsaProcessingHandler
        // at tick ingestion time so the batch-insert path writes a temporally
        // consistent row (HFT batch-integrity invariant).
        public double sgSlopeAtTick;
        public double sgAccelAtTick;
        public double ssaMacroTrendAtTick;

        public double ssaTrendSlope;
        public double sidewayScore;
        public double trendStrength;

        // 🌟 New C++ SSA Engine Fields
        public double finalSsaSmoothed;
        public double finalSsaSlope;
        public double finalSsaAccel;
        public double finalSsaMacroTrend;
        public int ssaLFast;
        public int ssaLSlow;
        public float ssaBlendWeight;
        public float ssaEvrFast;
        public float ssaEvrSlow;
        public float ssaEigenGapOut;
    }

    public static class SsaProcessingHandler implements EventHandler<TickEvent> {

        private final QuantDSP.MesaStrategyMEE mesaStrategy = new QuantDSP.MesaStrategyMEE(8.0, 330.0, 150, 5, 3);

        private final int LLE_WINDOW = 500;
        private final double[] ssaTrendBuffer = new double[LLE_WINDOW];
        private final double[] lleFlatBuffer = new double[LLE_WINDOW];
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

        // =========================================================================
        // 🌟 v1.2.0 — Adaptive Kalman Fusion (1D AKF)
        // -------------------------------------------------------------------------
        // Replaces the v1.1.0 heuristic linear fusion (K_gain · sgSlope) with a
        // stochastic state-space filter. The Kalman state x is the true zero-lag
        // macro trend; sgSlope is the control input (predict step); ssaMacroTrend
        // is the measurement (update step). Sage-Husa adaptive R/Q tracking lets
        // the filter continuously re-tune to non-stationary crypto microstructure.
        //
        // The regime is then a pure crossover against the Kalman-smoothed state:
        //   price > x  →  +1 (Long)
        //   price < x  →  -1 (Short)
        //   price == x →  hold (numerically rare; preserves previous label)
        // No hysteresis deadband — the Kalman state is already stochastically
        // smooth, so deadbands would only add lag without reducing whipsaws.
        // =========================================================================
        private final AdaptiveKalmanFusion kalmanFusion = new AdaptiveKalmanFusion();

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

        // 🌟 Continuous-state tracking for the Kinematic Ratchet
        private int lastV2Regime = 0;          // soft diagnostic label only
        private double emaDirCoupling = 0.0;   // smoothed D(t) ∈ [-1, +1]

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

        // -------------------------------------------------------------------------
        // 🌟 v1.3.0 — Java handler is now a pure fusion/execution layer.
        // -------------------------------------------------------------------------
        // Heavy SSA/SVD math has been offloaded to cpp-ssa-engine (port 5558) and
        // cpp-sg-dsp (port 5557).  This handler only consumes the async ZMQ fields
        // and runs the lightweight Kalman fusion + ratchet that the Java engine
        // is responsible for.  A small fallback path keeps the first ticks sane
        // before the C++ engines finish warmup.  This is identical in LIVE and
        // REPLAY modes because the C++ nodes always publish on the same ports.
        // -------------------------------------------------------------------------

        @Override
        public void onEvent(TickEvent event, long sequence, boolean endOfBatch) {
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

            // C++ engine readiness: timestamp > 0 and the value is finite.
            final boolean cppSsaReady = ssaTimestampNs > 0 && Double.isFinite(ssaSmoothed);
            final boolean cppSgReady  = sgTimestampNs  > 0 && Double.isFinite(sgSlope);

            // ----- Lightweight raw-signal fallback -----
            final double rawPc0          = cppSsaReady ? ssaSmoothed : event.price;
            final double rawTrendSlope   = cppSgReady  ? sgSlope
                                          : (cppSsaReady ? ssaSlope : 0.0);
            final double rawSidewayScore;
            if (cppSgReady) {
                rawSidewayScore = Math.max(0.0, Math.min(1.0, sgSideway));
            } else if (cppSsaReady && ssaEvrSlow > 0.0f) {
                rawSidewayScore = Math.max(0.0, Math.min(1.0, 1.0 - ssaEvrSlow));
            } else {
                rawSidewayScore = 1.0; // safest default before any DSP data arrives
            }
            final double rawEvr       = (cppSsaReady && ssaEvrSlow > 0.0f)
                                        ? ssaEvrSlow * 100.0 : 50.0;
            final double rawGapFactor = (cppSsaReady && ssaEvrSlow > 0.0f)
                                        ? Math.max(0.0, Math.min(1.0, ssaEvrSlow)) : 0.5;

            final double alphaMetrics = 0.05;
            final double alphaSlope   = 0.10;
            final double alphaSideway = 0.05;

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

            double currentResidual = event.price - emaPc0;
            residualHistory[residualHead] = currentResidual;
            residualHead = (residualHead + 1) % RESIDUAL_WINDOW;
            if (residualHead == 0) residualFull = true;

            double noiseStdDev = 0.0;
            int activeResCount = residualFull ? RESIDUAL_WINDOW : residualHead;

            if (activeResCount > 1) {
                double resMean = 0.0;
                for (int i = 0; i < activeResCount; i++) resMean += residualHistory[i];
                resMean /= activeResCount;

                double resVar = 0.0;
                for (int i = 0; i < activeResCount; i++) {
                    double diff = residualHistory[i] - resMean;
                    resVar += diff * diff;
                }
                noiseStdDev = Math.sqrt(resVar / (activeResCount - 1));
            } else if (activeResCount == 1) {
                noiseStdDev = Math.abs(residualHistory[0]);
            }

            // =========================================================================
            // 🌟 v1.2.0 — ADAPTIVE KALMAN FUSION & BINARY CROSSOVER REGIME
            // -------------------------------------------------------------------------
            // The Kalman filter fuses the zero-lag SG slope (gate signal) with the
            // lagging C++ SSA macro-trend (measurement).  All heavy DSP math lives
            // in the C++ engines; Java only runs the lightweight state update.
            // =========================================================================

            if (!kalmanFusion.isInitialised()) {
                kalmanFusion.reset(event.price);
            }

            kalmanFusion.step(sgSlope, ssaMacroTrend);
            double zero_lag_trend = kalmanFusion.getZeroLagTrend();

            if (!Double.isFinite(zero_lag_trend)) {
                zero_lag_trend = (emaPc0 != 0.0) ? emaPc0 : event.price;
            }

            event.zeroLagTrend = zero_lag_trend;
            event.kalmanGain   = kalmanFusion.getKalmanGain();
            event.kalmanR      = kalmanFusion.getR();
            event.kalmanQ      = kalmanFusion.getQ();

            if (event.price > zero_lag_trend) {
                currentMarketRegime = 1;
            } else if (event.price < zero_lag_trend) {
                currentMarketRegime = -1;
            }

            event.bandUpper = zero_lag_trend + noiseStdDev;
            event.bandLower = zero_lag_trend - noiseStdDev;

            event.pc0      = emaPc0;
            event.evr      = emaEvr;
            event.vress    = noiseStdDev;
            event.eigenGap = emaGapFactor;

            event.ssaTrendSlope = emaTrendSlope;
            event.sidewayScore  = emaSidewayScore;

            double trendPower = (emaEvr / 100.0) * emaGapFactor * emaProbTrend * (1.0 - emaSidewayScore);
            event.trendStrength = Math.max(0.0, Math.min(1.0, trendPower * 2.0));

            lastLogicalDistanceLine = zero_lag_trend;
            event.ssaTrend = zero_lag_trend;
            event.regime = currentMarketRegime;
            event.hmmRegime = currentHmmRegime;
            event.hmmProbTrend = emaProbTrend;
            event.hmmProbCrisis = emaProbCrisis;

            // =========================================================================
            // 🌟 MACRO–MICRO KINEMATIC FUSION — Adaptive Staircase Ratchet (continuous)
            // =========================================================================

            double slopeScale = Math.max(event.vress * 0.5, 1e-8);
            double macroSig = Math.tanh(projectedMacroSlope / slopeScale);
            double microSig = Math.tanh(emaTrendSlope       / slopeScale);
            double D_raw = macroSig * microSig;

            emaDirCoupling = 0.10 * D_raw + 0.90 * emaDirCoupling;
            double D = emaDirCoupling;
            double agreement   = 0.5 * (D + 1.0);
            double counterDamp = 1.0 - agreement;

            double cycleN    = Math.max(8.0, event.domCycle * 0.5);
            double alphaBase = 2.0 / (cycleN + 1.0);

            double priceScale = Math.max(Math.abs(event.price) * 1e-4, 1e-9);
            double noiseRatio = Math.min(1.0, event.vress / priceScale);
            double noiseDamp  = 1.0 - 0.7 * noiseRatio;

            double crisisDamp = 1.0 - 0.9 * emaProbCrisis;
            double sidewayDamp = 1.0 - 0.6 * emaSidewayScore;
            double dirDamp = Math.max(0.05, agreement);

            double alpha_v2 = alphaBase * noiseDamp * crisisDamp * sidewayDamp * dirDamp;
            alpha_v2 = Math.max(0.001, Math.min(0.20, alpha_v2));

            double wBull = Math.max(0.0,  macroSig);
            double wBear = Math.max(0.0, -macroSig);
            double wFlat = 1.0 - Math.abs(macroSig);
            double target = wBull * event.bandLower
                          + wFlat * emaPc0
                          + wBear * event.bandUpper;

            double minDistance = Math.max(event.vress * 1.5, 1e-6);
            if (macroSig > 0.0 && target > event.price - minDistance) {
                target = event.price - minDistance;
            } else if (macroSig < 0.0 && target < event.price + minDistance) {
                target = event.price + minDistance;
            }

            double prevV2 = (lastValue2 == 0.0) ? target : lastValue2;
            double rawStep = alpha_v2 * (target - prevV2);

            double allowedStep;
            if (macroSig > 0.0) {
                allowedStep = (rawStep >= 0.0) ? rawStep : rawStep * counterDamp;
            } else if (macroSig < 0.0) {
                allowedStep = (rawStep <= 0.0) ? rawStep : rawStep * counterDamp;
            } else {
                allowedStep = rawStep;
            }

            double quantumStep = event.vress * 0.5 * (1.0 + emaProbCrisis + emaSidewayScore);
            if (Math.abs(allowedStep) < quantumStep) {
                event.value2 = prevV2;
                isRatchetFrozen = true;
            } else {
                event.value2 = prevV2 + allowedStep;
                isRatchetFrozen = false;
            }

            if (macroSig > 0.0 && event.value2 > event.price - minDistance) {
                event.value2 = event.price - minDistance;
            } else if (macroSig < 0.0 && event.value2 < event.price + minDistance) {
                event.value2 = event.price + minDistance;
            }

            lastV2Regime  = (macroSig >  0.33) ?  1
                          : (macroSig < -0.33) ? -1
                          :                       0;
            lastEmaPc0     = emaPc0;
            lastValue2     = event.value2;

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
                System.out.printf("\n[DEBUG] Price: %.2f | Sideway: %.3f | Slope: %+.6f | TrendStr: %.3f | D: %+.3f | α_v2: %.4f | Frozen: %s | Val2: %.2f\n",
                                  event.price, emaSidewayScore, emaTrendSlope, event.trendStrength,
                                  D, alpha_v2, isRatchetFrozen ? "Y" : "N", event.value2);
            }

            // --- Lyapunov computation ---
            ssaTrendBuffer[ssaHead] = event.ssaTrend;
            ssaHead = (ssaHead + 1) % LLE_WINDOW;
            if (ssaHead == 0) ssaBufferFull = true;

            if (ssaBufferFull) {
                for (int i = 0; i < LLE_WINDOW; i++) lleFlatBuffer[i] = ssaTrendBuffer[(ssaHead + i) % LLE_WINDOW];

                if (sequence % 500 == 0) {
                    currentTau = ChaosMath.calculateAMI(lleFlatBuffer, 30, 20);
                    currentM = ChaosMath.calculateFNN(lleFlatBuffer, currentTau, 6, 15.0);
                }

                if (sequence % 10 == 0) {
                    currentLambda = QuantDSP.LyapunovEstimator.calculateRigorousLLE(
                            lleFlatBuffer, currentM, currentTau, currentTau * 2, 5
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

            // 🌟 Assign global SSA Engine values (updated async by ZMQ subscriber) to the current event
            event.finalSsaSmoothed   = ssaSmoothed;
            event.finalSsaSlope      = ssaSlope;
            event.finalSsaAccel      = ssaAccel;
            event.finalSsaMacroTrend = ssaMacroTrend;
            // Phase-3 per-tick snapshots of async ZMQ streams
            event.sgSlopeAtTick        = sgSlope;
            event.sgAccelAtTick        = sgAccel;
            event.ssaMacroTrendAtTick  = ssaMacroTrend;
            event.ssaLFast = ssaLFast;
            event.ssaLSlow = ssaLSlow;
            event.ssaBlendWeight = ssaBlendWeight;
            event.ssaEvrFast = ssaEvrFast;
            event.ssaEvrSlow = ssaEvrSlow;
            event.ssaEigenGapOut = ssaEigenGap;
        }
    }

    public static class ClickHouseBatchHandler implements EventHandler<TickEvent> {
        private Connection connection;
        private PreparedStatement statement;
        private final int batchSizeThreshold = 1000;
        private int currentBatchSize = 0;

        private long lastFlushTime = System.currentTimeMillis();
        private final long maxFlushDelayMs = 500;

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
                    // 🌟 Updated SQL Query matching the new 32-column init.sql structure
                    String sql = "INSERT INTO hft_market_data (timestamp, sequence, price, volume, ssa_smoothed, ssa_slope, ssa_accel, ssa_macro_trend, ssa_l_fast, ssa_l_slow, ssa_blend_weight, ssa_evr_fast, ssa_evr_slow, ssa_eigen_gap, lambda, is_frozen, regime, band_upper, band_lower, pc0, vress, hmm_regime, hmm_prob_trend, hmm_prob_crisis, value2, dom_cycle, momentum_signal, regime_weight, gated_momentum, position_size, dynamic_stop_loss, crisis_cap_active) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
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
                
                // 🌟 v1.2.0 Adaptive Kalman Fusion — ClickHouse column mapping
                // Legacy schema columns are repurposed (no schema migration needed):
                //
                //  col 5  ssa_smoothed    ← zero_lag_trend       (Kalman state x_{k|k})
                //  col 6  ssa_slope         ← sgSlope at tick       (SG zero-lag slope)
                //  col 7  ssa_accel         ← sgAccel at tick       (SG 2nd derivative)
                //  col 8  ssa_macro_trend  ← ssaMacroTrend at tick  (raw Ehlers baseline)
                //  col 11 ssa_blend_weight ← kalmanGain            (K_k ∈ [0,1])
                //  col 12 ssa_evr_fast       ← kalmanR               (adaptive measurement noise R_k)
                //  col 14 ssa_eigen_gap     ← kalmanQ               (adaptive process noise Q_k)
                //
                // Float32 columns (11, 12, 14) silently downcast Java double → float
                // in the JDBC driver. All Kalman bounds (R ∈ [1e-6, 1e-2],
                // Q ∈ [1e-8, 1e-4], K ∈ [0, 1]) fit well within Float32 precision.
                statement.setDouble(5, event.zeroLagTrend);
                statement.setDouble(6, event.sgSlopeAtTick);
                statement.setDouble(7, event.sgAccelAtTick);
                statement.setDouble(8, event.ssaMacroTrendAtTick);
                statement.setInt(9,    event.ssaLFast);
                statement.setInt(10,   event.ssaLSlow);
                statement.setDouble(11, event.kalmanGain);   // was ssaBlendWeight (Float32)
                statement.setDouble(12, event.kalmanR);      // was ssaEvrFast      (Float32)
                statement.setFloat(13,  event.ssaEvrSlow);   // untouched diagnostic
                statement.setDouble(14, event.kalmanQ);      // was ssaEigenGapOut   (Float32)
                
                // 🌟 Remaining Legacy & HMM Fields (18 fields)
                statement.setDouble(15, event.lambda);
                statement.setInt(16, event.isFrozen ? 1 : 0);
                statement.setInt(17, event.regime);
                statement.setDouble(18, event.bandUpper);
                statement.setDouble(19, event.bandLower);
                statement.setDouble(20, event.pc0);
                statement.setDouble(21, event.vress);
                statement.setInt(22, event.hmmRegime);
                statement.setDouble(23, event.hmmProbTrend);
                statement.setDouble(24, event.hmmProbCrisis);
                statement.setDouble(25, event.value2);
                statement.setDouble(26, event.domCycle);
                statement.setDouble(27, event.momentumSignal);
                statement.setDouble(28, event.regimeWeight);
                statement.setDouble(29, event.gatedMomentum);
                statement.setDouble(30, event.positionSize);
                statement.setDouble(31, event.dynamicStopLoss);
                statement.setInt(32, event.crisisCapActive);

                statement.addBatch();
                currentBatchSize++;
                
                long currentTime = System.currentTimeMillis();
                boolean timeLimitReached = (currentTime - lastFlushTime) >= maxFlushDelayMs;

                // Flush on batch-size threshold OR on time-limit to prevent the
                // Disruptor from stalling when the producer sends a slow trickle
                // of events (low replay speed / quiet live periods).
                if (currentBatchSize >= batchSizeThreshold || (timeLimitReached && currentBatchSize > 0)) {
                    flush();
                }
            } catch (SQLException e) {}
        }

        private void flush() {
            if (currentBatchSize == 0) return;
            try { 
                statement.executeBatch(); 
                currentBatchSize = 0; 
                lastFlushTime = System.currentTimeMillis();
            }
            catch (SQLException e) { 
                currentBatchSize = 0; 
                lastFlushTime = System.currentTimeMillis();
            }
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

        // SG-DSP subscriber: reads 52-byte binary frames from the C++ Adaptive SG filter
        Thread zmqSgThread = new Thread(() -> {
            ByteBuffer buf = ByteBuffer.allocateDirect(SG_FRAME_SIZE);
            buf.order(ByteOrder.LITTLE_ENDIAN);

            try (ZContext context = new ZContext()) {
                ZMQ.Socket subscriber = context.createSocket(SocketType.SUB);
                String sgHost = System.getenv("SG_ZMQ_HOST");
                if (sgHost == null || sgHost.trim().isEmpty()) sgHost = "localhost";
                String sgPort = System.getenv("SG_ZMQ_PORT");
                if (sgPort == null || sgPort.trim().isEmpty()) sgPort = "5557";

                String address = "tcp://" + sgHost + ":" + sgPort;
                subscriber.connect(address);
                subscriber.subscribe(new byte[0]);

                System.out.println("[SG-SUB] SG-DSP Subscriber active on " + address);

                while (!Thread.currentThread().isInterrupted()) {
                    byte[] raw = subscriber.recv(0);
                    if (raw == null || raw.length != SG_FRAME_SIZE) continue;

                    buf.clear();
                    buf.put(raw);
                    buf.flip();

                    int magic = buf.getInt();
                    if (magic != SG_FRAME_MAGIC) continue;

                    sgTimestampNs    = buf.getLong();
                    sgSmoothed       = buf.getDouble();
                    sgSlope          = buf.getDouble();
                    sgAccel          = buf.getDouble();
                    sgSideway        = buf.getDouble();
                    sgAdaptiveWindow = buf.getInt();
                    sgPolyOrder      = buf.getInt();
                }
            } catch (Exception e) {
                System.err.println("[SG-SUB] Error: " + e.getMessage());
            }
        });
        zmqSgThread.setDaemon(true);
        zmqSgThread.start();

        // 🌟 SSA-ENGINE subscriber: reads 68-byte binary frames from the C++ SSA Engine
        Thread zmqSsaThread = new Thread(() -> {
            ByteBuffer buf = ByteBuffer.allocateDirect(SSA_FRAME_SIZE);
            buf.order(ByteOrder.LITTLE_ENDIAN);

            try (ZContext context = new ZContext()) {
                ZMQ.Socket subscriber = context.createSocket(SocketType.SUB);
                String ssaHost = System.getenv("SSA_ZMQ_HOST");
                if (ssaHost == null || ssaHost.trim().isEmpty()) ssaHost = "localhost";
                String ssaPort = System.getenv("SSA_ZMQ_PORT");
                if (ssaPort == null || ssaPort.trim().isEmpty()) ssaPort = "5558";

                String address = "tcp://" + ssaHost + ":" + ssaPort;
                subscriber.connect(address);
                subscriber.subscribe(new byte[0]);

                System.out.println("[SSA-SUB] SSA Engine Subscriber active on " + address);

                while (!Thread.currentThread().isInterrupted()) {
                    byte[] raw = subscriber.recv(0);
                    if (raw == null || raw.length != SSA_FRAME_SIZE) continue;

                    buf.clear();
                    buf.put(raw);
                    buf.flip();

                    int magic = buf.getInt();
                    if (magic != SSA_FRAME_MAGIC) continue;

                    ssaTimestampNs   = buf.getLong();
                    ssaSmoothed      = buf.getDouble();
                    ssaSlope         = buf.getDouble();
                    ssaAccel         = buf.getDouble();
                    ssaMacroTrend    = buf.getDouble();
                    ssaLFast         = buf.getInt();
                    ssaLSlow         = buf.getInt();
                    ssaBlendWeight   = buf.getFloat();
                    ssaEvrFast       = buf.getFloat();
                    ssaEvrSlow       = buf.getFloat();
                    ssaEigenGap      = buf.getFloat();
                }
            } catch (Exception e) {
                System.err.println("[SSA-SUB] Error: " + e.getMessage());
            }
        });
        zmqSsaThread.setDaemon(true);
        zmqSsaThread.start();

        Disruptor<TickEvent> disruptor = new Disruptor<>(TickEvent::new, 65536, DaemonThreadFactory.INSTANCE, ProducerType.SINGLE, new YieldingWaitStrategy());
        disruptor.handleEventsWith(new SsaProcessingHandler()).then(new ClickHouseBatchHandler());
        RingBuffer<TickEvent> ringBuffer = disruptor.start();
        String binanceWsUrl = System.getenv("BINANCE_AGGTRADE_WS_URL");
        if (binanceWsUrl == null || binanceWsUrl.trim().isEmpty()) {
            binanceWsUrl = "wss://stream.binance.com:9443/ws/btcusdt@aggTrade";
        }
        System.out.println("[BinanceProducer] connecting to " + binanceWsUrl + " (mode=" + System.getenv().getOrDefault("DATA_MODE", "LIVE") + ")");
        BinanceProducer producer = null;
        int connectRetries = 30;
        while (connectRetries > 0) {
            try {
                producer = new BinanceProducer(new URI(binanceWsUrl), ringBuffer);
                if (producer.connectBlocking(5, TimeUnit.SECONDS) && producer.isOpen()) {
                    break;
                }
                System.err.println("[BinanceProducer] connection attempt failed (timeout/closed).");
            } catch (Exception e) {
                System.err.println("[BinanceProducer] connection attempt failed: " + e.getMessage());
            }
            connectRetries--;
            System.err.println("[BinanceProducer] retries left: " + connectRetries);
            try { Thread.sleep(2000); } catch (InterruptedException ie) {}
        }
        if (producer == null || !producer.isOpen()) {
            System.err.println("[BinanceProducer] unable to open WebSocket; exiting so Docker can restart.");
            System.exit(1);
        }
        Thread.currentThread().join();
    }
}