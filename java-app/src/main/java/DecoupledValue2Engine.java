import org.apache.commons.math3.complex.Complex;
import org.apache.commons.math3.linear.Array2DRowRealMatrix;
import org.apache.commons.math3.linear.ArrayRealVector;
import org.apache.commons.math3.linear.DecompositionSolver;
import org.apache.commons.math3.linear.LUDecomposition;
import org.apache.commons.math3.linear.RealMatrix;
import org.apache.commons.math3.linear.RealVector;
import org.apache.commons.math3.linear.SingularValueDecomposition;
import org.apache.commons.math3.transform.DftNormalization;
import org.apache.commons.math3.transform.FastFourierTransformer;
import org.apache.commons.math3.transform.TransformType;

import java.util.Arrays;

/**
 * DecoupledValue2Engine.java
 *
 * Stateful, tick-by-tick Java port of the Python "Decoupled Value2" strategy.
 *
 * Architecture highlights:
 *  - All per-tick state is held in primitive circular buffers and scalar
 *    doubles. The only heap allocation on the hot path is the returned
 *    {@link EngineResult} (immutable by design).
 *  - Apache Commons Math 3 is used for FFT, SVD and the symmetric-Toeplitz
 *    linear solve (the Toeplitz matrix is assembled explicitly and solved
 *    via LUDecomposition).
 *  - MesaStrategyMEE extracts the dominant cycle; the engine then computes the
 *    Voss-smoothed value2, an SSA pure-trend baseline, the freeze logic, and
 *    the binary opt_weight.
 *
 * Output fields:
 *  - value2      : frozen/decoupled Value2
 *  - optWeight   : -1.0 when value2 is frozen/flat, +1.0 otherwise
 *  - rawValue2   : pre-freeze value2_raw
 *  - tension     : (value2_frozen - price) / sigma_freeze during a freeze
 *  - sigmaFreeze : sample standard deviation of price since freeze start
 *  - dominantCycle, power : MEE diagnostics
 *  - ssaTrend, ssaSlope : SSA diagnostics
 *  - isFrozen    : 1 when freeze logic is active, 0 otherwise
 */
public class DecoupledValue2Engine {

    // ------------------------------------------------------------------------
    // Tunable parameters (match the Python prototype)
    // ------------------------------------------------------------------------
    private static final double LOWER_BOUND      = 8.0;
    private static final double UPPER_BOUND      = 330.0;
    private static final int    HISTORY_LENGTH   = 150;
    private static final int    MEE_K            = 5;
    private static final int    MEE_P            = 3;
    private static final int    MEE_M            = HISTORY_LENGTH / (MEE_K + 1); // 25
    private static final int    MEE_COV_LEN      = 2 * MEE_M - 1;               // 49
    private static final int    PAD              = 2048;
    private static final int    VOSS_SMOOTH_WIN  = 3;
    private static final int    CYCLE_SMOOTH_WIN = 3;
    private static final int    WAVEE_HISTORY    = 500;
    private static final double FREEZE_DIFF_EPS  = 1e-6;

    // ------------------------------------------------------------------------
    // Sub-filters used by the MEE cycle extractor
    // ------------------------------------------------------------------------
    private final HighPassFilter         hpFilter;
    private final DynamicSuperSmoother   ssFilter;
    private final DynamicUltimateSmoother envelFilter;
    private final DynamicSuperSmoother   value36Filter;
    private final DynamicUltimateSmoother waveeFilter;
    private final DynamicUltimateSmoother smoothLenFilter;
    private final DynamicSMA             volSma;

    // ------------------------------------------------------------------------
    // MEE state (pre-allocated to avoid per-tick array churn)
    // ------------------------------------------------------------------------
    private final double[] filtBuffer;
    private int filtBufferHead = 0;
    private int filtBufferSize = 0;
    private double currentDominantCycle = 20.0;
    private double currentPower = 0.0;
    private double prevClose = Double.NaN;
    private double lastHann = Double.NaN;
    private int barCount = 0;

    private final double[][] meeYBlocks;
    private final double[][] meeCovSequences;
    private final double[]   meeVariances;
    private final double[]   meeVarAutocorr;
    private final double[]   meeACoeffs;
    private final double[]   meePredictedCov;
    private final double[]   meePaddedCov;
    private final FastFourierTransformer meeFft;

    // ------------------------------------------------------------------------
    // Streaming process_logic state
    // ------------------------------------------------------------------------
    private final double[] closeBuffer;
    private final double[] domCycleBuffer;
    private final double[] value2RawBuffer;
    private final double[] vossResultBuffer;
    private final double[] waveeHistory;
    private int streamHead = 0;
    private int streamSize = 0;
    private int waveeHead = 0;
    private int waveeSize = 0;
    private double prevSsaTrend = Double.NaN;
    private double lastVal = Double.NaN;
    private double prevFinalV2 = Double.NaN;
    private EngineResult lastResult;

    // ------------------------------------------------------------------------
    // Elastic Potential Energy tracker — Welford one-pass variance, no arrays
    // ------------------------------------------------------------------------
    private long   welfordCount  = 0L;
    private double welfordMean   = 0.0;
    private double welfordM2     = 0.0;
    private double freezeAnchor  = Double.NaN;
    private boolean prevFrozen   = false;

    // ------------------------------------------------------------------------
    // Constructors
    // ------------------------------------------------------------------------
    public DecoupledValue2Engine() {
        this.hpFilter        = new HighPassFilter(UPPER_BOUND);
        this.ssFilter        = new DynamicSuperSmoother();
        this.envelFilter     = new DynamicUltimateSmoother();
        this.volSma          = new DynamicSMA(500);
        this.value36Filter   = new DynamicSuperSmoother();
        this.waveeFilter     = new DynamicUltimateSmoother();
        this.smoothLenFilter = new DynamicUltimateSmoother();

        this.filtBuffer      = new double[HISTORY_LENGTH];

        this.meeYBlocks      = new double[MEE_K][MEE_M];
        this.meeCovSequences = new double[MEE_K][MEE_COV_LEN];
        this.meeVariances    = new double[MEE_K];
        this.meeVarAutocorr  = new double[MEE_K];
        this.meeACoeffs      = new double[MEE_P];
        this.meePredictedCov = new double[MEE_COV_LEN];
        this.meePaddedCov    = new double[PAD];
        this.meeFft          = new FastFourierTransformer(DftNormalization.STANDARD);

        // The streaming buffers need to be long enough for the largest SSA
        // lookback (upperBound/2 * 2 = upperBound) plus Voss delay smoothing.
        int maxLookback = (int) Math.ceil(UPPER_BOUND) + CYCLE_SMOOTH_WIN + VOSS_SMOOTH_WIN + 10;
        this.closeBuffer      = new double[maxLookback];
        this.domCycleBuffer   = new double[maxLookback];
        this.value2RawBuffer  = new double[maxLookback];
        this.vossResultBuffer = new double[maxLookback];
        this.waveeHistory     = new double[WAVEE_HISTORY];
    }

    // ------------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------------

    /**
     * Process a single price tick (close = high = low = price).
     * For exact OHLC behavior use {@link #onTick(double, double, double)}.
     */
    public EngineResult onTick(double price) {
        return onTick(price, price, price);
    }

    /**
     * Process a single OHLC tick and emit the current engine state.
     */
    public EngineResult onTick(double high, double low, double close) {
        if (!Double.isFinite(high) || !Double.isFinite(low) || !Double.isFinite(close)) {
            return lastResult != null ? lastResult
                    : new EngineResult(0.0, 0.0, -1.0, 0.0, 0.0, 0.0,
                                       currentDominantCycle, currentPower,
                                       Double.NaN, 0.0, 0);
        }

        barCount++;

        // 1) MEE cycle extraction (matches MesaStrategyMEE.update)
        double medianPrice = 0.5 * (high + low);
        double hp = hpFilter.update(medianPrice);
        double filt = ssFilter.update(hp, LOWER_BOUND);

        pushFiltBuffer(filt);
        if (filtBufferSize == HISTORY_LENGTH) {
            double domCycle = extractMeeCycle(filtBuffer);

            double maxChange = currentDominantCycle * 0.10;
            double change = domCycle - currentDominantCycle;
            if (change > maxChange) domCycle = currentDominantCycle + maxChange;
            else if (change < -maxChange) domCycle = currentDominantCycle - maxChange;

            currentDominantCycle = currentDominantCycle * 0.8 + domCycle * 0.2;
        }
        double dominantCycle = currentDominantCycle;

        // 2) Generate value2_raw (MESA-style whitening + Hann smoothing)
        double deriv = Double.isNaN(prevClose) ? 0.0 : close - prevClose;
        prevClose = close;

        double envel   = envelFilter.update(Math.abs(deriv), dominantCycle);
        double vol     = volSma.update(envel, dominantCycle);
        double value36 = value36Filter.update(vol, dominantCycle);

        double whiten = (value36 != 0.0 && Double.isFinite(value36)) ? filt / value36 : 0.0;
        double wavee  = waveeFilter.update(whiten, dominantCycle);
        pushWaveeHistory(wavee);

        double smoothLength = smoothLenFilter.update(whiten, 5.0);
        double hannValue    = applyDynamicHannFilter(smoothLength);
        double value2Raw    = Double.isNaN(hannValue) ? 0.0 : hannValue;

        // 3) Stream buffers for process_logic
        pushStream(close, dominantCycle, value2Raw);

        // 4) Voss smoothing
        double cycleSmooth = rollingMean(domCycleBuffer, streamSize, CYCLE_SMOOTH_WIN);
        double voss        = computeVoss(cycleSmooth);

        // 5) SSA pure trend
        double ssaTrend = computeSsaTrend(closeBuffer, domCycleBuffer, streamSize);
        double ssaSlope = Double.isNaN(prevSsaTrend) ? 0.0 : ssaTrend - prevSsaTrend;
        prevSsaTrend = ssaTrend;

        // 6) Freeze logic
        double finalV2;
        int isFrozen;
        if (streamSize < 5) {
            finalV2 = voss;
            isFrozen = 0;
            if (Double.isNaN(lastVal)) lastVal = voss;
        } else if (ssaSlope < 0.0) {
            finalV2 = lastVal;
            isFrozen = 1;
        } else {
            finalV2 = voss;
            lastVal = voss;
            isFrozen = 0;
        }
        if (!Double.isFinite(finalV2)) finalV2 = 0.0;

        // 7) Binary opt_weight based on v2_diff (frozen => diff == 0 => -1)
        double v2Diff = Double.isNaN(prevFinalV2) ? 0.0 : Math.abs(finalV2 - prevFinalV2);
        double optWeight = (v2Diff <= FREEZE_DIFF_EPS) ? -1.0 : 1.0;
        prevFinalV2 = finalV2;

        // 8) Elastic Potential Energy tracker — Welford, zero allocations
        boolean currentlyFrozen = (isFrozen == 1);
        double sigmaFreeze;
        double tension;

        if (currentlyFrozen) {
            if (!prevFrozen) {
                // Freeze just started: anchor to the flat line and reset Welford
                freezeAnchor = Double.isFinite(lastVal) ? lastVal : close;
                welfordReset();
            }
            welfordUpdate(close);
            sigmaFreeze = welfordSigma();
            double denom = sigmaFreeze;
            tension = (denom > 0.0 && Double.isFinite(freezeAnchor))
                    ? (freezeAnchor - close) / denom
                    : 0.0;
        } else {
            if (prevFrozen) {
                welfordReset();
            }
            sigmaFreeze = 0.0;
            tension = 0.0;
        }
        prevFrozen = currentlyFrozen;

        if (!Double.isFinite(tension)) tension = 0.0;

        EngineResult res = new EngineResult(
                close,
                finalV2,
                optWeight,
                value2Raw,
                tension,
                sigmaFreeze,
                dominantCycle,
                currentPower,
                ssaTrend,
                ssaSlope,
                isFrozen
        );
        lastResult = res;
        return res;
    }

    // ------------------------------------------------------------------------
    // Welford's online variance — strictly primitive state, zero arrays
    // ------------------------------------------------------------------------

    private void welfordUpdate(double x) {
        if (!Double.isFinite(x)) return;
        welfordCount++;
        double delta = x - welfordMean;
        welfordMean += delta / welfordCount;
        double delta2 = x - welfordMean;
        welfordM2 += delta * delta2;
    }

    private double welfordSigma() {
        if (welfordCount < 2L) return 0.0;
        double variance = welfordM2 / (welfordCount - 1L);
        if (!Double.isFinite(variance) || variance <= 0.0) return 0.0;
        return Math.sqrt(variance);
    }

    private void welfordReset() {
        welfordCount = 0L;
        welfordMean  = 0.0;
        welfordM2    = 0.0;
        freezeAnchor = Double.NaN;
    }

    // ------------------------------------------------------------------------
    // MEE cycle extraction
    // ------------------------------------------------------------------------

    private void pushFiltBuffer(double value) {
        filtBuffer[filtBufferHead] = value;
        filtBufferHead = (filtBufferHead + 1) % HISTORY_LENGTH;
        if (filtBufferSize < HISTORY_LENGTH) filtBufferSize++;
    }

    private double extractMeeCycle(double[] dataArray) {
        if (dataArray.length != HISTORY_LENGTH || !allFinite(dataArray)) {
            return currentDominantCycle;
        }

        // yBlocks[l][i] = data[(l+1)*M + i] - mean(data[l*M .. (l+1)*M-1])
        for (int l = 1; l <= MEE_K; l++) {
            double meanPrev = 0.0;
            int basePrev = (l - 1) * MEE_M;
            for (int i = 0; i < MEE_M; i++) meanPrev += dataArray[basePrev + i];
            meanPrev /= MEE_M;

            int base = l * MEE_M;
            for (int i = 0; i < MEE_M; i++) {
                meeYBlocks[l - 1][i] = dataArray[base + i] - meanPrev;
            }
        }

        // Full autocovariance of each block and the zero-lag variance
        for (int k = 0; k < MEE_K; k++) {
            double meanY = 0.0;
            for (int i = 0; i < MEE_M; i++) meanY += meeYBlocks[k][i];
            meanY /= MEE_M;

            fullAutocovariance(meeYBlocks[k], meanY, meeCovSequences[k]);
            meeVariances[k] = meeCovSequences[k][MEE_M - 1];
        }

        // Right-half autocorrelation of the block variances
        correlateCenterRight(meeVariances, meeVarAutocorr);

        // Solve symmetric Toeplitz system for AR coefficients
        Arrays.fill(meeACoeffs, 0.0);
        if (MEE_P > 0 && meeVarAutocorr.length > MEE_P) {
            RealMatrix R = new Array2DRowRealMatrix(MEE_P, MEE_P);
            RealVector b = new ArrayRealVector(MEE_P);
            for (int i = 0; i < MEE_P; i++) {
                b.setEntry(i, meeVarAutocorr[i + 1]);
                for (int j = 0; j < MEE_P; j++) {
                    R.setEntry(i, j, meeVarAutocorr[Math.abs(i - j)]);
                }
            }
            try {
                DecompositionSolver solver = new LUDecomposition(R).getSolver();
                RealVector sol = solver.solve(b);
                for (int i = 0; i < MEE_P; i++) meeACoeffs[i] = sol.getEntry(i);
            } catch (Exception ignored) {
                Arrays.fill(meeACoeffs, 0.0);
            }
        }

        // Predicted covariance = AR combination of the most recent blocks' covariances
        Arrays.fill(meePredictedCov, 0.0);
        for (int b = 0; b < meeACoeffs.length; b++) {
            int blockIdx = MEE_K - 1 - b;
            double a = meeACoeffs[b];
            for (int i = 0; i < MEE_COV_LEN; i++) {
                meePredictedCov[i] += a * meeCovSequences[blockIdx][i];
            }
        }

        if (!allFinite(meePredictedCov)) return currentDominantCycle;

        // Pad to PAD and FFT
        Arrays.fill(meePaddedCov, 0.0);
        int startIdx = (PAD - MEE_COV_LEN) / 2;
        System.arraycopy(meePredictedCov, 0, meePaddedCov, startIdx, MEE_COV_LEN);

        Complex[] spectrum = meeFft.transform(meePaddedCov, TransformType.FORWARD);

        int validLen = PAD / 2 - 1;
        double maxPsd = -1.0;
        int peakIdx = -1;
        for (int i = 1; i < PAD / 2; i++) {
            double mag = Math.max(spectrum[i].abs(), 1e-10);
            if (mag > maxPsd) {
                maxPsd = mag;
                peakIdx = i - 1;
            }
        }

        if (peakIdx >= 0 && peakIdx < validLen) {
            double freq = (double) (peakIdx + 1) / PAD;
            double cyclePeriod = freq > 0.0 ? 1.0 / freq : UPPER_BOUND;
            double bounded = Math.max(LOWER_BOUND, Math.min(UPPER_BOUND, cyclePeriod));
            double meanPsd = meanSpectrum(spectrum, validLen);
            currentPower = (meanPsd > 0.0) ? (maxPsd / meanPsd) : 0.0;
            return bounded;
        }

        return currentDominantCycle;
    }

    private static void fullAutocovariance(double[] y, double mean, double[] out) {
        int n = y.length;
        Arrays.fill(out, 0.0);
        double meanSq = mean * mean;
        for (int lag = -(n - 1); lag <= n - 1; lag++) {
            double sum = 0.0;
            int start = Math.max(0, -lag);
            int end   = Math.min(n, n - lag);
            for (int i = start; i < end; i++) {
                sum += y[i] * y[i + lag];
            }
            out[lag + n - 1] = (sum / n) - meanSq;
        }
    }

    private static void correlateCenterRight(double[] y, double[] out) {
        int n = y.length;
        for (int lag = 0; lag < n; lag++) {
            double sum = 0.0;
            for (int i = 0; i < n - lag; i++) {
                sum += y[i] * y[i + lag];
            }
            out[lag] = sum / n;
        }
    }

    private static double meanSpectrum(Complex[] spectrum, int validLen) {
        double sum = 0.0;
        for (int i = 1; i <= validLen; i++) sum += spectrum[i].abs();
        return sum / validLen;
    }

    // ------------------------------------------------------------------------
    // Hann smoothing used to produce value2_raw
    // ------------------------------------------------------------------------

    private void pushWaveeHistory(double wavee) {
        waveeHistory[waveeHead] = Double.isFinite(wavee) ? wavee : 0.0;
        waveeHead = (waveeHead + 1) % WAVEE_HISTORY;
        if (waveeSize < WAVEE_HISTORY) waveeSize++;
    }

    private double applyDynamicHannFilter(double whiteN) {
        if (!Double.isFinite(whiteN) || whiteN <= 0.0) return lastHann;
        int window = (int) Math.floor(whiteN);
        if (window <= 0 || waveeSize < window) return lastHann;

        double filt = 0.0, coef = 0.0;
        for (int count = 1; count <= window; count++) {
            int idx = waveeHead - count;
            idx %= WAVEE_HISTORY;
            if (idx < 0) idx += WAVEE_HISTORY;
            double v = waveeHistory[idx];
            if (!Double.isFinite(v)) continue;
            double weight = 1.0 - Math.cos(Math.toRadians(360.0 * count / (whiteN + 1.0)));
            filt += weight * v;
            coef += weight;
        }
        if (coef == 0.0) return lastHann;
        lastHann = filt / coef;
        return lastHann;
    }

    // ------------------------------------------------------------------------
    // Streaming process_logic helpers
    // ------------------------------------------------------------------------

    private void pushStream(double close, double domCycle, double value2Raw) {
        int idx = streamHead;
        closeBuffer[idx]      = close;
        domCycleBuffer[idx]   = domCycle;
        value2RawBuffer[idx]  = value2Raw;
        vossResultBuffer[idx] = 0.0;
        streamHead = (streamHead + 1) % closeBuffer.length;
        if (streamSize < closeBuffer.length) streamSize++;
    }

    private double rollingMean(double[] buffer, int size, int window) {
        if (size <= 0) return Double.NaN;
        int w = Math.min(window, size);
        double sum = 0.0;
        for (int i = 0; i < w; i++) {
            int idx = streamHead - 1 - i;
            idx %= buffer.length;
            if (idx < 0) idx += buffer.length;
            sum += buffer[idx];
        }
        return sum / w;
    }

    private double computeVoss(double cycleSmooth) {
        if (streamSize == 0) return 0.0;
        int idxNewest = streamHead - 1;
        if (idxNewest < 0) idxNewest += closeBuffer.length;

        if (!Double.isFinite(cycleSmooth)) cycleSmooth = 2.0;
        double delayFloat = Math.max(2.0, cycleSmooth / 8.0);
        int delayInt = (int) delayFloat;
        double delayFrac = delayFloat - delayInt;

        double rawNow = value2RawBuffer[idxNewest];
        if (!Double.isFinite(rawNow)) rawNow = 0.0;

        double voss;
        if (streamSize > delayInt + 1) {
            double past1 = value2RawBuffer[wrap(idxNewest - delayInt)];
            double past2 = value2RawBuffer[wrap(idxNewest - (delayInt + 1))];
            double pastVal = (1.0 - delayFrac) * past1 + delayFrac * past2;
            voss = 3.5 * rawNow - 2.5 * pastVal;
        } else if (streamSize > delayInt) {
            double past1 = value2RawBuffer[wrap(idxNewest - delayInt)];
            voss = 3.5 * rawNow - 2.5 * past1;
        } else {
            voss = rawNow;
        }

        if (!Double.isFinite(voss)) voss = rawNow;
        vossResultBuffer[idxNewest] = voss;

        // 3-tick rolling mean of voss results
        double smoothed = rollingMean(vossResultBuffer, streamSize, VOSS_SMOOTH_WIN);
        return Double.isFinite(smoothed) ? smoothed : voss;
    }

    private int wrap(int idx) {
        idx %= closeBuffer.length;
        return (idx < 0) ? idx + closeBuffer.length : idx;
    }

    private double computeSsaTrend(double[] closeBuf, double[] cycleBuf, int size) {
        if (size == 0) return Double.NaN;
        int idxNewest = streamHead - 1;
        if (idxNewest < 0) idxNewest += closeBuf.length;
        double domCycle = cycleBuf[idxNewest];
        if (!Double.isFinite(domCycle)) return closeBuf[idxNewest];

        int L = Math.max(4, (int) Math.round(domCycle / 2.0));
        int Nssa = L * 2;
        int K = Nssa - L + 1;

        if (size < Nssa) return closeBuf[idxNewest];

        double[] data = new double[Nssa];
        for (int i = 0; i < Nssa; i++) {
            data[i] = closeBuf[wrap(idxNewest - (Nssa - 1 - i))];
        }
        if (!allFinite(data)) return closeBuf[idxNewest];

        try {
            RealMatrix X = new Array2DRowRealMatrix(L, K);
            for (int j = 0; j < K; j++) {
                for (int i = 0; i < L; i++) {
                    X.setEntry(i, j, data[j + i]);
                }
            }

            SingularValueDecomposition svd = new SingularValueDecomposition(X);
            RealMatrix U = svd.getU();
            RealMatrix V = svd.getV();
            double[] sigma = svd.getSingularValues();

            if (sigma.length == 0 || U == null || V == null) return closeBuf[idxNewest];

            // Rank-1 reconstruction: X1_{L-1,K-1} = sigma[0] * U[L-1,0] * V[K-1,0]
            double x1Last = sigma[0] * U.getEntry(L - 1, 0) * V.getEntry(K - 1, 0);
            return Double.isFinite(x1Last) ? x1Last : closeBuf[idxNewest];
        } catch (Exception e) {
            return closeBuf[idxNewest];
        }
    }

    // ------------------------------------------------------------------------
    // Utilities
    // ------------------------------------------------------------------------

    private static boolean allFinite(double[] arr) {
        for (double v : arr) {
            if (!Double.isFinite(v)) return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Result holder
    // ------------------------------------------------------------------------
    public static final class EngineResult {
        public final double close;
        public final double value2;
        public final double optWeight;
        public final double rawValue2;
        public final double tension;
        public final double sigmaFreeze;
        public final double dominantCycle;
        public final double power;
        public final double ssaTrend;
        public final double ssaSlope;
        public final int    isFrozen;

        public EngineResult(double close, double value2, double optWeight,
                            double rawValue2, double tension, double sigmaFreeze,
                            double dominantCycle, double power,
                            double ssaTrend, double ssaSlope, int isFrozen) {
            this.close         = close;
            this.value2        = value2;
            this.optWeight     = optWeight;
            this.rawValue2     = rawValue2;
            this.tension       = tension;
            this.sigmaFreeze   = sigmaFreeze;
            this.dominantCycle = dominantCycle;
            this.power         = power;
            this.ssaTrend      = ssaTrend;
            this.ssaSlope      = ssaSlope;
            this.isFrozen      = isFrozen;
        }
    }

    // ------------------------------------------------------------------------
    // Stateful DSP helpers (mirror the Python classes)
    // ------------------------------------------------------------------------

    private static final class DynamicSMA {
        private final double[] buffer;
        private int head = 0;
        private int size = 0;

        DynamicSMA(int maxLen) {
            this.buffer = new double[maxLen];
        }

        double update(double value, double length) {
            buffer[head] = value;
            head = (head + 1) % buffer.length;
            if (size < buffer.length) size++;

            int l = Math.max(1, (int) Math.round(length));
            if (l > size) l = size;
            double sum = 0.0;
            for (int i = 0; i < l; i++) {
                int idx = head - 1 - i;
                idx %= buffer.length;
                if (idx < 0) idx += buffer.length;
                sum += buffer[idx];
            }
            return sum / l;
        }
    }

    private static final class DynamicSuperSmoother {
        private final double[] priceHist  = new double[2];
        private final double[] smoothHist = new double[2];
        private int currentBar = 0;

        double update(double price, double period) {
            currentBar++;
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2.0 * a1 * Math.cos(Math.toRadians(1.414 * 180.0 / period));
            double c2 = b1;
            double c3 = -a1 * a1;
            double c1 = 1.0 - b1 - c3;

            double out;
            if (currentBar < 3) {
                out = price;
            } else {
                double p1 = priceHist[1];  // previous price
                double s1 = smoothHist[1]; // previous smoother
                double s2 = smoothHist[0]; // two ticks ago
                out = c1 * (price + p1) / 2.0 + c2 * s1 + c3 * s2;
            }

            priceHist[0]  = priceHist[1];
            priceHist[1]  = price;
            smoothHist[0] = smoothHist[1];
            smoothHist[1] = out;
            return out;
        }
    }

    private static final class DynamicUltimateSmoother {
        private final double[] priceHist = new double[3];
        private final double[] usHist    = new double[2];
        private int currentBar = 0;

        double update(double price, double period) {
            currentBar++;
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2.0 * a1 * Math.cos(Math.toRadians(1.414 * 180.0 / period));
            double c2 = b1;
            double c3 = -a1 * a1;
            double c1 = (1.0 + b1 - c3) / 4.0;

            double out;
            if (currentBar < 4) {
                out = price;
            } else {
                double p1  = priceHist[2]; // previous price
                double p2  = priceHist[1]; // two ticks ago
                double us1 = usHist[1];    // previous US
                double us2 = usHist[0];    // two ticks ago
                out = (1.0 - c1) * price
                        + (2.0 * c1 - c2) * p1
                        - (c1 + c3) * p2
                        + c2 * us1
                        + c3 * us2;
            }

            priceHist[0] = priceHist[1];
            priceHist[1] = priceHist[2];
            priceHist[2] = price;
            usHist[0]    = usHist[1];
            usHist[1]    = out;
            return out;
        }
    }

    private static final class HighPassFilter {
        private final double c1, c2, c3;
        private final double[] priceHist = new double[3];
        private final double[] hpHist    = new double[2];
        private int currentBar = 0;

        HighPassFilter(double period) {
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2.0 * a1 * Math.cos(Math.toRadians(1.414 * 180.0 / period));
            this.c2 = b1;
            this.c3 = -a1 * a1;
            this.c1 = (1.0 + this.c2 - this.c3) / 4.0;
        }

        double update(double price) {
            currentBar++;

            double p1, p2;
            if (currentBar <= 1) {
                p1 = price;
                p2 = price;
            } else if (currentBar == 2) {
                p1 = priceHist[2];
                p2 = price;
            } else {
                p1 = priceHist[2];
                p2 = priceHist[1];
            }

            double hp1 = (currentBar <= 1) ? 0.0 : hpHist[1];
            double hp2 = (currentBar <= 2) ? 0.0 : hpHist[0];

            double hp = c1 * (price - 2.0 * p1 + p2) + c2 * hp1 + c3 * hp2;
            if (currentBar < 4) hp = 0.0;

            priceHist[0] = priceHist[1];
            priceHist[1] = priceHist[2];
            priceHist[2] = price;
            hpHist[0]    = hpHist[1];
            hpHist[1]    = hp;
            return hp;
        }
    }
}
