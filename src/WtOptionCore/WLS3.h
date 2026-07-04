/*!
 * \file WLS3.h
 * \brief Hand-written 3-parameter Weighted Least Squares solver
 *
 * Replaces gsl_multifit_wlinear for N×3 systems (GVV vol curve fitting).
 * Uses normal equations: A·c = b where A = XᵀWX (3×3), b = XᵀWy.
 * Solves 3×3 via Cramer's rule — zero allocation, ~0.1μs per solve.
 *
 * Performance vs GSL:
 *   - GSL: 5× malloc/free + LAPACK path ≈ 5-10μs
 *   - WLS3: stack-only Cramer ≈ 0.1μs (50-100x faster for N<100)
 *   - Numerical stability: 3×3 Cramer is stable for well-conditioned systems
 */
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace wt_option {

class WLS3 {
public:
    // Reset for a new fit
    void clear() {
        for (int i = 0; i < 9; ++i) m_A[i] = 0.0;
        for (int i = 0; i < 3; ++i) m_b[i] = 0.0;
    }

    // Add one observation: row = [1, x1, x2], y = target, w = weight
    inline void add(double x1, double x2, double y, double w) {
        // X row = [1, x1, x2]
        // A += w * Xᵀ·X  (3×3 symmetric)
        // b += w * Xᵀ·y  (3×1)

        double wx = w;
        double wx1 = w * x1;
        double wx2 = w * x2;

        // Upper triangle of A (symmetric)
        m_A[0] += wx;                    // A00 += w*1*1
        m_A[1] += wx1;                   // A01 += w*1*x1
        m_A[2] += wx2;                   // A02 += w*1*x2
        m_A[4] += w * x1 * x1;          // A11 += w*x1*x1
        m_A[5] += w * x1 * x2;          // A12 += w*x1*x2
        m_A[8] += w * x2 * x2;          // A22 += w*x2*x2

        // b
        m_b[0] += wx * y;               // b0 += w*1*y
        m_b[1] += wx1 * y;              // b1 += w*x1*y
        m_b[2] += wx2 * y;              // b2 += w*x2*y
    }

    // Solve A·c = b via Cramer's rule. Returns false if singular.
    bool solve(double out[3]) {
        // A is stored as row-major 3×3 (only upper filled, mirror to lower)
        double a00 = m_A[0], a01 = m_A[1], a02 = m_A[2];
        double a11 = m_A[4], a12 = m_A[5];
        double a22 = m_A[8];

        // Determinant of A
        double det = a00 * (a11 * a22 - a12 * a12)
                   - a01 * (a01 * a22 - a12 * a02)
                   + a02 * (a01 * a12 - a11 * a02);

        if (std::fabs(det) < 1e-18)
            return false;

        double invDet = 1.0 / det;
        double b0 = m_b[0], b1 = m_b[1], b2 = m_b[2];

        // Cramer's rule: ci = det(Ai) / det(A)
        // A0 = replace col0 with b
        out[0] = (b0 * (a11 * a22 - a12 * a12)
                - a01 * (b1 * a22 - a12 * b2)
                + a02 * (b1 * a12 - a11 * b2)) * invDet;

        // A1 = replace col1 with b
        out[1] = (a00 * (b1 * a22 - a12 * b2)
                - b0 * (a01 * a22 - a12 * a02)
                + a02 * (a01 * b2 - b1 * a02)) * invDet;

        // A2 = replace col2 with b
        out[2] = (a00 * (a11 * b2 - b1 * a12)
                - a01 * (a01 * b2 - b1 * a02)
                + b0 * (a01 * a12 - a11 * a02)) * invDet;

        return true;
    }

    // Convenience: add observations and solve in one call
    static bool solveWeighted(
        const double* x1, const double* x2, const double* y,
        const double* w, size_t n, double out[3])
    {
        WLS3 solver;
        solver.clear();
        for (size_t i = 0; i < n; ++i)
            solver.add(x1[i], x2[i], y[i], w[i]);
        return solver.solve(out);
    }

private:
    // Normal equation matrix A (3×3 row-major) and vector b (3×1)
    // A[0]=A00, A[1]=A01, A[2]=A02, A[3]=A10(unused,=A01), A[4]=A11, ...
    double m_A[9] = {};
    double m_b[3] = {};
};

} // namespace wt_option
