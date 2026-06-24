/**
 * Student-t AR(1) Hidden Markov Model with Intel MKL
 *
 * Key differences from Gaussian HMM:
 * 1. Student-t emissions: Fat tails, robust to outliers
 * 2. AR(1) dynamics: x_t = φ_k * x_{t-1} + ε_t, ε_t ~ t(0, σ_k, ν_k)
 * 3. Per-regime tail thickness: ν_k (degrees of freedom)
 *
 * Outputs for downstream MMPF/RBPF:
 * - φ_k:  AR(1) coefficients (persistence)
 * - σ_k:  Scale parameters (volatility levels)
 * - ν_k:  Degrees of freedom (tail thickness)
 * - P:    Transition matrix
 *
 * NOTE: μ_k is NOT used for prediction. See hmm_role_and_limitations.md
 *
 * License: MIT
 */

#ifndef HMM_STUDENT_T_HPP
#define HMM_STUDENT_T_HPP

#include "eemd_mkl.hpp"

#include <cmath>
#include <limits>
#include <numeric>
#include <algorithm>
#include <omp.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace eemd
{
    // ============================================================================
    // Configuration
    // ============================================================================

    struct StudentTHMMConfig
    {
        int32_t n_states = 3;
        int32_t max_iter = 100;
        double convergence_tol = 1e-6;
        double min_variance = 1e-6;
        double min_nu = 2.1;            // ν > 2 for finite variance
        double max_nu = 100.0;          // Large ν → Gaussian
        bool estimate_nu = true;        // If false, use fixed initial ν
        uint32_t random_seed = 42;
        bool verbose = false;
        int32_t n_threads = 0;
        int32_t parallel_threshold = 5000;
    };

    // ============================================================================
    // Result Structure
    // ============================================================================

    struct StudentTHMMResult
    {
        std::vector<double> phi;                    // AR(1) coefficients [K]
        std::vector<double> sigma;                  // Scale parameters [K]
        std::vector<double> nu;                     // Degrees of freedom [K]
        std::vector<std::vector<double>> trans;     // Transition matrix [K x K]
        std::vector<double> pi;                     // Initial distribution [K]

        std::vector<int32_t> viterbi_path;
        std::vector<std::vector<double>> posteriors;

        double log_likelihood;
        int32_t iterations;
        bool converged;
    };

    // ============================================================================
    // IMM Parameters (for online filter)
    // ============================================================================

    struct StudentTIMMParams
    {
        std::vector<double> phi;
        std::vector<double> sigma;
        std::vector<double> nu;
        std::vector<std::vector<double>> trans;
    };

    // ============================================================================
    // Student-t AR(1) HMM Class
    // ============================================================================

    class StudentTHMM
    {
    public:
        explicit StudentTHMM(const StudentTHMMConfig &config = StudentTHMMConfig())
            : config_(config), K_(config.n_states)
        {
            if (config_.n_threads > 0)
            {
                omp_set_num_threads(config_.n_threads);
            }
        }

        /**
         * Fit Student-t AR-HMM to observations using EM algorithm
         */
        bool fit(const double *observations, int32_t T, StudentTHMMResult &result)
        {
            if (T < K_ * 2)
                return false;

            allocate_buffers(T);
            initialize_parameters(observations, T);

            double prev_ll = -std::numeric_limits<double>::infinity();

            for (int32_t iter = 0; iter < config_.max_iter; ++iter)
            {
                // E-step
                compute_emissions_student_t(observations, T);
                double ll = forward_scaled(T);
                backward_scaled(T);

                // Accumulate sufficient statistics
                reset_sufficient_stats();
                accumulate_stats_student_t(observations, T);

                if (config_.verbose)
                {
                    printf("Student-t HMM iter %d: LL = %.6f\n", iter, ll);
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

                // M-step
                m_step_student_t(observations, T);
            }

            viterbi_log(observations, T);
            pack_results(T, result);

            return true;
        }

        /**
         * Extract parameters for IMM/MMPF
         */
        StudentTIMMParams extract_imm_params() const
        {
            StudentTIMMParams params;
            params.phi = phi_;
            params.sigma = sigma_;
            params.nu = nu_;

            params.trans.resize(K_);
            for (int32_t i = 0; i < K_; ++i)
            {
                params.trans[i].resize(K_);
                for (int32_t j = 0; j < K_; ++j)
                {
                    params.trans[i][j] = trans_[i * K_ + j];
                }
            }

            return params;
        }

        StudentTHMMConfig &config() { return config_; }
        const StudentTHMMConfig &config() const { return config_; }

    private:
        StudentTHMMConfig config_;
        int32_t K_;

        // Model parameters
        std::vector<double> phi_;       // AR(1) coefficients [K]
        std::vector<double> sigma_;     // Scale parameters [K]
        std::vector<double> nu_;        // Degrees of freedom [K]
        std::vector<double> trans_;     // Transition matrix [K x K]
        std::vector<double> log_trans_;
        std::vector<double> pi_;        // Initial distribution [K]

        // Working buffers
        AlignedBuffer<double> emission_;
        AlignedBuffer<double> alpha_;
        AlignedBuffer<double> beta_;
        AlignedBuffer<double> scale_;
        AlignedBuffer<double> weights_;     // Student-t EM weights [T x K]

        // Viterbi
        AlignedBuffer<int32_t> viterbi_path_;
        AlignedBuffer<double> viterbi_delta_;
        AlignedBuffer<int32_t> viterbi_psi_;

        // Sufficient statistics
        std::vector<double> ss_gamma_sum_;
        std::vector<double> ss_phi_num_;        // Σ w_t * γ_t * x_t * x_{t-1}
        std::vector<double> ss_phi_denom_;      // Σ w_t * γ_t * x_{t-1}²
        std::vector<double> ss_sigma_sum_;      // Σ w_t * γ_t * (x_t - φ*x_{t-1})²
        std::vector<double> ss_weight_sum_;     // Σ w_t * γ_t (for ν update)
        std::vector<double> ss_log_weight_sum_; // Σ γ_t * log(w_t) (for ν update)
        std::vector<double> ss_xi_sum_;
        std::vector<double> ss_init_sum_;

        // ============================================================================
        // Student-t PDF and helpers
        // ============================================================================

        /**
         * Log of Student-t PDF
         * log t(x; μ, σ, ν) = log Γ((ν+1)/2) - log Γ(ν/2) - 0.5*log(νπ) - log(σ)
         *                    - (ν+1)/2 * log(1 + (x-μ)²/(νσ²))
         */
        static double log_student_t_pdf(double x, double mu, double sigma, double nu)
        {
            const double z = (x - mu) / sigma;
            const double z2 = z * z;

            // Use lgamma for numerical stability
            const double log_norm = std::lgamma((nu + 1.0) / 2.0) 
                                  - std::lgamma(nu / 2.0)
                                  - 0.5 * std::log(nu * M_PI)
                                  - std::log(sigma);

            const double log_kernel = -((nu + 1.0) / 2.0) * std::log(1.0 + z2 / nu);

            return log_norm + log_kernel;
        }

        /**
         * Student-t PDF (exp of log for numerical stability)
         */
        static double student_t_pdf(double x, double mu, double sigma, double nu)
        {
            return std::exp(log_student_t_pdf(x, mu, sigma, nu));
        }

        /**
         * Student-t EM weight for observation x
         * w = (ν + 1) / (ν + z²)
         * This down-weights outliers in the M-step
         */
        static double student_t_weight(double x, double mu, double sigma, double nu)
        {
            const double z = (x - mu) / sigma;
            const double z2 = z * z;
            return (nu + 1.0) / (nu + z2);
        }

        // ============================================================================
        // Buffer allocation
        // ============================================================================

        void allocate_buffers(int32_t T)
        {
            emission_.resize(T * K_);
            alpha_.resize(T * K_);
            beta_.resize(T * K_);
            scale_.resize(T);
            weights_.resize(T * K_);

            viterbi_path_.resize(T);
            viterbi_delta_.resize(T * K_);
            viterbi_psi_.resize(T * K_);

            ss_gamma_sum_.resize(K_);
            ss_phi_num_.resize(K_);
            ss_phi_denom_.resize(K_);
            ss_sigma_sum_.resize(K_);
            ss_weight_sum_.resize(K_);
            ss_log_weight_sum_.resize(K_);
            ss_xi_sum_.resize(K_ * K_);
            ss_init_sum_.resize(K_);
        }

        // ============================================================================
        // Parameter initialization
        // ============================================================================

        void initialize_parameters(const double *observations, int32_t T)
        {
            phi_.resize(K_);
            sigma_.resize(K_);
            nu_.resize(K_);
            trans_.resize(K_ * K_);
            log_trans_.resize(K_ * K_);
            pi_.resize(K_);

            // Compute local volatility for sorting
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

            // Initialize parameters by variance quantiles
            for (int32_t k = 0; k < K_; ++k)
            {
                int32_t start = (k * T) / K_;
                int32_t end = ((k + 1) * T) / K_;
                int32_t count = 0;

                double sum_obs = 0.0;
                double sum_obs2 = 0.0;
                double sum_cross = 0.0;
                double sum_lag2 = 0.0;

                for (int32_t i = start; i < end; ++i)
                {
                    int32_t t = var_idx[i].second;
                    if (t > 0)
                    {
                        sum_obs += observations[t];
                        sum_obs2 += observations[t] * observations[t];
                        sum_cross += observations[t] * observations[t - 1];
                        sum_lag2 += observations[t - 1] * observations[t - 1];
                        count++;
                    }
                }

                // Estimate φ via simple regression
                if (count > 0 && sum_lag2 > 1e-10)
                {
                    phi_[k] = sum_cross / sum_lag2;
                    phi_[k] = std::max(-0.999, std::min(0.999, phi_[k]));
                }
                else
                {
                    phi_[k] = 0.0;
                }

                // Estimate σ from residual variance
                double residual_var = 0.0;
                int32_t res_count = 0;
                for (int32_t i = start; i < end; ++i)
                {
                    int32_t t = var_idx[i].second;
                    if (t > 0)
                    {
                        double resid = observations[t] - phi_[k] * observations[t - 1];
                        residual_var += resid * resid;
                        res_count++;
                    }
                }
                residual_var = res_count > 0 ? residual_var / res_count : 0.01;
                sigma_[k] = std::sqrt(std::max(residual_var, config_.min_variance));

                // Initialize ν (more fat-tailed for higher volatility regimes)
                // Low vol (calm): higher ν (more Gaussian)
                // High vol (crisis): lower ν (fatter tails)
                nu_[k] = 5.0 + (K_ - 1 - k) * 5.0;  // e.g., K=3: [15, 10, 5]
                nu_[k] = std::max(config_.min_nu, std::min(config_.max_nu, nu_[k]));
            }

            // Initialize transition matrix (sticky)
            for (int32_t i = 0; i < K_; ++i)
            {
                double self_prob = 0.9;
                double other_prob = (1.0 - self_prob) / (K_ - 1);

                for (int32_t j = 0; j < K_; ++j)
                {
                    double p = (i == j) ? self_prob : other_prob;
                    trans_[i * K_ + j] = p;
                    log_trans_[i * K_ + j] = std::log(p);
                }
            }

            // Uniform initial distribution
            double init_p = 1.0 / K_;
            for (int32_t k = 0; k < K_; ++k)
                pi_[k] = init_p;
        }

        // ============================================================================
        // Emission computation (Student-t AR(1))
        // ============================================================================

        void compute_emissions_student_t(const double *observations, int32_t T)
        {
            const bool use_parallel = T > config_.parallel_threshold;

            // t = 0: No AR term, use unconditional (or μ = 0)
            for (int32_t k = 0; k < K_; ++k)
            {
                // First observation: use stationary variance σ² / (1 - φ²)
                double stationary_sigma = sigma_[k] / std::sqrt(1.0 - phi_[k] * phi_[k] + 1e-10);
                emission_.data[k] = student_t_pdf(observations[0], 0.0, stationary_sigma, nu_[k]);
                emission_.data[k] = std::max(emission_.data[k], 1e-300);
            }

            // t = 1 to T-1: AR(1) structure
            #pragma omp parallel for schedule(static) if(use_parallel)
            for (int32_t t = 1; t < T; ++t)
            {
                const double x_t = observations[t];
                const double x_prev = observations[t - 1];
                const int32_t offset = t * K_;

                for (int32_t k = 0; k < K_; ++k)
                {
                    // Conditional mean: φ_k * x_{t-1}
                    const double mu_cond = phi_[k] * x_prev;
                    double emit = student_t_pdf(x_t, mu_cond, sigma_[k], nu_[k]);
                    emission_.data[offset + k] = std::max(emit, 1e-300);
                }
            }
        }

        // ============================================================================
        // Forward-Backward (same as Gaussian, emissions already computed)
        // ============================================================================

        double forward_scaled(int32_t T)
        {
            // t = 0
            double sum = 0.0;
            for (int32_t k = 0; k < K_; ++k)
            {
                alpha_.data[k] = pi_[k] * emission_.data[k];
                sum += alpha_.data[k];
            }

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
                    for (int32_t i = 0; i < K_; ++i)
                    {
                        acc += alpha_.data[(t - 1) * K_ + i] * trans_[i * K_ + j];
                    }
                    alpha_.data[t * K_ + j] = acc * emission_.data[t * K_ + j];
                    sum += alpha_.data[t * K_ + j];
                }

                scale_.data[t] = 1.0 / (sum + 1e-300);
                for (int32_t k = 0; k < K_; ++k)
                {
                    alpha_.data[t * K_ + k] *= scale_.data[t];
                }
            }

            // Log-likelihood
            double log_lik = 0.0;
            const bool use_parallel = T > config_.parallel_threshold;
            #pragma omp parallel for reduction(+:log_lik) schedule(static) if(use_parallel)
            for (int32_t t = 0; t < T; ++t)
            {
                log_lik -= std::log(scale_.data[t]);
            }

            return log_lik;
        }

        void backward_scaled(int32_t T)
        {
            // t = T-1
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

        // ============================================================================
        // Sufficient statistics accumulation
        // ============================================================================

        void reset_sufficient_stats()
        {
            std::fill(ss_gamma_sum_.begin(), ss_gamma_sum_.end(), 0.0);
            std::fill(ss_phi_num_.begin(), ss_phi_num_.end(), 0.0);
            std::fill(ss_phi_denom_.begin(), ss_phi_denom_.end(), 0.0);
            std::fill(ss_sigma_sum_.begin(), ss_sigma_sum_.end(), 0.0);
            std::fill(ss_weight_sum_.begin(), ss_weight_sum_.end(), 0.0);
            std::fill(ss_log_weight_sum_.begin(), ss_log_weight_sum_.end(), 0.0);
            std::fill(ss_xi_sum_.begin(), ss_xi_sum_.end(), 0.0);
            std::fill(ss_init_sum_.begin(), ss_init_sum_.end(), 0.0);
        }

        void accumulate_stats_student_t(const double *observations, int32_t T)
        {
            const bool use_parallel = T > config_.parallel_threshold;

            // Compute Student-t weights and z² for all (t, k)
            // Also need z² for correct E[log u] computation
            #pragma omp parallel for schedule(static) if(use_parallel)
            for (int32_t t = 1; t < T; ++t)
            {
                const double x_t = observations[t];
                const double x_prev = observations[t - 1];
                const int32_t offset = t * K_;

                for (int32_t k = 0; k < K_; ++k)
                {
                    const double mu_cond = phi_[k] * x_prev;
                    const double z = (x_t - mu_cond) / sigma_[k];
                    const double z2 = z * z;
                    // w = E[u | x] = (ν+1)/(ν+z²)
                    weights_.data[offset + k] = (nu_[k] + 1.0) / (nu_[k] + z2);
                }
            }

            // Initial state
            {
                double inv_scale0 = 1.0 / scale_.data[0];
                for (int32_t k = 0; k < K_; ++k)
                {
                    double g = alpha_.data[k] * beta_.data[k] * inv_scale0;
                    ss_init_sum_[k] = g;
                }
            }

            // Thread-local accumulators
            const int max_threads = use_parallel ? omp_get_max_threads() : 1;
            std::vector<std::vector<double>> tl_gamma(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_phi_num(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_phi_denom(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_sigma(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_weight(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_log_weight(max_threads, std::vector<double>(K_, 0.0));
            std::vector<std::vector<double>> tl_xi(max_threads, std::vector<double>(K_ * K_, 0.0));

            #pragma omp parallel if(use_parallel)
            {
                int tid = use_parallel ? omp_get_thread_num() : 0;

                #pragma omp for schedule(static) nowait
                for (int32_t t = 1; t < T; ++t)
                {
                    const double inv_scale = 1.0 / scale_.data[t];
                    const double x_t = observations[t];
                    const double x_prev = observations[t - 1];
                    const int32_t offset = t * K_;

                    for (int32_t k = 0; k < K_; ++k)
                    {
                        const double g = alpha_.data[offset + k] * beta_.data[offset + k] * inv_scale;
                        const double w = weights_.data[offset + k];
                        const double mu_cond = phi_[k] * x_prev;
                        const double resid = x_t - mu_cond;
                        const double z2 = (resid / sigma_[k]) * (resid / sigma_[k]);

                        tl_gamma[tid][k] += g;
                        tl_phi_num[tid][k] += w * g * x_t * x_prev;
                        tl_phi_denom[tid][k] += w * g * x_prev * x_prev;
                        tl_sigma[tid][k] += w * g * resid * resid;
                        tl_weight[tid][k] += w * g;
                        
                        // CORRECT E[log u | x] = ψ((ν+1)/2) - log((ν+z²)/2)
                        // NOT log(w)!
                        double E_log_u = digamma((nu_[k] + 1.0) / 2.0) 
                                       - std::log((nu_[k] + z2) / 2.0);
                        tl_log_weight[tid][k] += g * E_log_u;

                        // Xi accumulation (t-1 → t)
                        if (t < T - 1)
                        {
                            const double alpha_k = alpha_.data[(t - 1) * K_ + k];
                            for (int32_t j = 0; j < K_; ++j)
                            {
                                double xi_val = alpha_k * trans_[k * K_ + j] *
                                               emission_.data[t * K_ + j] *
                                               beta_.data[t * K_ + j];
                                tl_xi[tid][k * K_ + j] += xi_val;
                            }
                        }
                    }
                }
            }

            // Reduce
            for (int tid = 0; tid < max_threads; ++tid)
            {
                for (int32_t k = 0; k < K_; ++k)
                {
                    ss_gamma_sum_[k] += tl_gamma[tid][k];
                    ss_phi_num_[k] += tl_phi_num[tid][k];
                    ss_phi_denom_[k] += tl_phi_denom[tid][k];
                    ss_sigma_sum_[k] += tl_sigma[tid][k];
                    ss_weight_sum_[k] += tl_weight[tid][k];
                    ss_log_weight_sum_[k] += tl_log_weight[tid][k];
                    for (int32_t j = 0; j < K_; ++j)
                    {
                        ss_xi_sum_[k * K_ + j] += tl_xi[tid][k * K_ + j];
                    }
                }
            }
        }

        // ============================================================================
        // M-step
        // ============================================================================

        void m_step_student_t(const double *observations, int32_t T)
        {
            // 1. Update π
            double sum_init = 0.0;
            for (int32_t k = 0; k < K_; ++k)
                sum_init += ss_init_sum_[k];
            for (int32_t k = 0; k < K_; ++k)
                pi_[k] = ss_init_sum_[k] / (sum_init + 1e-300);

            // 2. Update transition matrix
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

            // 3. Update φ (weighted least squares)
            for (int32_t k = 0; k < K_; ++k)
            {
                if (ss_phi_denom_[k] > 1e-10)
                {
                    phi_[k] = ss_phi_num_[k] / ss_phi_denom_[k];
                    phi_[k] = std::max(-0.999, std::min(0.999, phi_[k]));
                }
            }

            // 4. Update σ
            for (int32_t k = 0; k < K_; ++k)
            {
                double var = ss_sigma_sum_[k] / (ss_weight_sum_[k] + 1e-300);
                var = std::max(var, config_.min_variance);
                sigma_[k] = std::sqrt(var);
            }

            // 5. Update ν (if enabled)
            if (config_.estimate_nu)
            {
                update_nu();
            }
        }

        /**
         * Update ν using fixed-point iteration
         * 
         * For Student-t EM with latent scale u ~ Gamma(ν/2, ν/2):
         * The M-step for ν solves:
         *   log(ν/2) - ψ(ν/2) + 1 + E[log u] - E[u] = 0
         * 
         * where E[u] and E[log u] are averaged over the posterior.
         */
        void update_nu()
        {
            for (int32_t k = 0; k < K_; ++k)
            {
                if (ss_gamma_sum_[k] < 1e-10)
                    continue;

                // Compute normalized expectations
                double E_log_u = ss_log_weight_sum_[k] / ss_gamma_sum_[k];
                double E_u = ss_weight_sum_[k] / ss_gamma_sum_[k];

                // The constant term: 1 + E[log u] - E[u]
                double C = 1.0 + E_log_u - E_u;

                if (config_.verbose)
                {
                    printf("  ν update k=%d: E[log u]=%.4f, E[u]=%.4f, C=%.4f, current ν=%.2f\n",
                           k, E_log_u, E_u, C, nu_[k]);
                }

                // Fixed-point iteration to solve: log(ν/2) - ψ(ν/2) + C = 0
                // Rearranged: log(ν/2) - ψ(ν/2) = -C
                double nu_new = nu_[k];
                
                for (int iter = 0; iter < 20; ++iter)
                {
                    double half_nu = nu_new / 2.0;
                    double psi_val = digamma(half_nu);
                    double log_val = std::log(half_nu);
                    
                    // f(ν) = log(ν/2) - ψ(ν/2) + C
                    double f = log_val - psi_val + C;
                    
                    // f'(ν) = 1/ν - 0.5*ψ'(ν/2) = 1/ν - 0.5*trigamma(ν/2)
                    double f_prime = 1.0 / nu_new - 0.5 * trigamma(half_nu);
                    
                    if (std::abs(f_prime) < 1e-12)
                        break;
                    
                    double delta = f / f_prime;
                    nu_new = nu_new - delta;
                    
                    // Clamp to valid range
                    nu_new = std::max(config_.min_nu, std::min(config_.max_nu, nu_new));
                    
                    // Check convergence
                    if (std::abs(delta) < 0.01)
                        break;
                }

                if (config_.verbose)
                {
                    printf("  ν update k=%d: new ν=%.2f\n", k, nu_new);
                }

                nu_[k] = nu_new;
            }
        }

        // Digamma function (derivative of log gamma)
        static double digamma(double x)
        {
            // Asymptotic expansion for large x
            if (x > 6.0)
            {
                double inv_x = 1.0 / x;
                double inv_x2 = inv_x * inv_x;
                return std::log(x) - 0.5 * inv_x 
                       - inv_x2 * (1.0/12.0 - inv_x2 * (1.0/120.0 - inv_x2 / 252.0));
            }

            // Recurrence relation to shift to large x
            double result = 0.0;
            while (x < 6.0)
            {
                result -= 1.0 / x;
                x += 1.0;
            }

            double inv_x = 1.0 / x;
            double inv_x2 = inv_x * inv_x;
            result += std::log(x) - 0.5 * inv_x 
                      - inv_x2 * (1.0/12.0 - inv_x2 * (1.0/120.0 - inv_x2 / 252.0));

            return result;
        }

        // Trigamma function (second derivative of log gamma)
        static double trigamma(double x)
        {
            // Asymptotic expansion for large x
            if (x > 6.0)
            {
                double inv_x = 1.0 / x;
                double inv_x2 = inv_x * inv_x;
                return inv_x + 0.5 * inv_x2 
                       + inv_x2 * inv_x * (1.0/6.0 - inv_x2 * (1.0/30.0 - inv_x2 / 42.0));
            }

            // Recurrence relation
            double result = 0.0;
            while (x < 6.0)
            {
                result += 1.0 / (x * x);
                x += 1.0;
            }

            double inv_x = 1.0 / x;
            double inv_x2 = inv_x * inv_x;
            result += inv_x + 0.5 * inv_x2 
                      + inv_x2 * inv_x * (1.0/6.0 - inv_x2 * (1.0/30.0 - inv_x2 / 42.0));

            return result;
        }

        // ============================================================================
        // Viterbi (log-space for numerical stability)
        // ============================================================================

        void viterbi_log(const double *observations, int32_t T)
        {
            // Compute log emissions
            std::vector<double> log_emit(T * K_);

            log_emit[0] = std::log(emission_.data[0] + 1e-300);
            for (int32_t k = 1; k < K_; ++k)
            {
                log_emit[k] = std::log(emission_.data[k] + 1e-300);
            }

            for (int32_t t = 1; t < T; ++t)
            {
                for (int32_t k = 0; k < K_; ++k)
                {
                    log_emit[t * K_ + k] = std::log(emission_.data[t * K_ + k] + 1e-300);
                }
            }

            // t = 0
            for (int32_t k = 0; k < K_; ++k)
            {
                viterbi_delta_.data[k] = std::log(pi_[k] + 1e-300) + log_emit[k];
                viterbi_psi_.data[k] = 0;
            }

            // t = 1 to T-1
            for (int32_t t = 1; t < T; ++t)
            {
                for (int32_t j = 0; j < K_; ++j)
                {
                    double max_val = -std::numeric_limits<double>::infinity();
                    int32_t max_idx = 0;

                    for (int32_t i = 0; i < K_; ++i)
                    {
                        double val = viterbi_delta_.data[(t - 1) * K_ + i] + log_trans_[i * K_ + j];
                        if (val > max_val)
                        {
                            max_val = val;
                            max_idx = i;
                        }
                    }

                    viterbi_delta_.data[t * K_ + j] = max_val + log_emit[t * K_ + j];
                    viterbi_psi_.data[t * K_ + j] = max_idx;
                }
            }

            // Backtrack
            double max_val = -std::numeric_limits<double>::infinity();
            int32_t max_idx = 0;
            for (int32_t k = 0; k < K_; ++k)
            {
                if (viterbi_delta_.data[(T - 1) * K_ + k] > max_val)
                {
                    max_val = viterbi_delta_.data[(T - 1) * K_ + k];
                    max_idx = k;
                }
            }

            viterbi_path_.data[T - 1] = max_idx;
            for (int32_t t = T - 2; t >= 0; --t)
            {
                viterbi_path_.data[t] = viterbi_psi_.data[(t + 1) * K_ + viterbi_path_.data[t + 1]];
            }
        }

        // ============================================================================
        // Pack results
        // ============================================================================

        void pack_results(int32_t T, StudentTHMMResult &result)
        {
            result.phi = phi_;
            result.sigma = sigma_;
            result.nu = nu_;
            result.pi = pi_;

            result.trans.resize(K_);
            for (int32_t i = 0; i < K_; ++i)
            {
                result.trans[i].resize(K_);
                for (int32_t j = 0; j < K_; ++j)
                {
                    result.trans[i][j] = trans_[i * K_ + j];
                }
            }

            result.viterbi_path.resize(T);
            std::memcpy(result.viterbi_path.data(), viterbi_path_.data, T * sizeof(int32_t));

            // Recompute posteriors
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
    // Utility: Sort states by volatility
    // ============================================================================

    inline void sort_student_t_hmm_states(StudentTHMMResult &result)
    {
        int K = static_cast<int>(result.sigma.size());
        if (K < 2)
            return;

        std::vector<int> order(K);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return result.sigma[a] < result.sigma[b];
        });

        // Reorder all parameters
        std::vector<double> new_phi(K), new_sigma(K), new_nu(K), new_pi(K);
        std::vector<std::vector<double>> new_trans(K, std::vector<double>(K));

        for (int i = 0; i < K; ++i)
        {
            new_phi[i] = result.phi[order[i]];
            new_sigma[i] = result.sigma[order[i]];
            new_nu[i] = result.nu[order[i]];
            new_pi[i] = result.pi[order[i]];
        }

        for (int i = 0; i < K; ++i)
        {
            for (int j = 0; j < K; ++j)
            {
                new_trans[i][j] = result.trans[order[i]][order[j]];
            }
        }

        result.phi = new_phi;
        result.sigma = new_sigma;
        result.nu = new_nu;
        result.pi = new_pi;
        result.trans = new_trans;

        // Remap Viterbi path
        std::vector<int> inv_order(K);
        for (int i = 0; i < K; ++i)
            inv_order[order[i]] = i;

        for (auto &s : result.viterbi_path)
            s = inv_order[s];

        // Remap posteriors
        for (auto &post : result.posteriors)
        {
            std::vector<double> new_post(K);
            for (int i = 0; i < K; ++i)
                new_post[i] = post[order[i]];
            post = new_post;
        }
    }

    // ============================================================================
    // Print IMM parameters (for copy-paste into code)
    // ============================================================================

    inline void print_student_t_imm_params(const StudentTHMMResult &result)
    {
        int K = static_cast<int>(result.sigma.size());
        const char *names[] = {"Calm", "Trend", "Crisis", "State3", "State4"};

        printf("\n=== Student-t AR-HMM IMM Parameters ===\n");
        printf("// Paste into your IMM initialization:\n");
        printf("// State dynamics: x_t = phi * x_{t-1} + sigma * eps_t\n");
        printf("// where eps_t ~ Student-t(0, 1, nu)\n\n");

        for (int k = 0; k < K && k < 5; ++k)
        {
            printf("// %s: phi=%.4f, sigma=%.4f, nu=%.1f\n",
                   names[k], result.phi[k], result.sigma[k], result.nu[k]);
        }

        printf("\ndouble phi[%d] = {", K);
        for (int k = 0; k < K; ++k)
            printf("%.4f%s", result.phi[k], k < K - 1 ? ", " : "};\n");

        printf("double sigma[%d] = {", K);
        for (int k = 0; k < K; ++k)
            printf("%.4f%s", result.sigma[k], k < K - 1 ? ", " : "};\n");

        printf("double nu[%d] = {", K);
        for (int k = 0; k < K; ++k)
            printf("%.1f%s", result.nu[k], k < K - 1 ? ", " : "};\n");

        printf("double trans[%d][%d] = {\n", K, K);
        for (int i = 0; i < K; ++i)
        {
            printf("    {");
            for (int j = 0; j < K; ++j)
                printf("%.4f%s", result.trans[i][j], j < K - 1 ? ", " : "");
            printf("}%s\n", i < K - 1 ? "," : "");
        }
        printf("};\n");
    }

} // namespace eemd

#endif // HMM_STUDENT_T_HPP