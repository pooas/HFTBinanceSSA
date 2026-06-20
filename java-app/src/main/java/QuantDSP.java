import org.apache.commons.math3.complex.Complex;
import org.apache.commons.math3.transform.DftNormalization;
import org.apache.commons.math3.transform.FastFourierTransformer;
import org.apache.commons.math3.transform.TransformType;
import org.ejml.simple.SimpleMatrix;
import org.ejml.simple.SimpleSVD;

import java.util.LinkedList;

public class QuantDSP {

    // ==========================================
    // 1. فیلترهای پایه (دقیقاً معادل پایتون)
    // ==========================================
    public static class HighPassFilter {
        private final double c1, c2, c3;
        private final LinkedList<Double> priceHistory = new LinkedList<>();
        private final LinkedList<Double> hpHistory = new LinkedList<>();
        private int currentBar = 0;

        public HighPassFilter(double period) {
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2 * a1 * Math.cos(Math.toRadians(1.414 * 180 / period));
            this.c2 = b1;
            this.c3 = -a1 * a1;
            this.c1 = (1 + this.c2 - this.c3) / 4;
        }

        public double update(double price) {
            currentBar++;
            double p1 = priceHistory.size() >= 1 ? priceHistory.getLast() : price;
            double p2 = priceHistory.size() >= 2 ? priceHistory.get(priceHistory.size() - 2) : price;
            double hp1 = hpHistory.size() >= 1 ? hpHistory.getLast() : 0.0;
            double hp2 = hpHistory.size() >= 2 ? hpHistory.get(hpHistory.size() - 2) : 0.0;

            double hp = c1 * (price - 2 * p1 + p2) + c2 * hp1 + c3 * hp2;
            if (currentBar < 4) hp = 0.0;

            if (priceHistory.size() >= 3) priceHistory.removeFirst();
            priceHistory.addLast(price);
            if (hpHistory.size() >= 2) hpHistory.removeFirst();
            hpHistory.addLast(hp);

            return hp;
        }
    }

    public static class DynamicSuperSmoother {
        private final LinkedList<Double> priceHist = new LinkedList<>();
        private final LinkedList<Double> smoothHist = new LinkedList<>();
        private int currentBar = 0;

        public double update(double price, double period) {
            currentBar++;
            double a1 = Math.exp(-1.414 * Math.PI / period);
            double b1 = 2 * a1 * Math.cos(Math.toRadians(1.414 * 180 / period));
            double c2 = b1, c3 = -a1 * a1, c1 = 1 - b1 - (-a1 * a1);

            if (currentBar < 3) {
                if (priceHist.size() >= 2) priceHist.removeFirst();
                if (smoothHist.size() >= 2) smoothHist.removeFirst();
                priceHist.addLast(price);
                smoothHist.addLast(price);
                return price;
            }

            double p1 = priceHist.getLast();
            double s1 = smoothHist.getLast();
            double s2 = smoothHist.get(smoothHist.size() - 2);

            double smoother = (c1 * (price + p1) / 2) + c2 * s1 + c3 * s2;

            if (priceHist.size() >= 2) priceHist.removeFirst();
            if (smoothHist.size() >= 2) smoothHist.removeFirst();
            priceHist.addLast(price);
            smoothHist.addLast(smoother);

            return smoother;
        }
    }

    // ==========================================
    // 2. هسته استخراج چرخه MEE (معادل پایتون)
    // ==========================================
    public static class MesaStrategyMEE {
        private final double lowerBound;
        private final double upperBound;
        private final int K;
        private final int p;
        private final HighPassFilter hpFilter;
        private final DynamicSuperSmoother ssFilter;
        private final LinkedList<Double> filtBuffer;
        private double currentDominantCycle = 20.0;

        public MesaStrategyMEE(double lowerBound, double upperBound, int historyLength, int K, int p) {
            this.lowerBound = lowerBound;
            this.upperBound = upperBound;
            this.K = K;
            this.p = p;
            this.hpFilter = new HighPassFilter(upperBound);
            this.ssFilter = new DynamicSuperSmoother();
            this.filtBuffer = new LinkedList<>();
        }

        public double updateAndGetCycle(double close) {
            double hp = hpFilter.update(close);
            double filt = ssFilter.update(hp, lowerBound);

            if (filtBuffer.size() >= 150) filtBuffer.removeFirst();
            filtBuffer.addLast(filt);

            if (filtBuffer.size() == 150) {
                double[] data = filtBuffer.stream().mapToDouble(Double::doubleValue).toArray();
                double domCycle = extractMeeCycle(data);
                
                double maxChange = currentDominantCycle * 0.10;
                double change = domCycle - currentDominantCycle;
                if (change > maxChange) domCycle = currentDominantCycle + maxChange;
                else if (change < -maxChange) domCycle = currentDominantCycle - maxChange;
                
                currentDominantCycle = (currentDominantCycle * 0.8) + (domCycle * 0.2);
            }
            return currentDominantCycle;
        }

        private double extractMeeCycle(double[] dataArray) {
            int N = dataArray.length;
            int M = N / (K + 1);
            if (M < 2) return currentDominantCycle;

            // شبیه‌سازی Split و محاسبه اتوکورلیشن (Auto-correlation)
            double[][] yBlocks = new double[K][M];
            for (int l = 1; l <= K; l++) {
                double meanPrev = 0;
                for (int i = 0; i < M; i++) meanPrev += dataArray[(l - 1) * M + i];
                meanPrev /= M;

                for (int i = 0; i < M; i++) {
                    yBlocks[l - 1][i] = dataArray[l * M + i] - meanPrev;
                }
            }

            double[] variances = new double[K];
            for (int k = 0; k < K; k++) {
                double meanY = 0;
                for (double v : yBlocks[k]) meanY += v;
                meanY /= M;

                double centerCorr = 0;
                for (double v : yBlocks[k]) centerCorr += v * v; // نقطه مرکزی در full_corr
                variances[k] = (centerCorr / M) - (meanY * meanY);
            }

            double[] varAutocorr = correlateFullCenterRight(variances);

            // حل معادله توئپلیتز با EJML
            double[] aCoeffs = new double[p];
            if (p > 0 && varAutocorr.length > p) {
                SimpleMatrix R = new SimpleMatrix(p, p);
                SimpleMatrix rVec = new SimpleMatrix(p, 1);
                for (int i = 0; i < p; i++) {
                    rVec.set(i, 0, varAutocorr[i + 1]);
                    for (int j = 0; j < p; j++) {
                        R.set(i, j, varAutocorr[Math.abs(i - j)]);
                    }
                }
                try {
                    SimpleMatrix aMat = R.solve(rVec);
                    for (int i = 0; i < p; i++) aCoeffs[i] = aMat.get(i, 0);
                } catch (Exception ignored) {} // Fallback to 0
            }

            // پیش‌بینی کوواریانس
            double[] predictedCov = new double[variances.length];
            System.arraycopy(variances, 0, predictedCov, 0, variances.length);
            for (int b = 0; b < aCoeffs.length; b++) {
                int idx = variances.length - 1 - (b + 1);
                if (idx >= 0) {
                    for (int i = 0; i < predictedCov.length; i++) {
                        predictedCov[i] += aCoeffs[b] * variances[idx]; 
                    }
                }
            }

            // تبدیل فوریه (FFT) با پدینگ 2048
            int PAD = 2048;
            double[] paddedCov = new double[PAD];
            int startIdx = (PAD - predictedCov.length) / 2;
            System.arraycopy(predictedCov, 0, paddedCov, startIdx, predictedCov.length);

            FastFourierTransformer fft = new FastFourierTransformer(DftNormalization.STANDARD);
            Complex[] transformed = fft.transform(paddedCov, TransformType.FORWARD);

            double[] validPsd = new double[PAD / 2 - 1];
            double maxPsd = -1;
            int peakIdx = -1;

            for (int i = 1; i < PAD / 2; i++) {
                double mag = Math.max(transformed[i].abs(), 1e-10);
                validPsd[i - 1] = mag;
                if (mag > maxPsd) {
                    maxPsd = mag;
                    peakIdx = i - 1;
                }
            }

            if (peakIdx != -1) {
                double freq = (double) (peakIdx + 1) / PAD;
                double cyclePeriod = freq > 0 ? 1.0 / freq : upperBound;
                return Math.max(lowerBound, Math.min(upperBound, cyclePeriod));
            }

            return currentDominantCycle;
        }

        // پیاده‌سازی متد np.correlate(mode='full') بخش سمت راست
        private double[] correlateFullCenterRight(double[] y) {
            int n = y.length;
            double[] rightHalf = new double[n];
            for (int lag = 0; lag < n; lag++) {
                double sum = 0;
                for (int i = 0; i < n - lag; i++) sum += y[i] * y[i + lag];
                rightHalf[lag] = sum / n;
            }
            return rightHalf;
        }
    }

    // ==========================================
    // 3. موتور دقیق تئوری آشوب (Rigorous Lyapunov via Takens' Embedding)
    // ==========================================
    public static class LyapunovEstimator {
        
        /**
         * محاسبه دقیق نماگر لیاپانوف بر اساس بازسازی فضای فاز (الگوریتم روزنشتین)
         * @param timeSeries داده‌های تصفیه‌شده (خط زرد SSA)
         * @param m بُعدِ تعبیه (Embedding Dimension) - برای فضای سه‌بعدی عدد 3 بدهید
         * @param tau تأخیر زمانی (Time Delay) - فاصله کندل‌ها برای ساخت ابعاد
         * @param theilerWindow پنجره تایلر برای حذف همسایه‌های کاذبِ زمانی
         * @param evolutionSteps چند گام به جلو برویم تا واگرایی را بسنجیم؟
         * @return مقدار دقیق لاندا (بزرگترین نماگر لیاپانوف)
         */
        public static double calculateRigorousLLE(double[] timeSeries, int m, int tau, int theilerWindow, int evolutionSteps) {
            int n = timeSeries.length;
            // تعداد بردارهایی که می‌توان در فضای m-بعدی ساخت
            int numVectors = n - (m - 1) * tau;
            
            // اگر دیتای کافی برای ساخت فضای فاز نداریم، صفر برگردان
            if (numVectors < 50) return 0.0;

            // ----------------------------------------------------
            // مرحله 1: بازسازی فضای فاز (Takens' Phase Space Reconstruction)
            // ----------------------------------------------------
            // هر ردیف یک نقطه در فضای m-بعدی است: [x(t), x(t+tau), x(t+2tau), ...]
            double[][] phaseSpace = new double[numVectors][m];
            for (int i = 0; i < numVectors; i++) {
                for (int d = 0; d < m; d++) {
                    phaseSpace[i][d] = timeSeries[i + d * tau];
                }
            }

            double sumLogDivergence = 0.0;
            int validPairs = 0;

            // ----------------------------------------------------
            // مرحله 2 و 3: جستجوی نزدیک‌ترین همسایه و سنجش واگرایی (Divergence)
            // ----------------------------------------------------
            for (int i = 0; i < numVectors - evolutionSteps; i++) {
                double minDist = Double.MAX_VALUE;
                int nearestNeighbor = -1;

                // یافتن نزدیک‌ترین نقطه (همسایه) در فضای سه‌بعدی
                for (int j = 0; j < numVectors - evolutionSteps; j++) {
                    // اعمال قانون تایلر: نقاطی که از نظر زمانی به هم چسبیده‌اند را مقایسه نکن
                    if (Math.abs(i - j) > theilerWindow) {
                        double dist = euclideanDistance(phaseSpace[i], phaseSpace[j]);
                        
                        // نادیده گرفتن فواصل صفر (همپوشانی کامل)
                        if (dist > 1e-12 && dist < minDist) {
                            minDist = dist;
                            nearestNeighbor = j;
                        }
                    }
                }

                // اگر همسایه‌ای پیدا شد، می‌بینیم که پس از t گام در این فضا چقدر از هم دور شده‌اند
                if (nearestNeighbor != -1) {
                    double distAfterEvol = euclideanDistance(
                            phaseSpace[i + evolutionSteps],
                            phaseSpace[nearestNeighbor + evolutionSteps]
                    );

                    // فرمول روزنشتین: میانگین لگاریتمِ واگراییِ مسیرها
                    if (distAfterEvol > 1e-12) {
                        sumLogDivergence += Math.log(distAfterEvol / minDist);
                        validPairs++;
                    }
                }
            }

            // نرمال‌سازی بر اساس تعداد جفت‌های معتبر و زمان تکامل
            if (validPairs > 0) {
                return (sumLogDivergence / validPairs) / evolutionSteps;
            }
            return 0.0;
        }

        // تابع کمکی برای محاسبه فاصله اقلیدسی در فضای n-بعدی
        private static double euclideanDistance(double[] v1, double[] v2) {
            double sum = 0;
            for (int i = 0; i < v1.length; i++) {
                double diff = v1[i] - v2[i];
                sum += diff * diff;
            }
            return Math.sqrt(sum);
        }
    }

}

