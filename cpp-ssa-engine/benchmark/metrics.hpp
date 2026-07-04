#ifndef METRICS_HPP
#define METRICS_HPP

#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

// ============================================================================
// Benchmark Metrics for SSA Signal Reconstruction Quality
//
// All implementations are numerically stable:
//   - RMSE uses Kahan (compensated) summation
//   - Cross-correlation is normalized by sqrt of both auto-correlations
//   - No division by zero (guarded denominators)
// ============================================================================

namespace metrics {

// ============================================================================
// Cross-Correlation Lag
//
// Finds the integer lag at which the normalized cross-correlation between
// the reconstructed signal and the ground-truth driver is maximized.
//
// Returns: { optimal_lag, correlation_at_peak }
// Positive lag means signal lags behind driver.
// ============================================================================

struct LagResult {
    int    lag;
    double correlation;
};

inline LagResult cross_correlation_lag(const double* signal, const double* driver,
                                       int N, int max_lag)
{
    // Compute means
    double sum_s = 0.0, sum_d = 0.0;
    for (int i = 0; i < N; ++i) {
        sum_s += signal[i];
        sum_d += driver[i];
    }
    const double mean_s = sum_s / N;
    const double mean_d = sum_d / N;

    // Auto-correlations at lag 0 (for normalization)
    double ss = 0.0, dd = 0.0;
    for (int i = 0; i < N; ++i) {
        double ds = signal[i] - mean_s;
        double dd_val = driver[i] - mean_d;
        ss += ds * ds;
        dd += dd_val * dd_val;
    }
    const double denom = std::sqrt(ss * dd);
    if (denom < 1e-30) return { 0, 0.0 };

    // Scan lags in [-max_lag, +max_lag]
    LagResult best = { 0, -2.0 };

    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double cross = 0.0;
        int count = 0;

        for (int i = 0; i < N; ++i) {
            int j = i + lag;
            if (j < 0 || j >= N) continue;
            cross += (signal[i] - mean_s) * (driver[j] - mean_d);
            ++count;
        }

        if (count == 0) continue;

        // Normalize by sqrt of auto-correlations (not count — preserves scale)
        double r = cross / denom;

        if (r > best.correlation) {
            best.correlation = r;
            best.lag = lag;
        }
    }

    return best;
}

// ============================================================================
// RMSE — Root Mean Square Error with Kahan compensated summation
//
// Optionally aligns signal to driver by a given lag offset before computing.
// ============================================================================

inline double rmse(const double* signal, const double* driver, int N, int lag_offset = 0)
{
    double sum = 0.0;
    double comp = 0.0;  // Kahan compensation
    int count = 0;

    for (int i = 0; i < N; ++i) {
        int j = i + lag_offset;
        if (j < 0 || j >= N) continue;

        double err = signal[i] - driver[j];
        double term = err * err;

        // Kahan summation
        double y = term - comp;
        double t = sum + y;
        comp = (t - sum) - y;
        sum = t;

        ++count;
    }

    if (count == 0) return 0.0;
    return std::sqrt(sum / static_cast<double>(count));
}

// ============================================================================
// Phase Tracking Accuracy
//
// Fraction of points where |signal[i] - driver[i+lag]| < threshold * std(driver)
// Higher is better (1.0 = perfect tracking).
// ============================================================================

inline double phase_tracking_accuracy(const double* signal, const double* driver,
                                      int N, double threshold, int lag_offset = 0)
{
    // Compute std(driver)
    double sum = 0.0, sum_sq = 0.0;
    for (int i = 0; i < N; ++i) {
        sum += driver[i];
        sum_sq += driver[i] * driver[i];
    }
    double mean = sum / N;
    double var = sum_sq / N - mean * mean;
    double std_dev = (var > 0.0) ? std::sqrt(var) : 1.0;
    double thr = threshold * std_dev;

    int within = 0;
    int count = 0;

    for (int i = 0; i < N; ++i) {
        int j = i + lag_offset;
        if (j < 0 || j >= N) continue;

        if (std::abs(signal[i] - driver[j]) < thr) {
            ++within;
        }
        ++count;
    }

    if (count == 0) return 0.0;
    return static_cast<double>(within) / static_cast<double>(count);
}

// ============================================================================
// Regime Transition Delay
//
// Measures how many samples after a known transition point it takes for
// the reconstructed signal to cross a detection threshold relative to
// the pre/post transition levels.
//
// detection_threshold ∈ (0,1): fraction of the level change that must be
// reflected in the signal for "detection" to be declared.
//
// Returns number of samples delay (0 = immediate), or N if never detected.
// ============================================================================

inline int regime_transition_delay(const double* signal, int N,
                                   int transition_point, double detection_threshold)
{
    if (transition_point <= 5 || transition_point >= N - 5) return N;

    // Estimate pre-transition level (mean of 5 samples before)
    double pre_level = 0.0;
    for (int i = transition_point - 5; i < transition_point; ++i) {
        pre_level += signal[i];
    }
    pre_level /= 5.0;

    // Estimate post-transition level (mean of 5 samples well after)
    int post_start = std::min(transition_point + 20, N - 5);
    double post_level = 0.0;
    for (int i = post_start; i < post_start + 5; ++i) {
        post_level += signal[i];
    }
    post_level /= 5.0;

    double level_change = post_level - pre_level;
    if (std::abs(level_change) < 1e-12) return 0;

    double target = pre_level + detection_threshold * level_change;

    // Scan forward from transition point
    for (int i = transition_point; i < N; ++i) {
        bool crossed = (level_change > 0)
            ? (signal[i] >= target)
            : (signal[i] <= target);
        if (crossed) {
            return i - transition_point;
        }
    }

    return N - transition_point;
}

} // namespace metrics

#endif // METRICS_HPP
