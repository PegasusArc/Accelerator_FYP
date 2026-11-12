// ─────────────────────────────────────────────────────────────────────────────
// tb_equiv.cpp — does interleaving change the answer?
//
// The claim made for context interleaving is that it alters WHEN a tick is
// processed, not WHAT is computed. Each instrument carries its own order book
// and its own basis, so the arithmetic applied to instrument A should be
// identical whether A runs alone or shares the pipeline with others.
//
// That claim is easy to state and easy to get wrong. A single missing
// write-back of per-instrument state produces results that still look
// plausible - during bring-up the momentum EMA was loaded but never stored,
// and the output remained superficially sensible while every decision after
// the first was wrong. So the claim is tested rather than asserted.
//
// Method: replay one instrument alone, then replay the same instrument
// interleaved with NASSET-1 others, and compare the two decision streams beat
// for beat. They must be byte-identical.
//
//   ./tb_equiv <itch-file> [--limit N]
//
// WHAT THIS TEST CAN AND CANNOT SHOW
//
// It runs in C simulation, where hls::stream is unbounded and there is no
// pipelining. It therefore proves that the STATE INDEXING is correct - that
// each instrument reads and writes its own state and no other - which is the
// class of fault that actually occurred.
//
// It cannot exercise the pipeline hazard, because in C simulation each
// iteration completes before the next begins and there is nothing in flight.
// That question is settled separately by the scheduler: HLS chooses
// II = ceil(depth/N) precisely so that N iterations span the pipeline depth,
// which is why the measured products N x II all land at ~128 against a depth
// of 121-124. Only a hardware run confirms it end to end.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>

#include "qf_types.h"
#include "qf_shared.h"
#include "lob_filter.h"
#include "inctsvd_top.h"
#include "quant_apps.h"
#include "decision_sink.h"

// ─────────────────────────────────────────────────────────────────────────────
// A minimal ITCH reader, built into this test rather than shared with the host.
//
// The host's parser filters to a single symbol, which is exactly what this test
// must NOT do - it needs several symbols from one pass. Rather than modify the
// shared parser before the host is written, the small amount of decoding needed
// is done here. The record layout matches what upd_inject expects, taken from
// its source: w0 carries message type and side, w1 the price in Q16.16, w2 the
// quantity, w3 and w4 the reference number.
// ─────────────────────────────────────────────────────────────────────────────
#include <map>
#include <cstdint>

static uint16_t rd16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
static uint32_t rd32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint64_t rd64(const unsigned char* p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v;
}

struct RawUpd { uint8_t type, side; double price; uint32_t qty; uint64_t ref; };

// Read the file once, collecting updates for the requested stock locates.
// Returns, for each locate, the updates belonging to it in arrival order.
static std::map<uint16_t, std::vector<RawUpd>>
read_by_locate(const char* path, const std::vector<uint16_t>& want, long per_sym)
{
    std::map<uint16_t, std::vector<RawUpd>> out;
    for (uint16_t l : want) out[l];

    FILE* f = std::fopen(path, "rb");
    if (!f) return out;

    unsigned char len_buf[2], msg[128];
    long done = 0;
    while (std::fread(len_buf, 1, 2, f) == 2) {
        int len = rd16(len_buf);
        if (len <= 0 || len > 127) break;
        if ((int)std::fread(msg, 1, len, f) != len) break;

        uint16_t loc = rd16(msg + 1);
        auto it = out.find(loc);
        if (it == out.end() || (long)it->second.size() >= per_sym) continue;

        RawUpd u{};
        char t = (char)msg[0];
        if (t == 'A' || t == 'F') {                    // add order
            u.type  = 1;
            u.side  = (msg[19] == 'B') ? 0 : 1;
            u.qty   = rd32(msg + 20);
            u.price = (double)rd32(msg + 32) / 10000.0;
            u.ref   = rd64(msg + 11);
        } else if (t == 'D') {                         // delete
            u.type = 2; u.ref = rd64(msg + 11);
        } else if (t == 'E' || t == 'C') {             // execute
            u.type = 3; u.ref = rd64(msg + 11); u.qty = rd32(msg + 19);
        } else if (t == 'U') {                         // replace
            u.type  = 4;
            u.ref   = rd64(msg + 19);
            u.qty   = rd32(msg + 27);
            u.price = (double)rd32(msg + 31) / 10000.0;
        } else continue;

        it->second.push_back(u);
        if (++done > per_sym * (long)want.size() * 40) break;   // bounded scan
    }
    std::fclose(f);
    return out;
}

// Rank stock locates by message count over the first portion of the file, and
// map each to its ticker from the stock-directory messages.
static std::vector<std::pair<uint16_t, std::string>>
rank_locates(const char* path, long scan_bytes, int top_n)
{
    std::map<uint16_t, long> count;
    std::map<uint16_t, std::string> name;

    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    unsigned char len_buf[2], msg[128];
    long read_so_far = 0;
    while (std::fread(len_buf, 1, 2, f) == 2) {
        int len = rd16(len_buf);
        if (len <= 0 || len > 127) break;
        if ((int)std::fread(msg, 1, len, f) != len) break;
        read_so_far += len + 2;
        if (read_so_far > scan_bytes) break;

        uint16_t loc = rd16(msg + 1);
        if (msg[0] == 'R') {                           // stock directory
            char sym[9] = {0};
            std::memcpy(sym, msg + 11, 8);
            for (int i = 7; i >= 0 && sym[i] == ' '; i--) sym[i] = 0;
            name[loc] = sym;
        }
        char t = (char)msg[0];
        if (t=='A'||t=='F'||t=='D'||t=='E'||t=='C'||t=='U') count[loc]++;
    }
    std::fclose(f);

    std::vector<std::pair<uint16_t,long>> v(count.begin(), count.end());
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b){ return a.second > b.second; });

    std::vector<std::pair<uint16_t,std::string>> out;
    for (int i = 0; i < (int)v.size() && (int)out.size() < top_n; i++) {
        auto n = name.find(v[i].first);
        out.push_back({ v[i].first, n == name.end() ? "?" : n->second });
    }
    return out;
}

// Build the DMA record upd_inject expects from a decoded update.
static upd_rec_t make_rec(const RawUpd& u, uint32_t asset)
{
    upd_rec_t r{};
    r.w0    = (uint32_t)u.type | ((uint32_t)u.side << 8);
    r.w1    = (uint32_t)(int32_t)std::lround(u.price * 65536.0);   // Q16.16
    r.w2    = u.qty;
    r.w3    = (uint32_t)(u.ref & 0xFFFFFFFFu);
    r.w4    = (uint32_t)(u.ref >> 32);
    r.asset = asset;
    return r;
}
#include <cmath>


// Parameters, matching the delivered configuration exactly. Any divergence here
// would make the comparison meaningless.
static const double LAMBDA  = 0.90,  PSCALE  = 2.0,   QSCALE = 0.001;
static const double ALPHA   = 0.35,  DEV_HI  = 0.4179, DEV_LO = -0.4709;
static const double EMIN    = 1.0,   RTHR    = 5.9026, EPS    = 0.001;
static const double LSLOW   = 0.999, TTHR    = 1.0087, VTHR   = 136.3473;
static const int    RMODE   = 1,     UNORM   = 1;

// Run the whole chain over a batch of updates and return the decisions.
// This mirrors what the host does, minus the device transfers.
static std::vector<dec_rec_t> run_chain(const std::vector<upd_rec_t>& ups,
                                        const std::vector<double>& price_ref)
{
    const int N = (int)ups.size();

    hls::stream<upd_axi_t> s_upd;
    hls::stream<bt_axi_t>  s_bt;
    hls::stream<sv_axi_t>  s_st, s_pm;
    hls::stream<sv_axi_t>  st_a, st_b, st_c, pm_a, pm_b;
    hls::stream<res_axi_t> r_mom, r_rsk, r_rgm;

    // Pack the records into stream beats exactly as upd_inject does. The
    // layout is copied verbatim from that kernel, so the beats are identical.
    // Calling it directly would drag in an extern "C" linkage question that has
    // no bearing on what this test measures.
    for (int t = 0; t < N; t++) {
        const upd_rec_t& r = ups[t];
        ap_uint<32> w0 = r.w0, w1 = r.w1, w2 = r.w2, w3 = r.w3, w4 = r.w4;
        upd_axi_t pkt;
        pkt.data = 0;
        pkt.data.range(7, 0)     = w0.range(7, 0);     // msg_type
        pkt.data.range(15, 8)    = w0.range(15, 8);    // side
        pkt.data.range(47, 16)   = w1;                 // price, Q16.16
        pkt.data.range(79, 48)   = w2;                 // qty
        pkt.data.range(111, 80)  = w3;                 // ref low
        pkt.data.range(143, 112) = w4;                 // ref high
        pkt.id   = (ap_uint<8>)(r.asset & 0xFF);
        pkt.keep = -1;
        pkt.strb = -1;
        pkt.last = (t == N - 1) ? 1 : 0;
        s_upd.write(pkt);
    }

    // One centring offset per instrument, indexed by the beat's identifier.
    // An instrument with no events in this batch contributes zero, which is
    // harmless since its slot is never read.
    price_t pref_arr[NASSET];
    for (int a = 0; a < NASSET; a++)
        pref_arr[a] = (price_t)(a < (int)price_ref.size() ? price_ref[a] : 0.0);

    lob_filter(s_upd, s_bt, N, pref_arr,
               (fixed_t)PSCALE, (fixed_t)QSCALE, (ap_uint<1>)UNORM);

    const int M = (int)s_bt.size();
    if (M == 0) return {};

    inctsvd_top(s_bt, s_st, s_pm, M, (fixed_t)LAMBDA);

    // Fan out by hand. The broadcast kernels copy the whole beat including the
    // instrument identifier, so replicating the struct here is equivalent.
    while (!s_st.empty()) { sv_axi_t b = s_st.read(); st_a.write(b); st_b.write(b); st_c.write(b); }
    while (!s_pm.empty()) { sv_axi_t b = s_pm.read(); pm_a.write(b); pm_b.write(b); }

    momentum_signal(st_a, pm_a, M, (fixed_t)ALPHA, (fixed_t)DEV_HI,
                    (fixed_t)DEV_LO, (fixed_t)EMIN, r_mom);
    risk_matrix(pm_b, st_b, M, (fixed_t)RTHR, (fixed_t)EPS,
                (ap_uint<1>)RMODE, (fixed_t)LSLOW, r_rsk);
    regime_classifier(st_c, M, (fixed_t)TTHR, (fixed_t)VTHR, (fixed_t)EPS, r_rgm);

    std::vector<dec_rec_t> out(M);
    decision_sink(r_mom, r_rsk, r_rgm, out.data(), M);
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: %s <itch-file> [--limit N]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    long limit = 20000;
    for (int i = 2; i < argc; i++)
        if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = atol(argv[++i]);

    std::printf("=== Interleaving equivalence test ===\n");
    std::printf("NASSET compiled in : %d\n", NASSET);
    if (NASSET < 2) {
        std::printf("\nBuilt with NASSET=%d, so there is nothing to interleave.\n"
                    "Rebuild with -DNASSET=8 or higher for this test to mean\n"
                    "anything.\n", NASSET);
        return 1;
    }

    // Pick the busiest instruments in the file. Thin instruments would spend
    // the run inside the decomposition's warm-up, where a comparison proves
    // little.
    auto ranked = rank_locates(path, 200L * 1024 * 1024, NASSET);
    const int NSYM = std::min((int)NASSET, (int)ranked.size());
    if (NSYM < 2) { std::printf("too few symbols found in %s\n", path); return 1; }

    std::vector<uint16_t> locs;
    std::printf("instruments        : ");
    for (int i = 0; i < NSYM; i++) {
        locs.push_back(ranked[i].first);
        std::printf("%s ", ranked[i].second.c_str());
    }
    std::printf("\nmessages per stock : %ld\n\n", limit);

    auto by_loc = read_by_locate(path, locs, limit);

    // ── Pass 1: each instrument alone ───────────────────────────────────────
    // Every beat is tagged instrument 0, so only the first slot of state is
    // ever touched. This is the reference each interleaved result is compared
    // against.
    std::vector<std::vector<dec_rec_t>> alone(NSYM);
    std::vector<std::vector<upd_rec_t>> per_sym(NSYM);
    std::vector<double> pref(NSYM, 0.0);

    for (int k = 0; k < NSYM; k++) {
        const auto& raw = by_loc[locs[k]];
        std::vector<upd_rec_t> ups;
        for (const auto& u : raw) {
            if (pref[k] == 0.0 && u.price > 0.0) pref[k] = u.price;
            ups.push_back(make_rec(u, 0));       // solo run: everything slot 0
        }
        per_sym[k] = ups;
        alone[k] = run_chain(ups, { pref[k] });
        std::printf("  %-6s alone       : %6zu updates -> %6zu decisions\n",
                    ranked[k].second.c_str(), ups.size(), alone[k].size());
    }

    // ── Pass 2: all instruments interleaved in strict rotation ──────────────
    // Each beat now carries its own instrument index, so each touches its own
    // state. Round-robin: one update from each instrument in turn, so an
    // instrument is revisited exactly every NSYM beats.
    std::vector<upd_rec_t> mixed;
    {
        std::vector<size_t> pos(NSYM, 0);
        bool any = true;
        while (any) {
            any = false;
            for (int k = 0; k < NSYM; k++) {
                if (pos[k] < per_sym[k].size()) {
                    upd_rec_t u = per_sym[k][pos[k]++];
                    u.asset = (uint32_t)k;   // tag with its own slot
                    mixed.push_back(u);
                    any = true;
                }
            }
        }
    }
    std::printf("\n  interleaved batch : %zu updates in strict rotation\n\n",
                mixed.size());

    // Each instrument is now centred against its own opening price, so every
    // instrument in the batch is computed correctly rather than only the first.
    // The comparison below still examines instrument 0, but it is now a genuine
    // check of interleaving rather than a check made possible by everything
    // else being wrong in the same way.
    std::vector<dec_rec_t> inter = run_chain(mixed, pref);

    // ── Compare instrument 0 ────────────────────────────────────────────────
    std::vector<dec_rec_t> got;
    for (const auto& d : inter) if (d.asset == 0) got.push_back(d);

    const std::vector<dec_rec_t>& ref = alone[0];
    std::printf("  %-6s alone       : %6zu decisions\n",
                ranked[0].second.c_str(), ref.size());
    std::printf("  %-6s interleaved : %6zu decisions\n\n",
                ranked[0].second.c_str(), got.size());

    if (ref.size() != got.size()) {
        std::printf("*** FAIL: different number of decisions.\n"
                    "    The interleaved run produced %zu where the solo run\n"
                    "    produced %zu. Either some beats were misrouted, or the\n"
                    "    identifier is not reaching decision_sink.\n",
                    got.size(), ref.size());
        return 1;
    }

    long diff = 0, first_diff = -1;
    for (size_t i = 0; i < ref.size(); i++) {
        if (std::memcmp(&ref[i], &got[i], sizeof(dec_rec_t)) != 0) {
            if (first_diff < 0) first_diff = (long)i;
            diff++;
        }
    }

    if (diff == 0) {
        std::printf("*** PASS: byte-identical across %zu decisions.\n\n"
                    "    Interleaving %d instruments changed nothing about what\n"
                    "    was computed for this one. Each instrument reads and\n"
                    "    writes only its own state.\n", ref.size(), NSYM);
        return 0;
    }

    std::printf("*** FAIL: %ld of %zu decisions differ, first at index %ld.\n\n",
                diff, ref.size(), first_diff);
    std::printf("    tick %ld   alone: sig=%d S1=%d rho=%d   interleaved: sig=%d S1=%d rho=%d\n",
                first_diff,
                (int)ref[first_diff].final_sig, (int)ref[first_diff].s1, (int)ref[first_diff].rho,
                (int)got[first_diff].final_sig, (int)got[first_diff].s1, (int)got[first_diff].rho);
    std::printf("\n    Where the difference begins tells you which block is at\n"
                "    fault. A difference from the very first decision points at\n"
                "    the order book or the identifier plumbing. A difference that\n"
                "    appears later, after some correct decisions, points at state\n"
                "    that is read but not written back - the fault found in the\n"
                "    momentum EMA during bring-up.\n");
    return 1;
}
