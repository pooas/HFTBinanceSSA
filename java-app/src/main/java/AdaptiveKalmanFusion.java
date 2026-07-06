// ============================================================================
// AdaptiveKalmanFusion.java  —  1D Adaptive Kalman Filter for Sensor Fusion
// ----------------------------------------------------------------------------
// State-space formulation:
//
//   State x        : the true, zero-lag macro trend (the signal we want to track)
//   Control s_k    : sgSlope  — the zero-lag kinematic derivative from cpp-sg-dsp
//   Measurement y_k: ssaMacroTrend — the smooth, lagging Ehlers trend from
//                    cpp-ssa-engine (the long-horizon baseline)
//
// The filter projects the state forward using the zero-lag SG slope, then
// corrects the projection using the smooth macro baseline. The result is a
// fused trend estimate that is simultaneously smooth (Kalman-weighted against
// the macro line) AND low-latency (driven forward by the SG slope).
//
// Adaptive noise tracking (Innovation-based R, posterior-covariance-based Q)
// lets the filter continuously re-tune to non-stationary crypto microstructure
// — widening R in choppy regimes (less trust in macro), tightening R in clean
// trends (more trust in macro); symmetrically for Q and the SG prediction.
//
// Hot-path contract: step() performs ZERO heap allocations. All state is held
// in 4 primitive double fields. NaN/Inf from corrupted ZMQ ticks is absorbed
// without throwing — the filter snaps back to the macro baseline and continues.
// ============================================================================

public class AdaptiveKalmanFusion {

    // ========================================================================
    // Static bounds — tuned for crypto tick data at ~20 Hz polling cadence.
    // ------------------------------------------------------------------------
    // These are `static final` deliberately: HFT discipline forbids runtime
    // tuning of numerical bounds (no configuration drift during live trading).
    // Re-tuning requires a recompile + controlled restart.
    // ========================================================================

    // --- Measurement noise R (ssaMacroTrend's smoothness / trust) ---
    // r_min raises a floor against overconfidence when the innovation is
    // near-zero for long stretches (would otherwise drive K -> 1 and the
    // filter would chase the macro line blindly).
    // r_max raises a ceiling against underconfidence during HMM crisis
    // spikes where the macro line temporarily decouples from price.
    // beta_r = 0.01 -> a 100-tick EMA (5 s at 20 Hz): fast enough to track
    // genuine volatility regime shifts, slow enough to reject tick noise.
    private static final double R_MIN   = 1e-6;
    private static final double R_MAX   = 1e-2;
    private static final double BETA_R  = 0.01;

    // --- Process noise Q (sgSlope's prediction uncertainty) ---
    // q_min floors against the case where the SG slope momentarily flattens
    // (we still want the filter to entertain a sliver of process noise so P
    // doesn't collapse to zero — P=0 permanently locks the filter).
    // q_max caps runaway P during volatile bursts (which would cause K -> 1
    // and the filter would blindly chase SG slope — the opposite failure mode
    // of the one we're trying to fix).
    // beta_q = 0.005 -> a 200-tick EMA (10 s at 20 Hz): slower than beta_r
    // because process-noise estimation has higher variance and needs more
    // samples to converge.
    private static final double Q_MIN   = 1e-8;
    private static final double Q_MAX   = 1e-4;
    private static final double BETA_Q  = 0.005;

    // --- Initial state covariance ---
    // Moderate initial uncertainty: neither overtrust the first measurement
    // (P=0) nor overtrust the first SG prediction (P=∞). The filter converges
    // to steady-state P within ~50-100 ticks.
    private static final double P_INIT  = 1e-4;

    // ========================================================================
    // Filter state — all primitive doubles, no boxing, no auto-allocation.
    // ========================================================================

    // State estimate (posterior): the zero-lag trend estimate x_{k|k}.
    private double x;

    // State covariance (posterior): uncertainty in x.
    private double p;

    // Adaptive measurement noise R_k  (tracks e_k² smoothed).
    private double r;

    // Adaptive process noise Q_k     (tracks K_k² × P_{k|k-1} smoothed).
    private double q;

    // Last computed Kalman gain K_k — kept as a member so getKalmanGain()
    // can expose it for ClickHouse diagnostic logging without re-running any
    // math. Also used as the "has the filter run at least once?" sentinel
    // (Double.NaN initial state means "never stepped").
    private double k;

    // Indicates whether reset() has been called at least once. step() refuses
    // to run until this is true (defensive against cold-start garbage).
    private boolean initialised;

    public AdaptiveKalmanFusion() {
        // Defensive ctor: mark uninitialised so the first step() — if the
        // caller forgets to call reset() — no-ops rather than corrupting state.
        this.x = 0.0;
        this.p = P_INIT;
        this.r = R_MAX;
        this.q = Q_MAX;
        this.k = Double.NaN;
        this.initialised = false;
    }

    // ------------------------------------------------------------------------
    // reset() — call on first tick or after a ZMQ connection drop.
    // Resets the state to the supplied price (typically the first macro trend
    // sample available), covariances to their conservative defaults, and the
    // adaptive noise estimates to their *maximum* bounds so the filter starts
    // defensively (high initial uncertainty — trust neither source until the
    // data tells us otherwise).
    // ------------------------------------------------------------------------
    public void reset(double initialPrice) {
        this.x = initialPrice;
        this.p = P_INIT;
        this.r = R_MAX;   // start defensively: low trust in the macro measurement
        this.q = Q_MAX;   // start defensively: low trust in the SG prediction
        this.k = 0.0;     // neutral gain until we have at least one innovation
        this.initialised = true;
    }

    // ========================================================================
    // step() — the hot-path tick update.
    //
    // Both inputs MUST be finite; if either is NaN/Inf (corrupted ZMQ frame,
    // SG cold-start, etc.), the method will detect this and snap the state
    // to the (presumed-good) ssaMacroTrend baseline, reset covariances, and
    // return. No exceptions are ever thrown.
    //
    // Memory contract: zero heap allocations. Every temporary is a primitive
    // double on the stack frame.
    // ========================================================================
    public void step(double sgSlope, double ssaMacroTrend) {
        // --- (0) Pre-condition + input validation -----------------------------
        if (!initialised) {
            // Caller forgot to call reset() — refuse to run rather than
            // corrupt state. This is a programmer error but we fail safe.
            return;
        }
        if (!Double.isFinite(sgSlope) || !Double.isFinite(ssaMacroTrend)) {
            // Corrupted tick — snap to the macro baseline and recover.
            // We treat ssaMacroTrend as authoritative (it's the smooth
            // measurement; even if it's NaN we can't do better than leave
            // x where it was — see the second guard below).
            if (Double.isFinite(ssaMacroTrend)) {
                x = ssaMacroTrend;
            }
            // If ssaMacroTrend is also NaN, leave x alone — next valid tick
            // will resume the filter cleanly.
            p = P_INIT;
            r = R_MAX;
            q = Q_MAX;
            k = 0.0;
            return;
        }

        // ====================================================================
        // Step A — PREDICT (Time Update)
        // ====================================================================
        // The SG slope is the *control input*: it projects the state forward
        // by one tick. Because SG slope is zero-lag (polynomial fit on a
        // sliding window), this prediction is the lag-cancellation mechanism
        // — we pre-empt where the trend is heading rather than waiting for the
        // IIR macro line to catch up.
        //
        //   x_{k|k-1} = x_{k-1|k-1} + s_k
        //   P_{k|k-1} = P_{k-1|k-1} + q_k
        //
        // p_pred is saved because Step C's adaptive Q update needs the
        // *prior* covariance P_{k|k-1}, not the posterior P_{k|k}.
        // ====================================================================
        final double x_pred = x + sgSlope;
        final double p_pred = p + q;

        // ====================================================================
        // Step B — UPDATE (Measurement Update)
        // ====================================================================
        // The macro trend is the *measurement*: it's the smooth, lagging
        // baseline against we correct our zero-lag prediction. The innovation
        // e_k is "how surprised the filter is by the macro line" — large |e|
        // means the SG slope has been overshooting; small |e| means the SG
        // slope has been accurate.
        //
        //   e_k = y_k - x_{k|k-1}                    (innovation)
        //   K_k = P_{k|k-1} / (P_{k|k-1} + r_k)      (Kalman gain)
        //   x_{k|k} = x_{k|k-1} + K_k · e_k          (state correction)
        //   P_{k|k} = (1 - K_k) · P_{k|k-1}          (covariance contraction)
        //
        // Denominator guard: P + R could be zero only if both P and R have hit
        // their floors simultaneously (extremely low-uncertainty steady
        // state). We add a tiny epsilon to avoid div-by-zero — k becomes 0
        // (no correction) which is the correct behaviour in that regime.
        // ====================================================================
        final double e = ssaMacroTrend - x_pred;          // innovation
        final double denom = p_pred + r;
        final double k_local = (denom > 1e-18) ? (p_pred / denom) : 0.0;
        final double x_post = x_pred + k_local * e;
        final double p_post = (1.0 - k_local) * p_pred;

        // ====================================================================
        // Step C — ADAPT (Online Noise Variance Tracking)
        // ====================================================================
        // Non-stationary crypto microstructure forces us to continuously
        // re-estimate R and Q. We use exponential smoothing on two
        // innovation-flavoured statistics:
        //
        //   R_k = (1 - β_r) · R_{k-1} + β_r · e_k²
        //   Q_k = (1 - β_q) · Q_{k-1} + β_q · (K_k² · P_{k|k-1})
        //
        // Both are hard-clamped to their [MIN, MAX] bounds to keep the
        // filter numerically stable in pathological regimes (HMM crisis,
        // flash crash, dead-flat pre-market, etc.).
        //
        // Q's choice of "K² × P_prior" is the standard Sage-Husa innovation
        // statistic: it captures "how much the prediction diverged from the
        // measurement AFTER we already corrected for it via K". High K and
        // high P_prior (lots of uncertainty + lots of trust in measurement)
        // pushes Q up so the next prediction allows more drift.
        // ====================================================================
        final double eSq = e * e;
        double r_new = (1.0 - BETA_R) * r + BETA_R * eSq;
        if (r_new < R_MIN) r_new = R_MIN;
        else if (r_new > R_MAX) r_new = R_MAX;

        final double qInnovation = k_local * k_local * p_pred;
        double q_new = (1.0 - BETA_Q) * q + BETA_Q * qInnovation;
        if (q_new < Q_MIN) q_new = Q_MIN;
        else if (q_new > Q_MAX) q_new = Q_MAX;

        // ====================================================================
        // Commit state + post-correct safety guards
        // ====================================================================
        // We commit only AFTER all math has been computed, so a mid-step
        // corruption (e.g. an arithmetic overflow producing Inf) cannot
        // leave the filter in a half-updated state. If any guard trips, we
        // snap to the macro baseline (the measurement) and reset covariances.
        // ====================================================================
        if (!Double.isFinite(x_post) || Double.isNaN(k_local) || Double.isInfinite(k_local)) {
            // Math explosion — recover gracefully without throwing.
            x = ssaMacroTrend;       // macro baseline is the safest fallback
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
        k = k_local;   // store last gain for diagnostics
    }

    // ========================================================================
    // Getters — trivially inlined by the JIT, zero cost.
    // ========================================================================

    /** Returns the current fused zero-lag trend estimate x_{k|k}. */
    public double getZeroLagTrend() {
        return x;
    }

    /** Returns the last computed Kalman gain K_k (for ClickHouse/Grafana diagnostics). */
    public double getKalmanGain() {
        return k;
    }

    /** Returns the current measurement-noise estimate R_k (diagnostics). */
    public double getR() {
        return r;
    }

    /** Returns the current process-noise estimate Q_k (diagnostics). */
    public double getQ() {
        return q;
    }

    /** Returns the current state covariance P_{k|k} (diagnostics). */
    public double getP() {
        return p;
    }

    /** Returns true iff reset() has been called at least once. */
    public boolean isInitialised() {
        return initialised;
    }
}