// ─────────────────────────────────────────────────────────────────────────────
// host_rr.cpp — multi-instrument host for the context-interleaved accelerator
//
// The delivered host (host_xrt_ip.cpp) follows one instrument and carries the
// full diagnostic apparatus built up during bring-up. This one does a different
// job: it follows several instruments at once, interleaves their events through
// a single pipeline, and separates the decisions again on return. It is
// deliberately leaner, because the numerical behaviour it would otherwise
// re-check has already been established by the single-instrument host and by
// the C-simulation equivalence test.
//
// It answers one question the other host cannot: does carrying several
// instruments through the same hardware change what any of them computes.
//
//   ./host_rr <xclbin> <itch-file> [options]
//
//     --symbols A,B,C   instruments to follow. Defaults to the busiest in the
//                       file, chosen by a preliminary scan.
//     --limit N         events per instrument (default 20000)
//     --verify          additionally run instrument 0 alone and compare the two
//                       decision streams byte for byte
//     --table N         print the first N decisions of instrument 0
//
// ─── DESIGN DECISIONS, AND WHY ───────────────────────────────────────────────
//
// STRICT ROTATION. Events are presented one per instrument in a fixed cycle.
// The alternative, sending whichever instrument has an event waiting, wastes no
// slots on quiet instruments and is the better scheme in a live system. It is
// not used here because the kernels assert to the synthesis tool that any
// instrument's state is touched at most once every NASSET iterations, and that
// assertion is what allows the initiation interval to fall from 116 to 4. A
// demand-driven order can present the same instrument twice in quick
// succession, which violates the assertion, and the resulting corruption is
// silent. Demand-driven scheduling with a minimum separation of NASSET events
// would preserve the guarantee and is the natural next step; strict rotation is
// what the hardware has been built and measured against.
//
// PER-INSTRUMENT price_ref. Each instrument is centred against its own opening
// price, supplied here as a small array the kernel reads by identifier. A
// shared reference would centre AAPL near 209 against SPY's 301 and the price
// column would be meaningless. The reference is taken from a preliminary parse;
// a deployed system would latch the first price it observed instead.
//
// EQUALLY SIZED BATCHES. Every instrument contributes the same number of events,
// so the rotation never has to skip a slot. Real instruments do not oblige, and
// a production scheduler would need to handle exhaustion; that is deferred.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <thread>

#include "experimental/xrt_ip.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_bo.h"
#include "xrt/xrt_kernel.h"

#include "qf_shared.h"
#include "itch_parser.h"
#include "golden_model.h"
#include "refdump.h"                  
std::string g_dumpref;

#ifndef NASSET
#define NASSET 128
#endif

// Register offsets, from the argument map dumped during bring-up. These are the
// AXI-Lite addresses each kernel exposes; they are not derivable from the source
// and must match the built xclbin.
static const uint32_t R_UPD_BUF   = 0x10, R_UPD_TICKS = 0x1c;
// lob_filter's map SHIFTED when price_ref became a pointer. A scalar occupied
// four bytes at 0x18; a pointer occupies eight, and every argument after it
// moved up by four. Verified against the built xclbin with argdump rather than
// assumed:
//
//     n_ticks     0x10  4 bytes
//     price_ref   0x18  8 bytes   <- the pointer
//     price_scale 0x24  4 bytes   (was 0x20)
//     qty_scale   0x2c  4 bytes   (was 0x28)
//     unit_norm   0x34  4 bytes   (was 0x30)
//
// Writing the old offsets put price_scale into the pointer's upper half, so the
// kernel read its centring offsets from a corrupted address, and left unit_norm
// unwritten. The result was a degenerate tick matrix, S1 repeating in runs, rho
// identically zero, and a stalled pipeline. Always re-run argdump after
// changing a kernel signature.
static const uint32_t R_LOB_TICKS = 0x10, R_LOB_PREF  = 0x18;
static const uint32_t R_LOB_PSC   = 0x24, R_LOB_QSC   = 0x2c, R_LOB_UNORM = 0x34;
static const uint32_t R_SVD_TICKS = 0x10, R_SVD_LAMBDA= 0x18;
static const uint32_t R_MOM_TICKS = 0x10, R_MOM_ALPHA = 0x18;
static const uint32_t R_MOM_HI    = 0x20, R_MOM_LO    = 0x28, R_MOM_EMIN = 0x30;
static const uint32_t R_RSK_TICKS = 0x10, R_RSK_THR   = 0x18, R_RSK_EPS  = 0x20;
static const uint32_t R_RSK_MODE  = 0x28, R_RSK_LSLOW = 0x30;
static const uint32_t R_RGM_TICKS = 0x10, R_RGM_TREND = 0x18;
static const uint32_t R_RGM_VOL   = 0x20, R_RGM_EPS   = 0x28;
static const uint32_t R_DEC_BUF   = 0x10, R_DEC_TICKS = 0x1c;
// Appended argument, so every offset above is unchanged. Verify with
// argdump against the new xclbin before believing any result.
static const uint32_t R_DEC_EVENTS = 0x24;

static const double LAMBDA = 0.90,  PSCALE = 2.0,    QSCALE = 0.001;
static const double ALPHA  = 0.35,  DEV_HI = 0.4179, DEV_LO = -0.4709;
static const double EMIN   = 1.0,   RTHR   = 5.9026, EPS    = 0.001;
static const double LSLOW  = 0.999, TTHR   = 1.0087, VTHR   = 136.3473;
static const int    RMODE  = 1,     UNORM  = 1;

static uint32_t q12(double v) { return (uint32_t)(int32_t)std::lround(v * 4096.0); }
static uint32_t q16(double v) { return (uint32_t)(int32_t)std::lround(v * 65536.0); }
static double  dq12(int32_t v){ return (double)v / 4096.0; }

// ── A batch of interleaved events, and the decisions it produced ────────────
struct Batch {
    std::vector<upd_rec_t> updates;      // interleaved, each tagged
    std::vector<double>    price_ref;    // one per instrument
    int                    decided = 0;  // two-sided events, counted by the model
};

#include "golden_check.h"
#include "rr_sched.h"

// Run one batch on the card. Returns the decisions in the order produced,
// which is interleaved; the caller separates them.
static std::vector<dec_rec_t>
run_on_card(xrt::device& dev, const xrt::uuid& uuid, const Batch& b)
{
    const int N = (int)b.updates.size();
    const int M = b.decided;
    if (M == 0) return {};

    xrt::ip ip_upd(dev, uuid, "upd_inject");
    xrt::ip ip_lob(dev, uuid, "lob_filter");
    xrt::ip ip_svd(dev, uuid, "inctsvd_top");
    xrt::ip ip_mom(dev, uuid, "momentum_signal");
    xrt::ip ip_rsk(dev, uuid, "risk_matrix");
    xrt::ip ip_rgm(dev, uuid, "regime_classifier");
    xrt::ip ip_dec(dev, uuid, "decision_sink");

    // Device buffers. These are the largest single allocations in the run and
    // the first thing to fail as the batch grows, so the failure is reported
    // plainly rather than left to surface as an inexplicable result later.
    const size_t in_bytes  = (size_t)N * sizeof(upd_rec_t);
    const size_t out_bytes = (size_t)M * sizeof(dec_rec_t);
    std::printf("  buffers           : in %.2f MB, out %.2f MB\n",
                in_bytes / 1048576.0, out_bytes / 1048576.0);

    xrt::bo bo_in, bo_out, bo_pr;
    try {
        bo_in  = xrt::bo(dev, in_bytes,  xrt::bo::flags::normal, 0);
        bo_out = xrt::bo(dev, out_bytes, xrt::bo::flags::normal, 1);
        bo_pr  = xrt::bo(dev, NASSET * sizeof(uint32_t), xrt::bo::flags::normal, 0);
    } catch (const std::exception& e) {
        std::printf("\n  BUFFER ALLOCATION FAILED: %s\n"
                    "  Requested %.2f MB in and %.2f MB out. Reduce --limit.\n",
                    e.what(), in_bytes / 1048576.0, out_bytes / 1048576.0);
        return {};
    }

    std::memcpy(bo_in.map<void*>(), b.updates.data(), N * sizeof(upd_rec_t));

    // The per-instrument centring offsets, in the same Q16.16 encoding the
    // kernel expects. An instrument with no events contributes zero, which is
    // harmless since its state is never touched.
    {
        uint32_t* pr = bo_pr.map<uint32_t*>();
        for (int a = 0; a < NASSET; a++)
            pr[a] = q16(a < (int)b.price_ref.size() ? b.price_ref[a] : 0.0);
    }
    bo_in.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bo_pr.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    ip_upd.write_register(R_UPD_BUF,       bo_in.address() & 0xFFFFFFFF);
    ip_upd.write_register(R_UPD_BUF + 4,   bo_in.address() >> 32);
    ip_upd.write_register(R_UPD_TICKS,     N);

    ip_lob.write_register(R_LOB_TICKS,     N);
    ip_lob.write_register(R_LOB_PREF,      bo_pr.address() & 0xFFFFFFFF);
    ip_lob.write_register(R_LOB_PREF + 4,  bo_pr.address() >> 32);
    ip_lob.write_register(R_LOB_PSC,       q12(PSCALE));
    ip_lob.write_register(R_LOB_QSC,       q12(QSCALE));
    ip_lob.write_register(R_LOB_UNORM,     UNORM);

    ip_svd.write_register(R_SVD_TICKS,     N);   // events, not decisions
    ip_svd.write_register(R_SVD_LAMBDA,    q12(LAMBDA));

    ip_mom.write_register(R_MOM_TICKS,     N);   // events, not decisions
    ip_mom.write_register(R_MOM_ALPHA,     q12(ALPHA));
    ip_mom.write_register(R_MOM_HI,        q12(DEV_HI));
    ip_mom.write_register(R_MOM_LO,        q12(DEV_LO));
    ip_mom.write_register(R_MOM_EMIN,      q12(EMIN));

    ip_rsk.write_register(R_RSK_TICKS,     N);   // events, not decisions
    ip_rsk.write_register(R_RSK_THR,       q12(RTHR));
    ip_rsk.write_register(R_RSK_EPS,       q12(EPS));
    ip_rsk.write_register(R_RSK_MODE,      RMODE);
    ip_rsk.write_register(R_RSK_LSLOW,     q12(LSLOW));

    ip_rgm.write_register(R_RGM_TICKS,     N);   // events, not decisions
    ip_rgm.write_register(R_RGM_TREND,     q12(TTHR));
    ip_rgm.write_register(R_RGM_VOL,       q12(VTHR));
    ip_rgm.write_register(R_RGM_EPS,       q12(EPS));

    ip_dec.write_register(R_DEC_BUF,       bo_out.address() & 0xFFFFFFFF);
    ip_dec.write_register(R_DEC_BUF + 4,   bo_out.address() >> 32);
    ip_dec.write_register(R_DEC_TICKS,     M);   // records to write
    ip_dec.write_register(R_DEC_EVENTS,    N);   // beats to read

    // Confirm the configuration reached the card. During bring-up the standard
    // argument-setting mechanism failed silently on kernels with stream ports,
    // producing zero-length loops and an empty output buffer with no error at
    // all. Reading back is cheap and makes that class of failure visible.
    uint32_t chk_evt = ip_dec.read_register(R_DEC_EVENTS);
    if ((int)chk_evt != N) {
        std::printf("decision_sink event-count register reads %u, expected %d.\n"
                    "The appended argument is not at 0x%02x - check argdump.\n",
                    chk_evt, N, R_DEC_EVENTS);
        return {};
    }
    uint32_t chk_upd = ip_upd.read_register(R_UPD_TICKS);
    uint32_t chk_lob = ip_lob.read_register(R_LOB_TICKS);
    uint32_t chk_dec = ip_dec.read_register(R_DEC_TICKS);
    if ((int)chk_upd != N || (int)chk_lob != N || (int)chk_dec != M) {
        std::printf("register readback mismatch: upd %u lob %u dec %u "
                    "(expected %d %d %d)\n", chk_upd, chk_lob, chk_dec, N, N, M);
        return {};
    }

    // Clear any completion flags left over from a previous run.
    //
    // The control register's done bit is set when a kernel finishes and stays
    // set until it is acknowledged. If the host starts a new run without
    // clearing it, the very first poll sees done already set and concludes the
    // run finished instantly - in about seven microseconds, which is the time
    // to write the start registers and read one back. The output buffer then
    // still holds whatever was there before, and those stale contents get
    // reported as decisions.
    //
    // That is exactly what happened here: the same configuration gave 9.25 ms
    // after a card reset and 0.007 ms without one, and the difference was
    // entirely whether the flags happened to be clear. Reading the register
    // acknowledges and clears the bit, so a read of each one before starting
    // removes the dependence on remembering to reset the card.
    {
        xrt::ip* all[] = { &ip_upd, &ip_lob, &ip_svd, &ip_mom,
                           &ip_rsk, &ip_rgm, &ip_dec };
        for (auto* ip : all) {
            for (int i = 0; i < 4 && (ip->read_register(0x00) & 0x2); i++) {
                // reading is what acknowledges it; a few attempts in case a
                // kernel needs a moment to settle
            }
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    ip_dec.write_register(0x00, 1);
    ip_rgm.write_register(0x00, 1);
    ip_rsk.write_register(0x00, 1);
    ip_mom.write_register(0x00, 1);
    ip_svd.write_register(0x00, 1);
    ip_lob.write_register(0x00, 1);
    ip_upd.write_register(0x00, 1);

    // Poll rather than sleep. An earlier version slept a millisecond between
    // checks, which swamped a batch that completes in a fraction of that and
    // made the timing figure meaningless.
    auto wait = [&](xrt::ip& ip, const char* nm) {
        for (long i = 0; i < 200000000L; i++) {
            if (ip.read_register(0x00) & 0x2) return true;
            std::this_thread::yield();
        }
        std::printf("  %s did not report done\n", nm);
        return false;
    };
    // A kernel that never reports done means the run did not complete, and
    // anything read from the output buffer afterwards is whatever was there
    // before. Earlier versions of this host ignored the result and printed
    // those stale contents as though they were decisions, which cost a full
    // debugging cycle to see through. Treat it as fatal.
    bool ok = true;
    ok &= wait(ip_upd, "upd_inject");   ok &= wait(ip_lob, "lob_filter");
    ok &= wait(ip_svd, "inctsvd_top");  ok &= wait(ip_mom, "momentum");
    ok &= wait(ip_rsk, "risk");         ok &= wait(ip_rgm, "regime");
    ok &= wait(ip_dec, "decision_sink");
    auto t1 = std::chrono::high_resolution_clock::now();

    if (!ok) {
        std::printf("\n  RUN DID NOT COMPLETE. The output buffer holds whatever\n"
                    "  was in that memory beforehand, so no results are reported.\n");
        return {};
    }

    bo_out.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
    std::vector<dec_rec_t> out(M);
    std::memcpy(out.data(), bo_out.map<void*>(), M * sizeof(dec_rec_t));

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  card time         : %.3f ms for %d decisions -> %.3f M/s\n",
                ms, M, M / (ms * 1000.0));
    return out;
}

// Count how many events produce a decision, per instrument. The kernels only
// emit on events where both sides of an instrument's book are populated, so the
// output length cannot be assumed from the input length.
static int count_decided(const std::vector<upd_rec_t>& ups, int nasset);
#include "rot_test.h"
#include "rot_dump.h"

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: %s <xclbin> <itch-file> [--symbols A,B,C] "
                    "[--limit N] [--verify] [--table N]\n", argv[0]);
        return 2;
    }
    const std::string xclbin = argv[1];
    const std::string itch   = argv[2];

    long limit = 20000;
    int  table = 0;

    bool verify = false;
    bool rot    = false;
    int  shift  = 0;
    const char* dump = 0;
    bool sched  = false;
    int  probe  = 0;
    std::vector<std::string> want;

    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--table") && i + 1 < argc) table = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--verify")) verify = true;
        else if (!std::strcmp(argv[i], "--rot")) rot = true;
        else if (!std::strcmp(argv[i], "--shift") && i + 1 < argc) shift = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--dumpref") && i + 1 < argc) g_dumpref = argv[++i];
        else if (!std::strcmp(argv[i], "--sched")) sched = true;
        else if (!std::strcmp(argv[i], "--probe") && i + 1 < argc) probe = atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc) {
            std::string csv = argv[++i], cur;
            for (char c : csv) {
                if (c == ',') { if (!cur.empty()) want.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) want.push_back(cur);
        } else {
            std::printf("unrecognised option: %s\n", argv[i]);
            return 2;
        }
    }

    std::printf("=== Context-interleaved run ===\n");
    std::printf("NASSET built in    : %d\n", NASSET);
    // Host-only survey of the feed. Prints a --symbols line and exits.
    if (probe > 0) { rr_probe(itch, limit, probe, NASSET); return 0; }

    // Choose instruments if none were named. The busiest are used, since thin
    // instruments spend the run inside the decomposition's warm-up where the
    // comparison establishes little.
    if (want.empty()) {
        auto ranked = ITCHParser::scan_symbols(itch, 200 * 1024 * 1024);
        for (size_t i = 0; i < ranked.size() && (int)want.size() < NASSET; i++)
            want.push_back(ranked[i].first);
    }
    if (want.empty()) { std::printf("no instruments found\n"); return 1; }
    if ((int)want.size() > NASSET) want.resize(NASSET);

    // The kernels assert that any instrument's state is touched at most once
    // every NASSET events, and the initiation interval was scheduled on that
    // promise. Feeding fewer instruments than NASSET revisits each one sooner
    // than that, so the hardware reads state it has not yet written - silently,
    // with plausible-looking results. Every earlier run on this bitstream using
    // --symbols with fewer than NASSET instruments was invalid for this reason.
    if ((int)want.size() != NASSET) {
        std::printf("\nERROR: this design was built for %d instruments and %zu were\n"
                    "given. Feeding fewer revisits each instrument sooner than the\n"
                    "hardware assumes, which corrupts its state without reporting\n"
                    "anything. Supply exactly %d instruments, or omit --symbols to\n"
                    "let the busiest %d be chosen.\n",
                    NASSET, want.size(), NASSET, NASSET);
        return 1;
    }

    std::printf("instruments        : ");
    for (auto& s : want) std::printf("%s ", s.c_str());
    std::printf("\nevents per stock   : %ld\n\n", limit);

    // ── Parse all instruments in a single pass ──────────────────────────────
    std::vector<std::vector<upd_rec_t>> per(want.size());
    std::vector<double> pref(want.size(), 0.0);
    {
        ITCHParser p;
        p.set_symbols(want);
        p.parse_file(itch, [&](const LOBUpdate_host& u) {
            size_t a = u.asset;
            if (a >= per.size() || (long)per[a].size() >= limit) return;
            upd_rec_t r{};
            r.w0    = (uint32_t)u.msg_type | ((uint32_t)u.side << 8);
            r.w1    = q16(u.price);
            r.w2    = u.qty;
            r.w3    = (uint32_t)(u.ref_num & 0xFFFFFFFFu);
            r.w4    = (uint32_t)(u.ref_num >> 32);
            r.asset = (uint32_t)a;
            per[a].push_back(r);
        });
        for (size_t a = 0; a < want.size(); a++) pref[a] = p.first_price_of(a);
    }

    for (size_t a = 0; a < want.size(); a++)
        std::printf("  %-6s : %6zu events, price_ref %.4f\n",
                    want[a].c_str(), per[a].size(), pref[a]);

    // ── Interleave in strict rotation ───────────────────────────────────────
    // One event from each instrument in turn. An instrument is therefore
    // revisited exactly every NSYM events, which is the separation the kernels
    // have been told to assume.
    Batch batch;
    batch.price_ref = pref;
    {
        std::vector<size_t> pos(per.size(), 0);
        bool any = true;
        while (any) {
            any = false;
            for (size_t a = 0; a < per.size(); a++) {
                if (pos[a] < per[a].size()) { batch.updates.push_back(per[a][pos[a]++]); any = true; }
            }
        }
    }
    // Rebuild the rotation so the DECIDED subsequence is exactly
    // NASSET-spaced. See rr_sched.h for why the plain one is not.
    if (shift) apply_shift(per, pref, shift, batch);
    if (sched) rr_schedule(per, batch);
    batch.decided = count_decided(batch.updates, (int)per.size());
    std::printf("\n  interleaved batch : %zu events, %d expected decisions\n\n",
                batch.updates.size(), batch.decided);

    // ── Load the design and run ─────────────────────────────────────────────
    auto dev  = xrt::device(0);
    auto uuid = dev.load_xclbin(xclbin);
    std::printf("  running interleaved...\n");
    auto inter = run_on_card(dev, uuid, batch);
    if (inter.empty()) { std::printf("no decisions returned\n"); return 1; }

    // The interleaved stream is what we actually care about. Compare it
    // against a reference that has genuinely separate per-instrument state.
    {
        auto gref = run_golden(batch, (int)per.size());
        compare_to_golden(inter, gref, "interleaved");
    }

    // ── Separate the decisions by instrument ────────────────────────────────
    std::vector<std::vector<dec_rec_t>> by_asset(per.size());
    for (const auto& d : inter)
        if (d.asset >= 0 && d.asset < (int)per.size()) by_asset[d.asset].push_back(d);

    std::printf("\n  decisions per instrument:\n");
    for (size_t a = 0; a < per.size(); a++) {
        long buy = 0, sell = 0, hold = 0;
        for (const auto& d : by_asset[a]) {
            if (d.final_sig == 1) buy++; else if (d.final_sig == 2) sell++; else hold++;
        }
        std::printf("    %-6s %6zu   BUY %5ld  SELL %5ld  HOLD %5ld\n",
                    want[a].c_str(), by_asset[a].size(), buy, sell, hold);
    }

    if (table > 0 && !by_asset[0].empty()) {
        int n = std::min((int)by_asset[0].size(), table);
        auto sig = [](int v){ return v == 1 ? "BUY " : v == 2 ? "SELL" : "HOLD"; };
        std::printf("\n  first %d decisions for %s:\n", n, want[0].c_str());
        std::printf("  +-------+----------+-----------+----------+----------+----------+----------+\n");
        std::printf("  | Tick  |    S1    |    rho    | Momentum |   Risk   |  Regime  | DECISION |\n");
        std::printf("  +-------+----------+-----------+----------+----------+----------+----------+\n");
        for (int i = 0; i < n; i++) {
            const auto& d = by_asset[0][i];
            std::printf("  | %5d | %8.3f | %9.2f |   %s   |   %s   |   %s   |   %s   |\n",
                        i, dq12(d.s1), dq12(d.rho),
                        sig(d.mom_sig), sig(d.rsk_sig), sig(d.rgm_sig), sig(d.final_sig));
        }
        std::printf("  +-------+----------+-----------+----------+----------+----------+----------+\n");
    }

    if (dump) {
        if (dump_decisions(dump, shift, by_asset, want) != 0) return 1;
    }
    if (!g_dumpref.empty()) {
    thesis::dump_hw_csv(g_dumpref + "_hw.csv", inter.data(), inter.size());
    }

    // ── Rotation invariance: the same instruments, shifted round ───────────
    if (rot) {
        if (rotation_test(dev, uuid, per, pref, by_asset, want) != 0) return 1;
    }

    // ── Equivalence: instrument 0 alone against instrument 0 interleaved ────
    if (verify) {
        std::printf("\n=== Equivalence check ===\n");
        std::printf("  Running %s alone, then comparing against its decisions\n"
                    "  from the interleaved run. Interleaving alters when an\n"
                    "  event is processed, not what is computed, so these must\n"
                    "  be identical.\n\n", want[0].c_str());

        Batch solo;
        solo.updates   = per[0];
        for (auto& r : solo.updates) r.asset = 0;
        solo.price_ref = { pref[0] };
        solo.decided   = count_decided(solo.updates, 1);

        std::printf("  running solo...\n");
        auto alone = run_on_card(dev, uuid, solo);

        // Same comparison for the solo run. If this one is far worse than the
        // interleaved one, the solo path is violating the dependence distance
        // the bitstream was built with, and the solo-vs-interleaved test above
        // was never a valid reference in the first place.
        {
            auto gsolo = run_golden(solo, 1);
            compare_to_golden(alone, gsolo, "solo");
        }

        const auto& got = by_asset[0];
        std::printf("\n  %s alone       : %6zu decisions\n", want[0].c_str(), alone.size());
        std::printf("  %s interleaved : %6zu decisions\n\n", want[0].c_str(), got.size());

        if (alone.size() != got.size()) {
            std::printf("*** FAIL: decision counts differ.\n");
            return 1;
        }
        long diff = 0, first = -1;
        for (size_t i = 0; i < alone.size(); i++) {
            // The asset field necessarily differs in the two runs, so compare
            // everything else.
            dec_rec_t a = alone[i], b = got[i];
            a.asset = b.asset = 0;
            if (std::memcmp(&a, &b, sizeof(dec_rec_t)) != 0) {
                if (first < 0) first = (long)i;
                diff++;
            }
        }
        if (diff == 0) {
            std::printf("*** PASS: byte-identical across %zu decisions.\n"
                        "    Carrying %zu instruments through the same pipeline\n"
                        "    changed nothing about what any one of them computed.\n",
                        alone.size(), per.size());
        } else {
            std::printf("*** FAIL: %ld of %zu decisions differ, first at %ld.\n",
                        diff, alone.size(), first);
            return 1;
        }
    }

    std::printf("\n*** INTERLEAVED RUN COMPLETE ***\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Counting how many events produce a decision.
//
// The output buffer must be sized before the run, and only events where BOTH
// sides of an instrument's book carry resting interest produce one. The count
// has to be EXACT: predict too few and the kernel writes past the end of the
// buffer, the SVD stops while lob_filter is still producing, the stream between
// them fills, and the pipeline deadlocks with nothing reported.
//
// Two earlier versions of this function reproduced the book logic by hand and
// both were wrong - the first by 292 decisions on SPY, the second by 52. Each
// error cost a run to discover, because the failure mode is a hang rather than
// a wrong answer. So this version does not reproduce the logic at all: it uses
// golden::Book, the same model the delivered host uses to size its buffer. The
// count matches the kernel by construction rather than by my getting the
// details right.
//
// One book per instrument, since each carries independent state.
// ─────────────────────────────────────────────────────────────────────────────
static int count_decided(const std::vector<upd_rec_t>& ups, int nasset)
{
    std::vector<golden::Book> books(std::max(nasset, 1));
    for (auto& b : books) b.clear();

    int decided = 0;
    for (const auto& r : ups) {
        int a = (int)r.asset;
        if (a < 0 || a >= (int)books.size()) continue;

        golden::Update u;
        u.msg_type = (uint8_t)(r.w0 & 0xFF);
        u.side     = (uint8_t)((r.w0 >> 8) & 0xFF);
        u.price    = (double)(int32_t)r.w1 / 65536.0;
        u.qty      = r.w2;
        u.ref       = ((uint64_t)r.w4 << 32) | r.w3;

        books[a].apply(u);
        if (books[a].ready()) decided++;
    }
    return decided;
}
