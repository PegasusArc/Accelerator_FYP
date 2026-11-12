// ─────────────────────────────────────────────────────────────────────────────
// fx_sweep.cpp — does changing the fixed-point format actually help?
//
// Runs the whole kernel chain in C-simulation under whatever ap_fixed format
// qf_types.h was compiled with, against real market data, and reports:
//
//     agreement with the double-precision reference
//     largest |S1| seen, against that format's ceiling
//     how many values saturated
//     how far the output distribution sits from the reference
//
// No hardware, no bitstream. run_fx_sweep.sh drives this across several formats
// and tabulates the results.
//
// The point of the exercise: within a fixed word width, integer bits and
// fractional bits trade off one-for-one. More range means coarser steps. Since
// precision loss is what makes a fixed-point path diverge from a double
// reference, widening the integer side to buy range you are not using should
// make agreement WORSE, and narrowing it should make agreement BETTER — until
// the range gets tight enough to start saturating. This measures where that
// balance actually sits instead of arguing about it.
//
// Takes MANY symbols in one invocation. That matters: the market data file is
// 799 MB, and re-reading it once per symbol per format turns a ten-minute sweep
// into an hour. Here it is read once, then each symbol is parsed from the
// in-memory buffer.
//
// Build (see run_fx_sweep.sh — it patches a temporary copy of qf_types.h):
//   g++ -std=c++17 -O2 -I. -I<tb> -I<host> -I<hls-include> \
//       fx_sweep.cpp lob_filter.cpp inctsvd_top.cpp quant_apps.cpp \
//       upd_inject.cpp decision_sink.cpp golden_model.cpp itch_parser.cpp \
//       -o fx_sweep
// Run:
//   ./fx_sweep <itch-file> <limit> <max-MB> <sym1> [sym2 ...]
// ─────────────────────────────────────────────────────────────────────────────

#include "qf_types.h"
#include "upd_inject.h"
#include "lob_filter.h"
#include "inctsvd_top.h"
#include "quant_apps.h"
#include "decision_sink.h"
#include "golden_model.h"
#include "itch_parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>

static void run_one(const uint8_t* filebuf, size_t flen,
                    const std::string& symbol, long limit);

// Fractional bits, and therefore the scale the packed integers decode by.
// fx_pack sign-extends the raw fixed_t bits into 32, so the divisor is
// 2^(total bits - integer bits).
#ifndef FIXED_W
#define FIXED_W 24
#endif
#ifndef FIXED_I
#define FIXED_I 12
#endif
static const int    FX_FRAC  = FIXED_W - FIXED_I;
static const double FX_SCALE = (double)(1u << FX_FRAC);
static const double FX_MAX   = (double)(1u << (FIXED_I - 1));   // +/- ceiling

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: " << argv[0]
                  << " <itch-file> <limit> <max-MB> <sym1> [sym2 ...]\n";
        return 1;
    }
    const std::string itch  = argv[1];
    const long        limit = atol(argv[2]);
    const size_t      maxmb = (size_t)atol(argv[3]);
    std::vector<std::string> symbols;
    for (int i = 4; i < argc; i++) symbols.push_back(argv[i]);

    // Read the market data once. Capping the read keeps the sweep quick: the
    // busiest instruments reach their message quota well inside the first few
    // hundred megabytes.
    std::vector<uint8_t> filebuf;
    {
        std::ifstream f(itch, std::ios::binary);
        if (!f) { std::cerr << "cannot open " << itch << "\n"; return 2; }
        filebuf.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
    }
    size_t flen = filebuf.size();
    if (maxmb && maxmb * 1024 * 1024 < flen) flen = maxmb * 1024 * 1024;

    for (const std::string& symbol : symbols)
        run_one(filebuf.data(), flen, symbol, limit);
    return 0;
}

// One symbol, start to finish: parse, run the reference model, run the kernel
// chain, print a single RESULT line for the sweep script to collect.
static void run_one(const uint8_t* filebuf, size_t flen,
                    const std::string& symbol, long limit) {
    // ── Configuration: identical to the delivered host ──────────────────────
    const double LAMBDA = 0.90, PSCALE = 2.0, QSCALE = 0.001;
    const double ALPHA = 0.35, DEV_HI = 0.4179, DEV_LO = -0.4709, EMIN = 1.0;
    const double RTHR = 5.9026, EPS = 0.001, LSLOW = 0.999;
    const double TTHR = 1.0087, VTHR = 136.3473;
    const int    UNORM = 1, RMODE = 1;

    // ── Parse ─────────────────────────────────────────────────────────────
    ITCHParser parser;
    parser.set_symbol(symbol);

    std::vector<::LOBUpdate>    hw_upd;
    std::vector<golden::Update> gm_upd;

    // Quantise the price to price_t once, and give BOTH sides that same value,
    // so the reference is not handed an input the hardware never saw. Without
    // this the chaotic SVD walks onto a different trajectory and the comparison
    // measures nothing.
    auto capture = [&](const LOBUpdate_host& u) {
        if ((long)hw_upd.size() >= limit) return;
        ::LOBUpdate h;
        h.msg_type = u.msg_type; h.side = u.side;
        h.price = (price_t)u.price; h.qty = (qty_t)u.qty; h.ref_num = (ref_t)u.ref_num;
        hw_upd.push_back(h);

        golden::Update g;
        g.msg_type = u.msg_type; g.side = u.side;
        g.price = (double)h.price; g.qty = (uint32_t)h.qty; g.ref = u.ref_num;
        gm_upd.push_back(g);
    };

    parser.parse(filebuf, flen, capture);
    if (hw_upd.empty()) {
        std::cout << "RESULT skipped=1 sym=" << symbol << "\n";
        return;
    }

    const int N = (int)hw_upd.size();

    // Quantise every parameter the same way the hardware receives it.
    auto Q = [](double v) { return (double)(fixed_t)v; };
    double price_ref = parser.have_first ? Q(parser.first_price) : 0.0;

    // ── Reference pass: count decided ticks and record votes + S1 ──────────
    golden::Book gb; golden::SVD gs; gb.clear(); gs.seed();
    double gema = 0.0, gmbar[golden::GK] = {0,0,0,0};
    bool   gminit = true;
    std::vector<int>    gvotes;
    std::vector<double> gm_s1;

    for (int k = 0; k < N; k++) {
        gb.apply(gm_upd[k]);
        if (!gb.ready()) continue;

        double Bt[golden::GN1][golden::GN3];
        gb.tick_matrix(price_ref, Q(PSCALE), Q(QSCALE), Bt);
        if (UNORM) {
            double n2 = 0;
            for (int a = 0; a < golden::GN1; a++)
                for (int c = 0; c < golden::GN3; c++) n2 += Bt[a][c]*Bt[a][c];
            double g = n2 > 1e-9 ? 1.0/std::sqrt(n2) : 0.0;
            for (int a = 0; a < golden::GN1; a++)
                for (int c = 0; c < golden::GN3; c++) Bt[a][c] *= g;
        }
        double St[golden::GK], Pm[golden::GK], qn;
        gs.step(Bt, Q(LAMBDA), St, Pm, qn);

        double gema_out;
        int gmom = golden::momentum(St, Pm, gema, Q(ALPHA), Q(DEV_HI), Q(DEV_LO),
                                    Q(EMIN), gema_out);
        gema = gema_out;

        double ref[golden::GK], cov[golden::GK*golden::GK], rho;
        if (gminit) { for (int j=0;j<golden::GK;j++) gmbar[j]=Pm[j]; gminit=false; }
        for (int j=0;j<golden::GK;j++) ref[j] = RMODE ? gmbar[j] : St[j];
        int grsk = golden::risk(Pm, ref, Q(RTHR), Q(EPS), cov, rho);
        for (int j=0;j<golden::GK;j++)
            gmbar[j] = Q(LSLOW)*gmbar[j] + (1-Q(LSLOW))*Pm[j];

        int R; bool v;
        int grgm = golden::regime(St, Q(TTHR), Q(VTHR), Q(EPS), R, v);

        gvotes.push_back(golden::vote(gmom, grsk, grgm));
        gm_s1.push_back(St[0]);
    }

    const int M = (int)gvotes.size();

    // A symbol with too few decided ticks tells us nothing about the format.
    // The incremental SVD needs a few thousand ticks to converge from its seed
    // (measured between 776 and 4,682), so below that every sample sits inside
    // the warm-up transient and the distribution comparison is meaningless -
    // it will report enormous errors regardless of which format is used.
    // Including such symbols would let an illiquid instrument dictate the
    // recommendation, which is exactly what happened before this guard existed.
    const int MIN_TICKS = 5000;
    if (M < MIN_TICKS) {
        std::cout << "RESULT skipped=1 ticks=" << M << " sym=" << symbol << "\n";
        return;
    }

    // ── Kernel chain, in whatever format this build was compiled with ──────
    std::vector<upd_rec_t> updates(N);
    for (int k = 0; k < N; k++) {
        ap_uint<32> w0 = 0;
        w0.range(7, 0)  = hw_upd[k].msg_type;
        w0.range(15, 8) = hw_upd[k].side;
        updates[k].w0 = (uint32_t)w0;
        updates[k].w1 = (uint32_t)(ap_uint<32>)hw_upd[k].price.range(31, 0);
        updates[k].w2 = (uint32_t)hw_upd[k].qty;
        updates[k].w3 = (uint32_t)(ap_uint<32>)hw_upd[k].ref_num.range(31, 0);
        updates[k].w4 = (uint32_t)(ap_uint<32>)hw_upd[k].ref_num.range(63, 32);
        updates[k].pad0 = updates[k].pad1 = updates[k].pad2 = 0;
    }

    hls::stream<upd_axi_t> s_upd("s_upd");
    hls::stream<bt_axi_t>  s_bt("s_bt");
    hls::stream<sv_axi_t>  s_st("s_st"), s_pm("s_pm");
    hls::stream<sv_axi_t>  s_st_mom, s_st_rsk, s_st_rgm, s_pm_mom, s_pm_rsk;
    hls::stream<res_axi_t> s_mom("s_mom"), s_rsk("s_rsk"), s_rgm("s_rgm");

    upd_inject(updates.data(), N, s_upd);
    lob_filter(s_upd, s_bt, N, (fixed_t)price_ref, (fixed_t)PSCALE,
               (fixed_t)QSCALE, (ap_uint<1>)UNORM);

    int emitted = (int)s_bt.size();
    inctsvd_top(s_bt, s_st, s_pm, emitted, (fixed_t)LAMBDA);

    for (int i = 0; i < emitted; i++) {
        sv_axi_t st = s_st.read(), pm = s_pm.read();
        s_st_mom.write(st); s_st_rsk.write(st); s_st_rgm.write(st);
        s_pm_mom.write(pm); s_pm_rsk.write(pm);
    }

    momentum_signal(s_st_mom, s_pm_mom, emitted, (fixed_t)ALPHA, (fixed_t)DEV_HI,
                    (fixed_t)DEV_LO, (fixed_t)EMIN, s_mom);
    risk_matrix(s_pm_rsk, s_st_rsk, emitted, (fixed_t)RTHR, (fixed_t)EPS,
                (ap_uint<1>)RMODE, (fixed_t)LSLOW, s_rsk);
    regime_classifier(s_st_rgm, emitted, (fixed_t)TTHR, (fixed_t)VTHR,
                      (fixed_t)EPS, s_rgm);

    std::vector<dec_rec_t> dec(emitted);
    decision_sink(s_mom, s_rsk, s_rgm, dec.data(), emitted);

    // ── Compare ────────────────────────────────────────────────────────────
    const int CMP = std::min(M, emitted);
    long match = 0, sat_s1 = 0, sat_rho = 0;
    double max_s1 = 0.0;
    std::vector<double> hw_s1(CMP);

    for (int i = 0; i < CMP; i++) {
        if ((int)dec[i].final_sig == gvotes[i]) match++;
        hw_s1[i] = dec[i].s1 / FX_SCALE;
        double a = std::fabs(hw_s1[i]);
        max_s1 = std::max(max_s1, a);
        if (a >= FX_MAX * 0.999) sat_s1++;
        if (std::fabs(dec[i].rho / FX_SCALE) >= FX_MAX * 0.999) sat_rho++;
    }

    std::vector<double> a = hw_s1, b(gm_s1.begin(), gm_s1.begin() + CMP);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    double hw_med = a[a.size()/2],      gm_med = b[b.size()/2];
    double hw_p95 = a[a.size()*95/100], gm_p95 = b[b.size()*95/100];
    double med_err = gm_med > 1e-9 ? std::fabs(hw_med-gm_med)/gm_med*100.0 : 0.0;
    double p95_err = gm_p95 > 1e-9 ? std::fabs(hw_p95-gm_p95)/gm_p95*100.0 : 0.0;

    // Single machine-readable line for the sweep script to collect.
    std::cout << "RESULT"
              << " W=" << FIXED_W
              << " I=" << FIXED_I
              << " frac=" << FX_FRAC
              << " range=" << FX_MAX
              << " step=" << std::scientific << std::setprecision(2) << (1.0/FX_SCALE)
              << std::fixed << std::setprecision(2)
              << " ticks=" << CMP
              << " agree=" << (100.0*match/CMP)
              << " maxS1=" << max_s1
              << " satS1=" << sat_s1
              << " satRho=" << sat_rho
              << std::setprecision(4)
              << " medErr=" << med_err
              << " p95Err=" << p95_err
              << " sym=" << symbol
              << "\n";
}
