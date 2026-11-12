// ─────────────────────────────────────────────────────────────────────────────
// golden_model.h — double-precision reference implementation
//
// Purpose: this is the artifact the Offer 1 doc §5.3 marks as
// "Measurement study scheduled under Offer 2". It mirrors the fixed-point
// pipeline stage for stage in IEEE double, with NO Newton-Raphson (it uses
// 1/std::sqrt directly) and NO quantisation, so the difference against the
// hardware C-sim isolates exactly two error sources:
//   (a) ap_fixed<24,12> quantisation
//   (b) Newton-Raphson residual after the extended ladder
//
// It is host-only C++ — never synthesised, never compiled into the bitstream.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <vector>

namespace golden {

// NOTE: qf_types.h (the synthesisable side) defines N1/K/N3 as preprocessor
// MACROS. When tb_system.cpp includes qf_types.h before this header, the
// preprocessor — which ignores namespaces — would rewrite `golden::N1` into
// `golden::10` and break compilation. So the golden model's own dimensions are
// named GN1/GK/GN3, which cannot collide with those macros. Values are
// identical to N1/K/N3 by construction; they are asserted equal in tb_system.
constexpr int GN1 = 10;
constexpr int GK  = 4;
constexpr int GN3 = 2;

// ─── Order book reference ────────────────────────────────────────────────────
struct Update {
    uint8_t  msg_type;
    uint8_t  side;
    double   price;
    uint32_t qty;
    uint64_t ref;
};

class Book {
public:
    void clear();
    void apply(const Update &u);
    // Emits the same centred/scaled 10x2 matrix as lob_filter
    void tick_matrix(double price_ref, double price_scale, double qty_scale,
                     double Bt[GN1][GN3]) const;
    bool ready() const;
private:
    struct Ent { double price; uint32_t qty; uint64_t ref; bool valid; };
    Ent bid_[5]{}, ask_[5]{};
};

// ─── SVD reference ───────────────────────────────────────────────────────────
class SVD {
public:
    void seed();                              // identity basis, St = 8.0
    // One tick. Returns St and projection magnitudes.
    void step(const double Bt[GN1][GN3], double lambda,
              double St_out[GK], double proj_out[GK], double &qnorm_out);
private:
    double Ut_[GN1][GK][GN3]{};
    double St_[GK]{};
    bool   seeded_ = false;
};

// ─── Application references ──────────────────────────────────────────────────
int momentum(const double St[GK], const double pm[GK],
             double ema_in, double alpha, double dev_hi, double dev_lo,
             double energy_min, double &ema_out);

int risk(const double pm[GK], const double St[GK], double thresh, double eps,
         double cov[GK * GK], double &rho_out);

int regime(const double St[GK], double trend_thresh, double vol_thresh,
           double eps, int &regime_out, bool &valid_out);

int vote(int mom, int rsk, int rgm);

} // namespace golden
