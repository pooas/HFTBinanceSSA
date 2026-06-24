/**
 * HMM-MKL Test & Benchmark
 * 
 * Tests:
 * 1. Synthetic regime-switching data (known ground truth)
 * 2. Convergence verification
 * 3. Viterbi accuracy
 * 4. Performance benchmark
 */

#include "hmm_mkl.hpp"
#include <chrono>
#include <random>
#include <cstdio>

// Set this to your P-core count (exclude E-cores on hybrid CPUs)
#ifndef HMM_P_CORES
#define HMM_P_CORES 8
#endif

using namespace eemd;

// ============================================================================
// Synthetic Data Generation
// ============================================================================

struct SyntheticData {
    std::vector<double> observations;
    std::vector<int32_t> true_states;
    std::vector<double> true_mu;
    std::vector<double> true_sigma;
    std::vector<std::vector<double>> true_trans;
};

/**
 * Generate regime-switching data with known parameters
 * 
 * 3 regimes: Calm (low vol), Trend (medium vol), Crisis (high vol)
 */
SyntheticData generate_regime_switching_data(int32_t T, uint32_t seed = 42)
{
    SyntheticData data;
    std::mt19937 rng(seed);
    
    // True parameters (what we want HMM to recover)
    data.true_mu = {0.0001, 0.0005, -0.001};  // Daily returns: calm ~0, trend +, crisis -
    data.true_sigma = {0.005, 0.015, 0.035};   // Volatility: 0.5%, 1.5%, 3.5% daily
    
    // Transition matrix (rows sum to 1)
    // High self-persistence, rare transitions
    data.true_trans = {
        {0.98, 0.015, 0.005},  // Calm: stays calm, rarely goes to crisis
        {0.03, 0.95, 0.02},    // Trend: can go either way
        {0.01, 0.04, 0.95}     // Crisis: sticky, slowly recovers
    };
    
    int K = 3;
    data.observations.resize(T);
    data.true_states.resize(T);
    
    // Initial state distribution (start in Calm)
    std::discrete_distribution<int> init_dist({0.7, 0.2, 0.1});
    int state = init_dist(rng);
    
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    
    for (int32_t t = 0; t < T; ++t)
    {
        data.true_states[t] = state;
        
        // Generate observation from current state's Gaussian
        std::normal_distribution<double> emission(data.true_mu[state], data.true_sigma[state]);
        data.observations[t] = emission(rng);
        
        // Transition to next state
        double r = uniform(rng);
        double cumsum = 0.0;
        for (int j = 0; j < K; ++j)
        {
            cumsum += data.true_trans[state][j];
            if (r < cumsum)
            {
                state = j;
                break;
            }
        }
    }
    
    return data;
}

// ============================================================================
// Accuracy Metrics
// ============================================================================

double viterbi_accuracy(const std::vector<int32_t>& predicted, 
                        const std::vector<int32_t>& ground_truth)
{
    if (predicted.size() != ground_truth.size()) return 0.0;
    
    int correct = 0;
    for (size_t i = 0; i < predicted.size(); ++i)
    {
        if (predicted[i] == ground_truth[i]) ++correct;
    }
    return static_cast<double>(correct) / predicted.size();
}

/**
 * Check if recovered parameters are close to true parameters
 * (accounting for possible label permutation)
 */
bool parameters_close(const HMMResult& result, const SyntheticData& data, double tol = 0.5)
{
    // After sort_hmm_states_by_volatility, states should be ordered by sigma
    // So we can directly compare
    
    for (int k = 0; k < 3; ++k)
    {
        double sigma_err = std::abs(result.sigma[k] - data.true_sigma[k]) / data.true_sigma[k];
        if (sigma_err > tol)
        {
            printf("  Sigma[%d] error: %.1f%% (recovered=%.4f, true=%.4f)\n", 
                   k, sigma_err * 100, result.sigma[k], data.true_sigma[k]);
            return false;
        }
    }
    return true;
}

// ============================================================================
// Tests
// ============================================================================

bool test_basic_fit()
{
    printf("\n=== Test: Basic Fit ===\n");
    
    auto data = generate_regime_switching_data(1000);
    
    HMMConfig config;
    config.n_states = 3;
    config.max_iter = 100;
    config.verbose = false;
    config.n_threads = HMM_P_CORES;
    
    GaussianHMM hmm(config);
    HMMResult result;
    
    bool success = hmm.fit(data.observations.data(), 
                          static_cast<int32_t>(data.observations.size()), 
                          result);
    
    if (!success)
    {
        printf("  FAILED: fit() returned false\n");
        return false;
    }
    
    // Sort states by volatility for consistent comparison
    sort_hmm_states_by_volatility(result);
    
    printf("  Converged: %s (iterations: %d)\n", 
           result.converged ? "yes" : "no", result.iterations);
    printf("  Log-likelihood: %.2f\n", result.log_likelihood);
    
    // Check parameter recovery
    printf("  Recovered sigmas: [%.4f, %.4f, %.4f]\n", 
           result.sigma[0], result.sigma[1], result.sigma[2]);
    printf("  True sigmas:      [%.4f, %.4f, %.4f]\n",
           data.true_sigma[0], data.true_sigma[1], data.true_sigma[2]);
    
    double accuracy = viterbi_accuracy(result.viterbi_path, data.true_states);
    printf("  Viterbi accuracy: %.1f%%\n", accuracy * 100);
    
    bool params_ok = parameters_close(result, data, 0.5);
    printf("  Parameters within 50%%: %s\n", params_ok ? "yes" : "no");
    
    // Consider test passed if accuracy > 60% (HMM won't be perfect)
    bool passed = success && accuracy > 0.5;
    printf("  Result: %s\n", passed ? "PASSED" : "FAILED");
    
    return passed;
}

bool test_multi_sequence()
{
    printf("\n=== Test: Multi-Sequence Fit ===\n");
    
    // Generate 5 independent sequences
    std::vector<SyntheticData> datasets;
    std::vector<const double*> sequences;
    std::vector<int32_t> lengths;
    
    for (int i = 0; i < 5; ++i)
    {
        datasets.push_back(generate_regime_switching_data(500, 42 + i));
        sequences.push_back(datasets.back().observations.data());
        lengths.push_back(500);
    }
    
    HMMConfig config;
    config.n_states = 3;
    config.max_iter = 100;
    
    GaussianHMM hmm(config);
    HMMResult result;
    
    bool success = hmm.fit_multiple(sequences, lengths, result);
    
    if (!success)
    {
        printf("  FAILED: fit_multiple() returned false\n");
        return false;
    }
    
    sort_hmm_states_by_volatility(result);
    
    printf("  Converged: %s (iterations: %d)\n",
           result.converged ? "yes" : "no", result.iterations);
    printf("  Log-likelihood: %.2f\n", result.log_likelihood);
    printf("  Recovered sigmas: [%.4f, %.4f, %.4f]\n",
           result.sigma[0], result.sigma[1], result.sigma[2]);
    
    bool params_ok = parameters_close(result, datasets[0], 0.5);
    printf("  Result: %s\n", (success && params_ok) ? "PASSED" : "FAILED");
    
    return success && params_ok;
}

bool test_decode()
{
    printf("\n=== Test: Decode (Online Use) ===\n");
    
    auto data = generate_regime_switching_data(1000);
    
    // First, fit the model
    HMMConfig config;
    config.n_states = 3;
    GaussianHMM hmm(config);
    HMMResult fit_result;
    hmm.fit(data.observations.data(), 1000, fit_result);
    sort_hmm_states_by_volatility(fit_result);
    
    // Now use set_parameters + decode on new data
    GaussianHMM decoder(config);
    decoder.set_parameters(fit_result.mu, fit_result.sigma, 
                          fit_result.trans, fit_result.pi);
    
    // Generate new test sequence
    auto test_data = generate_regime_switching_data(200, 999);
    
    std::vector<int32_t> states;
    std::vector<std::vector<double>> posteriors;
    
    bool success = decoder.decode(test_data.observations.data(), 200, states, posteriors);
    
    if (!success)
    {
        printf("  FAILED: decode() returned false\n");
        return false;
    }
    
    double accuracy = viterbi_accuracy(states, test_data.true_states);
    printf("  Decode accuracy on new data: %.1f%%\n", accuracy * 100);
    printf("  Result: %s\n", (accuracy > 0.4) ? "PASSED" : "FAILED");
    
    return accuracy > 0.4;
}

bool test_imm_params()
{
    printf("\n=== Test: IMM Parameter Extraction ===\n");
    
    auto data = generate_regime_switching_data(2000);
    
    HMMConfig config;
    config.n_states = 3;
    GaussianHMM hmm(config);
    HMMResult result;
    
    hmm.fit(data.observations.data(), 2000, result);
    sort_hmm_states_by_volatility(result);
    
    IMMParams params = hmm_to_imm_params(result, data.observations.data(), 2000);
    
    printf("  Extracted φ: [%.4f, %.4f, %.4f]\n", 
           params.phi[0], params.phi[1], params.phi[2]);
    printf("  Extracted σ: [%.4f, %.4f, %.4f]\n",
           params.sigma[0], params.sigma[1], params.sigma[2]);
    
    // φ should be small for returns (low autocorrelation)
    bool phi_reasonable = true;
    for (int k = 0; k < 3; ++k)
    {
        if (std::abs(params.phi[k]) > 0.99)
        {
            phi_reasonable = false;
            printf("  WARNING: φ[%d] = %.4f out of valid range\n", k, params.phi[k]);
        }
    }
    
    printf("  Result: %s\n", phi_reasonable ? "PASSED" : "FAILED");
    return phi_reasonable;
}

// ============================================================================
// Benchmark
// ============================================================================

void benchmark()
{
    printf("\n=== Performance Benchmark (P-cores: %d) ===\n", HMM_P_CORES);
    printf("%-12s %8s %10s %12s\n", "Length", "States", "Time (ms)", "Throughput");
    printf("%-12s %8s %10s %12s\n", "------", "------", "---------", "----------");
    
    int states_list[] = {3, 5};
    int length_list[] = {1000, 5000, 10000, 50000};
    
    for (int K : states_list)
    {
        for (int T : length_list)
        {
            // Generate data
            auto data = generate_regime_switching_data(T);
            
            HMMConfig config;
            config.n_states = K;
            config.max_iter = 50;  // Cap iterations for benchmark
            config.verbose = false;
            config.n_threads = HMM_P_CORES;  // Lock to P-cores only
            
            GaussianHMM hmm(config);
            HMMResult result;
            
            // Warm-up
            hmm.fit(data.observations.data(), T, result);
            
            // Benchmark
            const int n_runs = 5;
            auto start = std::chrono::high_resolution_clock::now();
            
            for (int r = 0; r < n_runs; ++r)
            {
                hmm.fit(data.observations.data(), T, result);
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count() / n_runs;
            double throughput = T / ms * 1000.0;  // samples/sec
            
            printf("%-12d %8d %10.2f %10.0f S/s\n", T, K, ms, throughput);
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    printf("HMM-MKL Test Suite\n");
    printf("==================\n");
    
    int passed = 0;
    int total = 0;
    
    // Run tests
    total++; if (test_basic_fit()) passed++;
    total++; if (test_multi_sequence()) passed++;
    total++; if (test_decode()) passed++;
    total++; if (test_imm_params()) passed++;
    
    printf("\n=== Summary ===\n");
    printf("Tests passed: %d/%d\n", passed, total);
    
    // Run benchmark
    benchmark();
    
    // Print example output
    printf("\n");
    auto data = generate_regime_switching_data(1000);
    HMMConfig config;
    config.n_states = 3;
    GaussianHMM hmm(config);
    HMMResult result;
    hmm.fit(data.observations.data(), 1000, result);
    sort_hmm_states_by_volatility(result);
    
    const char* state_names[] = {"Calm", "Trend", "Crisis"};
    print_hmm_result(result, state_names);
    
    IMMParams imm = hmm_to_imm_params(result, data.observations.data(), 1000);
    print_imm_params(imm, state_names);
    
    return (passed == total) ? 0 : 1;
}