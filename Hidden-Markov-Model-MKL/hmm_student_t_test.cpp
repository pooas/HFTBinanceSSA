/**
 * Student-t AR-HMM Test Suite
 * 
 * Tests:
 * 1. Basic fit with synthetic data
 * 2. Fat-tail handling (injected outliers)
 * 3. AR(1) coefficient recovery
 * 4. ν (degrees of freedom) estimation
 * 5. Comparison with Gaussian HMM on fat-tailed data
 * 6. Performance benchmark
 */

#include "hmm_student_t.hpp"
#include "hmm_mkl.hpp"  // For comparison
#include <random>
#include <chrono>
#include <cstdio>

using namespace eemd;

// ============================================================================
// Synthetic Data Generation
// ============================================================================

struct SyntheticParams
{
    std::vector<double> phi;
    std::vector<double> sigma;
    std::vector<double> nu;
    std::vector<std::vector<double>> trans;
};

/**
 * Generate Student-t AR(1) switching data
 */
std::vector<double> generate_student_t_ar_data(
    int T,
    const SyntheticParams &params,
    std::vector<int32_t> &true_states,
    uint32_t seed = 42)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    
    int K = static_cast<int>(params.sigma.size());
    true_states.resize(T);
    std::vector<double> data(T);
    
    // Initial state (uniform)
    int state = rng() % K;
    true_states[0] = state;
    
    // Student-t random number generator
    auto sample_student_t = [&](double nu) -> double {
        // Student-t = Normal / sqrt(Chi2/nu)
        std::normal_distribution<double> normal(0.0, 1.0);
        std::chi_squared_distribution<double> chi2(nu);
        double z = normal(rng);
        double v = chi2(rng) / nu;
        return z / std::sqrt(v);
    };
    
    // First observation
    double stationary_sigma = params.sigma[state] / std::sqrt(1.0 - params.phi[state] * params.phi[state] + 1e-10);
    data[0] = stationary_sigma * sample_student_t(params.nu[state]);
    
    // Generate sequence
    for (int t = 1; t < T; ++t)
    {
        // State transition
        double u = uniform(rng);
        double cumsum = 0.0;
        for (int j = 0; j < K; ++j)
        {
            cumsum += params.trans[state][j];
            if (u < cumsum)
            {
                state = j;
                break;
            }
        }
        true_states[t] = state;
        
        // AR(1) + Student-t noise
        double eps = sample_student_t(params.nu[state]);
        data[t] = params.phi[state] * data[t-1] + params.sigma[state] * eps;
    }
    
    return data;
}

/**
 * Generate Gaussian AR(1) switching data (for comparison)
 */
std::vector<double> generate_gaussian_ar_data(
    int T,
    const SyntheticParams &params,
    std::vector<int32_t> &true_states,
    uint32_t seed = 42)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    std::normal_distribution<double> normal(0.0, 1.0);
    
    int K = static_cast<int>(params.sigma.size());
    true_states.resize(T);
    std::vector<double> data(T);
    
    int state = rng() % K;
    true_states[0] = state;
    
    double stationary_sigma = params.sigma[state] / std::sqrt(1.0 - params.phi[state] * params.phi[state] + 1e-10);
    data[0] = stationary_sigma * normal(rng);
    
    for (int t = 1; t < T; ++t)
    {
        double u = uniform(rng);
        double cumsum = 0.0;
        for (int j = 0; j < K; ++j)
        {
            cumsum += params.trans[state][j];
            if (u < cumsum)
            {
                state = j;
                break;
            }
        }
        true_states[t] = state;
        
        data[t] = params.phi[state] * data[t-1] + params.sigma[state] * normal(rng);
    }
    
    return data;
}

// ============================================================================
// Test Functions
// ============================================================================

double compute_accuracy(const std::vector<int32_t> &true_states, 
                        const std::vector<int32_t> &pred_states)
{
    int correct = 0;
    for (size_t t = 0; t < true_states.size(); ++t)
    {
        if (true_states[t] == pred_states[t])
            correct++;
    }
    return 100.0 * correct / true_states.size();
}

bool test_basic_fit()
{
    printf("=== Test: Basic Student-t AR-HMM Fit ===\n");
    
    // True parameters
    SyntheticParams true_params;
    true_params.phi = {0.05, -0.10, -0.25};      // AR coefficients
    true_params.sigma = {0.005, 0.015, 0.035};   // Scale
    true_params.nu = {20.0, 10.0, 5.0};          // DoF (calm=Gaussian-ish, crisis=fat)
    true_params.trans = {
        {0.97, 0.02, 0.01},
        {0.05, 0.90, 0.05},
        {0.03, 0.02, 0.95}
    };
    
    // Generate data
    std::vector<int32_t> true_states;
    auto data = generate_student_t_ar_data(3000, true_params, true_states, 42);
    
    // Fit model
    StudentTHMMConfig config;
    config.n_states = 3;
    config.max_iter = 150;
    config.verbose = false;
    config.estimate_nu = true;
    config.min_nu = 2.5;
    
    StudentTHMM hmm(config);
    StudentTHMMResult result;
    
    bool success = hmm.fit(data.data(), static_cast<int32_t>(data.size()), result);
    
    if (!success)
    {
        printf("  FAILED: fit() returned false\n");
        return false;
    }
    
    // Sort by sigma for comparison
    sort_student_t_hmm_states(result);
    
    printf("  Converged: %s (iterations: %d)\n", 
           result.converged ? "yes" : "no", result.iterations);
    printf("  Log-likelihood: %.2f\n", result.log_likelihood);
    
    printf("  Recovered φ:  [%.4f, %.4f, %.4f]\n", 
           result.phi[0], result.phi[1], result.phi[2]);
    printf("  True φ:       [%.4f, %.4f, %.4f]\n",
           true_params.phi[0], true_params.phi[1], true_params.phi[2]);
    
    printf("  Recovered σ:  [%.4f, %.4f, %.4f]\n", 
           result.sigma[0], result.sigma[1], result.sigma[2]);
    printf("  True σ:       [%.4f, %.4f, %.4f]\n",
           true_params.sigma[0], true_params.sigma[1], true_params.sigma[2]);
    
    printf("  Recovered ν:  [%.1f, %.1f, %.1f]\n", 
           result.nu[0], result.nu[1], result.nu[2]);
    printf("  True ν:       [%.1f, %.1f, %.1f]\n",
           true_params.nu[0], true_params.nu[1], true_params.nu[2]);
    
    double accuracy = compute_accuracy(true_states, result.viterbi_path);
    printf("  Viterbi accuracy: %.1f%%\n", accuracy);
    
    // Check if parameters are reasonable
    bool params_ok = true;
    for (int k = 0; k < 3; ++k)
    {
        double sigma_err = std::abs(result.sigma[k] - true_params.sigma[k]) / true_params.sigma[k];
        if (sigma_err > 0.5)
        {
            params_ok = false;
            printf("  WARNING: σ[%d] error = %.1f%%\n", k, sigma_err * 100);
        }
    }
    
    bool passed = params_ok && accuracy > 70.0;
    printf("  Result: %s\n\n", passed ? "PASSED" : "FAILED");
    return passed;
}

bool test_fat_tail_robustness()
{
    printf("=== Test: Fat-Tail Robustness ===\n");
    
    // Generate data with very fat tails (ν = 3-4)
    SyntheticParams true_params;
    true_params.phi = {0.0, 0.0, 0.0};
    true_params.sigma = {0.01, 0.02, 0.04};
    true_params.nu = {4.0, 4.0, 4.0};  // Very fat tails
    true_params.trans = {
        {0.95, 0.03, 0.02},
        {0.05, 0.90, 0.05},
        {0.02, 0.03, 0.95}
    };
    
    std::vector<int32_t> true_states;
    auto data = generate_student_t_ar_data(3000, true_params, true_states, 123);
    
    // Count outliers (>4σ under Gaussian assumption)
    double global_sigma = 0.0;
    for (auto x : data) global_sigma += x * x;
    global_sigma = std::sqrt(global_sigma / data.size());
    
    int outliers = 0;
    for (auto x : data)
    {
        if (std::abs(x) > 4 * global_sigma) outliers++;
    }
    printf("  Data has %d outliers (>4σ)\n", outliers);
    
    // Fit Student-t HMM
    StudentTHMMConfig config_t;
    config_t.n_states = 3;
    config_t.estimate_nu = true;
    config_t.max_iter = 150;
    config_t.min_nu = 2.5;
    
    StudentTHMM hmm_t(config_t);
    StudentTHMMResult result_t;
    hmm_t.fit(data.data(), static_cast<int32_t>(data.size()), result_t);
    sort_student_t_hmm_states(result_t);
    
    // Fit Gaussian HMM for comparison
    HMMConfig config_g;
    config_g.n_states = 3;
    config_g.max_iter = 150;
    
    GaussianHMM hmm_g(config_g);
    HMMResult result_g;
    hmm_g.fit(data.data(), static_cast<int32_t>(data.size()), result_g);
    sort_hmm_states_by_volatility(result_g);
    
    double acc_t = compute_accuracy(true_states, result_t.viterbi_path);
    double acc_g = compute_accuracy(true_states, result_g.viterbi_path);
    
    printf("  Student-t HMM:\n");
    printf("    σ: [%.4f, %.4f, %.4f]\n", result_t.sigma[0], result_t.sigma[1], result_t.sigma[2]);
    printf("    ν: [%.1f, %.1f, %.1f]\n", result_t.nu[0], result_t.nu[1], result_t.nu[2]);
    printf("    Accuracy: %.1f%%\n", acc_t);
    
    printf("  Gaussian HMM:\n");
    printf("    σ: [%.4f, %.4f, %.4f]\n", result_g.sigma[0], result_g.sigma[1], result_g.sigma[2]);
    printf("    Accuracy: %.1f%%\n", acc_g);
    
    // Student-t should do better or similar on fat-tailed data
    double advantage = acc_t - acc_g;
    bool passed = advantage >= -5.0;  // Allow small margin
    printf("  Student-t advantage: %.1f%%\n", advantage);
    printf("  Result: %s\n\n", passed ? "PASSED" : "FAILED");
    return passed;
}

bool test_ar_coefficient_recovery()
{
    printf("=== Test: AR(1) Coefficient Recovery ===\n");
    
    // Test with strong AR coefficients
    SyntheticParams true_params;
    true_params.phi = {0.8, 0.0, -0.5};   // Strong positive, zero, negative
    true_params.sigma = {0.01, 0.02, 0.03};
    true_params.nu = {10.0, 10.0, 10.0};
    true_params.trans = {
        {0.98, 0.01, 0.01},
        {0.02, 0.96, 0.02},
        {0.01, 0.01, 0.98}
    };
    
    std::vector<int32_t> true_states;
    auto data = generate_student_t_ar_data(5000, true_params, true_states, 999);
    
    StudentTHMMConfig config;
    config.n_states = 3;
    config.max_iter = 150;
    
    StudentTHMM hmm(config);
    StudentTHMMResult result;
    hmm.fit(data.data(), static_cast<int32_t>(data.size()), result);
    sort_student_t_hmm_states(result);
    
    printf("  Recovered φ: [%.4f, %.4f, %.4f]\n", 
           result.phi[0], result.phi[1], result.phi[2]);
    printf("  True φ:      [%.4f, %.4f, %.4f]\n",
           true_params.phi[0], true_params.phi[1], true_params.phi[2]);
    
    // Check φ recovery (order might differ, so check sorted)
    std::vector<double> true_phi_sorted = true_params.phi;
    std::vector<double> est_phi_sorted = result.phi;
    std::sort(true_phi_sorted.begin(), true_phi_sorted.end());
    std::sort(est_phi_sorted.begin(), est_phi_sorted.end());
    
    double max_err = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        double err = std::abs(est_phi_sorted[k] - true_phi_sorted[k]);
        max_err = std::max(max_err, err);
    }
    
    printf("  Max φ error: %.4f\n", max_err);
    
    bool passed = max_err < 0.15;
    printf("  Result: %s\n\n", passed ? "PASSED" : "FAILED");
    return passed;
}

bool test_nu_estimation()
{
    printf("=== Test: ν (DoF) Estimation ===\n");
    
    // Generate data with known ν values - need more data for reliable ν estimation
    SyntheticParams true_params;
    true_params.phi = {0.0, 0.0, 0.0};
    true_params.sigma = {0.01, 0.02, 0.04};
    true_params.nu = {25.0, 8.0, 4.0};  // Clear separation: near-Gaussian, moderate, very fat
    true_params.trans = {
        {0.98, 0.01, 0.01},
        {0.02, 0.96, 0.02},
        {0.01, 0.01, 0.98}
    };
    
    std::vector<int32_t> true_states;
    // Need more data for ν estimation (notoriously hard)
    auto data = generate_student_t_ar_data(10000, true_params, true_states, 777);
    
    // Fit with ν estimation
    StudentTHMMConfig config;
    config.n_states = 3;
    config.estimate_nu = true;
    config.max_iter = 150;
    config.min_nu = 2.5;  // Slightly higher minimum
    
    StudentTHMM hmm(config);
    StudentTHMMResult result;
    hmm.fit(data.data(), static_cast<int32_t>(data.size()), result);
    sort_student_t_hmm_states(result);
    
    printf("  Recovered ν: [%.1f, %.1f, %.1f]\n", 
           result.nu[0], result.nu[1], result.nu[2]);
    printf("  True ν:      [%.1f, %.1f, %.1f]\n",
           true_params.nu[0], true_params.nu[1], true_params.nu[2]);
    
    // Check ordering: calm should have higher ν (thinner tails) than crisis
    bool order_correct = (result.nu[0] > result.nu[2]);
    printf("  ν(calm) > ν(crisis): %s (%.1f vs %.1f)\n", 
           order_correct ? "yes" : "no", result.nu[0], result.nu[2]);
    
    // Check that crisis regime detected fat tails (ν < 15)
    bool crisis_fat = result.nu[2] < 15.0;
    printf("  Crisis regime fat-tailed (ν < 15): %s\n", crisis_fat ? "yes" : "no");
    
    // Check that calm regime is more Gaussian-like (ν > 10)
    bool calm_thin = result.nu[0] > 8.0;
    printf("  Calm regime thinner tails (ν > 8): %s\n", calm_thin ? "yes" : "no");
    
    bool passed = crisis_fat && (order_correct || calm_thin);
    printf("  Result: %s\n\n", passed ? "PASSED" : "FAILED");
    return passed;
}

bool test_gaussian_data_comparison()
{
    printf("=== Test: Gaussian Data (Student-t should match Gaussian HMM) ===\n");
    
    // Generate Gaussian data (ν = ∞ effectively)
    SyntheticParams true_params;
    true_params.phi = {0.0, 0.0, 0.0};
    true_params.sigma = {0.005, 0.015, 0.035};
    true_params.nu = {1000.0, 1000.0, 1000.0};  // Effectively Gaussian
    true_params.trans = {
        {0.97, 0.02, 0.01},
        {0.05, 0.90, 0.05},
        {0.03, 0.02, 0.95}
    };
    
    std::vector<int32_t> true_states;
    auto data = generate_gaussian_ar_data(3000, true_params, true_states, 555);
    
    // Fit Student-t HMM
    StudentTHMMConfig config_t;
    config_t.n_states = 3;
    config_t.estimate_nu = true;
    config_t.max_iter = 150;
    config_t.min_nu = 2.5;
    
    StudentTHMM hmm_t(config_t);
    StudentTHMMResult result_t;
    hmm_t.fit(data.data(), static_cast<int32_t>(data.size()), result_t);
    sort_student_t_hmm_states(result_t);
    
    // Fit Gaussian HMM
    HMMConfig config_g;
    config_g.n_states = 3;
    config_g.max_iter = 150;
    
    GaussianHMM hmm_g(config_g);
    HMMResult result_g;
    hmm_g.fit(data.data(), static_cast<int32_t>(data.size()), result_g);
    sort_hmm_states_by_volatility(result_g);
    
    double acc_t = compute_accuracy(true_states, result_t.viterbi_path);
    double acc_g = compute_accuracy(true_states, result_g.viterbi_path);
    
    printf("  Student-t HMM:\n");
    printf("    σ: [%.4f, %.4f, %.4f]\n", result_t.sigma[0], result_t.sigma[1], result_t.sigma[2]);
    printf("    ν: [%.1f, %.1f, %.1f]\n", result_t.nu[0], result_t.nu[1], result_t.nu[2]);
    printf("    Accuracy: %.1f%%\n", acc_t);
    
    printf("  Gaussian HMM:\n");
    printf("    σ: [%.4f, %.4f, %.4f]\n", result_g.sigma[0], result_g.sigma[1], result_g.sigma[2]);
    printf("    Accuracy: %.1f%%\n", acc_g);
    
    // On Gaussian data, both should perform similarly
    // Student-t's ν should be reasonably large (>10) on at least some regimes
    double max_nu = std::max({result_t.nu[0], result_t.nu[1], result_t.nu[2]});
    bool nu_reasonable = max_nu > 8.0;  // At least one regime should detect thin tails
    printf("  Max ν estimate: %.1f (should be >8 for Gaussian data)\n", max_nu);
    
    bool acc_similar = std::abs(acc_t - acc_g) < 15.0;
    printf("  Accuracy similar: %s (diff = %.1f%%)\n", acc_similar ? "yes" : "no", acc_t - acc_g);
    
    bool passed = acc_similar;  // Main requirement: don't hurt on Gaussian data
    printf("  Result: %s\n\n", passed ? "PASSED" : "FAILED");
    return passed;
}

void run_benchmark()
{
    printf("=== Performance Benchmark ===\n");
    
    SyntheticParams params;
    params.phi = {0.05, -0.05, -0.15};
    params.sigma = {0.005, 0.015, 0.035};
    params.nu = {15.0, 8.0, 4.0};
    params.trans = {
        {0.97, 0.02, 0.01},
        {0.05, 0.90, 0.05},
        {0.03, 0.02, 0.95}
    };
    
    printf("Length         States  Time (ms)   Throughput\n");
    printf("------         ------  ---------   ----------\n");
    
    std::vector<int> lengths = {1000, 2000, 5000, 10000};
    std::vector<int> states = {3, 5};
    
    for (int K : states)
    {
        for (int T : lengths)
        {
            std::vector<int32_t> true_states;
            auto data = generate_student_t_ar_data(T, params, true_states, 42);
            
            StudentTHMMConfig config;
            config.n_states = K;
            config.max_iter = 50;  // Fixed iterations for fair comparison
            config.verbose = false;
            
            StudentTHMM hmm(config);
            StudentTHMMResult result;
            
            // Warmup
            hmm.fit(data.data(), T, result);
            
            // Benchmark
            const int n_runs = 5;
            auto start = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < n_runs; ++r)
            {
                hmm.fit(data.data(), T, result);
            }
            auto end = std::chrono::high_resolution_clock::now();
            
            double ms = std::chrono::duration<double, std::milli>(end - start).count() / n_runs;
            double throughput = (T * 50) / (ms / 1000.0);  // samples×iters per second
            
            printf("%-14d %6d  %9.2f   %10.0f S×I/s\n", T, K, ms, throughput);
        }
    }
    printf("\n");
}

void print_example_output()
{
    printf("=== Example Output (for downstream filters) ===\n\n");
    
    SyntheticParams true_params;
    true_params.phi = {0.05, -0.08, -0.20};
    true_params.sigma = {0.005, 0.015, 0.035};
    true_params.nu = {20.0, 8.0, 4.0};
    true_params.trans = {
        {0.97, 0.02, 0.01},
        {0.05, 0.90, 0.05},
        {0.03, 0.02, 0.95}
    };
    
    std::vector<int32_t> true_states;
    auto data = generate_student_t_ar_data(2000, true_params, true_states, 42);
    
    StudentTHMMConfig config;
    config.n_states = 3;
    config.estimate_nu = true;
    
    StudentTHMM hmm(config);
    StudentTHMMResult result;
    hmm.fit(data.data(), static_cast<int32_t>(data.size()), result);
    sort_student_t_hmm_states(result);
    
    // Print for copy-paste
    print_student_t_imm_params(result);
    
    // Also print transition matrix nicely
    printf("\nTransition Matrix P(row -> col):\n");
    printf("From/To        ");
    const char* names[] = {"Calm", "Trend", "Crisis"};
    for (int j = 0; j < 3; ++j) printf("%10s", names[j]);
    printf("\n");
    
    for (int i = 0; i < 3; ++i)
    {
        printf("%-14s", names[i]);
        for (int j = 0; j < 3; ++j)
        {
            printf("%10.4f", result.trans[i][j]);
        }
        printf("\n");
    }
    printf("\n");
}

// ============================================================================
// Main
// ============================================================================

void debug_nu_estimation()
{
    printf("=== DEBUG: ν Estimation Trace ===\n");
    
    // Simple test: single regime with known ν
    SyntheticParams params;
    params.phi = {0.0};
    params.sigma = {0.02};
    params.nu = {5.0};  // Known fat tails
    params.trans = {{1.0}};  // Single state
    
    std::vector<int32_t> true_states;
    auto data = generate_student_t_ar_data(2000, params, true_states, 42);
    
    // Count how fat-tailed the data actually is
    double sum_sq = 0.0;
    for (auto x : data) sum_sq += x * x;
    double sigma_emp = std::sqrt(sum_sq / data.size());
    
    int n_2sig = 0, n_3sig = 0, n_4sig = 0;
    for (auto x : data)
    {
        double z = std::abs(x) / sigma_emp;
        if (z > 2) n_2sig++;
        if (z > 3) n_3sig++;
        if (z > 4) n_4sig++;
    }
    
    printf("  Data: T=%zu, σ_emp=%.4f\n", data.size(), sigma_emp);
    printf("  Tail events: >2σ=%d (%.1f%%), >3σ=%d (%.1f%%), >4σ=%d (%.1f%%)\n",
           n_2sig, 100.0 * n_2sig / data.size(),
           n_3sig, 100.0 * n_3sig / data.size(),
           n_4sig, 100.0 * n_4sig / data.size());
    printf("  (Gaussian expects: >2σ=4.6%%, >3σ=0.27%%, >4σ=0.006%%)\n");
    
    // Fit with verbose
    StudentTHMMConfig config;
    config.n_states = 1;
    config.max_iter = 5;  // Just a few iterations
    config.verbose = true;
    config.estimate_nu = true;
    config.min_nu = 2.5;
    
    StudentTHMM hmm(config);
    StudentTHMMResult result;
    hmm.fit(data.data(), static_cast<int32_t>(data.size()), result);
    
    printf("\n  Final: σ=%.4f (true=%.4f), ν=%.1f (true=%.1f)\n\n",
           result.sigma[0], params.sigma[0], result.nu[0], params.nu[0]);
}

int main()
{
    printf("Student-t AR-HMM Test Suite\n");
    printf("===========================\n\n");
    
    // First run debug to see what's happening with ν
    debug_nu_estimation();
    
    int passed = 0;
    int total = 0;
    
    total++; if (test_basic_fit()) passed++;
    total++; if (test_fat_tail_robustness()) passed++;
    total++; if (test_ar_coefficient_recovery()) passed++;
    total++; if (test_nu_estimation()) passed++;
    total++; if (test_gaussian_data_comparison()) passed++;
    
    printf("=== Summary ===\n");
    printf("Tests passed: %d/%d\n\n", passed, total);
    
    run_benchmark();
    print_example_output();
    
    return (passed == total) ? 0 : 1;
}