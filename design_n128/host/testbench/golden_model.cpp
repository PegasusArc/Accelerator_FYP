#include "golden_model.h"
#include <cmath>
#include <algorithm>

namespace golden {

// ─── Book ────────────────────────────────────────────────────────────────────
void Book::clear() {
    for (int i = 0; i < 5; i++) { bid_[i] = Ent{}; ask_[i] = Ent{}; }
}

void Book::apply(const Update &u) {
    Ent *b = (u.side == 0) ? bid_ : ask_;

    auto find = [&](uint64_t r) {
        for (int i = 0; i < 5; i++) if (b[i].valid && b[i].ref == r) return i;
        return -1;
    };
    auto slot = [&]() {
        for (int i = 0; i < 5; i++) if (!b[i].valid) return i;
        return 4;                                  // evict worst level
    };

    switch (u.msg_type) {
        case 0x01: { int s = slot(); b[s] = { u.price, u.qty, u.ref, true }; break; }
        case 0x02: { int i = find(u.ref); if (i >= 0) b[i].valid = false; break; }
        case 0x03: { int i = find(u.ref);
                     if (i >= 0) { if (b[i].qty <= u.qty) b[i].valid = false;
                                   else b[i].qty -= u.qty; } break; }
        case 0x04: { int i = find(u.ref); if (i >= 0) b[i].valid = false;
                     int s = slot(); b[s] = { u.price, u.qty, u.ref, true }; break; }
        default: break;
    }

    // Ordering must match lob_filter EXACTLY, including tie-breaking.
    //
    // This deliberately does NOT use std::stable_sort. The hardware sorts with
    // an unrolled swap-based selection sort, which is not stable: when two
    // orders sit at the same price — which happens constantly, since the book
    // holds individual orders rather than aggregated price levels — the two
    // algorithms can leave them in different row positions. Same levels,
    // different row order, different tick matrix, and the SVD then diverges.
    // The golden model exists to model the hardware's algorithm in double
    // precision, not a different algorithm in double precision.
    auto bid_ahead = [](const Ent &x, const Ent &y) {
        if (!x.valid) return false;
        if (!y.valid) return true;
        return x.price > y.price;
    };
    auto ask_ahead = [](const Ent &x, const Ent &y) {
        if (!x.valid) return false;
        if (!y.valid) return true;
        return x.price < y.price;
    };
    Ent *arr = (u.side == 0) ? bid_ : ask_;
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 5; j++) {
            bool swap_it = (u.side == 0) ? bid_ahead(arr[j], arr[i])
                                         : ask_ahead(arr[j], arr[i]);
            if (swap_it) { Ent t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
        }
    }
}

bool Book::ready() const { return bid_[0].valid && ask_[0].valid; }

void Book::tick_matrix(double price_ref, double price_scale, double qty_scale,
                       double Bt[GN1][GN3]) const {
    for (int i = 0; i < 5; i++) {
        Bt[i][0] = bid_[i].valid ? (bid_[i].price - price_ref) * price_scale : 0.0;
        Bt[i][1] = bid_[i].valid ? (double)bid_[i].qty * qty_scale : 0.0;
        Bt[5 + i][0] = ask_[i].valid ? (ask_[i].price - price_ref) * price_scale : 0.0;
        Bt[5 + i][1] = ask_[i].valid ? (double)ask_[i].qty * qty_scale : 0.0;
    }
}

// ─── SVD ─────────────────────────────────────────────────────────────────────
void SVD::seed() {
    for (int i = 0; i < GN1; i++)
        for (int j = 0; j < GK; j++)
            for (int s = 0; s < GN3; s++)
                Ut_[i][j][s] = (i == j) ? 1.0 : 0.0;
    for (int j = 0; j < GK; j++) St_[j] = 8.0;
    seeded_ = true;
}

void SVD::step(const double Bt[GN1][GN3], double lambda,
               double St_out[GK], double proj_out[GK], double &qnorm_out) {
    if (!seeded_) seed();

    double Ut_f[GN1][GK][GN3], Bt_f[GN1][GN3];

    // Stage 1-2: 2-point butterfly
    for (int i = 0; i < GN1; i++) {
        for (int j = 0; j < GK; j++) {
            Ut_f[i][j][0] = Ut_[i][j][0] + Ut_[i][j][1];
            Ut_f[i][j][1] = Ut_[i][j][0] - Ut_[i][j][1];
        }
        Bt_f[i][0] = Bt[i][0] + Bt[i][1];
        Bt_f[i][1] = Bt[i][0] - Bt[i][1];
    }

    // Stage 3: projection
    double P[GK][GN3];
    for (int s = 0; s < GN3; s++)
        for (int j = 0; j < GK; j++) {
            double a = 0.0;
            for (int i = 0; i < GN1; i++) a += Ut_f[i][j][s] * Bt_f[i][s];
            P[j][s] = a;
        }

    // Stage 4: residual
    double R[GN1][GN3];
    for (int s = 0; s < GN3; s++)
        for (int i = 0; i < GN1; i++) {
            double a = Bt_f[i][s];
            for (int j = 0; j < GK; j++) a -= Ut_f[i][j][s] * P[j][s];
            R[i][s] = a;
        }

    // Stage 5: normalisation — exact inverse sqrt, no Newton-Raphson
    double Q[GN1][GN3];
    double last_qn = 0.0;
    for (int s = 0; s < GN3; s++) {
        double n2 = 0.0;
        for (int i = 0; i < GN1; i++) n2 += R[i][s] * R[i][s];
        double inv = (n2 < 1e-6) ? 0.0 : 1.0 / std::sqrt(n2);
        for (int i = 0; i < GN1; i++) Q[i][s] = R[i][s] * inv;
        last_qn = n2 * inv * inv;
    }
    qnorm_out = last_qn;

    // Stage 6: singular value update with forgetting factor
    for (int j = 0; j < GK; j++) {
        double m = 0.0;
        for (int s = 0; s < GN3; s++) m += P[j][s] * P[j][s];
        proj_out[j] = m;
        St_[j] = lambda * St_[j] + m;
        St_out[j] = St_[j];
    }

    // Window shift
    for (int i = 0; i < GN1; i++) {
        for (int j = 0; j < GK - 1; j++)
            for (int s = 0; s < GN3; s++) Ut_[i][j][s] = Ut_[i][j + 1][s];
        for (int s = 0; s < GN3; s++) Ut_[i][GK - 1][s] = Q[i][s];
    }
}

// ─── Applications ────────────────────────────────────────────────────────────
int momentum(const double St[GK], const double pm[GK],
             double ema_in, double alpha, double dev_hi, double dev_lo,
             double energy_min, double &ema_out) {
    double S1 = St[0];
    ema_out = alpha * S1 + (1.0 - alpha) * ema_in;
    double energy = 0.0;
    for (int j = 0; j < GK; j++) energy += pm[j];
    double dev = S1 - ema_out;
    if (dev > dev_hi && energy > energy_min) return 1;
    if (dev < dev_lo) return 2;
    return 0;
}

int risk(const double pm[GK], const double St[GK], double thresh, double eps,
         double cov[GK * GK], double &rho_out) {
    double trace = 0.0;
    for (int i = 0; i < GK; i++)
        for (int j = 0; j < GK; j++) {
            double d = St[i] * St[j];
            if (std::fabs(d) < eps) d = eps;
            double c = (pm[i] * pm[j]) / d;
            cov[i * GK + j] = c;
            if (i == j) trace += c;
        }
    rho_out = trace;
    if (trace > thresh) return 2;
    if (trace < 0.3 * thresh) return 1;
    return 0;
}

int regime(const double St[GK], double trend_thresh, double vol_thresh,
           double eps, int &regime_out, bool &valid_out) {
    valid_out = (St[1] > eps) && (St[2] > eps);
    double d1 = (St[1] > eps) ? St[1] : eps;
    double d2 = (St[2] > eps) ? St[2] : eps;
    double r12 = St[0] / d1;
    double r23 = St[1] / d2;
    double tot = 0.0;
    for (int j = 0; j < GK; j++) tot += St[j];

    if (r12 > trend_thresh) { regime_out = 0; return 1; }
    if (tot > vol_thresh)   { regime_out = 2; return 0; }
    regime_out = 1;
    return (r23 < 1.0) ? 2 : 1;
}

int vote(int mom, int rsk, int rgm) {
    int b = (mom == 1) + (rsk == 1) + (rgm == 1);
    int s = (mom == 2) + (rsk == 2) + (rgm == 2);
    if (b >= 2) return 1;
    if (s >= 2) return 2;
    return 0;
}

} // namespace golden
