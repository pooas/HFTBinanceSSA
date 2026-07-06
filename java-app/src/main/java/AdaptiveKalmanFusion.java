// ============================================================================
// AdaptiveKalmanFusion.java  —  1D Adaptive Kalman Filter for Sensor Fusion
// ----------------------------------------------------------------------------
// State-space formulation:
//
//   State x        : the true, zero-lag macro trend (the signal we want to track)
//   Control s_k    : sgSlope  — the zero-lag kinematic derivative from cpp-sg-dsp
//   Measurement y_k: ssaMacroTrend — the smooth, lagging Ehlers trend from
//                    cpp-ssa-engine (the long-horizon baseline)
//   dt             : time delta between ticks (in tick units; 1.0 = 1 tick)
//
// The filter projects the state forward using the zero-lag SG slope, then
// corrects the projection using the smooth macro baseline. The result is a
// fused trend estimate that is simultaneously smooth (Kalman-weighted against
// the macro line) AND low-latency (driven forward by the SG slope).
//
// Adaptive noise tracking (Innovation-based R, posterior-covariance-based Q)
// lets the filter continuously re-tune to non-stationary crypto microstructure.
//
// Three protective guards against Kalman divergence (v1.2.0 → v1.3.0):
//   (1) Innovation clamp — caps |e| before the R update to stop the
//       "R-matrix death spiral" where a single SG-slope spike drives
//       R → ∞ → K → 0 → filter goes blind to the measurement.
//   (2) Kinematic dt scaling — x += sgSlope * dt   (not x += sgSlope alone)
//       so the prediction scales correctly with the actual time gap
//       between ticks (essential for non-uniform tick pacing or gap
//       recovery after ZMQ dropouts).
//   (3) Gravity bound — enforces a strict max-distance ceiling between
//       the posterior state x_{k|k} and the measurement y_k. If the
//       filter ever drifts beyond this bound (divergence), the state is
//       clamped back into the measurement's gravity well.
//
// Hot-path contract: step() performs ZERO heap allocations. All state is held
// in 4 primitive double fields plus k and initialised. NaN/Inf from corrupted
// ZMQ ticks is absorbed without throwing.
// ============================================================================

public class AdaptiveKalmanFusion {

    // ========================================================================
    // Static bounds — tuned for crypto tick data at ~20 Hz polling cadence.
    // ========================================================================

    // --- Measurement noise R (ssaMacroTrend's smoothness / trust) ---
    private static final double R_MIN   = 1e-6;
    private static final double R_MAX   = 1e-2;
    private static final double BETA_R  = 0.01;

    // --- Process noise Q (sgSlope's prediction uncertainty) ---
    private static final double Q_MIN   = 1e-8;
    private static final double Q_MAX   = 1e-4;
    private static final double BETA_Q  = 0.005;

    // --- Initial state covariance ---
    private static final double P_INIT  = 1e-4;

    // ========================================================================
    // v1.3.0 Protective bounds — prevent Kalman divergence during volatility
    // ========================================================================

    // --- (1) Innovation clamp ---
    // Caps the absolute innovation error before it is squared for the R
    // adaptation. Without this cap, a single 5% BTC move (e ≈ $3000,
    // e² ≈ 9e6) drives R from 0.01 to hundreds of thousands in one tick,
    // K drops to machine-zero, and the filter permanently ignores the
    // measurement — the "R-matrix death spiral."
    //
    // MAX_INNOVATION = 100.0  →  e²_capped ≤ 10 000 per tick.
    // For BTC at $60k this represents a ~0.16% single-tick price swing —
    // well beyond typical tick noise, yet low enough that the EMA(0.01)
    // will have barely lifted R before the next tick arrives.
    private static final double MAX_INNOVATION = 100.0;

    // --- (3) Gravity bound ---
    // The posterior Kalman state x_{k|k} must never drift farther than
    // GRAVITY_BOUND_PCT × |macro| from the Ehlers macro measurement.
    // If it does, the state is clamped — the macro line IS the ground truth.
    //
    // 2% of $60k = $1200 — several orders of magnitude wider than normal
    // micro-tick noise, but tight enough to prevent runaway.
    // GRAVITY_BOUND_ABS = $50 is a safety floor for very-low-price regimes
    // (e.g. penny stocks, alt-coins, or cold-start where macro ≈ 0).
    private static final double GRAVITY_BOUND_PCT = 0.02;
    private static final double GRAVITY_BOUND_ABS = 50.0;

    // ========================================================================
    // Filter state — all primitive doubles, no boxing, no auto-allocation.
    // ========================================================================

    private double x;          // posterior state x_{k|k} (zero-lag trend estimate)
    private double p;          // posterior covariance P_{k|k}
    private double r;          // adaptive measurement noise R_k
    private double q;          // adaptive process noise Q_k
    private double k;          // last Kalman gain K_k (NaN → never-stepped sentinel)
    private boolean initialised;

    // ========================================================================
    // Constructor
    // ========================================================================

    public AdaptiveKalmanFusion() {
        this.x = 0.0;
        this.p = P_INIT;
        this.r = R_MAX;
        this.q = Q_MAX;
        this.k = Double.NaN;
        this.initialised = false;
    }

    // ========================================================================
    // reset() — first tick / ZMQ-drop re-init
    // ========================================================================

    public void reset(double initialPrice) {
        this.x = initialPrice;
        this.p = P_INIT;
        this.r = R_MAX;
        this.q = Q_MAX;
        this.k = 0.0;
        this.initialised = true;
    }

    // ========================================================================
    // step() — backward-compatible overload (dt = 1.0 tick)
    // ========================================================================

    public void step(double sgSlope, double ssaMacroTrend) {
        step(sgSlope, ssaMacroTrend, 1.0);
    }

    // ========================================================================
    // step(dt) — v1.3.0 kinematic-scaled hot-path tick update
    //
    // Memory contract: zero heap allocations. Every temporary is a primitive
    // double on the stack frame. No exceptions thrown.
    // ========================================================================

    public void step(double sgSlope, double ssaMacroTrend, double dt) {
        // --- (0) Pre-condition checks -------------------------------
        if (!initialised) {
            return;
        }
        if (!Double.isFinite(sgSlope) || !Double.isFinite(ssaMacroTrend)) {
            if (Double.isFinite(ssaMacroTrend)) {
                x = ssaMacroTrend;
            }
            p = P_INIT;
            r = R_MAX;
            q = Q_MAX;
            k = 0.0;
            return;
        }

        // Normalise dt: reject NaN/neg/zero, default to 1.0 tick
        final double dt_safe = (dt > 1e-12 && Double.isFinite(dt)) ? dt : 1.0;

        // ============================================================
        // STEP A — PREDICT (Time Update) — kinematic dt scaling
        //
        //   x_{k|k-1} = x_{k-1|k-1} + s_k · dt
        //   P_{k|k-1} = P_{k-1|k-1} + q_k  · dt
        //
        // p_pred is saved because Step C's Q adaptation requires the
        // prior covariance P_{k|k-1} to compute K_k²·P_{k|k-1}.
        // ============================================================
        final double x_pred = x + sgSlope * dt_safe;
        final double p_pred = p + q * dt_safe;

        // ============================================================
        // STEP B — UPDATE (Measurement Update)
        //
        //   e_k = y_k - x_{k|k-1}             (innovation — raw, for
        //                                        state correction)
        //   K_k = P_{k|k-1} / (P_{k|k-1} + r_k)
        //   x_{k|k} = x_{k|k-1} + K_k · e_k
        //   P_{k|k} = (1 - K_k) · P_{k|k-1}
        // ============================================================
        final double e = ssaMacroTrend - x_pred;
        final double denom = p_pred + r;
        final double k_local = (denom > 1e-18) ? (p_pred / denom) : 0.0;

        // Use the raw (unclamped) innovation for the state correction.
        // K_k naturally downweights large innovations — clamping them
        // for the correction would defeat the filter's noise rejection.
        double x_post = x_pred + k_local * e;
        final double p_post = (1.0 - k_local) * p_pred;

        // ============================================================
        // STEP C — ADAPT (Online noise variance tracking)
        //
        //   R_k = (1-β_r)·R_{k-1} + β_r · (e²_clamped)
        //   Q_k = (1-β_q)·Q_{k-1} + β_q · (K_k²·P_{k|k-1})
        // ============================================================

        // ---- (1) Innovation clamp for R adaptation ----
        // Only the R update uses the clamped innovation. The raw e is
        // used for the state correction above — that's intentional:
        // we want the state to track large moves, but we don't want
        // one outlier to permanently poison the R estimate.
        final double e_abs = Math.abs(e);
        final double e_capped = Math.min(e_abs, MAX_INNOVATION);
        final double eSq = e_capped * e_capped;

        double r_new = (1.0 - BETA_R) * r + BETA_R * eSq;
        if (r_new < R_MIN) r_new = R_MIN;
        else if (r_new > R_MAX) r_new = R_MAX;

        // ---- Q adaptation (Sage-Husa innovation statistic) ----
        final double q_innovation = k_local * k_local * p_pred;
        double q_new = (1.0 - BETA_Q) * q + BETA_Q * q_innovation;
        if (q_new < Q_MIN) q_new = Q_MIN;
        else if (q_new > Q_MAX) q_new = Q_MAX;

        // ============================================================
        // (3) GRAVITY BOUND — max-distance clamp from measurement
        //
        // Enforces that the posterior state never drifts beyond a fixed
        // percentage of the macro trend. If the SG slope pushed the
        // prediction far, the Kalman correction should have pulled it
        // back — but if K was tiny (due to historical R inflation) the
        // correction may have been too weak. This bound is the
        // hard-edged safety net.
        //
        // Applied AFTER the Kalman update so the filter can still
        // partially track large moves — we clamp only extreme
        // divergence, not moderate corrections.
        // ============================================================
        if (Double.isFinite(ssaMacroTrend)) {
            final double gravDist = Math.max(
                GRAVITY_BOUND_PCT * Math.abs(ssaMacroTrend),
                GRAVITY_BOUND_ABS);
            if (x_post > ssaMacroTrend + gravDist) {
                x_post = ssaMacroTrend + gravDist;
            } else if (x_post < ssaMacroTrend - gravDist) {
                x_post = ssaMacroTrend - gravDist;
            }
        }

        // ============================================================
        // COMMIT state + post-correct safety guards
        //
        // State is only mutated AFTER every guard has passed — a
        // mid-step NaN cannot leave the filter half-updated.
        // ============================================================
        if (!Double.isFinite(x_post) || Double.isNaN(k_local) || Double.isInfinite(k_local)) {
            x = ssaMacroTrend;
            p = P_INIT;
            r = R_MAX;
            q = Q_MAX;
            k = 0.0;
            return;
        }

        x = x_post;
        p = p_post;
        r = r_new;
        q = q_new;
        k = k_local;
    }

    // ========================================================================
    // Getters — trivially inlined by the JIT, zero overhead.
    // ========================================================================

    public double getZeroLagTrend()   { return x; }
    public double getKalmanGain()     { return k; }
    public double getR()              { return r; }
    public double getQ()              { return q; }
    public double getP()              { return p; }
    public boolean isInitialised()    { return initialised; }
}