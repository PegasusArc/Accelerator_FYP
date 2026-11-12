// ─────────────────────────────────────────────────────────────────────────────
// golden_check.h — run the double-precision reference over the SAME interleaved
// event stream the card just processed, and compare.
//
// Why this exists. The --verify check compares one hardware run against another
// hardware run: instrument 0 alone, against instrument 0 taken out of the
// interleaved batch. That test assumes the solo run is correct. On a bitstream
// built for NASSET instruments it is not. Feeding a single instrument means
// every tick carries the same id, and every stateful kernel in the design is
// compiled with `dependence ... inter distance=NASSET true`, which promises the
// tool that ticks closer than NASSET apart belong to different instruments.
// Break that promise and the hardware does not stall - it reads state that has
// not been written yet. So the solo run is the LEAST trustworthy thing in the
// system, and comparing against it tells us nothing about the interleaved one.
//
// The per-tick sequence below is copied from tb_system_b.cpp, which is the
// driver that produced the validated Offer 2 numbers. Two steps in it are easy
// to miss and both are fatal if dropped:
//
//   1. The unit normalisation is NOT inside tick_matrix(). lob_filter applies
//      it after building the matrix, under the UNORM register. Leave it out and
//      the reference SVD is fed matrices orders of magnitude too large and
//      diverges within a few hundred ticks.
//   2. golden::risk() takes a slow reference vector, not St. With RMODE=1 that
//      is mbar, seeded from the first projection and then decayed by LSLOW
//      after every decision. Passing St instead makes every risk signal wrong.
//
// The only change from the testbench is that every piece of state here is held
// per instrument, which is the whole point: the reference has no dependence
// hazard, so it shows what the hardware should have produced.
//
// It will NOT agree bit for bit, and it is not supposed to. The reference is
// IEEE double with no quantisation and a real 1/sqrt where the hardware runs
// ap_fixed<24,12> through a Newton-Raphson ladder. What we are looking for is
// whether the hardware TRACKS the reference or has come loose from it.
//
// Include this after the constant block and the Batch struct in host_rr.cpp.
//
// ── Thesis export (added for the write-up; off unless asked for) ─────────────
//
//   THESIS_EXPORT=~/quant_accel/thesis_out ~/quant_accel/rr --limit 5000
//
// With THESIS_EXPORT set to a directory, and only then:
//   * the reference loop is timed, THESIS_REPS passes (default 3), for the C++
//     row of the software comparison table. The timed pass is the unchanged
//     reference loop; nothing extra runs inside it.
//   * the books are replayed once more, after the timing, to record for every
//     decision its event number within the instrument and the best bid and
//     ask (the mid-price of the decision-timeline figure).
//   * the interleaved comparison writes, into that directory,
//       decisions.csv  one row per decision, hardware and reference side by
//                      side, sigma_1 and rho as the raw q12 integers
//       summary.txt    counts, the reference timing and the pooled gate values
//     which thesis_data.py turns into the figures and tables.
// Unset, none of this runs and every line printed is what it was before.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "golden_model.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

// Per-instrument reference state. The whole point is that these are separate.
struct GoldState {
    golden::Book book;
    golden::SVD  svd;
    double       ema = 0.0;
    double       mbar[golden::GK] = {0.0, 0.0, 0.0, 0.0};
    bool         mbar_init = true;
};

// ── thesis export state ─────────────────────────────────────────────────────
namespace thesis_export {

// One entry per reference decision, in the same order as run_golden's output.
struct Side { int32_t slot; int32_t event; double bid; double ask; };

static std::vector<Side>   g_side;
static std::vector<double> g_ms;          // reference loop time of each timed pass
static size_t              g_events = 0;  // updates in the batch that was timed

static const char* dir()
{
    const char* d = std::getenv("THESIS_EXPORT");
    return (d && *d) ? d : nullptr;
}

static int reps()
{
    const char* r = std::getenv("THESIS_REPS");
    const int n = r ? std::atoi(r) : 3;
    return n < 1 ? 1 : (n > 20 ? 20 : n);
}

} // namespace thesis_export

// Walk the interleaved stream in order, emitting one record per decided tick,
// in the same order the card emits them.
static std::vector<dec_rec_t> golden_pass(const Batch& b, int nasset)
{
    std::vector<GoldState> st((size_t)nasset);
    for (auto& s : st) { s.book.clear(); s.svd.seed(); }

    std::vector<dec_rec_t> out;
    out.reserve(b.updates.size());

    for (const auto& r : b.updates) {
        const int a = (nasset == 1) ? 0 : (int)(r.asset % (uint32_t)nasset);
        GoldState& g = st[(size_t)a];

        golden::Update u;
        u.msg_type = (uint8_t)( r.w0        & 0xFF);
        u.side     = (uint8_t)((r.w0 >> 8)  & 0xFF);
        u.price    = (double)(int32_t)r.w1 / 65536.0;   // price_t raw Q16.16
        u.qty      = r.w2;
        u.ref      = ((uint64_t)r.w4 << 32) | (uint64_t)r.w3;

        g.book.apply(u);
        if (!g.book.ready()) continue;                  // same gate as lob_filter

        double Bt[golden::GN1][golden::GN3];
        g.book.tick_matrix(b.price_ref[(size_t)a], PSCALE, QSCALE, Bt);

        // lob_filter's normalisation block, exactly as tb_system_b.cpp does it.
        if (UNORM) {
            double n2 = 0.0;
            for (int i = 0; i < golden::GN1; i++)
                for (int s = 0; s < golden::GN3; s++) n2 += Bt[i][s] * Bt[i][s];
            const double gn = (n2 > 1e-9) ? 1.0 / std::sqrt(n2) : 0.0;
            for (int i = 0; i < golden::GN1; i++)
                for (int s = 0; s < golden::GN3; s++) Bt[i][s] *= gn;
        }

        double St[golden::GK], pm[golden::GK], qn = 0.0;
        g.svd.step(Bt, LAMBDA, St, pm, qn);

        double ema_out = g.ema;
        const int ms = golden::momentum(St, pm, g.ema, ALPHA,
                                        DEV_HI, DEV_LO, EMIN, ema_out);
        g.ema = ema_out;

        // RMODE selects what risk measures against. 1 = the slow reference.
        double ref[golden::GK], cov[golden::GK * golden::GK], rho = 0.0;
        if (RMODE) {
            if (g.mbar_init) {
                for (int j = 0; j < golden::GK; j++) g.mbar[j] = pm[j];
                g.mbar_init = false;
            }
            for (int j = 0; j < golden::GK; j++) ref[j] = g.mbar[j];
        } else {
            for (int j = 0; j < golden::GK; j++) ref[j] = St[j];
        }
        const int rs = golden::risk(pm, ref, RTHR, EPS, cov, rho);
        if (RMODE)
            for (int j = 0; j < golden::GK; j++)
                g.mbar[j] = LSLOW * g.mbar[j] + (1.0 - LSLOW) * pm[j];

        int  rg_code  = 0;
        bool rg_valid = false;
        const int gs = golden::regime(St, TTHR, VTHR, EPS, rg_code, rg_valid);

        dec_rec_t d{};
        d.ready     = 1;
        d.final_sig = (uint32_t)golden::vote(ms, rs, gs);
        d.mom_sig   = (uint32_t)ms;
        d.rsk_sig   = (uint32_t)rs;
        d.rgm_sig   = (uint32_t)gs;
        d.rgm_valid = rg_valid ? 1u : 0u;
        d.s1        = (int32_t)std::lround(St[0] * 4096.0);
        d.rho       = (int32_t)std::lround(rho   * 4096.0);
        d.r12       = 0;                                // not modelled; not compared
        d.asset     = a;
        out.push_back(d);
    }
    return out;
}

namespace thesis_export {

// Replay only the books, with the same gate as golden_pass, and record where
// each decision sits: its event number within the instrument (every update of
// that instrument counts, decided or not) and the best bid and ask. With a
// centring price of 0 and unit gains, tick_matrix() returns raw prices, and it
// lays the rows out as lob_filter does: row 0 the best bid, row 5 the best ask.
static void replay_books(const Batch& b, int nasset)
{
    std::vector<golden::Book> books((size_t)nasset);
    for (auto& k : books) k.clear();
    std::vector<int32_t> seen((size_t)nasset, 0);

    g_side.clear();
    g_side.reserve(b.updates.size());
    for (const auto& r : b.updates) {
        const int a = (nasset == 1) ? 0 : (int)(r.asset % (uint32_t)nasset);

        golden::Update u;
        u.msg_type = (uint8_t)( r.w0        & 0xFF);
        u.side     = (uint8_t)((r.w0 >> 8)  & 0xFF);
        u.price    = (double)(int32_t)r.w1 / 65536.0;
        u.qty      = r.w2;
        u.ref      = ((uint64_t)r.w4 << 32) | (uint64_t)r.w3;

        const int32_t ev = seen[(size_t)a]++;
        books[(size_t)a].apply(u);
        if (!books[(size_t)a].ready()) continue;

        double Bt[golden::GN1][golden::GN3];
        books[(size_t)a].tick_matrix(0.0, 1.0, 1.0, Bt);
        g_side.push_back(Side{a, ev, Bt[0][0], Bt[golden::GN1 / 2][0]});
    }
}

} // namespace thesis_export

static std::vector<dec_rec_t> run_golden(const Batch& b, int nasset)
{
    if (!thesis_export::dir()) return golden_pass(b, nasset);

    // Export mode. The timed pass is golden_pass exactly as above; the
    // bookkeeping for the figures happens afterwards, outside the clock.
    std::vector<dec_rec_t> out;
    thesis_export::g_ms.clear();
    const int n = thesis_export::reps();
    for (int k = 0; k < n; k++) {
        const auto t0 = std::chrono::steady_clock::now();
        out = golden_pass(b, nasset);
        const auto t1 = std::chrono::steady_clock::now();
        thesis_export::g_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    thesis_export::g_events = b.updates.size();
    thesis_export::replay_books(b, nasset);
    return out;
}

namespace thesis_export {

// Called from compare_to_golden for the interleaved run only.
static void write_files(const std::vector<dec_rec_t>& hw,
                        const std::vector<dec_rec_t>& gd,
                        double hw_med, double gm_med, double hw_p95, double gm_p95)
{
    const char* d = dir();
    if (!d) return;
    ::mkdir(d, 0755);                                   // fine if it exists

    std::printf("\n  --- thesis export -> %s ---\n", d);
    if (hw.size() != gd.size() || g_side.size() != gd.size()) {
        std::printf("    NOT WRITTEN: hardware %zu, reference %zu, book replay %zu decisions.\n"
                    "    The three must agree for the rows to line up.\n",
                    hw.size(), gd.size(), g_side.size());
        return;
    }

    const std::string p_csv = std::string(d) + "/decisions.csv";
    const std::string p_sum = std::string(d) + "/summary.txt";

    long slot_mismatch = 0;
    std::FILE* f = std::fopen(p_csv.c_str(), "w");
    if (!f) { std::printf("    cannot write %s\n", p_csv.c_str()); return; }
    std::fprintf(f, "i,slot,event,bid,ask,hw_dec,ref_dec,hw_s1_q12,ref_s1_q12,hw_rho_q12\n");
    for (size_t i = 0; i < hw.size(); i++) {
        const Side& s = g_side[i];
        if (hw[i].asset != s.slot || gd[i].asset != s.slot) slot_mismatch++;
        std::fprintf(f, "%zu,%d,%d,%.4f,%.4f,%u,%u,%d,%d,%d\n",
                     i, (int)hw[i].asset, (int)s.event, s.bid, s.ask,
                     (unsigned)hw[i].final_sig, (unsigned)gd[i].final_sig,
                     (int)hw[i].s1, (int)gd[i].s1, (int)hw[i].rho);
    }
    std::fclose(f);

    std::vector<double> ms = g_ms;
    std::sort(ms.begin(), ms.end());
    f = std::fopen(p_sum.c_str(), "w");
    if (!f) { std::printf("    cannot write %s\n", p_sum.c_str()); return; }
    std::fprintf(f, "events %zu\n", g_events);
    std::fprintf(f, "hw_decisions %zu\n", hw.size());
    std::fprintf(f, "ref_decisions %zu\n", gd.size());
    std::fprintf(f, "slot_mismatch %ld\n", slot_mismatch);
    std::fprintf(f, "ref_reps %zu\n", g_ms.size());
    std::fprintf(f, "ref_ms");
    for (double v : g_ms) std::fprintf(f, " %.3f", v);
    std::fprintf(f, "\n");
    std::fprintf(f, "ref_ms_min %.3f\n", ms.front());
    std::fprintf(f, "ref_ms_median %.3f\n", ms[ms.size() / 2]);
    std::fprintf(f, "hw_median %.4f\nref_median %.4f\nhw_p95 %.4f\nref_p95 %.4f\n",
                 hw_med, gm_med, hw_p95, gm_p95);
    std::fclose(f);

    const double t = ms[ms.size() / 2];
    std::printf("    decisions.csv : %zu rows (slot mismatches %ld, should be 0)\n",
                hw.size(), slot_mismatch);
    std::printf("    reference     : %zu events, %zu decisions in %.1f ms (median of %zu)\n"
                "                    %.3f M events/s, %.3f M decisions/s on one core\n",
                g_events, gd.size(), t, g_ms.size(),
                g_events / (t * 1000.0), gd.size() / (t * 1000.0));
    std::printf("    summary.txt   : written. Next: python3 thesis_data.py\n");
}

} // namespace thesis_export

// Compare a hardware decision stream against the reference, using the gates
// this project actually validates against.
//
// Per-tick signal agreement is NOT one of them. tb_system_b.cpp prints it
// labelled "informational, not a gate; §4.5", because the incremental SVD is
// chaotic: any quantisation difference between ap_fixed<24,12> and IEEE double
// is amplified tick over tick, so two correct implementations diverge per-tick
// while still describing the same process. It is reported below for continuity
// with earlier runs, and it should not be read as an error rate.
//
// The two gates that decided the delivered design:
//   1. No saturation.        max |S1| must stay under 0.90 x 2048.
//   2. Distribution match.   median and p95 of the S1 series must sit within
//                            25% of the reference's.
static void compare_to_golden(const std::vector<dec_rec_t>& hw,
                              const std::vector<dec_rec_t>& gd,
                              const char* label)
{
    std::printf("\n  --- %s vs double-precision reference ---\n", label);
    std::printf("    hardware  : %6zu decisions\n", hw.size());
    std::printf("    reference : %6zu decisions\n", gd.size());

    if (hw.empty() || gd.empty()) { std::printf("    nothing to compare.\n"); return; }
    if (hw.size() != gd.size())
        std::printf("    *** counts differ - the book gate itself disagrees,\n"
                    "        which is a parsing or ordering problem, not numeric.\n");

    const size_t n = std::min(hw.size(), gd.size());

    // ── informational: per-tick agreement ───────────────────────────────────
    long dfin = 0, dmom = 0, drsk = 0, drgm = 0;
    for (size_t i = 0; i < n; i++) {
        if (hw[i].final_sig != gd[i].final_sig) dfin++;
        if (hw[i].mom_sig   != gd[i].mom_sig)   dmom++;
        if (hw[i].rsk_sig   != gd[i].rsk_sig)   drsk++;
        if (hw[i].rgm_sig   != gd[i].rgm_sig)   drgm++;
    }
    std::printf("    per-tick agreement (informational, not a gate):\n"
                "      final %.1f%%  momentum %.1f%%  risk %.1f%%  regime %.1f%%\n",
                100.0 * (double)(n - dfin) / (double)n,
                100.0 * (double)(n - dmom) / (double)n,
                100.0 * (double)(n - drsk) / (double)n,
                100.0 * (double)(n - drgm) / (double)n);

    // ── gate 1: saturation ──────────────────────────────────────────────────
    const double CEIL = 2048.0, LIMIT = 0.90 * CEIL;
    double hw_max = 0.0, rho_max = 0.0;
    long   rho_railed = 0;
    for (size_t i = 0; i < n; i++) {
        hw_max  = std::max(hw_max,  std::fabs(dq12(hw[i].s1)));
        rho_max = std::max(rho_max, std::fabs(dq12(hw[i].rho)));
        // A saturated ap_fixed<24,12> reads back as 2047.9998, never 2048.0,
        // so the threshold has to sit just below the ceiling or this never
        // fires and reports zero while the trace is visibly railed.
        if (std::fabs(dq12(hw[i].rho)) >= 2047.99) rho_railed++;
    }
    bool pass1 = hw_max <= LIMIT;
    std::printf("    GATE 1 saturation   : max |S1| %.3f vs ceiling %.1f  -> %s\n",
                hw_max, LIMIT, pass1 ? "PASS" : "FAIL");
    std::printf("      (rho max %.4f, railed on %ld of %zu ticks - rho is not gated)\n",
                rho_max, rho_railed, n);

    // ── gate 2: S1 distribution ─────────────────────────────────────────────
    std::vector<double> a(n), b(n);
    for (size_t i = 0; i < n; i++) { a[i] = dq12(hw[i].s1); b[i] = dq12(gd[i].s1); }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    const double hw_med = a[n/2],        gm_med = b[n/2];
    const double hw_p95 = a[n*95/100],   gm_p95 = b[n*95/100];
    const double med_err = gm_med > 1e-9 ? std::fabs(hw_med - gm_med) / gm_med : 0.0;
    const double p95_err = gm_p95 > 1e-9 ? std::fabs(hw_p95 - gm_p95) / gm_p95 : 0.0;
    bool pass2 = (med_err <= 0.25 && p95_err <= 0.25);
    std::printf("    GATE 2 distribution : median %.4f vs %.4f (%.1f%% apart)\n"
                "                          p95    %.4f vs %.4f (%.1f%% apart)  -> %s\n",
                hw_med, gm_med, med_err * 100.0,
                hw_p95, gm_p95, p95_err * 100.0, pass2 ? "PASS" : "FAIL");

    // ── worst single instrument, since the gates above pool all of them ─────
    int nas = 0;
    for (size_t i = 0; i < n; i++) if (hw[i].asset + 1 > nas) nas = hw[i].asset + 1;
    if (nas > 1) {
        double worst = 0.0; int worst_a = 0;
        for (int aa = 0; aa < nas; aa++) {
            std::vector<double> x, y;
            for (size_t i = 0; i < n; i++)
                if (hw[i].asset == aa) { x.push_back(dq12(hw[i].s1));
                                         y.push_back(dq12(gd[i].s1)); }
            if (x.size() < 8) continue;
            std::sort(x.begin(), x.end()); std::sort(y.begin(), y.end());
            const double mx = x[x.size()/2], my = y[y.size()/2];
            const double e = my > 1e-9 ? std::fabs(mx - my) / my : 0.0;
            if (e > worst) { worst = e; worst_a = aa; }
        }
        std::printf("    worst instrument    : %d, median %.1f%% apart\n",
                    worst_a, worst * 100.0);
    }

    std::printf("    VERDICT             : %s\n",
                (pass1 && pass2) ? "PASS on both gates" : "FAIL");

    if (std::strcmp(label, "interleaved") == 0)
        thesis_export::write_files(hw, gd, hw_med, gm_med, hw_p95, gm_p95);
}
