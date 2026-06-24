/**
 * Gaussian Hidden Markov Model with Intel MKL - OPTIMIZED
 *
 * Optimizations over original:
 * 1. Scaled forward-backward: No log/exp in inner loops (~2x speedup)
 * 2. VML batched emissions: Vectorized Gaussian PDF computation
 * 3. Contiguous memory layout: Better cache utilization
 * 4. Fused loops where possible
 *
 * Algorithm: Baum-Welch (EM) with scaled forward-backward
 * Reference: Rabiner (1989), "A Tutorial on Hidden Markov Models"
 *
 * License: MIT
 */

#ifndef HMM_MKL_HPP
#define HMM_MKL_HPP

#include "eemd_mkl.hpp" // For AlignedBuffer, SIMD macros, MKL includes

#include <cmath>
#include <limits>
#include <numeric>
#include <omp.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace eemd
{

    // ============================================================================
    // Configuration
    // ============================================================================

    struct HMMConfig
    {
        int32_t n_states = 3;          // K (e.g., Calm, Trend, Crisis)
        int32_t max_iter = 100;        // Baum-Welch iterations
        double convergence_tol = 1e-6; // LL change threshold
        double min_variance = 1e-6;    // Floor to prevent collapse
        uint32_t random_seed = 42;     // For initialization
        bool verbose = false;
        int32_t n_threads = 0;             // 0 = auto, >0 = lock to N threads (set to P-core count)
        int32_t parallel_threshold = 5000; // Only parallelize if T > this value
    };

    // ============================================================================
    // IMM Parameters (defined BEFORE AnchorFader which depends on it)
    // ============================================================================

    struct IMMParams
    {
        std::vector<double> phi;   // AR(1) coefficients
        std::vector<double> sigma; // Process noise
        std::vector<std::vector<double>> trans;
    };

    // ============================================================================
    // Result Structure
    // ============================================================================

    struct HMMResult
    {
        std::vector<double> mu;                      // State means [K]
        std::vector<double> sigma;                   // State std devs [K]
        std::vector<std::vector<double>> trans;      // Transition matrix [K x K]
        std::vector<double> pi;                      // Initial state probs [K]
        std::vector<int32_t> viterbi_path;           // Most likely state sequence [T]
        std::vector<std::vector<double>> posteriors; // P(state|obs) for each t [T x K]
        double log_likelihood;
        int32_t iterations;
        bool converged;
    };

    // ============================================================================
    // Optimized Gaussian HMM Class
    // ============================================================================

    class GaussianHMM
    {
    public:
        explicit GaussianHMM(const HMMConfig &config = HMMConfig())
            : config_(config), K_(config.n_states)
        {
            // Set thread count for OpenMP (only affects parallel regions)
            if (config_.n_threads > 0)
            {
                omp_set_num_threads(config_.n_threads);
            }
            // Note: Affinity is set lazily when parallel regions are actually used
        }

        /**
         * Fit HMM to observations using Baum-Welch algorithm
         */
        bool fit(const double *observations, int32_t T, HMMResult &result)
        {
            if (T < K_ * 2)
                return false;

            allocate_buffers(T);
            initialize_parameters(observations, T);

            double prev_ll = -std::numeric_limits<double>::infinity();

            for (int32_t iter = 0; iter < config_.max_iter; ++iter)
            {
                // E-step: Emissions + Forward-Backward
                compute_emissions_vml(observations, T);
                double ll = forward_scaled(T);
                backward_scaled(T);

                // Fused: Compute posteriors AND accumulate sufficient stats in one pass
                // This eliminates the massive xi buffer write/read
                reset_sufficient_stats();
                accumulate_stats_fused(observations, T);

                if (config_.verbose)
                {
                    printf("HMM iter %d: LL = %.6f\n", iter, ll);
                }

                // Check convergence
                if (std::abs(ll - prev_ll) < config_.convergence_tol)
                {
                    result.converged = true;
                    result.iterations = iter + 1;
                    result.log_likelihood = ll;
                    break;
                }

                prev_ll = ll;
                result.log_likelihood = ll;
                result.iterations = iter + 1;
                result.converged = false;

                // M-step from accumulated stats
                m_step_from_sufficient_stats();
            }

            // Viterbi (still uses log-space for numerical stability in max)
            viterbi_log(observations, T);
            pack_results(T, result);

            return true;
        }

        /**
         * Fit HMM to multiple sequences
         */
        bool fit_multiple(const std::vector<const double *> &sequences,
                          const std::vector<int32_t> &lengths,
                          HMMResult &result)
        {
            if (sequences.empty())
                return false;

            int32_t max_T = *std::max_element(lengths.begin(), lengths.end());
            allocate_buffers(max_T);
            initialize_parameters(sequences[0], lengths[0]);

            double prev_ll = -std::numeric_limits<double>::infinity();

            for (int32_t iter = 0; iter < config_.max_iter; ++iter)
            {
                reset_sufficient_stats();
                double total_ll = 0.0;

                for (size_t s = 0; s < sequences.size(); ++s)
                {
                    const double *obs = sequences[s];
                    int32_t T = lengths[s];

                    compute_emissions_vml(obs, T);
                    double ll = forward_scaled(T);
                    backward_scaled(T);
                    total_ll += ll;

                    // Fused accumulation for this sequence
                    accumulate_stats_fused_additive(obs, T);
                }

                if (config_.verbose)
                {
                    printf("HMM iter %d: LL = %.6f\n", iter, total_ll);
                }

                if (std::abs(total_ll - prev_ll) < config_.convergence_tol)
                {
                    result.converged = true;
                    result.iterations = iter + 1;
                    break;
                }

                prev_ll = total_ll;
                result.log_likelihood = total_ll;
                result.iterations = iter + 1;
                result.converged = false;

                m_step_from_sufficient_stats();
            }

            size_t longest = std::max_element(lengths.begin(), lengths.end()) - lengths.begin();
            compute_emissions_vml(sequences[longest], lengths[longest]);
            forward_scaled(lengths[longest]);
            backward_scaled(lengths[longest]);
            viterbi_log(sequences[longest], lengths[longest]);
            pack_results(lengths[longest], result);

            return true;
        }

        /**
         * Decode a new sequence using learned parameters
         */
        bool decode(const double *observations, int32_t T,
                    std::vector<int32_t> &states, std::vector<std::vector<double>> &posteriors)
        {
            if (T < 1)
                return false;

            allocate_buffers(T);
            compute_emissions_vml(observations, T);
            forward_scaled(T);
            backward_scaled(T);
            viterbi_log(observations, T);

            states.resize(T);
            std::memcpy(states.data(), viterbi_path_.data, T * sizeof(int32_t));

            // Compute posteriors on the fly (no gamma_ buffer)
            posteriors.resize(T);
            for (int32_t t = 0; t < T; ++t)
            {
                posteriors[t].resize(K_);
                const double inv_scale = 1.0 / scale_.data[t];
                const int32_t offset = t * K_;
                for (int32_t k = 0; k < K_; ++k)
                {
                    posteriors[t][k] = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                }
            }

            return true;
        }

        /**
         * Set parameters directly (for decode-only use)
         */
        void set_parameters(const std::vector<double> &mu,
                            const std::vector<double> &sigma,
                            const std::vector<std::vector<double>> &trans,
                            const std::vector<double> &pi)
        {
            mu_ = mu;
            sigma_ = sigma;
            pi_ = pi;

            trans_.resize(K_ * K_);
            log_trans_.resize(K_ * K_);

            for (int32_t i = 0; i < K_; ++i)
            {
                for (int32_t j = 0; j < K_; ++j)
                {
                    trans_[i * K_ + j] = trans[i][j];
                    log_trans_[i * K_ + j] = std::log(trans[i][j] + 1e-300);
                }
            }
        }

        HMMConfig &config() { return config_; }
        const HMMConfig &config() const { return config_; }

    private:
        HMMConfig config_;
        int32_t K_;

        // Model parameters (linear space for scaled algorithm)
        std::vector<double> mu_;        // State means [K]
        std::vector<double> sigma_;     // State std devs [K]
        std::vector<double> trans_;     // Transition matrix [K x K] (linear)
        std::vector<double> log_trans_; // Log transition matrix (for Viterbi)
        std::vector<double> pi_;        // Initial probs [K]

        // Working buffers
        AlignedBuffer<double> emission_; // [T x K] emission probabilities
        AlignedBuffer<double> alpha_;    // [T x K] scaled forward
        AlignedBuffer<double> beta_;     // [T x K] scaled backward
        AlignedBuffer<double> scale_;    // [T] scaling factors
        AlignedBuffer<int32_t> viterbi_path_;
        AlignedBuffer<double> viterbi_delta_;
        AlignedBuffer<int32_t> viterbi_psi_;

        // VML scratch buffer for batched exp
        AlignedBuffer<double> vml_scratch_; // [T x K] for vdExp input

        // Sufficient statistics
        std::vector<double> ss_gamma_sum_;
        std::vector<double> ss_gamma_obs_sum_;
        std::vector<double> ss_gamma_obs2_sum_;
        std::vector<double> ss_xi_sum_;
        std::vector<double> ss_init_sum_;

        void allocate_buffers(int32_t T)
        {
            emission_.resize(T * K_);
            alpha_.resize(T * K_);
            beta_.resize(T * K_);
            scale_.resize(T);
            viterbi_path_.resize(T);
            viterbi_delta_.resize(T * K_);
            viterbi_psi_.resize(T * K_);
            vml_scratch_.resize(T * K_); // For batched vdExp
        }

        /**
         * Initialize parameters using variance-based clustering
         */
        void initialize_parameters(const double *observations, int32_t T)
        {
            mu_.resize(K_);
            sigma_.resize(K_);
            trans_.resize(K_ * K_);
            log_trans_.resize(K_ * K_);
            pi_.resize(K_);

            const int32_t window = std::max(10, std::min(50, T / 20));
            std::vector<std::pair<double, int32_t>> var_idx(T);

            for (int32_t t = 0; t < T; ++t)
            {
                int32_t start = std::max(0, t - window / 2);
                int32_t end = std::min(T, t + window / 2 + 1);
                int32_t len = end - start;

                double local_mean = 0.0;
                for (int32_t i = start; i < end; ++i)
                    local_mean += observations[i];
                local_mean /= len;

                double local_var = 0.0;
                for (int32_t i = start; i < end; ++i)
                {
                    double diff = observations[i] - local_mean;
                    local_var += diff * diff;
                }
                local_var /= len;

                var_idx[t] = {local_var, t};
            }

            std::sort(var_idx.begin(), var_idx.end());

            for (int32_t k = 0; k < K_; ++k)
            {
                int32_t q_start = (k * T) / K_;
                int32_t q_end = ((k + 1) * T) / K_;

                double sum = 0.0, sum_sq = 0.0;
                int32_t count = q_end - q_start;

                for (int32_t i = q_start; i < q_end; ++i)
                {
                    int32_t t = var_idx[i].second;
                    sum += observations[t];
                    sum_sq += observations[t] * observations[t];
                }

                mu_[k] = sum / count;
                double var = sum_sq / count - mu_[k] * mu_[k];
                var = std::max(var, config_.min_variance);
                sigma_[k] = std::sqrt(var);
            }

            std::sort(sigma_.begin(), sigma_.end());

            // Transition matrix with self-loop bias
            for (int32_t i = 0; i < K_; ++i)
            {
                double self_prob = 0.98 - 0.04 * i;
                self_prob = std::max(0.8, self_prob);
                double other_prob = (1.0 - self_prob) / (K_ - 1);

                for (int32_t j = 0; j < K_; ++j)
                {
                    double p = (i == j) ? self_prob : other_prob;
                    trans_[i * K_ + j] = p;
                    log_trans_[i * K_ + j] = std::log(p);
                }
            }

            double init_p = 1.0 / K_;
            for (int32_t k = 0; k < K_; ++k)
                pi_[k] = init_p;
        }

        /**
         * Compute emission probabilities using VML - PARALLELIZED
         *
         * B[t,k] = N(obs_t | mu_k, sigma_k) = (1/sqrt(2pi)*sigma_k) * exp(-0.5*((obs_t-mu_k)/sigma_k)^2)
         *
         * OPTIMIZED: Uses MKL vdExp for batched exponentiation (~4-6x faster)
         */
        void compute_emissions_vml(const double *observations, int32_t T)
        {
            const double inv_sqrt_2pi = 0.3989422804014327; // 1/sqrt(2*pi)
            const bool use_parallel = T > config_.parallel_threshold;
            const int32_t total_elements = T * K_;

// Step 1: Compute exponents into scratch buffer
#pragma omp parallel for schedule(static) if (use_parallel)
            for (int32_t t = 0; t < T; ++t)
            {
                const double obs_t = observations[t];
                const int32_t offset = t * K_;
                for (int32_t k = 0; k < K_; ++k)
                {
                    const double z = (obs_t - mu_[k]) / sigma_[k];
                    vml_scratch_.data[offset + k] = -0.5 * z * z;
                }
            }

            // Step 2: Batched exp via MKL VML (processes 8 doubles at once with AVX-512)
            vdExp(total_elements, vml_scratch_.data, emission_.data);

// Step 3: Apply normalization constants and clamp underflow
#pragma omp parallel for schedule(static) if (use_parallel)
            for (int32_t t = 0; t < T; ++t)
            {
                const int32_t offset = t * K_;
                for (int32_t k = 0; k < K_; ++k)
                {
                    double val = emission_.data[offset + k] * inv_sqrt_2pi / sigma_[k];
                    emission_.data[offset + k] = val < 1e-300 ? 1e-300 : val;
                }
            }
        }

        /**
         * SCALED Forward Algorithm
         *
         * alpha_hat[t,k] = (1/c[t]) * B[t,k] * sum_i(alpha_hat[t-1,i] * A[i,k])
         *
         * where c[t] is chosen so that sum_k alpha_hat[t,k] = 1
         *
         * Returns: log P(O|lambda) = -sum_t log(c[t])
         */
        double forward_scaled(int32_t T)
        {
            // t = 0
            double sum = 0.0;
            for (int32_t k = 0; k < K_; ++k)
            {
                alpha_.data[k] = pi_[k] * emission_.data[k];
                sum += alpha_.data[k];
            }

            // Scale
            scale_.data[0] = 1.0 / (sum + 1e-300);
            for (int32_t k = 0; k < K_; ++k)
            {
                alpha_.data[k] *= scale_.data[0];
            }

            // t = 1 to T-1
            for (int32_t t = 1; t < T; ++t)
            {
                sum = 0.0;

                for (int32_t j = 0; j < K_; ++j)
                {
                    double acc = 0.0;

                    // sum_i alpha[t-1,i] * A[i,j]
                    for (int32_t i = 0; i < K_; ++i)
                    {
                        acc += alpha_.data[(t - 1) * K_ + i] * trans_[i * K_ + j];
                    }

                    alpha_.data[t * K_ + j] = acc * emission_.data[t * K_ + j];
                    sum += alpha_.data[t * K_ + j];
                }

                // Scale
                scale_.data[t] = 1.0 / (sum + 1e-300);
                for (int32_t k = 0; k < K_; ++k)
                {
                    alpha_.data[t * K_ + k] *= scale_.data[t];
                }
            }

            // Log-likelihood = -sum log(c[t]) - PARALLEL
            double log_lik = 0.0;
            const bool use_parallel = T > config_.parallel_threshold;
#pragma omp parallel for reduction(+ : log_lik) schedule(static) if (use_parallel)
            for (int32_t t = 0; t < T; ++t)
            {
                log_lik -= std::log(scale_.data[t]);
            }

            return log_lik;
        }

        /**
         * SCALED Backward Algorithm
         *
         * beta_hat[t,i] = c[t] * sum_j(A[i,j] * B[t+1,j] * beta_hat[t+1,j])
         *
         * Uses same scaling factors as forward pass
         */
        void backward_scaled(int32_t T)
        {
            // t = T-1: beta[T-1,k] = c[T-1] (scaled to match forward)
            for (int32_t k = 0; k < K_; ++k)
            {
                beta_.data[(T - 1) * K_ + k] = scale_.data[T - 1];
            }

            // t = T-2 down to 0
            for (int32_t t = T - 2; t >= 0; --t)
            {
                for (int32_t i = 0; i < K_; ++i)
                {
                    double acc = 0.0;

                    for (int32_t j = 0; j < K_; ++j)
                    {
                        acc += trans_[i * K_ + j] *
                               emission_.data[(t + 1) * K_ + j] *
                               beta_.data[(t + 1) * K_ + j];
                    }

                    beta_.data[t * K_ + i] = acc * scale_.data[t];
                }
            }
        }

        /**
         * FUSED: Compute posteriors AND accumulate sufficient stats in one pass
         * Eliminates massive xi buffer (~2.4MB for T=10k, K=3) write/read
         *
         * gamma[t,k] = alpha[t,k] * beta[t,k] / scale[t]
         * xi[t,i,j] = alpha[t,i] * trans[i,j] * emit[t+1,j] * beta[t+1,j]
         */
        void accumulate_stats_fused(const double *observations, int32_t T)
        {
            const bool use_parallel = T > config_.parallel_threshold;

            // Thread-local accumulators to avoid false sharing
            const int max_threads = use_parallel ? omp_get_max_threads() : 1;
            std::vector<std::vector<double>> tl_gamma_sum(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_gamma_obs(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_gamma_obs2(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_xi_sum(max_threads, std::vector<double>(K_ * K_, 0.0));

            // Initial state (t=0) - only needs gamma for ss_init_sum_
            {
                double inv_scale0 = 1.0 / scale_.data[0];
                for (int32_t k = 0; k < K_; ++k)
                {
                    double g = alpha_.data[k] * beta_.data[k] * inv_scale0;
                    ss_init_sum_[k] = g;
                }
            }

#pragma omp parallel if (use_parallel)
            {
                int tid = use_parallel ? omp_get_thread_num() : 0;
                auto &loc_gamma_sum = tl_gamma_sum[tid];
                auto &loc_gamma_obs = tl_gamma_obs[tid];
                auto &loc_gamma_obs2 = tl_gamma_obs2[tid];
                auto &loc_xi = tl_xi_sum[tid];

// Process t = 0 to T-2: both gamma and xi
#pragma omp for schedule(static) nowait
                for (int32_t t = 0; t < T - 1; ++t)
                {
                    const double inv_scale = 1.0 / scale_.data[t];
                    const double obs_t = observations[t];
                    const double obs_t2 = obs_t * obs_t;
                    const int32_t offset = t * K_;
                    const int32_t next_offset = (t + 1) * K_;

                    for (int32_t k = 0; k < K_; ++k)
                    {
                        // Gamma accumulation
                        const double g = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                        loc_gamma_sum[k] += g;
                        loc_gamma_obs[k] += g * obs_t;
                        loc_gamma_obs2[k] += g * obs_t2;

                        // Xi accumulation (fused - no buffer write!)
                        const double alpha_k = alpha_.data[offset + k];
                        for (int32_t j = 0; j < K_; ++j)
                        {
                            const double xi_val = alpha_k * trans_[k * K_ + j] *
                                                  emission_.data[next_offset + j] *
                                                  beta_.data[next_offset + j];
                            loc_xi[k * K_ + j] += xi_val;
                        }
                    }
                }

// Process t = T-1: gamma only (no xi for last timestep)
#pragma omp single
                {
                    const int32_t t = T - 1;
                    const double inv_scale = 1.0 / scale_.data[t];
                    const double obs_t = observations[t];
                    const double obs_t2 = obs_t * obs_t;
                    const int32_t offset = t * K_;

                    for (int32_t k = 0; k < K_; ++k)
                    {
                        const double g = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                        ss_gamma_sum_[k] += g;
                        ss_gamma_obs_sum_[k] += g * obs_t;
                        ss_gamma_obs2_sum_[k] += g * obs_t2;
                    }
                }
            }

            // Reduce thread-local accumulators to global
            for (int tid = 0; tid < max_threads; ++tid)
            {
                for (int32_t k = 0; k < K_; ++k)
                {
                    ss_gamma_sum_[k] += tl_gamma_sum[tid][k];
                    ss_gamma_obs_sum_[k] += tl_gamma_obs[tid][k];
                    ss_gamma_obs2_sum_[k] += tl_gamma_obs2[tid][k];
                    for (int32_t j = 0; j < K_; ++j)
                    {
                        ss_xi_sum_[k * K_ + j] += tl_xi_sum[tid][k * K_ + j];
                    }
                }
            }
        }

        /**
         * Additive version for multi-sequence training
         * Same as accumulate_stats_fused but adds to ss_init_sum_ instead of assigning
         */
        void accumulate_stats_fused_additive(const double *observations, int32_t T)
        {
            const bool use_parallel = T > config_.parallel_threshold;

            const int max_threads = use_parallel ? omp_get_max_threads() : 1;
            std::vector<std::vector<double>> tl_gamma_sum(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_gamma_obs(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_gamma_obs2(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_xi_sum(max_threads, std::vector<double>(K_ * K_, 0.0));

            // Initial state (t=0) - ADD to existing stats
            {
                double inv_scale0 = 1.0 / scale_.data[0];
                for (int32_t k = 0; k < K_; ++k)
                {
                    double g = alpha_.data[k] * beta_.data[k] * inv_scale0;
                    ss_init_sum_[k] += g; // += for multi-sequence
                }
            }

#pragma omp parallel if (use_parallel)
            {
                int tid = use_parallel ? omp_get_thread_num() : 0;
                auto &loc_gamma_sum = tl_gamma_sum[tid];
                auto &loc_gamma_obs = tl_gamma_obs[tid];
                auto &loc_gamma_obs2 = tl_gamma_obs2[tid];
                auto &loc_xi = tl_xi_sum[tid];

#pragma omp for schedule(static) nowait
                for (int32_t t = 0; t < T - 1; ++t)
                {
                    const double inv_scale = 1.0 / scale_.data[t];
                    const double obs_t = observations[t];
                    const double obs_t2 = obs_t * obs_t;
                    const int32_t offset = t * K_;
                    const int32_t next_offset = (t + 1) * K_;

                    for (int32_t k = 0; k < K_; ++k)
                    {
                        const double g = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                        loc_gamma_sum[k] += g;
                        loc_gamma_obs[k] += g * obs_t;
                        loc_gamma_obs2[k] += g * obs_t2;

                        const double alpha_k = alpha_.data[offset + k];
                        for (int32_t j = 0; j < K_; ++j)
                        {
                            const double xi_val = alpha_k * trans_[k * K_ + j] *
                                                  emission_.data[next_offset + j] *
                                                  beta_.data[next_offset + j];
                            loc_xi[k * K_ + j] += xi_val;
                        }
                    }
                }

#pragma omp single
                {
                    const int32_t t = T - 1;
                    const double inv_scale = 1.0 / scale_.data[t];
                    const double obs_t = observations[t];
                    const double obs_t2 = obs_t * obs_t;
                    const int32_t offset = t * K_;

                    for (int32_t k = 0; k < K_; ++k)
                    {
                        const double g = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                        ss_gamma_sum_[k] += g;
                        ss_gamma_obs_sum_[k] += g * obs_t;
                        ss_gamma_obs2_sum_[k] += g * obs_t2;
                    }
                }
            }

            for (int tid = 0; tid < max_threads; ++tid)
            {
                for (int32_t k = 0; k < K_; ++k)
                {
                    ss_gamma_sum_[k] += tl_gamma_sum[tid][k];
                    ss_gamma_obs_sum_[k] += tl_gamma_obs[tid][k];
                    ss_gamma_obs2_sum_[k] += tl_gamma_obs2[tid][k];
                    for (int32_t j = 0; j < K_; ++j)
                    {
                        ss_xi_sum_[k * K_ + j] += tl_xi_sum[tid][k * K_ + j];
                    }
                }
            }
        }

        void reset_sufficient_stats()
        {
            ss_gamma_sum_.assign(K_, 0.0);
            ss_gamma_obs_sum_.assign(K_, 0.0);
            ss_gamma_obs2_sum_.assign(K_, 0.0);
            ss_xi_sum_.assign(K_ * K_, 0.0);
            ss_init_sum_.assign(K_, 0.0);
        }

        void m_step_from_sufficient_stats()
        {
            double init_sum = 0.0;
            for (int32_t k = 0; k < K_; ++k)
                init_sum += ss_init_sum_[k];
            for (int32_t k = 0; k < K_; ++k)
                pi_[k] = ss_init_sum_[k] / (init_sum + 1e-300);

            for (int32_t i = 0; i < K_; ++i)
            {
                double row_sum = 0.0;
                for (int32_t j = 0; j < K_; ++j)
                    row_sum += ss_xi_sum_[i * K_ + j];
                for (int32_t j = 0; j < K_; ++j)
                {
                    trans_[i * K_ + j] = ss_xi_sum_[i * K_ + j] / (row_sum + 1e-300);
                    log_trans_[i * K_ + j] = std::log(trans_[i * K_ + j] + 1e-300);
                }
            }

            for (int32_t k = 0; k < K_; ++k)
            {
                mu_[k] = ss_gamma_obs_sum_[k] / (ss_gamma_sum_[k] + 1e-300);
                double var = ss_gamma_obs2_sum_[k] / (ss_gamma_sum_[k] + 1e-300) - mu_[k] * mu_[k];
                var = std::max(var, config_.min_variance);
                sigma_[k] = std::sqrt(var);
            }
        }

        /**
         * Viterbi algorithm (log-space for numerical stability in argmax)
         */
        void viterbi_log(const double *observations, int32_t T)
        {
            // Precompute log emissions
            const double log_2pi = std::log(2.0 * M_PI);

            // t = 0
            for (int32_t k = 0; k < K_; ++k)
            {
                double log_emit = -0.5 * log_2pi - std::log(sigma_[k]) - 0.5 * std::pow((observations[0] - mu_[k]) / sigma_[k], 2);
                viterbi_delta_.data[k] = std::log(pi_[k] + 1e-300) + log_emit;
                viterbi_psi_.data[k] = 0;
            }

            // t = 1 to T-1
            for (int32_t t = 1; t < T; ++t)
            {
                for (int32_t j = 0; j < K_; ++j)
                {
                    double log_emit = -0.5 * log_2pi - std::log(sigma_[j]) - 0.5 * std::pow((observations[t] - mu_[j]) / sigma_[j], 2);

                    double best_val = -std::numeric_limits<double>::infinity();
                    int32_t best_i = 0;

                    for (int32_t i = 0; i < K_; ++i)
                    {
                        double val = viterbi_delta_.data[(t - 1) * K_ + i] + log_trans_[i * K_ + j];
                        if (val > best_val)
                        {
                            best_val = val;
                            best_i = i;
                        }
                    }

                    viterbi_delta_.data[t * K_ + j] = best_val + log_emit;
                    viterbi_psi_.data[t * K_ + j] = best_i;
                }
            }

            // Backtrack
            int32_t best_final = 0;
            double best_final_val = viterbi_delta_.data[(T - 1) * K_];
            for (int32_t k = 1; k < K_; ++k)
            {
                if (viterbi_delta_.data[(T - 1) * K_ + k] > best_final_val)
                {
                    best_final_val = viterbi_delta_.data[(T - 1) * K_ + k];
                    best_final = k;
                }
            }

            viterbi_path_.data[T - 1] = best_final;
            for (int32_t t = T - 2; t >= 0; --t)
            {
                viterbi_path_.data[t] = viterbi_psi_.data[(t + 1) * K_ + viterbi_path_.data[t + 1]];
            }
        }

        void pack_results(int32_t T, HMMResult &result)
        {
            result.mu = mu_;
            result.sigma = sigma_;

            result.trans.resize(K_);
            for (int32_t i = 0; i < K_; ++i)
            {
                result.trans[i].resize(K_);
                for (int32_t j = 0; j < K_; ++j)
                {
                    result.trans[i][j] = trans_[i * K_ + j];
                }
            }

            result.pi = pi_;

            result.viterbi_path.resize(T);
            std::memcpy(result.viterbi_path.data(), viterbi_path_.data, T * sizeof(int32_t));

            // Recompute posteriors from alpha/beta/scale (gamma was not stored)
            result.posteriors.resize(T);
            for (int32_t t = 0; t < T; ++t)
            {
                result.posteriors[t].resize(K_);
                const double inv_scale = 1.0 / scale_.data[t];
                const int32_t offset = t * K_;
                for (int32_t k = 0; k < K_; ++k)
                {
                    result.posteriors[t][k] = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                }
            }
        }
    };

    // ============================================================================
    // Utility: Sort HMM states by volatility
    // ============================================================================

    inline void sort_hmm_states_by_volatility(HMMResult &result)
    {
        int K = static_cast<int>(result.sigma.size());
        if (K < 2)
            return;

        std::vector<int> order(K);
        for (int i = 0; i < K; ++i)
            order[i] = i;

        std::sort(order.begin(), order.end(), [&](int a, int b)
                  { return result.sigma[a] < result.sigma[b]; });

        bool already_sorted = true;
        for (int i = 0; i < K; ++i)
        {
            if (order[i] != i)
            {
                already_sorted = false;
                break;
            }
        }
        if (already_sorted)
            return;

        std::vector<double> new_mu(K), new_sigma(K), new_pi(K);
        for (int i = 0; i < K; ++i)
        {
            new_mu[i] = result.mu[order[i]];
            new_sigma[i] = result.sigma[order[i]];
            new_pi[i] = result.pi[order[i]];
        }
        result.mu = std::move(new_mu);
        result.sigma = std::move(new_sigma);
        result.pi = std::move(new_pi);

        std::vector<std::vector<double>> new_trans(K, std::vector<double>(K));
        for (int i = 0; i < K; ++i)
        {
            for (int j = 0; j < K; ++j)
            {
                new_trans[i][j] = result.trans[order[i]][order[j]];
            }
        }
        result.trans = std::move(new_trans);

        std::vector<int> reverse_order(K);
        for (int i = 0; i < K; ++i)
        {
            reverse_order[order[i]] = i;
        }
        for (auto &state : result.viterbi_path)
        {
            state = reverse_order[state];
        }

        for (auto &post : result.posteriors)
        {
            std::vector<double> new_post(K);
            for (int i = 0; i < K; ++i)
            {
                new_post[i] = post[order[i]];
            }
            post = std::move(new_post);
        }
    }

    // ============================================================================
    // Utility: AnchorFader
    // ============================================================================

    struct AnchorFader
    {
        std::vector<double> old_phi, new_phi;
        std::vector<double> old_sigma, new_sigma;
        std::vector<std::vector<double>> old_trans, new_trans;

        int fade_ticks = 0;
        int current_tick = 0;
        bool is_active = false;

        void start(const IMMParams &old_params, const IMMParams &new_params, int ticks)
        {
            old_phi = old_params.phi;
            new_phi = new_params.phi;
            old_sigma = old_params.sigma;
            new_sigma = new_params.sigma;
            old_trans = old_params.trans;
            new_trans = new_params.trans;

            fade_ticks = ticks;
            current_tick = 0;
            is_active = true;
        }

        bool active() const { return is_active; }

        IMMParams step()
        {
            if (!is_active)
            {
                IMMParams p;
                p.phi = new_phi;
                p.sigma = new_sigma;
                p.trans = new_trans;
                return p;
            }

            current_tick++;
            double alpha = static_cast<double>(current_tick) / fade_ticks;
            alpha = std::min(1.0, alpha);

            if (current_tick >= fade_ticks)
            {
                is_active = false;
            }

            IMMParams blended;
            int K = static_cast<int>(old_phi.size());
            blended.phi.resize(K);
            blended.sigma.resize(K);
            blended.trans.resize(K, std::vector<double>(K));

            for (int k = 0; k < K; ++k)
            {
                blended.phi[k] = (1.0 - alpha) * old_phi[k] + alpha * new_phi[k];
                blended.sigma[k] = (1.0 - alpha) * old_sigma[k] + alpha * new_sigma[k];

                for (int j = 0; j < K; ++j)
                {
                    blended.trans[k][j] = (1.0 - alpha) * old_trans[k][j] + alpha * new_trans[k][j];
                }
            }

            return blended;
        }
    };

    // ============================================================================
    // Utility: Print functions
    // ============================================================================

    inline void print_hmm_result(const HMMResult &result, const char *state_names[] = nullptr)
    {
        const char *default_names[] = {"State 0", "State 1", "State 2", "State 3", "State 4"};
        if (!state_names)
            state_names = default_names;

        int K = static_cast<int>(result.mu.size());

        printf("\n=== HMM Calibration Results ===\n");
        printf("Converged: %s (iterations: %d)\n",
               result.converged ? "yes" : "no", result.iterations);
        printf("Log-likelihood: %.4f\n\n", result.log_likelihood);

        printf("Emission Parameters:\n");
        printf("%-12s %12s %12s\n", "State", "Mean (mu)", "Std (sigma)");
        printf("%-12s %12s %12s\n", "-----", "---------", "----------");
        for (int k = 0; k < K; ++k)
        {
            printf("%-12s %12.6f %12.6f\n", state_names[k], result.mu[k], result.sigma[k]);
        }

        printf("\nTransition Matrix P(row -> col):\n");
        printf("%-12s", "From/To");
        for (int j = 0; j < K; ++j)
        {
            printf(" %10s", state_names[j]);
        }
        printf("\n");

        for (int i = 0; i < K; ++i)
        {
            printf("%-12s", state_names[i]);
            for (int j = 0; j < K; ++j)
            {
                printf(" %10.4f", result.trans[i][j]);
            }
            printf("\n");
        }

        printf("\nInitial Distribution:\n");
        for (int k = 0; k < K; ++k)
        {
            printf("  %s: %.4f\n", state_names[k], result.pi[k]);
        }

        printf("\nRegime Statistics (from Viterbi path):\n");
        std::vector<std::vector<int>> durations(K);
        int current_state = result.viterbi_path[0];
        int current_duration = 1;

        for (size_t t = 1; t < result.viterbi_path.size(); ++t)
        {
            if (result.viterbi_path[t] == current_state)
            {
                ++current_duration;
            }
            else
            {
                durations[current_state].push_back(current_duration);
                current_state = result.viterbi_path[t];
                current_duration = 1;
            }
        }
        durations[current_state].push_back(current_duration);

        printf("%-12s %8s %8s %8s %8s\n", "State", "Count", "Mean", "Min", "Max");
        for (int k = 0; k < K; ++k)
        {
            if (durations[k].empty())
            {
                printf("%-12s %8d %8s %8s %8s\n", state_names[k], 0, "-", "-", "-");
            }
            else
            {
                double mean = std::accumulate(durations[k].begin(), durations[k].end(), 0.0) /
                              durations[k].size();
                int min_d = *std::min_element(durations[k].begin(), durations[k].end());
                int max_d = *std::max_element(durations[k].begin(), durations[k].end());
                printf("%-12s %8zu %8.1f %8d %8d\n",
                       state_names[k], durations[k].size(), mean, min_d, max_d);
            }
        }
        printf("\n");
    }

    inline IMMParams hmm_to_imm_params(const HMMResult &hmm_result,
                                       const double *observations,
                                       int32_t T)
    {
        IMMParams params;
        int K = static_cast<int>(hmm_result.mu.size());

        params.sigma = hmm_result.sigma;
        params.trans = hmm_result.trans;
        params.phi.resize(K);

        for (int k = 0; k < K; ++k)
        {
            double sum_xy = 0.0;
            double sum_x2 = 0.0;

            for (int t = 1; t < T; ++t)
            {
                double w = hmm_result.posteriors[t][k] * hmm_result.posteriors[t - 1][k];
                double x = observations[t - 1] - hmm_result.mu[k];
                double y = observations[t] - hmm_result.mu[k];

                sum_xy += w * x * y;
                sum_x2 += w * x * x;
            }

            if (sum_x2 > 1e-10)
            {
                params.phi[k] = sum_xy / sum_x2;
                params.phi[k] = std::max(-0.999, std::min(0.999, params.phi[k]));
            }
            else
            {
                params.phi[k] = 0.95;
            }
        }

        return params;
    }

    inline void print_imm_params(const IMMParams &params, const char *state_names[] = nullptr)
    {
        const char *default_names[] = {"Calm", "Trend", "Crisis"};
        if (!state_names)
            state_names = default_names;

        int K = static_cast<int>(params.phi.size());

        printf("\n=== IMM Parameters (for online filter) ===\n\n");
        printf("// Paste into your IMM initialization:\n\n");

        printf("// State dynamics: x_t = phi * x_{t-1} + sigma * eps_t\n");
        for (int k = 0; k < K; ++k)
        {
            printf("// %s: phi=%.4f, sigma=%.4f\n", state_names[k], params.phi[k], params.sigma[k]);
        }

        printf("\ndouble phi[%d] = {", K);
        for (int k = 0; k < K; ++k)
        {
            printf("%.4f%s", params.phi[k], k < K - 1 ? ", " : "");
        }
        printf("};\n");

        printf("double sigma[%d] = {", K);
        for (int k = 0; k < K; ++k)
        {
            printf("%.4f%s", params.sigma[k], k < K - 1 ? ", " : "");
        }
        printf("};\n");

        printf("\ndouble trans[%d][%d] = {\n", K, K);
        for (int i = 0; i < K; ++i)
        {
            printf("    {");
            for (int j = 0; j < K; ++j)
            {
                printf("%.4f%s", params.trans[i][j], j < K - 1 ? ", " : "");
            }
            printf("}%s\n", i < K - 1 ? "," : "");
        }
        printf("};\n");
    }

} // namespace eemd

#endif // HMM_MKL_HPP