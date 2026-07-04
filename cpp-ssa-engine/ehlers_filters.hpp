#ifndef EHLERS_FILTERS_HPP
#define EHLERS_FILTERS_HPP

#include <cmath>
#include <cstring>
#include <algorithm>

// ============================================================================
// Ehlers IIR Filters — Phase-coherent, guaranteed-stable digital filters
//
// All filters have poles at radius exp(-1.414·π/period) < 1 for any finite
// period, so they CANNOT produce eigenvalue explosions or numerical blowups.
// Unity DC gain on the smoothers means price-level tracking is exact.
//
// State: 2-3 doubles per filter. Zero heap allocations. O(1) per tick.
// ============================================================================

// ============================================================================
// HighPassFilter — 2-pole Butterworth high-pass with fixed cutoff
//
// Removes trend below the cutoff period. Used to detrend price before
// cycle estimation so the MEE sees only the oscillatory component.
//
// H(z) = c1·(1 - 2z⁻¹ + z⁻²) / (1 - c2·z⁻¹ - c3·z⁻²)
// ============================================================================

class HighPassFilter {
public:
    explicit HighPassFilter(double period = 330.0) {
        set_period(period);
        std::memset(p_, 0, sizeof(p_));
        std::memset(hp_, 0, sizeof(hp_));
        bar_ = 0;
    }

    void set_period(double period) {
        const double alpha = 1.414 * M_PI / period;
        const double a1 = std::exp(-alpha);
        const double b1 = 2.0 * a1 * std::cos(alpha);
        c2_ = b1;
        c3_ = -(a1 * a1);
        c1_ = (1.0 + c2_ - c3_) / 4.0;
    }

    double update(double price) {
        ++bar_;

        const double p1 = p_[1];
        const double p2 = p_[0];
        const double hp1 = hp_[1];
        const double hp2 = hp_[0];

        double hp = c1_ * (price - 2.0 * p1 + p2) + c2_ * hp1 + c3_ * hp2;

        if (bar_ < 4) hp = 0.0;

        p_[0] = p_[1];
        p_[1] = price;
        hp_[0] = hp_[1];
        hp_[1] = hp;

        return hp;
    }

    void reset() {
        std::memset(p_, 0, sizeof(p_));
        std::memset(hp_, 0, sizeof(hp_));
        bar_ = 0;
    }

private:
    double c1_, c2_, c3_;
    double p_[2];   // price history: [n-2, n-1]
    double hp_[2];  // highpass history: [n-2, n-1]
    int    bar_;
};

// ============================================================================
// DynamicSuperSmoother — 2-pole low-pass with dynamic period
//
// Ehlers' Super Smoother: equivalent to a 2-pole Butterworth low-pass but
// with better transient response. Coefficients recomputed on each tick
// because the period (from MEE) changes dynamically.
//
// H(z) = c1·(1+z⁻¹)/2 / (1 - c2·z⁻¹ - c3·z⁻²)
// DC gain = 1.0 (exact price-level tracking)
// ============================================================================

class DynamicSuperSmoother {
public:
    DynamicSuperSmoother() : p_prev_(0.0), bar_(0) {
        s_[0] = 0.0;
        s_[1] = 0.0;
    }

    double update(double price, double period) {
        ++bar_;

        const double alpha = 1.414 * M_PI / std::max(period, 2.0);
        const double a1 = std::exp(-alpha);
        const double b1 = 2.0 * a1 * std::cos(alpha);
        const double c2 = b1;
        const double c3 = -(a1 * a1);
        const double c1 = 1.0 - c2 - c3;

        if (bar_ < 3) {
            p_prev_ = price;
            s_[0] = s_[1];
            s_[1] = price;
            return price;
        }

        const double out = c1 * (price + p_prev_) * 0.5
                         + c2 * s_[1]
                         + c3 * s_[0];

        p_prev_ = price;
        s_[0] = s_[1];
        s_[1] = out;

        return out;
    }

    void reset() {
        p_prev_ = 0.0;
        s_[0] = 0.0;
        s_[1] = 0.0;
        bar_ = 0;
    }

private:
    double p_prev_;  // price[n-1]
    double s_[2];    // smoother history: [n-2, n-1]
    int    bar_;
};

// ============================================================================
// DynamicUltimateSmoother — Ehlers' zero-lag IIR smoother
//
// Subtracts a portion of the high-pass component from the input before
// smoothing, achieving near-zero phase lag while maintaining excellent
// smoothness. Dynamic period from MEE.
//
// us(n) = (1-c1)·p(n) + (2c1-c2)·p(n-1) - (c1+c3)·p(n-2)
//       + c2·us(n-1) + c3·us(n-2)
//
// where c1 = (1 + c2 + |c3|) / 4
// DC gain = 1.0
// ============================================================================

class DynamicUltimateSmoother {
public:
    DynamicUltimateSmoother() : bar_(0) {
        std::memset(p_, 0, sizeof(p_));
        std::memset(us_, 0, sizeof(us_));
    }

    double update(double price, double period) {
        ++bar_;

        const double alpha = 1.414 * M_PI / std::max(period, 2.0);
        const double a1 = std::exp(-alpha);
        const double b1 = 2.0 * a1 * std::cos(alpha);
        const double c2 = b1;
        const double c3 = -(a1 * a1);
        const double c1 = (1.0 + c2 - c3) / 4.0;  // = (1 + b1 + a1²) / 4

        if (bar_ < 4) {
            p_[0] = p_[1];
            p_[1] = price;
            us_[0] = us_[1];
            us_[1] = price;
            return price;
        }

        const double us = (1.0 - c1) * price
                        + (2.0 * c1 - c2) * p_[1]
                        - (c1 + c3) * p_[0]
                        + c2 * us_[1]
                        + c3 * us_[0];

        p_[0] = p_[1];
        p_[1] = price;
        us_[0] = us_[1];
        us_[1] = us;

        return us;
    }

    void reset() {
        std::memset(p_, 0, sizeof(p_));
        std::memset(us_, 0, sizeof(us_));
        bar_ = 0;
    }

private:
    double p_[2];   // price history: [n-2, n-1]
    double us_[2];  // ultimate smoother history: [n-2, n-1]
    int    bar_;
};

#endif // EHLERS_FILTERS_HPP
