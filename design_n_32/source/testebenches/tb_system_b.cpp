// ─────────────────────────────────────────────────────────────────────────────
// tb_system_b.cpp — Option B end-to-end C-sim
//
// Drives the REAL Option-B kernels through hls::stream exactly as the xclbin
// wires them (feeder -> lob -> svd -> [broadcast] -> apps -> sink), and runs the
// double-precision golden model in parallel on the same input. Because the
// pipeline decides only on a two-sided book, the golden pass is run FIRST to
// count the decided ticks M; that M is what every downstream kernel is sized to
// (the host does the same at run time).
//
// Pass criteria mirror the Offer-2 system C-sim: NOT per-tick equality (the SVD
// is chaotic — see README §4.5), but no saturation + S1 distribution match.
//
// Build/run (inside Vitis HLS, per run_hls.tcl, any set_top):
//   csim_design -argv "<path>/ticks.itch"
// or standalone host g++:
//   g++ -std=c++17 -I. -Itb -I../host tb/tb_system_b.cpp \
//       lob_filter.cpp inctsvd_top.cpp quant_apps.cpp upd_inject.cpp \
//       decision_sink.cpp tb/golden_model.cpp ../host/itch_parser.cpp \
//       -I$XILINX_HLS/include -o tb_b && ./tb_b ticks.itch
// ─────────────────────────────────────────────────────────────────────────────
#include "../source/qf_types.h"
#include "../source/upd_inject.h"
#include "../source/lob_filter.h"
#include "../source/inctsvd_top.h"
#include "../source/quant_apps.h"
#include "../source/decision_sink.h"
#include "golden_model.h"
#include "../source/../host/itch_parser.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>

struct Params {
    double lambda      = 0.90;
    double threshold   = 29.7557;
    double alpha       = 0.35;
    double dev_hi      = 0.4179;
    double dev_lo      = -0.4709;
    double energy_min  = 1.0;
    double risk_thresh = 5.9026;
    double eps         = 0.001;
    double trend_thr   = 1.0087;
    double vol_thr     = 136.3473;
    double price_scale = 2.0;
    double qty_scale   = 0.001;
    double price_ref   = 0.0;
    int    unit_norm   = 1;
    int    ref_mode    = 1;      // risk reference: 1 = slow EMA (README §4.2)
    double lambda_slow = 0.999;
};

static const char* SIGNAME(int s) { return s == 1 ? "BUY" : s == 2 ? "SELL" : "HOLD"; }

int main(int argc, char** argv) {
    const char* itch_path = (argc > 1) ? argv[1] : "ticks.itch";
    Params p;

    // ── Collect updates from the ITCH file (price quantised to price_t for BOTH
    //    the hardware pack and the golden book, so the books never disagree). ──
    ITCHParser parser;
    std::vector<::LOBUpdate>    hw_upd;
    std::vector<golden::Update> gm_upd;

    auto capture = [&](const LOBUpdate_host& u) {
        ::LOBUpdate h;
        h.msg_type = u.msg_type; h.side = u.side;
        h.price = (price_t)u.price; h.qty = (qty_t)u.qty; h.ref_num = (ref_t)u.ref_num;
        hw_upd.push_back(h);
        golden::Update g;
        g.msg_type = u.msg_type; g.side = u.side;
        g.price = (double)h.price; g.qty = (uint32_t)h.qty; g.ref = u.ref_num;
        gm_upd.push_back(g);
    };

    try {
        parser.parse_file(itch_path, capture);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what()
                  << "\nGenerate one:  python3 tools/gen_itch.py --out ticks.itch\n";
        return 2;
    }
    if (hw_upd.empty()) { std::cerr << "ERROR: no LOB updates decoded\n"; return 2; }

    const int N = (int)hw_upd.size();
    p.price_ref   = parser.have_first ? parser.first_price : 0.0;
    p.price_ref   = (double)(fixed_t)p.price_ref;
    p.price_scale = (double)(fixed_t)p.price_scale;
    p.qty_scale   = (double)(fixed_t)p.qty_scale;
    p.lambda      = (double)(fixed_t)p.lambda;

    std::cout << "=== Option B system C-sim: " << itch_path << " ===\n"
              << "LOB updates decoded : " << N << "\n"
              << "price_ref           : " << p.price_ref << "\n\n";

    // ─────────────────────────────────────────────────────────────────────────
    // GOLDEN PASS — decide on ready ticks only; record the votes + S1 series.
    // ─────────────────────────────────────────────────────────────────────────
    golden::Book gbook; gbook.clear();
    golden::SVD  gsvd;   gsvd.seed();
    double gema = 0.0, gmbar[golden::GK] = {0,0,0,0};
    bool   gmbar_init = true;
    std::vector<int>    gvotes;
    std::vector<double> gm_s1;

    for (int k = 0; k < N; k++) {
        gbook.apply(gm_upd[k]);
        if (!gbook.ready()) continue;

        double gBt[golden::GN1][golden::GN3];
        gbook.tick_matrix(p.price_ref, p.price_scale, p.qty_scale, gBt);
        if (p.unit_norm) {
            double n2 = 0.0;
            for (int i = 0; i < golden::GN1; i++)
                for (int s = 0; s < golden::GN3; s++) n2 += gBt[i][s] * gBt[i][s];
            double g = (n2 > 1e-9) ? 1.0 / std::sqrt(n2) : 0.0;
            for (int i = 0; i < golden::GN1; i++)
                for (int s = 0; s < golden::GN3; s++) gBt[i][s] *= g;
        }

        double gSt[golden::GK], gPm[golden::GK], gqn;
        gsvd.step(gBt, p.lambda, gSt, gPm, gqn);

        double gema_out;
        int gmom = golden::momentum(gSt, gPm, gema, p.alpha, p.dev_hi, p.dev_lo,
                                    p.energy_min, gema_out);
        gema = gema_out;

        double gref[golden::GK], gcov[golden::GK * golden::GK], grho;
        if (p.ref_mode) {
            if (gmbar_init) { for (int j = 0; j < golden::GK; j++) gmbar[j] = gPm[j]; gmbar_init = false; }
            for (int j = 0; j < golden::GK; j++) gref[j] = gmbar[j];
        } else {
            for (int j = 0; j < golden::GK; j++) gref[j] = gSt[j];
        }
        int grsk = golden::risk(gPm, gref, p.risk_thresh, p.eps, gcov, grho);
        if (p.ref_mode)
            for (int j = 0; j < golden::GK; j++)
                gmbar[j] = p.lambda_slow * gmbar[j] + (1.0 - p.lambda_slow) * gPm[j];

        int greg; bool gvalid;
        int grgm = golden::regime(gSt, p.trend_thr, p.vol_thr, p.eps, greg, gvalid);

        gvotes.push_back(golden::vote(gmom, grsk, grgm));
        gm_s1.push_back(gSt[0]);
    }

    const int M = (int)gvotes.size();
    std::cout << "Decided (two-sided) ticks M : " << M
              << "   (skipped one-sided: " << (N - M) << ")\n\n";
    if (M == 0) { std::cerr << "ERROR: no two-sided ticks — nothing to decide\n"; return 2; }

    // ─────────────────────────────────────────────────────────────────────────
    // KERNEL PASS — feeder -> lob -> svd -> [broadcast] -> apps -> sink.
    // Sequential calls; hls::stream is unbounded in csim so each stage fully
    // fills before the next drains it. lob is sized to N, the rest to M.
    // ─────────────────────────────────────────────────────────────────────────
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
    hls::stream<sv_axi_t>  s_st_mom("s_st_mom"), s_st_rsk("s_st_rsk"), s_st_rgm("s_st_rgm");
    hls::stream<sv_axi_t>  s_pm_mom("s_pm_mom"), s_pm_rsk("s_pm_rsk");
    hls::stream<res_axi_t> s_mom("s_mom"), s_rsk("s_rsk"), s_rgm("s_rgm");

    upd_inject(updates.data(), N, s_upd);

    lob_filter(s_upd, s_bt, N,
               (fixed_t)p.price_ref, (fixed_t)p.price_scale,
               (fixed_t)p.qty_scale, (ap_uint<1>)p.unit_norm);

    // lob should have emitted exactly M beats.
    if ((int)s_bt.size() != M)
        std::cout << "WARN: lob emitted " << s_bt.size() << " beats, golden M=" << M
                  << " (book-model mismatch — investigate before trusting agreement)\n";

    inctsvd_top(s_bt, s_st, s_pm, M, (fixed_t)p.lambda);

    // Broadcasters (modelled: same beat into each consumer stream).
    for (int i = 0; i < M; i++) {
        sv_axi_t st = s_st.read(), pm = s_pm.read();
        s_st_mom.write(st); s_st_rsk.write(st); s_st_rgm.write(st);
        s_pm_mom.write(pm); s_pm_rsk.write(pm);
    }

    momentum_signal(s_st_mom, s_pm_mom, M,
                    (fixed_t)p.alpha, (fixed_t)p.dev_hi, (fixed_t)p.dev_lo,
                    (fixed_t)p.energy_min, s_mom);
    risk_matrix(s_pm_rsk, s_st_rsk, M,
                (fixed_t)p.risk_thresh, (fixed_t)p.eps,
                (ap_uint<1>)p.ref_mode, (fixed_t)p.lambda_slow, s_rsk);
    regime_classifier(s_st_rgm, M,
                      (fixed_t)p.trend_thr, (fixed_t)p.vol_thr, (fixed_t)p.eps, s_rgm);

    std::vector<dec_rec_t> decisions(M);
    decision_sink(s_mom, s_rsk, s_rgm, decisions.data(), M);

    // ─────────────────────────────────────────────────────────────────────────
    // COMPARE
    // ─────────────────────────────────────────────────────────────────────────
    int match = 0;
    std::vector<double> hw_s1(M);
    double hw_max_abs = 0.0;
    for (int i = 0; i < M; i++) {
        if ((int)decisions[i].final_sig == gvotes[i]) match++;
        hw_s1[i] = decisions[i].s1 / 4096.0;
        hw_max_abs = std::max(hw_max_abs, std::fabs(hw_s1[i]));
        if (i < 8)
            std::cout << "tick " << std::setw(3) << i
                      << " | S1 " << std::setw(9) << hw_s1[i]
                      << " | rho " << std::setw(9) << decisions[i].rho / 4096.0
                      << " | " << SIGNAME((int)decisions[i].final_sig)
                      << " (gm " << SIGNAME(gvotes[i]) << ")\n";
    }
    double agree = 100.0 * match / M;

    std::cout << "\nPer-tick signal agreement : " << match << "/" << M
              << "  (" << std::fixed << std::setprecision(1) << agree
              << "%  — informational, not a gate; §4.5)\n" << std::defaultfloat;

    bool pass = true;

    // Gate 1: no saturation.
    if (hw_max_abs > 0.90 * 2048.0) {
        std::cout << "FAIL: |S1| reached " << hw_max_abs << " (ap_fixed<24,12> ceiling 2048)\n";
        pass = false;
    } else {
        std::cout << "PASS: no saturation (max |S1| = " << hw_max_abs << ")\n";
    }

    // Gate 2: S1 distribution match within 25%.
    std::vector<double> a = hw_s1, b = gm_s1;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    double hw_med = a[a.size()/2],       gm_med = b[b.size()/2];
    double hw_p95 = a[a.size()*95/100],  gm_p95 = b[b.size()*95/100];
    double med_err = gm_med > 1e-9 ? std::fabs(hw_med - gm_med) / gm_med : 0.0;
    double p95_err = gm_p95 > 1e-9 ? std::fabs(hw_p95 - gm_p95) / gm_p95 : 0.0;
    std::cout << std::fixed << std::setprecision(4)
              << "S1 distribution           : hw median " << hw_med << " vs golden " << gm_med
              << "  (" << med_err*100 << "% apart)\n"
              << "                            hw p95    " << hw_p95 << " vs golden " << gm_p95
              << "  (" << p95_err*100 << "% apart)\n" << std::defaultfloat;
    if (med_err > 0.25 || p95_err > 0.25) {
        std::cout << "FAIL: S1 distribution differs by more than 25%\n";
        pass = false;
    } else {
        std::cout << "PASS: S1 distribution matches within 25%\n";
    }

    std::cout << (pass ? "\n*** OPTION B CSIM PASSED ***\n"
                       : "\n*** OPTION B CSIM FAILED ***\n");
    return pass ? 0 : 1;
}
