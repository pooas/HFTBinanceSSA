// ============================================================================
// AdaptiveKalmanFusion.java  —  Regime-Gated Random Walk Kalman Filter v2.0
// ----------------------------------------------------------------------------
// Architecture: zero-velocity random walk with a momentum-volatility gate.
//
//   State x        : the true, zero-lag macro trend (the signal we track)
//   Measurement y_k: ssaMacroTrend — the smooth, lagging Ehlers trend from
//                    cpp-ssa-engine (the long-horizon baseline)
//   Gate signal g_k: sgSlope — zero-lag SG derivative from cpp-sg-dsp
//
// The previous kinematic model  x_pred = x + sgSlope * dt  was toxic for
// tick data: the derivative of a discrete random walk (bid-ask bounce) is
// white noise, and feeding it as velocity made the Kalman state vibrate,
// producing constant false Long/Short crossovers.
//
// v2.0 replaces that model with a regime-gated random walk:
//
//   PREDICT (random walk, no velocity):
//     x_{k|k-1} = x_{k-1|k-1}
//     P_{k|k-1} = P_{k-1|k-1} + q_k * dt
//
//   GATE (momentum-volatility threshold, function of measurement noise R):
//     threshold = GATE_STD_MULT * sqrt(R_k)
//     gateOpen  = |sgSlope| > threshold  &&  |innovation| > threshold
//
//   UPDATE (only when gate is open — dense staircase tracking):
//     if gateOpen: full Kalman correction with agile noise adaptation
//     else:        K_k = 0, state frozen, noise estimates held constant
//
// The result is an adaptive staircase: perfectly flat during noise chop,
// then a rapid, dense step toward the macro line only when both momentum
// and measurement disagreement statistically break out of the noise band.
//
// Safety guards retained from v1.3:
//   (1) Innovation clamp — caps e² before it enters the R adaptation to
//       prevent a single outlier from inflating R to infinity.
//   (2) Gravity bound — clamps the posterior state to within a fixed
//       percentage of the macro measurement to prevent runaway divergence.
//
// Hot-path contract: zero heap allocations, primitive doubles only, no
// exceptions thrown on corrupted ZMQ ticks.
// ============================================================================

public class AdaptiveKalmanFusion {

    // ========================================================================
    // Static bounds — HFT discipline: recompile to retune, no runtime drift.
    // ========================================================================

    // --- Measurement noise R (ssaMacroTrend trust) ---
    private static final double R_MIN   = 1e-6;
    private static final double R_MAX   = 1e-2;
    private static final double BETA_R  = 0.01;      // baseline smoothing

    // --- Process noise Q (prediction uncertainty) ---
    private static final double Q_MIN   = 1e-8;
    private static final double Q_MAX   = 1e-4;
    private static final double BETA_Q  = 0.005;     // baseline smoothing

    // --- Initial state covariance ---
    private static final double P_INIT  = 1e-4;

    // --- (1) Innovation clamp (prevents R death spiral) ---
    private static final double MAX_INNOVATION = 100.0;

    // --- (3) Gravity bound (prevents state runaway) ---
    private static final double GRAVITY_BOUND_PCT = 0.02;
    private static final double GRAVITY_BOUND_ABS = 50.0;

    // --- v2.0 Regime gate constants ---
    // Gate threshold = GATE_STD_MULT * sqrt(R_k).  2.0 sigma is a conservative
    // noise band: sgSlope must break two standard deviations of the current
    // measurement-noise estimate before the filter is allowed to move.
    private static final double GATE_STD_MULT = 2.0;

    // When the gate opens, noise estimates adapt faster so the filter can
    // re-tune to the breakout regime within a few ticks (dense tracking).
    private static final double AGILE_BETA_R  = 0.05;
    private static final double AGILE_BETA_Q  = 0.02;

    // ========================================================================
    // Filter state — all primitive doubles, no boxing, no auto-allocation.
    // ========================================================================

    private double x;          // posterior state x_{k|k}
    private double p;          // posterior covariance P_{k|k}
    private double r;          // adaptive measurement noise R_k
    private double q;          // adaptive process noise Q_k
    private double k;          // last Kalman gain K_k (NaN = never stepped)
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
    // step(dt) — v2.0 hot-path tick update
    //
    // Zero-velocity predict.  sgSlope is NOT added to the state; it is used
    // purely as the momentum gate signal.  The dt parameter still scales the
    // process-noise covariance growth so uncertainty grows correctly across
    // irregular tick spacing or ZMQ dropouts, but no velocity is ever applied.
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

        final double dt_safe = (dt > 1e-12 && Double.isFinite(dt)) ? dt : 1.0;

        // ============================================================
        // STEP A — PREDICT (Zero-Velocity Random Walk)
        //
        //   x_{k|k-1} = x_{k-1|k-1}
        //   P_{k|k-1} = P_{k-1|k-1} + q_k * dt
        //
        // The SG slope is deliberately NOT used here.  Adding the derivative
        // of a discrete random walk injects white noise directly into the
        // state and causes high-frequency whipsaw.
        // ============================================================
        final double x_pred = x;
        final double p_pred = p + q * dt_safe;

        // ============================================================
        // STEP B — REGIME GATE (Momentum-Volatility Threshold)
        //
        // threshold_k = GATE_STD_MULT * sqrt(R_k)
        //
        // The gate opens only when BOTH the momentum signal and the
        // measurement disagreement exceed the current noise band.  This
        // creates the staircase: flat in chop, dense step on breakout.
        // ============================================================
        final double e = ssaMacroTrend - x_pred;
        final double e_abs = Math.abs(e);
        final double slope_abs = Math.abs(sgSlope);

        final double noiseStd = Math.sqrt(Math.max(r, R_MIN));
        final double gateThreshold = GATE_STD_MULT * noiseStd;

        final boolean gateOpen = slope_abs > gateThreshold && e_abs > gateThreshold;

        // ============================================================
        // STEP C — UPDATE (Kalman correction only when gate is open)
        //
        // Gate closed: K = 0, state frozen, R/Q held constant.  P continues
        // to grow by q*dt so the filter is ready to snap on the next true
        // breakout.
        //
        // Gate open: full correction plus agile R/Q adaptation.
        // ============================================================
        final double k_local;
        final double x_post;
        final double p_post;
        final double r_new;
        final double q_new;

        if (gateOpen) {
            final double denom = p_pred + r;
            k_local = (denom > 1e-18) ? (p_pred / denom) : 0.0;

            // Raw innovation drives the correction; K downweights it naturally.
            x_post = x_pred + k_local * e;
            p_post = (1.0 - k_local) * p_pred;

            // ---- Agile R adaptation with innovation clamp ----
            final double e_capped = Math.min(e_abs, MAX_INNOVATION);
            final double eSq = e_capped * e_capped;
            r_new = (1.0 - AGILE_BETA_R) * r + AGILE_BETA_R * eSq;

            // ---- Agile Q adaptation (Sage-Husa statistic) ----
            final double q_innovation = k_local * k_local * p_pred;
            q_new = (1.0 - AGILE_BETA_Q) * q + AGILE_BETA_Q * q_innovation;
        } else {
            // Regime chop: freeze the staircase.
            k_local = 0.0;
            x_post = x_pred;
            p_post = p_pred;
            r_new = r;
            q_new = q;
        }

        // ============================================================
        // (2) GRAVITY BOUND — max-distance clamp from measurement
        //
        // Applied to the posterior regardless of gate state.  This is the
        // last-resort safety net if a long closure left x far behind a
        // drifting macro line.
        // ============================================================
        double x_bounded = x_post;
        if (Double.isFinite(ssaMacroTrend)) {
            final double gravDist = Math.max(
                GRAVITY_BOUND_PCT * Math.abs(ssaMacroTrend),
                GRAVITY_BOUND_ABS);
            if (x_bounded > ssaMacroTrend + gravDist) {
                x_bounded = ssaMacroTrend + gravDist;
            } else if (x_bounded < ssaMacroTrend - gravDist) {
                x_bounded = ssaMacroTrend - gravDist;
            }
        }

        // ============================================================
        // STEP D — COMMIT state + clamp noise bounds
        //
        // All state mutations happen here after every guard has passed.
        // A mid-step numerical failure leaves the previous state intact.
        // ============================================================
        if (!Double.isFinite(x_bounded) || !Double.isFinite(p_post) ||
            !Double.isFinite(r_new) || !Double.isFinite(q_new) ||
            Double.isNaN(k_local) || Double.isInfinite(k_local)) {
            x = ssaMacroTrend;
            p = P_INIT;
            r = R_MAX;
            q = Q_MAX;
            k = 0.0;
            return;
        }

        x = x_bounded;
        p = p_post;
        r = clamp(r_new, R_MIN, R_MAX);
        q = clamp(q_new, Q_MIN, Q_MAX);
        k = k_local;
    }

    // ========================================================================
    // Clamp helper — primitive only, JIT-inlined.
    // ========================================================================

    private static double clamp(double value, double min, double max) {
        return (value < min) ? min : ((value > max) ? max : value);
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
