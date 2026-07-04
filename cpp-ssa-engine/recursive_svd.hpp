#ifndef RECURSIVE_SVD_HPP
#define RECURSIVE_SVD_HPP

#include "ssa_core.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

// ============================================================================
// Small-Matrix Jacobi SVD — for (r+1)×(r+1) intermediate matrices (r <= 4)
//
// One-sided Jacobi: computes the FULL SVD of an n×n matrix A where n <= 5.
// Output: A = U_out * diag(sigma_out) * V_out^T
// Convergent in ~10 sweeps. No dynamic allocation.
// ============================================================================

namespace svd_detail {

static constexpr int SMALL_N = SSA_MAX_R + 1; // max 5×5
static constexpr double SVD_EPS = 1e-15;
static constexpr int JACOBI_MAX_SWEEPS = 30;

// Givens rotation parameters for zeroing A[p][q] in a symmetric matrix
inline void sym_schur2(const double* A, int n, int p, int q,
                       double& cs, double& sn)
{
    double app = A[p * n + p];
    double aqq = A[q * n + q];
    double apq = A[p * n + q];

    if (std::abs(apq) < SVD_EPS * (std::abs(app) + std::abs(aqq) + SVD_EPS)) {
        cs = 1.0;
        sn = 0.0;
        return;
    }

    double tau = (aqq - app) / (2.0 * apq);
    double t;
    if (tau >= 0.0)
        t = 1.0 / (tau + std::sqrt(1.0 + tau * tau));
    else
        t = -1.0 / (-tau + std::sqrt(1.0 + tau * tau));

    cs = 1.0 / std::sqrt(1.0 + t * t);
    sn = t * cs;
}

// Full SVD of an n×n matrix (n <= 5) via two-sided Jacobi.
// Input:  M (n×n, row-major) — destroyed in-place.
// Output: U_out (n×n), sigma_out (n), V_out (n×n)
// Returns actual rank (number of non-negligible singular values).
inline int small_svd(double* M, int n,
                     double* U_out, double* sigma_out, double* V_out)
{
    // Strategy: compute M^T M, eigendecompose it for V and sigma^2,
    // then recover U = M V sigma^{-1}.
    // For n<=5, this is numerically adequate and avoids complex two-sided iteration.

    double MtM[SMALL_N * SMALL_N];
    std::memset(MtM, 0, sizeof(MtM));

    // M^T M (symmetric)
    for (int i = 0; i < n; ++i) {
        for (int j = i; j < n; ++j) {
            double s = 0.0;
            for (int k = 0; k < n; ++k) {
                s += M[k * n + i] * M[k * n + j];
            }
            MtM[i * n + j] = s;
            MtM[j * n + i] = s;
        }
    }

    // Initialize V_out = I
    std::memset(V_out, 0, n * n * sizeof(double));
    for (int i = 0; i < n; ++i) V_out[i * n + i] = 1.0;

    // Jacobi eigendecomposition of MtM (symmetric): MtM = V diag(lambda) V^T
    for (int sweep = 0; sweep < JACOBI_MAX_SWEEPS; ++sweep) {
        double off_norm = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                off_norm += MtM[i * n + j] * MtM[i * n + j];

        if (off_norm < SVD_EPS * SVD_EPS) break;

        for (int p = 0; p < n - 1; ++p) {
            for (int q = p + 1; q < n; ++q) {
                if (std::abs(MtM[p * n + q]) < SVD_EPS * (std::abs(MtM[p * n + p]) + std::abs(MtM[q * n + q]) + SVD_EPS))
                    continue;

                double cs, sn;
                sym_schur2(MtM, n, p, q, cs, sn);

                // Apply rotation to MtM: J^T * MtM * J
                // Rows/cols p and q of MtM
                for (int i = 0; i < n; ++i) {
                    double mip = MtM[i * n + p];
                    double miq = MtM[i * n + q];
                    MtM[i * n + p] =  cs * mip + sn * miq;
                    MtM[i * n + q] = -sn * mip + cs * miq;
                }
                for (int j = 0; j < n; ++j) {
                    double mpj = MtM[p * n + j];
                    double mqj = MtM[q * n + j];
                    MtM[p * n + j] =  cs * mpj + sn * mqj;
                    MtM[q * n + j] = -sn * mpj + cs * mqj;
                }

                // Accumulate V
                for (int i = 0; i < n; ++i) {
                    double vip = V_out[i * n + p];
                    double viq = V_out[i * n + q];
                    V_out[i * n + p] =  cs * vip + sn * viq;
                    V_out[i * n + q] = -sn * vip + cs * viq;
                }
            }
        }
    }

    // Extract eigenvalues (diagonal of MtM) → singular values
    // Sort in descending order
    int idx[SMALL_N];
    for (int i = 0; i < n; ++i) {
        sigma_out[i] = MtM[i * n + i];
        idx[i] = i;
    }

    // Insertion sort by descending eigenvalue (n <= 5, trivial)
    for (int i = 1; i < n; ++i) {
        int key = idx[i];
        double key_val = sigma_out[key];
        int j = i - 1;
        while (j >= 0 && sigma_out[idx[j]] < key_val) {
            idx[j + 1] = idx[j];
            --j;
        }
        idx[j + 1] = key;
    }

    // Reorder V columns and compute sigma = sqrt(lambda)
    double V_sorted[SMALL_N * SMALL_N];
    for (int k = 0; k < n; ++k) {
        int src = idx[k];
        double lam = sigma_out[src];
        sigma_out[k] = (lam > SVD_EPS) ? std::sqrt(lam) : 0.0;
        for (int i = 0; i < n; ++i) {
            V_sorted[i * n + k] = V_out[i * n + src];
        }
    }
    std::memcpy(V_out, V_sorted, n * n * sizeof(double));

    // Relative threshold: discard singular values negligible vs the largest.
    // Prevents catastrophic amplification in U = M * V * Σ^{-1}.
    const double sigma_max = sigma_out[0];
    const double sigma_rel_thresh = std::max(sigma_max * 1e-8, 1e-12);

    // Compute U = M * V * Sigma^{-1}
    std::memset(U_out, 0, n * n * sizeof(double));
    int rank = 0;
    for (int k = 0; k < n; ++k) {
        if (sigma_out[k] < sigma_rel_thresh) {
            sigma_out[k] = 0.0;
            break;
        }
        ++rank;
        double inv_s = 1.0 / sigma_out[k];
        for (int i = 0; i < n; ++i) {
            double s = 0.0;
            for (int j = 0; j < n; ++j) {
                s += M[i * n + j] * V_out[j * n + k];
            }
            U_out[i * n + k] = s * inv_s;
        }
    }

    return rank;
}

} // namespace svd_detail

// ============================================================================
// RecursiveSVD — Incremental rank-r SVD with Brand's rank-1 update
//
// Maintains top-r singular triplets (σ_i, u_i, v_i).
// All working buffers pre-allocated. ZERO heap allocations in update().
//
// Memory layout (column-major for vectorization):
//   U_[k * SSA_MAX_L + i] = U[i][k]  (left singular vectors, L×r)
//   V_[k * SSA_MAX_K + j] = V[j][k]  (right singular vectors, K×r)
// ============================================================================

class RecursiveSVD {
public:
    RecursiveSVD()
        : L_(0), K_(0), r_(0), update_count_(0), frobenius_sq_(0.0),
          initialized_(false), needs_recompute_(false)
    {
        std::memset(U_, 0, sizeof(U_));
        std::memset(V_, 0, sizeof(V_));
        std::memset(sigma_, 0, sizeof(sigma_));
        std::memset(p_, 0, sizeof(p_));
        std::memset(q_, 0, sizeof(q_));
        std::memset(a_hat_, 0, sizeof(a_hat_));
        std::memset(b_hat_, 0, sizeof(b_hat_));
        std::memset(M_buf_, 0, sizeof(M_buf_));
        std::memset(M_U_, 0, sizeof(M_U_));
        std::memset(M_V_, 0, sizeof(M_V_));
        std::memset(M_sigma_, 0, sizeof(M_sigma_));
        std::memset(U_new_col_, 0, sizeof(U_new_col_));
        std::memset(V_new_col_, 0, sizeof(V_new_col_));
    }

    // Cold-start: compute full SVD of the trajectory matrix, extract top-r triplets.
    // Allocates temporary workspace on the heap (cold path only).
    void init(TrajectoryMatrix& X, int rank_r) {
        L_ = X.L();
        K_ = X.K();
        r_ = std::min(rank_r, std::min(L_, K_));
        update_count_ = 0;
        initialized_ = true;
        needs_recompute_ = false;

        compute_full_svd(X);
        compute_frobenius_sq(X);
    }

    // Incremental rank-2 update: remove one column, add one column.
    // ZERO heap allocations. O(Lr + Kr + r^3) per call.
    void update(const double* col_removed, const double* col_added) {
        double norm_old_sq = 0.0, norm_new_sq = 0.0;
        for (int i = 0; i < L_; ++i) {
            norm_old_sq += col_removed[i] * col_removed[i];
            norm_new_sq += col_added[i] * col_added[i];
        }
        frobenius_sq_ += norm_new_sq - norm_old_sq;
        frobenius_sq_ = std::max(frobenius_sq_, 0.0);

        brand_update_neg_col(col_removed, 0);
        shift_V_left();
        brand_update_pos_col(col_added, K_ - 1);

        ++update_count_;

        // Post-update sigma validation — catch NaN/Inf/negative immediately
        for (int k = 0; k < r_; ++k) {
            if (!std::isfinite(sigma_[k]) || sigma_[k] < 0.0) {
                needs_recompute_ = true;
                return;
            }
        }

        // Re-orthogonalize every 25 updates (prevents drift accumulation)
        if (update_count_ % 25 == 0) {
            reorthogonalize();
        }

        // Full recompute every 200 updates to reset all accumulated error
        if (update_count_ % 200 == 0) {
            needs_recompute_ = true;
        }
    }

    // Full recompute when L changes (cold path, ~100µs).
    void recompute(TrajectoryMatrix& X) {
        L_ = X.L();
        K_ = X.K();
        r_ = std::min(r_, std::min(L_, K_));
        update_count_ = 0;
        needs_recompute_ = false;

        compute_full_svd(X);
        compute_frobenius_sq(X);
    }

    // Accessors (pointers to internal column-major storage)
    const double* U() const { return U_; }
    const double* V() const { return V_; }
    const double* sigma() const { return sigma_; }
    int L() const { return L_; }
    int K() const { return K_; }
    int r() const { return r_; }
    bool initialized() const { return initialized_; }
    bool needs_recompute() const { return needs_recompute_; }

    double evr() const {
        if (frobenius_sq_ < 1e-12) return 1.0;
        double sum_sq = 0.0;
        for (int k = 0; k < r_; ++k) {
            sum_sq += sigma_[k] * sigma_[k];
        }
        double result = sum_sq / std::max(frobenius_sq_, 1e-12);
        return std::min(std::max(result, 0.0), 1.0);
    }

    double eigen_gap() const {
        if (r_ < 2 || sigma_[0] < 1e-12) return 1.0;
        double ratio = sigma_[1] / std::max(sigma_[0], 1e-12);
        return std::min(std::max(1.0 - ratio, 0.0), 1.0);
    }

private:
    // --- Dimensions ---
    int L_;
    int K_;
    int r_;
    int update_count_;
    double frobenius_sq_;
    bool initialized_;
    bool needs_recompute_;

    // --- Singular triplets (column-major) ---
    alignas(64) double U_[SSA_MAX_R * SSA_MAX_L];   // U[i][k] = U_[k*MAX_L + i]
    alignas(64) double V_[SSA_MAX_R * SSA_MAX_K];   // V[j][k] = V_[k*MAX_K + j]
    double sigma_[SSA_MAX_R];

    // --- Pre-allocated working buffers for Brand's update (ZERO alloc in hot path) ---
    alignas(64) double p_[SSA_MAX_R + 1];           // U^T * a (r-vector)
    alignas(64) double q_[SSA_MAX_R + 1];           // V^T * b (r-vector)
    alignas(64) double a_hat_[SSA_MAX_L];           // normalized residual of a
    alignas(64) double b_hat_[SSA_MAX_K];           // normalized residual of b
    alignas(64) double M_buf_[(SSA_MAX_R+1) * (SSA_MAX_R+1)];  // intermediate matrix
    alignas(64) double M_U_[(SSA_MAX_R+1) * (SSA_MAX_R+1)];    // SVD of M: left vecs
    alignas(64) double M_V_[(SSA_MAX_R+1) * (SSA_MAX_R+1)];    // SVD of M: right vecs
    double M_sigma_[SSA_MAX_R + 1];                             // SVD of M: values
    alignas(64) double U_new_col_[SSA_MAX_L];       // temp for U rotation
    alignas(64) double V_new_col_[SSA_MAX_K];       // temp for V rotation

    // ========================================================================
    // Brand's rank-1 update: X' = X - col * e_j^T (downdate)
    // ========================================================================
    void brand_update_neg_col(const double* col, int j_pos) {
        const int r = r_;
        const int L = L_;
        const int K = K_;

        // p = U^T * (-col) = -(U^T * col)
        for (int k = 0; k < r; ++k) {
            double dot = 0.0;
            const double* Uk = &U_[k * SSA_MAX_L];
            for (int i = 0; i < L; ++i) {
                dot += Uk[i] * col[i];
            }
            p_[k] = -dot;
        }

        // a_hat = (-col) - U * p = -col - U * p
        for (int i = 0; i < L; ++i) {
            a_hat_[i] = -col[i];
        }
        for (int k = 0; k < r; ++k) {
            const double pk = p_[k];
            const double* Uk = &U_[k * SSA_MAX_L];
            for (int i = 0; i < L; ++i) {
                a_hat_[i] -= pk * Uk[i];
            }
        }

        double rho_a = 0.0;
        for (int i = 0; i < L; ++i) {
            rho_a += a_hat_[i] * a_hat_[i];
        }
        rho_a = std::sqrt(rho_a);

        // Normalize a_hat (if residual is non-negligible)
        bool expand_U = (rho_a > 1e-8);
        if (expand_U) {
            double inv_rho = 1.0 / rho_a;
            for (int i = 0; i < L; ++i) {
                a_hat_[i] *= inv_rho;
            }
        }

        // q = V^T * e_j = V[j_pos, :] (j-th row of V)
        for (int k = 0; k < r; ++k) {
            q_[k] = V_[k * SSA_MAX_K + j_pos];
        }

        // b_hat = e_j - V * q
        std::memset(b_hat_, 0, K * sizeof(double));
        b_hat_[j_pos] = 1.0;
        for (int k = 0; k < r; ++k) {
            const double qk = q_[k];
            const double* Vk = &V_[k * SSA_MAX_K];
            for (int jj = 0; jj < K; ++jj) {
                b_hat_[jj] -= qk * Vk[jj];
            }
        }

        double rho_b = 0.0;
        for (int jj = 0; jj < K; ++jj) {
            rho_b += b_hat_[jj] * b_hat_[jj];
        }
        rho_b = std::sqrt(rho_b);

        bool expand_V = (rho_b > 1e-8);
        if (expand_V) {
            double inv_rho = 1.0 / rho_b;
            for (int jj = 0; jj < K; ++jj) {
                b_hat_[jj] *= inv_rho;
            }
        }

        apply_brand_core(rho_a, rho_b, expand_U, expand_V);
    }

    // ========================================================================
    // Brand's rank-1 update: X' = X + col * e_j^T (update)
    // ========================================================================
    void brand_update_pos_col(const double* col, int j_pos) {
        const int r = r_;
        const int L = L_;
        const int K = K_;

        // p = U^T * col
        for (int k = 0; k < r; ++k) {
            double dot = 0.0;
            const double* Uk = &U_[k * SSA_MAX_L];
            for (int i = 0; i < L; ++i) {
                dot += Uk[i] * col[i];
            }
            p_[k] = dot;
        }

        // a_hat = col - U * p
        for (int i = 0; i < L; ++i) {
            a_hat_[i] = col[i];
        }
        for (int k = 0; k < r; ++k) {
            const double pk = p_[k];
            const double* Uk = &U_[k * SSA_MAX_L];
            for (int i = 0; i < L; ++i) {
                a_hat_[i] -= pk * Uk[i];
            }
        }

        double rho_a = 0.0;
        for (int i = 0; i < L; ++i) {
            rho_a += a_hat_[i] * a_hat_[i];
        }
        rho_a = std::sqrt(rho_a);

        bool expand_U = (rho_a > 1e-8);
        if (expand_U) {
            double inv_rho = 1.0 / rho_a;
            for (int i = 0; i < L; ++i) {
                a_hat_[i] *= inv_rho;
            }
        }

        // q = V^T * e_j = V[j_pos, :]
        for (int k = 0; k < r; ++k) {
            q_[k] = V_[k * SSA_MAX_K + j_pos];
        }

        // b_hat = e_j - V * q
        std::memset(b_hat_, 0, K * sizeof(double));
        b_hat_[j_pos] = 1.0;
        for (int k = 0; k < r; ++k) {
            const double qk = q_[k];
            const double* Vk = &V_[k * SSA_MAX_K];
            for (int jj = 0; jj < K; ++jj) {
                b_hat_[jj] -= qk * Vk[jj];
            }
        }

        double rho_b = 0.0;
        for (int jj = 0; jj < K; ++jj) {
            rho_b += b_hat_[jj] * b_hat_[jj];
        }
        rho_b = std::sqrt(rho_b);

        bool expand_V = (rho_b > 1e-8);
        if (expand_V) {
            double inv_rho = 1.0 / rho_b;
            for (int jj = 0; jj < K; ++jj) {
                b_hat_[jj] *= inv_rho;
            }
        }

        apply_brand_core(rho_a, rho_b, expand_U, expand_V);
    }

    // ========================================================================
    // Brand core: construct (r+1)×(r+1) intermediate matrix, SVD it,
    // rotate U and V, truncate back to rank r.
    // ========================================================================
    void apply_brand_core(double rho_a, double rho_b,
                          bool expand_U, bool expand_V) {
        const int r = r_;
        const int L = L_;
        const int K = K_;

        // Determine intermediate matrix dimension
        const int n = r + 1; // always work in (r+1)×(r+1) space

        // Extended p_tilde = [p_0, ..., p_{r-1}, rho_a] if expand_U, else [p_0,...,p_{r-1}, 0]
        p_[r] = expand_U ? rho_a : 0.0;

        // Extended q_tilde = [q_0, ..., q_{r-1}, rho_b] if expand_V, else [q_0,...,q_{r-1}, 0]
        q_[r] = expand_V ? rho_b : 0.0;

        // M = diag([sigma_0,...,sigma_{r-1}, 0]) + p_tilde * q_tilde^T
        std::memset(M_buf_, 0, n * n * sizeof(double));
        for (int k = 0; k < r; ++k) {
            M_buf_[k * n + k] = sigma_[k];
        }
        // Add outer product p_tilde * q_tilde^T
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                M_buf_[i * n + j] += p_[i] * q_[j];
            }
        }

        // SVD of M (n×n, n <= 5)
        svd_detail::small_svd(M_buf_, n, M_U_, M_sigma_, M_V_);

        // --- Rotate U: U_new = [U | a_hat] * M_U[:, 0:r] ---
        // U_new[:,k] = sum_{m=0}^{r-1} M_U[m][k] * U_old[:,m] + M_U[r][k] * a_hat
        for (int k = 0; k < r; ++k) {
            // Compute new column k of U
            std::memset(U_new_col_, 0, L * sizeof(double));

            for (int m = 0; m < r; ++m) {
                const double coeff = M_U_[m * n + k];
                const double* Um = &U_[m * SSA_MAX_L];
                for (int i = 0; i < L; ++i) {
                    U_new_col_[i] += coeff * Um[i];
                }
            }
            if (expand_U) {
                const double coeff = M_U_[r * n + k];
                for (int i = 0; i < L; ++i) {
                    U_new_col_[i] += coeff * a_hat_[i];
                }
            }

            // Write back to U_[k * SSA_MAX_L ..]
            std::memcpy(&U_[k * SSA_MAX_L], U_new_col_, L * sizeof(double));
        }

        // --- Rotate V: V_new = [V | b_hat] * M_V[:, 0:r] ---
        for (int k = 0; k < r; ++k) {
            std::memset(V_new_col_, 0, K * sizeof(double));

            for (int m = 0; m < r; ++m) {
                const double coeff = M_V_[m * n + k];
                const double* Vm = &V_[m * SSA_MAX_K];
                for (int jj = 0; jj < K; ++jj) {
                    V_new_col_[jj] += coeff * Vm[jj];
                }
            }
            if (expand_V) {
                const double coeff = M_V_[r * n + k];
                for (int jj = 0; jj < K; ++jj) {
                    V_new_col_[jj] += coeff * b_hat_[jj];
                }
            }

            std::memcpy(&V_[k * SSA_MAX_K], V_new_col_, K * sizeof(double));
        }

        // --- Update singular values (truncated to rank r, clamped non-negative) ---
        for (int k = 0; k < r; ++k) {
            sigma_[k] = std::max(M_sigma_[k], 0.0);
        }
    }

    // ========================================================================
    // Shift V rows left by 1: row[i] = row[i+1], row[K-1] = 0
    // Models the column re-indexing after removing column 0.
    // ========================================================================
    void shift_V_left() {
        const int K = K_;
        for (int k = 0; k < r_; ++k) {
            double* Vk = &V_[k * SSA_MAX_K];
            std::memmove(Vk, Vk + 1, (K - 1) * sizeof(double));
            Vk[K - 1] = 0.0;
        }
    }

    // ========================================================================
    // Modified Gram-Schmidt re-orthogonalization of U columns.
    // O(L * r^2). Called every 100 updates.
    // ========================================================================
    void reorthogonalize() {
        const int L = L_;
        const int r = r_;

        for (int k = 0; k < r; ++k) {
            double* Uk = &U_[k * SSA_MAX_L];

            // Subtract projections onto previous columns
            for (int m = 0; m < k; ++m) {
                const double* Um = &U_[m * SSA_MAX_L];
                double dot = 0.0;
                for (int i = 0; i < L; ++i) {
                    dot += Uk[i] * Um[i];
                }
                for (int i = 0; i < L; ++i) {
                    Uk[i] -= dot * Um[i];
                }
            }

            // Normalize
            double norm = 0.0;
            for (int i = 0; i < L; ++i) {
                norm += Uk[i] * Uk[i];
            }
            norm = std::sqrt(norm);
            if (norm > 1e-10) {
                double inv_norm = 1.0 / norm;
                for (int i = 0; i < L; ++i) {
                    Uk[i] *= inv_norm;
                }
            } else {
                std::memset(Uk, 0, L * sizeof(double));
            }
        }

        // Also re-orthogonalize V
        for (int k = 0; k < r; ++k) {
            double* Vk = &V_[k * SSA_MAX_K];

            for (int m = 0; m < k; ++m) {
                const double* Vm = &V_[m * SSA_MAX_K];
                double dot = 0.0;
                for (int jj = 0; jj < K_; ++jj) {
                    dot += Vk[jj] * Vm[jj];
                }
                for (int jj = 0; jj < K_; ++jj) {
                    Vk[jj] -= dot * Vm[jj];
                }
            }

            double norm = 0.0;
            for (int jj = 0; jj < K_; ++jj) {
                norm += Vk[jj] * Vk[jj];
            }
            norm = std::sqrt(norm);
            if (norm > 1e-10) {
                double inv_norm = 1.0 / norm;
                for (int jj = 0; jj < K_; ++jj) {
                    Vk[jj] *= inv_norm;
                }
            } else {
                std::memset(Vk, 0, K_ * sizeof(double));
            }
        }
    }

    // ========================================================================
    // Cold-start: full SVD via Gram matrix eigendecomposition
    //
    // Method: C = X X^T (L×L symmetric), eigendecompose, then V = X^T U Σ^{-1}
    // Heap allocation here is permitted (cold path only).
    // ========================================================================
    void compute_full_svd(TrajectoryMatrix& X) {
        const int L = L_;
        const int K = K_;
        const int r = r_;

        // Allocate working memory (cold path — heap OK)
        std::vector<double> C(L * L, 0.0);       // Gram matrix X X^T
        std::vector<double> E(L * L, 0.0);       // eigenvectors
        std::vector<double> col_buf(L);           // column extraction buffer

        // Build C = X X^T via rank-1 updates from each column
        for (int j = 0; j < K; ++j) {
            X.column(j, col_buf.data());
            for (int ii = 0; ii < L; ++ii) {
                const double ci = col_buf[ii];
                for (int jj = ii; jj < L; ++jj) {
                    C[ii * L + jj] += ci * col_buf[jj];
                }
            }
        }
        // Symmetrize
        for (int ii = 0; ii < L; ++ii) {
            for (int jj = 0; jj < ii; ++jj) {
                C[ii * L + jj] = C[jj * L + ii];
            }
        }

        // Jacobi eigendecomposition: C = E * diag(lambda) * E^T
        // Initialize E = I
        for (int i = 0; i < L; ++i) E[i * L + i] = 1.0;

        jacobi_symmetric_eigen(C.data(), E.data(), L);

        // Extract eigenvalues (diagonal of C after Jacobi) and sort descending
        std::vector<std::pair<double, int>> eig_pairs(L);
        for (int i = 0; i < L; ++i) {
            eig_pairs[i] = { C[i * L + i], i };
        }
        std::sort(eig_pairs.begin(), eig_pairs.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        // Store top-r singular values and left singular vectors
        for (int k = 0; k < r; ++k) {
            double lam = eig_pairs[k].first;
            sigma_[k] = (lam > 1e-30) ? std::sqrt(lam) : 0.0;

            int src_col = eig_pairs[k].second;
            double* Uk = &U_[k * SSA_MAX_L];
            for (int i = 0; i < L; ++i) {
                Uk[i] = E[i * L + src_col];
            }
        }

        // Compute V = X^T U Σ^{-1}
        const double sigma_max_full = (r > 0) ? sigma_[0] : 1.0;
        const double sigma_thresh_full = std::max(sigma_max_full * 1e-8, 1e-12);
        for (int k = 0; k < r; ++k) {
            if (sigma_[k] < sigma_thresh_full) {
                sigma_[k] = 0.0;
                std::memset(&V_[k * SSA_MAX_K], 0, K * sizeof(double));
                continue;
            }
            const double inv_s = 1.0 / sigma_[k];
            const double* Uk = &U_[k * SSA_MAX_L];
            double* Vk = &V_[k * SSA_MAX_K];

            for (int j = 0; j < K; ++j) {
                X.column(j, col_buf.data());
                double dot = 0.0;
                for (int i = 0; i < L; ++i) {
                    dot += col_buf[i] * Uk[i];
                }
                Vk[j] = dot * inv_s;
            }
        }
    }

    // ========================================================================
    // Jacobi eigendecomposition for symmetric matrix (cyclic sweeps).
    // A is L×L row-major — overwritten to diagonal form.
    // E is L×L row-major — accumulates eigenvectors.
    // ========================================================================
    static void jacobi_symmetric_eigen(double* A, double* E, int n) {
        static constexpr int MAX_SWEEPS = 50;
        static constexpr double TOL = 1e-12;

        for (int sweep = 0; sweep < MAX_SWEEPS; ++sweep) {
            // Check off-diagonal convergence
            double off_norm = 0.0;
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j)
                    off_norm += A[i * n + j] * A[i * n + j];

            if (off_norm < TOL * TOL) break;

            for (int p = 0; p < n - 1; ++p) {
                for (int q = p + 1; q < n; ++q) {
                    double apq = A[p * n + q];
                    if (std::abs(apq) < TOL * (std::abs(A[p * n + p]) + std::abs(A[q * n + q]) + TOL))
                        continue;

                    double app = A[p * n + p];
                    double aqq = A[q * n + q];
                    double tau = (aqq - app) / (2.0 * apq);
                    double t;
                    if (tau >= 0.0)
                        t = 1.0 / (tau + std::sqrt(1.0 + tau * tau));
                    else
                        t = -1.0 / (-tau + std::sqrt(1.0 + tau * tau));

                    double cs = 1.0 / std::sqrt(1.0 + t * t);
                    double sn = t * cs;

                    // Rotate rows and columns p, q of A
                    for (int i = 0; i < n; ++i) {
                        double aip = A[i * n + p];
                        double aiq = A[i * n + q];
                        A[i * n + p] =  cs * aip + sn * aiq;
                        A[i * n + q] = -sn * aip + cs * aiq;
                    }
                    for (int j = 0; j < n; ++j) {
                        double apj = A[p * n + j];
                        double aqj = A[q * n + j];
                        A[p * n + j] =  cs * apj + sn * aqj;
                        A[q * n + j] = -sn * apj + cs * aqj;
                    }

                    // Accumulate eigenvectors
                    for (int i = 0; i < n; ++i) {
                        double eip = E[i * n + p];
                        double eiq = E[i * n + q];
                        E[i * n + p] =  cs * eip + sn * eiq;
                        E[i * n + q] = -sn * eip + cs * eiq;
                    }
                }
            }
        }
    }

    // ========================================================================
    // Compute full Frobenius norm squared from the trajectory matrix
    // ========================================================================
    void compute_frobenius_sq(TrajectoryMatrix& X) {
        double sum = 0.0;
        alignas(64) double col_tmp[SSA_MAX_L];
        for (int j = 0; j < K_; ++j) {
            X.column(j, col_tmp);
            for (int i = 0; i < L_; ++i) {
                sum += col_tmp[i] * col_tmp[i];
            }
        }
        frobenius_sq_ = sum;
    }
};

#endif // RECURSIVE_SVD_HPP
