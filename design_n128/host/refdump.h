// ─────────────────────────────────────────────────────────────────────────────
// refdump.h  — thesis data export (Fig 4.1, Fig 4.2, Table 4.10, Table A.1)
//
// Header-only. Include it in host_rr.cpp after qf_shared.h. It adds nothing to
// the kernels, the xclbin or the 64-byte record, so it needs a host rebuild
// only, no relink.
//
// Writes two CSV files when host_rr is run with  --dumpref <prefix>:
//
//   <prefix>_hw.csv   asset,k,final_sig,s1,rho,r12
//                     one line per decision record read back from the card
//   <prefix>_ref.csv  asset,k,s1_ref,mid
//                     one line per DECIDED event of the double-precision
//                     reference in golden_check
//
// k is the index of the decision within its own instrument (0, 1, 2 ...), so
// the plot script joins the two files on (asset, k) and does not depend on the
// order in which the host or the card emits them.
//
// Also provides RefTimer for Table 4.10 (C++ reference rate). Take that timing
// from a run WITHOUT --dumpref: the CSV writes sit inside the timed loop.
//
// ── Three edits in host_rr.cpp ──────────────────────────────────────────────
// 1. Options, next to where --dump is parsed, in the same style:
//        #include "refdump.h"                 // after #include "qf_shared.h"
//        static std::string g_dumpref;        // file scope, so golden_check sees it
//        ... else if (arg == "--dumpref" && i + 1 < argc) g_dumpref = argv[++i];
//
// 2. Where --dump writes its file (decision buffer already synced from HBM):
//        if (!g_dumpref.empty())
//            thesis::dump_hw_csv(g_dumpref + "_hw.csv", <dec_rec_t* buffer>, <n_ticks>);
//
// 3. Inside golden_check, around the reference loop over all events:
//        thesis::RefDump refdump;
//        if (!g_dumpref.empty()) refdump.open(g_dumpref + "_ref.csv");
//        thesis::RefTimer rt;  rt.start();
//        for (... every event ...) {
//            ... existing reference code ...
//            if (<book two-sided, i.e. a decided event>) {
//                ... existing code that yields the reference S1 ...
//                refdump.add(<asset>, <s1_ref>, 0.5 * (<best_bid> + <best_ask>));
//            }
//        }
//        rt.stop(<n_events>, refdump.on());
//    The <...> names are whatever golden_check already uses. mid must be in
//    dollars: divide by 1e4 if the book holds ITCH integer prices, by 65536.0
//    if it holds raw Q16.16.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "qf_shared.h"

namespace thesis {

// ---- Reference series (golden_check) ---------------------------------------
class RefDump {
public:
    bool open(const std::string &path) {
        f_ = std::fopen(path.c_str(), "w");
        if (!f_) { std::perror(path.c_str()); return false; }
        std::fprintf(f_, "asset,k,s1_ref,mid\n");
        cnt_.assign(256, 0);
        return true;
    }
    bool on() const { return f_ != nullptr; }
    // s1_ref : the reference sigma_1 for this decided event (double)
    // mid    : (best bid + best ask) / 2 from the reference book, IN DOLLARS
    void add(int asset, double s1_ref, double mid) {
        if (!f_ || asset < 0 || asset > 255) return;
        std::fprintf(f_, "%d,%u,%.6f,%.6f\n", asset, cnt_[asset]++, s1_ref, mid);
    }
    ~RefDump() { if (f_) std::fclose(f_); }
private:
    FILE *f_ = nullptr;
    std::vector<uint32_t> cnt_;
};

// ---- Hardware series (the dec_rec_t buffer read back from HBM bank 1) -------
// Fixed-point fields are fx_pack raw bits: value = raw / 4096.0 (qf_shared.h).
inline bool dump_hw_csv(const std::string &path, const dec_rec_t *d, size_t n) {
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) { std::perror(path.c_str()); return false; }
    std::fprintf(f, "asset,k,final_sig,s1,rho,r12\n");
    std::vector<uint32_t> cnt(256, 0);
    size_t written = 0;
    for (size_t i = 0; i < n; ++i) {
        const dec_rec_t &r = d[i];
        if (!r.ready || r.asset < 0 || r.asset > 255) continue;
        std::fprintf(f, "%d,%u,%u,%.6f,%.6f,%.6f\n", r.asset, cnt[r.asset]++,
                     r.final_sig, r.s1 / 4096.0, r.rho / 4096.0, r.r12 / 4096.0);
        ++written;
    }
    std::fclose(f);
    std::printf("[thesis] %zu hardware decisions -> %s\n", written, path.c_str());
    return true;
}

// ---- Table 4.10: wall time of the reference loop ----------------------------
struct RefTimer {
    std::chrono::steady_clock::time_point t0;
    void start() { t0 = std::chrono::steady_clock::now(); }
    void stop(size_t n_events, bool dumping) {
        double s = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0).count();
        std::printf("[thesis] C++ reference: %zu events in %.3f ms = %.0f events/s%s\n",
                    n_events, s * 1e3, n_events / s,
                    dumping ? "   (NOT for Table 4.10: --dumpref was on)" : "");
    }
};

} // namespace thesis
