# HMM: What It's Actually For (And What It's Not)

## The Core Insight

**Past predicts near future for VOLATILITY. Past does NOT predict future for RETURNS.**

This distinction determines everything about how to use HMM in a trading stack.

---

## What HMM Learns vs What's Actually Useful

| Parameter | What HMM Learns | Predictive Power | Use It? |
|-----------|-----------------|------------------|---------|
| **σ_k** | Volatility levels per regime | ✅ Strong | Yes |
| **φ_k** | Persistence/dynamics per regime | ⚠️ Moderate | Yes |
| **ν_k** | Tail thickness per regime (Student-t) | ⚠️ Moderate | Yes |
| **P** | Transition frequencies | ⚠️ Weak (timing is random) | Yes (for mixing) |
| **μ_k** | Mean returns per regime | ❌ Useless | **No** |

---

## Why σ Is Predictable But μ Is Not

### Volatility (σ) - Predictable

Volatility is tied to **market structure**:
- Leverage levels
- Liquidity depth
- Participant behavior
- Market microstructure

These change slowly. Crisis volatility in 2020 ≈ crisis volatility in 2024.

```
Vol clustering is real:
  High vol today → High vol tomorrow (persistence)
  Crisis σ ≈ 3-5% across different crises
  Calm σ ≈ 0.5-1% across different calm periods
```

### Returns (μ) - Not Predictable

Returns are tied to **information arrival**:
- News
- Tweets
- Policy announcements
- Black swan events

Information is by definition unpredictable. That's what makes it information.

```
HMM learns from 2020-2024:
  μ_crisis = -0.15% per day

Trump announces crypto ban tomorrow:
  Actual μ = -15% instantly

HMM's μ_k is completely irrelevant.
```

---

## The Trump Test

Any μ estimation method must pass this test:

```
Good μ source:
  Before tweet: Funding rate +0.1% → μ estimate slightly positive
  Trump tweets: Funding rate inverts → μ estimate flips negative
  
  → Signal adapts to new information ✓

Bad μ source (HMM μ_k):
  Before tweet: "Crisis regime has μ = -0.15% historically"
  Trump tweets: Still using -0.15% from historical fit
  
  → Stale, doesn't adapt ✗
```

---

## What HMM Is Actually For

### Use HMM For: Volatility Structure Calibration

```
HMM tells MMPF:
  "Calm looks like:   σ ≈ 0.5%, φ ≈ 0.98, ν ≈ 30"
  "Trend looks like:  σ ≈ 1.5%, φ ≈ 0.95, ν ≈ 10"
  "Crisis looks like: σ ≈ 3.5%, φ ≈ 0.80, ν ≈ 4"

MMPF uses this to:
  - Position particles in the right σ regions
  - Compute emission likelihoods correctly
  - Keep all regimes "warm" via transition matrix

This is about CLASSIFICATION, not PREDICTION.
```

### Don't Use HMM For: Return Prediction

```
HMM μ_k is a historical artifact, not a forecast.

μ_calm = +0.01% means "in past calm periods, average return was +0.01%"
It does NOT mean "next calm period will have +0.01% returns"
```

---

## Where μ Should Actually Come From

### Option 1: Signals (Recommended)

| Signal | Logic | Adapts to News? |
|--------|-------|-----------------|
| Momentum | Recent trend continues short-term | Yes (price reflects news) |
| Mean reversion | Extreme deviation corrects | Yes |
| Funding rate | Arbitrage relationship | Yes (updates real-time) |
| Order flow | Buy/sell imbalance | Yes |
| ICEEMDAN trend IMF | Extracted trend component | Somewhat |

### Option 2: μ = 0 (Honest Ignorance)

If you don't have a signal, admit it:

```cpp
double mu = 0.0;
double mu_var = large;  // High uncertainty

// Kelly becomes pure vol-sizing
f = 0 / (σ² + large) = 0  // Or use minimum position
```

### Option 3: Regime-Weighted Signals (Best of Both)

**Regimes don't give you μ. Regimes tell you WHICH signal to trust.**

```cpp
// Different signals work in different regimes
double mu_momentum = momentum_signal(prices, 20) * 0.0001;
double mu_reversion = mean_reversion_signal(prices, 50) * 0.0001;

// Regime-weight them
double mu = regime_weights[CALM] * mu_reversion     // Calm: mean revert
          + regime_weights[TREND] * mu_momentum     // Trend: momentum
          + regime_weights[CRISIS] * 0.0;           // Crisis: stay flat

// Always maintain high uncertainty
double mu_var = 0.001 * 0.001;
```

---

## The Honest Kelly

### Naive (Dangerous)

```cpp
// Pretending HMM knows μ
f = μ_k / σ_k²

// Trading on historical averages as if they're forecasts
// Will blow up on regime-inconsistent news
```

### Honest (Recommended)

```cpp
// μ from signals, with large uncertainty
double mu = signal_based_mu;           // Small, possibly wrong
double mu_var = 0.001 * 0.001;         // Large uncertainty
double sigma_t = rbpf_get_sigma();     // Accurate from RBPF

f = 0.25 * mu / (sigma_t * sigma_t + lambda * mu_var);
//  ^^^^                              ^^^^^^^^^^^^^^^^^
//  Fractional Kelly (safety)         Uncertainty dominates
```

This formulation means:
- **Small positions** (safe)
- **Scales with 1/σ²** (your strength - accurate vol tracking)
- **Doesn't bet big on μ** (honest about uncertainty)

---

## Revised Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         HMM                                 │
│                                                             │
│   Learns: σ_k, φ_k, ν_k, P                                  │
│   Ignores: μ_k (or sets to 0)                               │
│                                                             │
│   Purpose: Volatility structure calibration                 │
│            NOT return prediction                            │
│                                                             │
└─────────────────────────┬───────────────────────────────────┘
                          │
                          │ (σ_k, φ_k, ν_k, P)
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                        MMPF                                 │
│                                                             │
│   Uses HMM params for:                                      │
│   - Emission likelihoods (which σ_k fits observation?)      │
│   - State dynamics (φ_k per regime)                         │
│   - Transition mixing (P keeps regimes warm)                │
│                                                             │
│   Outputs: regime_weights[K]                                │
│                                                             │
└─────────────────────────┬───────────────────────────────────┘
                          │
          ┌───────────────┴───────────────┐
          │                               │
          ▼                               ▼
┌─────────────────────┐       ┌─────────────────────┐
│       RBPF          │       │   Signal Layer      │
│                     │       │                     │
│ Output: σ_t         │       │ Inputs:             │
│ (accurate, fat-     │       │ - Momentum          │
│  tail aware)        │       │ - Mean reversion    │
│                     │       │ - Funding rate      │
│                     │       │ - Order flow        │
│                     │       │                     │
│                     │       │ Output: μ_signal    │
└─────────┬───────────┘       └─────────┬───────────┘
          │                             │
          │                             │
          │         ┌───────────────────┘
          │         │
          ▼         ▼
┌─────────────────────────────────────────────────────────────┐
│                        Kelly                                │
│                                                             │
│   // Regime-weighted signal selection                       │
│   μ = Σ regime_weights[k] * signal_trust[k] * μ_signal     │
│                                                             │
│   // Large uncertainty (honest)                             │
│   μ_var = large                                             │
│                                                             │
│   // Position sizing (σ_t dominates)                        │
│   f = 0.25 * μ / (σ_t² + λ * μ_var)                        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## Signal Trust by Regime

Different signals work in different market conditions:

| Regime | Momentum | Mean Reversion | Funding | Recommended |
|--------|----------|----------------|---------|-------------|
| **Calm** | ❌ Weak (ranging) | ✅ Strong | ⚠️ Moderate | Mean reversion |
| **Trend** | ✅ Strong | ❌ Dangerous | ⚠️ Moderate | Momentum |
| **Crisis** | ❌ Chaotic | ❌ Dangerous | ✅ Strong (extremes) | Funding or flat |

```cpp
// Example implementation
double get_regime_weighted_mu(double* weights, double mu_mom, double mu_rev, double mu_fund) {
    // Calm: trust mean reversion
    // Trend: trust momentum  
    // Crisis: mostly stay flat, maybe funding
    
    return weights[CALM] * (0.8 * mu_rev + 0.2 * mu_fund)
         + weights[TREND] * (0.8 * mu_mom + 0.2 * mu_fund)
         + weights[CRISIS] * (0.3 * mu_fund);  // Mostly flat in crisis
}
```

---

## Summary

### HMM Is:
- A volatility structure calibrator
- A regime classifier trainer
- A provider of (σ_k, φ_k, ν_k, P) for downstream filters

### HMM Is NOT:
- A return predictor
- A source of tradeable μ
- An oracle for future price direction

### The Honest Position:

```
σ side:  High confidence (HMM → MMPF → RBPF)
         Your edge. Size positions based on this.

μ side:  Low confidence (Signals + high uncertainty)
         Fractional Kelly. Survive being wrong.

Combined: Survive when μ is wrong, profit from σ being right.
```

---

## Key Takeaways

1. **Don't use HMM's μ_k for trading.** It's a historical artifact, not a forecast.

2. **Do use HMM's (σ_k, φ_k, ν_k, P).** Volatility structure is stable and predictable.

3. **Get μ from signals** that adapt to new information (momentum, mean reversion, funding).

4. **Regimes help signal selection**, not μ prediction. Different signals work in different regimes.

5. **Keep μ_var large.** Admit uncertainty. Let σ_t dominate Kelly sizing.

6. **Your edge is volatility tracking**, not return prediction. Build around that truth.
