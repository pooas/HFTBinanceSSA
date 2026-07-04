/**
 * SSA Benchmark Runner — standalone test harness
 *
 * Compiles independently of ZeroMQ/ClickHouse. Tests the SSA pipeline modules
 * against synthetic signals to measure reconstruction quality, phase lag,
 * and tracking accuracy across multiple configurations.
 *
 * Configuration matrix:
 *   A: Fixed L=30, single pipeline, rank 2
 *   B: Fixed L=80, single pipeline, rank 2
 *   C: Adaptive L(t), single pipeline, rank 2
 *   D: Adaptive L(t), dual pipeline + soft blend, rank 2
 *
 * Build: cmake target `ssa_benchmark` (no ZMQ/curl link)
 * Run:   ./ssa_benchmark [--csv benchmark_results.csv]
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>
#include <cstring>
#include <chrono>

#include "../ssa_types.hpp"
#include "../ssa_core.hpp"
#include "../recursive_svd.hpp"
#include "../ssa_pipeline.hpp"
#include "../soft_blender.hpp"
#include "../adaptive_l.hpp"
#include "synthetic_data.hpp"
#include "metrics.hpp"

// ============================================================================
// Benchmark Configuration
// ============================================================================

static constexpr int RANK = 2;
static constexpr int WARMUP_FRACTION = 3;  // use first 1/3 of signal for warm-up

struct BenchConfig {
    const char* name;
    bool adaptive;
    bool dual_blend;
    int  fixed_L;       // used if !adaptive
};

static const BenchConfig CONFIGS[] = {
    { "A: Fixed L=30",        false, false, 30  },
    { "B: Fixed L=80",        false, false, 80  },
    { "C: Adaptive (single)", true,  false, 0   },
    { "D: Adaptive (dual)",   true,  true,  0   },
};
static constexpr int NUM_CONFIGS = 4;

// ============================================================================
// Test Signal Definitions
// ============================================================================

struct TestCase {
    const char* name;
    synth::SyntheticSignal (*generate)(int N);
    int N;
    int transition_point; // for transition delay metric (-1 if N/A)
};

static synth::SyntheticSignal gen_trending(int N) {
    return synth::generate_trending(N, 0.08, 0.3);
}

static synth::SyntheticSignal gen_regime_switch(int N) {
    std::vector<synth::RegimeSegment> segs = {
        { 0.10,  N / 4, 0.2 },   // up-trend, low noise
        { -0.15, N / 4, 0.5 },   // down-trend, high noise
        { 0.02,  N / 4, 0.1 },   // slow drift, very low noise
        { 0.20,  N / 4, 0.3 },   // sharp up-trend
    };
    return synth::generate_regime_switch(N, segs);
}

static synth::SyntheticSignal gen_noise_burst(int N) {
    return synth::generate_noise_burst(N, N / 3, N / 6, 2.0, 0.05, 0.15);
}

static synth::SyntheticSignal gen_sideways(int N) {
    return synth::generate_sideways(N, 1.5, 80.0, 0.2);
}

static const TestCase TEST_CASES[] = {
    { "Trending",      gen_trending,      2000,  -1      },
    { "RegimeSwitch",  gen_regime_switch,  2000,  500     },
    { "NoiseBurst",    gen_noise_burst,    2000,  666     },
    { "Sideways",      gen_sideways,       2000,  -1      },
};
static constexpr int NUM_TESTS = 4;

// ============================================================================
// Run SSA processing on a synthetic signal with a given configuration
//
// Returns the reconstructed signal (same length as input, NaN-padded during
// warm-up period).
// ============================================================================

static std::vector<double> run_ssa(const BenchConfig& cfg,
                                   const std::vector<double>& signal)
{
    const int N = static_cast<int>(signal.size());
    const int warmup_len = N / WARMUP_FRACTION;
    std::vector<double> output(N, std::nan(""));

    CircularPriceBuffer buffer;

    // Determine initial L values
    int L_slow = cfg.adaptive ? AdaptiveL::L_MIN + (AdaptiveL::L_MAX - AdaptiveL::L_MIN) / 2
                              : cfg.fixed_L;
    int L_fast = cfg.adaptive ? std::max(AdaptiveL::L_MIN, (L_slow * 4) / 10 | 1)
                              : cfg.fixed_L;

    // Ensure L is odd
    L_slow |= 1;
    L_fast |= 1;

    if (cfg.dual_blend) {
        // --- Configuration D: dual pipeline + adaptive + blend ---
        PipelineController pipeline;
        AdaptiveL adaptive_l(L_slow);

        // Ingest warm-up data
        for (int t = 0; t < warmup_len; ++t) {
            buffer.push(signal[t]);
        }

        pipeline.configure(&buffer, L_fast, L_slow, RANK);
        pipeline.warm_up();

        // Process remaining samples
        for (int t = warmup_len; t < N; ++t) {
            buffer.push(signal[t]);

            // Simulate calm regime for adaptive benchmarks (no HMM in standalone)
            double p_calm = 0.6, p_trend = 0.3, p_crisis = 0.1;
            AdaptiveLResult lr = adaptive_l.update(p_calm, p_trend, p_crisis);
            if (lr.changed) {
                pipeline.resize(lr.L_fast, lr.L_slow);
            }

            PipelineOutput pout = pipeline.step();
            if (pout.valid) {
                BlendedOutput blended = SoftBlender::blend(
                    pout.fast, pout.slow, p_calm, p_trend, p_crisis);
                output[t] = blended.smoothed;
            }
        }
    } else if (cfg.adaptive) {
        // --- Configuration C: single adaptive pipeline ---
        SsaPipeline pipeline;
        AdaptiveL adaptive_l(L_slow);

        for (int t = 0; t < warmup_len; ++t) {
            buffer.push(signal[t]);
        }

        pipeline.configure(&buffer, L_slow, RANK);
        pipeline.warm_up();

        for (int t = warmup_len; t < N; ++t) {
            buffer.push(signal[t]);

            double p_calm = 0.6, p_trend = 0.3, p_crisis = 0.1;
            AdaptiveLResult lr = adaptive_l.update(p_calm, p_trend, p_crisis);
            if (lr.changed) {
                pipeline.resize(lr.L_slow);
            }

            SsaResult result;
            if (pipeline.step(result)) {
                output[t] = result.smoothed;
            }
        }
    } else {
        // --- Configurations A, B: fixed L, single pipeline ---
        SsaPipeline pipeline;

        for (int t = 0; t < warmup_len; ++t) {
            buffer.push(signal[t]);
        }

        pipeline.configure(&buffer, cfg.fixed_L, RANK);
        pipeline.warm_up();

        for (int t = warmup_len; t < N; ++t) {
            buffer.push(signal[t]);

            SsaResult result;
            if (pipeline.step(result)) {
                output[t] = result.smoothed;
            }
        }
    }

    return output;
}

// ============================================================================
// Metric computation for one (config, test) pair
// ============================================================================

struct BenchResult {
    const char* config_name;
    const char* test_name;
    int    phase_lag;
    double correlation;
    double rmse_val;
    double tracking_accuracy;
    int    transition_delay;
    double elapsed_us;
};

static BenchResult evaluate(const BenchConfig& cfg, const TestCase& tc)
{
    // Generate synthetic data
    synth::SyntheticSignal data = tc.generate(tc.N);
    const int N = tc.N;
    const int warmup_len = N / WARMUP_FRACTION;

    // Time the SSA processing
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<double> reconstructed = run_ssa(cfg, data.signal);
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // Extract valid (non-NaN) portion for metric computation
    int valid_start = warmup_len;
    int valid_len = N - valid_start;

    // Build contiguous arrays for metric functions
    std::vector<double> sig_valid(valid_len);
    std::vector<double> drv_valid(valid_len);
    bool has_valid = false;

    for (int i = 0; i < valid_len; ++i) {
        double v = reconstructed[valid_start + i];
        if (std::isnan(v)) {
            sig_valid[i] = data.signal[valid_start + i]; // fallback
        } else {
            sig_valid[i] = v;
            has_valid = true;
        }
        drv_valid[i] = data.driver[valid_start + i];
    }

    BenchResult res;
    res.config_name = cfg.name;
    res.test_name = tc.name;
    res.elapsed_us = elapsed_us;

    if (!has_valid) {
        res.phase_lag = -1;
        res.correlation = 0.0;
        res.rmse_val = 999.0;
        res.tracking_accuracy = 0.0;
        res.transition_delay = -1;
        return res;
    }

    // Cross-correlation lag
    int max_lag = std::min(50, valid_len / 4);
    metrics::LagResult lr = metrics::cross_correlation_lag(
        sig_valid.data(), drv_valid.data(), valid_len, max_lag);
    res.phase_lag = lr.lag;
    res.correlation = lr.correlation;

    // RMSE (aligned by detected lag)
    res.rmse_val = metrics::rmse(sig_valid.data(), drv_valid.data(), valid_len, lr.lag);

    // Phase tracking accuracy (threshold = 0.5 std)
    res.tracking_accuracy = metrics::phase_tracking_accuracy(
        sig_valid.data(), drv_valid.data(), valid_len, 0.5, lr.lag);

    // Regime transition delay (if applicable)
    if (tc.transition_point > 0 && tc.transition_point > valid_start) {
        int rel_tp = tc.transition_point - valid_start;
        res.transition_delay = metrics::regime_transition_delay(
            sig_valid.data(), valid_len, rel_tp, 0.5);
    } else {
        res.transition_delay = -1;
    }

    return res;
}

// ============================================================================
// Output Formatting
// ============================================================================

static void print_table(const std::vector<BenchResult>& results)
{
    std::cout << "\n";
    std::cout << "| Config                  | Test          | Lag | Corr   | RMSE     "
              << "| Track% | TransDly | Time(µs)   |\n";
    std::cout << "|-------------------------|---------------|-----|--------|----------"
              << "|--------|----------|------------|\n";

    for (const auto& r : results) {
        std::cout << "| " << std::left << std::setw(24) << r.config_name
                  << "| " << std::setw(14) << r.test_name
                  << "| " << std::setw(4) << r.phase_lag
                  << "| " << std::fixed << std::setprecision(4) << std::setw(7) << r.correlation
                  << "| " << std::setprecision(6) << std::setw(9) << r.rmse_val
                  << "| " << std::setprecision(2) << std::setw(5) << (r.tracking_accuracy * 100.0) << "%"
                  << " | " << std::setw(8);
        if (r.transition_delay >= 0)
            std::cout << r.transition_delay;
        else
            std::cout << "N/A";
        std::cout << " | " << std::setprecision(0) << std::setw(10) << r.elapsed_us
                  << " |\n";
    }

    std::cout << "\n";
}

static void write_csv(const std::vector<BenchResult>& results, const std::string& path)
{
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[BENCH] Failed to open " << path << " for writing.\n";
        return;
    }

    f << "config,test,phase_lag,correlation,rmse,tracking_accuracy,transition_delay,elapsed_us\n";
    for (const auto& r : results) {
        f << r.config_name << ","
          << r.test_name << ","
          << r.phase_lag << ","
          << std::fixed << std::setprecision(6) << r.correlation << ","
          << r.rmse_val << ","
          << r.tracking_accuracy << ","
          << r.transition_delay << ","
          << std::setprecision(1) << r.elapsed_us << "\n";
    }

    f.close();
    std::cout << "[BENCH] Results written to " << path << "\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
    std::cout << "========================================\n";
    std::cout << "  SSA Engine Benchmark Suite\n";
    std::cout << "========================================\n\n";

    std::string csv_path = "benchmark_results.csv";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        }
    }

    std::vector<BenchResult> all_results;
    all_results.reserve(NUM_CONFIGS * NUM_TESTS);

    for (int ci = 0; ci < NUM_CONFIGS; ++ci) {
        for (int ti = 0; ti < NUM_TESTS; ++ti) {
            std::cout << "[BENCH] Running: " << CONFIGS[ci].name
                      << " × " << TEST_CASES[ti].name << "..." << std::flush;

            BenchResult r = evaluate(CONFIGS[ci], TEST_CASES[ti]);
            all_results.push_back(r);

            std::cout << " done (" << std::fixed << std::setprecision(0)
                      << r.elapsed_us << " µs)\n";
        }
    }

    // Print markdown summary table
    print_table(all_results);

    // Write full results to CSV
    write_csv(all_results, csv_path);

    std::cout << "[BENCH] Complete. " << all_results.size() << " evaluations.\n";
    return 0;
}
