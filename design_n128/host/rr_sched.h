// ─────────────────────────────────────────────────────────────────────────────
// rr_sched.h — --sched: measure whether a drop-free window exists, and if it
// does, build the batch from it.
//
// Background, and why the obvious approach does not work.
//
// Every stateful kernel is compiled with `dependence ... inter distance=NASSET
// true`, which tells the tool it may keep NASSET iterations in flight before a
// read-after-write on its state array matters. Two kernels rely on that promise
// against two DIFFERENT streams:
//
//   lob_filter   holds bid_all[NASSET] / ask_all[NASSET] and sees EVERY event,
//                so it needs the INPUT stream exactly NASSET-spaced.
//   inctsvd_top  holds Ut_all[NASSET] / St_all[NASSET] and sees only the ticks
//                lob_filter decided to emit, so it needs the DECIDED stream
//                exactly NASSET-spaced.
//
// The plain rotation satisfies the first exactly and the second only when
// nothing is dropped. The drop rate here is about 10% overall, so a read 32
// back from a tick whose predecessor is only 29 back reaches past that
// predecessor's write and picks up the state from two of its own ticks ago.
// The instrument's trajectory splits into two independent chains, which is
// exactly what the S1 trace showed.
//
// The tempting fix is to reorder the input so the decided subsequence comes out
// evenly spaced. That cannot work. Bunching an instrument's non-deciding events
// next to its deciding one is what makes the decided stream even, and it is
// also precisely what breaks lob_filter's own spacing. The two constraints are
// incompatible for any non-zero drop rate, and trying it deadlocks the pipeline
// because the hardware then decides on a different set of events than the host
// predicted.
//
// They coincide in exactly one case: a window in which nothing is dropped. If
// every instrument takes the same number of warm-up events and then decides on
// every event after that, the input stream and the decided stream are the same
// sequence and both promises hold exactly.
//
// So this does not try to be clever. It measures, per instrument, how long the
// warm-up is and how long the drop-free run after it is, prints the result, and
// builds the batch only if a common window actually exists. If it does not, it
// says so and leaves the plain rotation alone rather than producing a batch
// that hangs the card.
//
// This is a bench technique either way. Real feeds drop. The permanent fix is
// for lob_filter to emit on every event with a valid bit, so the stream the SVD
// sees is the input stream and the two constraints stop being separate.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "golden_model.h"

static void rr_schedule(const std::vector<std::vector<upd_rec_t>>& per,
                        Batch& batch)
{
    const size_t NS = per.size();
    if (NS == 0) return;

    // warm[a] = index of a's first deciding event.
    // run[a]  = how many consecutive deciding events follow from there.
    std::vector<size_t> warm(NS, 0), run(NS, 0);

    for (size_t a = 0; a < NS; a++) {
        golden::Book bk;
        bk.clear();
        bool found = false;

        for (size_t i = 0; i < per[a].size(); i++) {
            const upd_rec_t& r = per[a][i];
            golden::Update u;
            u.msg_type = (uint8_t)( r.w0        & 0xFF);
            u.side     = (uint8_t)((r.w0 >> 8)  & 0xFF);
            u.price    = (double)(int32_t)r.w1 / 65536.0;
            u.qty      = r.w2;
            u.ref      = ((uint64_t)r.w4 << 32) | (uint64_t)r.w3;
            bk.apply(u);

            if (!found) {
                if (bk.ready()) { warm[a] = i; found = true; run[a] = 1; }
            } else {
                if (bk.ready()) run[a]++;
                else break;                 // first drop ends the clean window
            }
        }
        if (!found) { warm[a] = per[a].size(); run[a] = 0; }
    }

    size_t wmin = warm[0], wmax = warm[0], rmin = run[0];
    size_t thin = 0;
    for (size_t a = 1; a < NS; a++) {
        if (warm[a] < wmin) wmin = warm[a];
        if (warm[a] > wmax) wmax = warm[a];
        if (run[a]  < rmin) { rmin = run[a]; thin = a; }
    }

    std::printf("  drop-free window  : warm-up %zu..%zu events, "
                "shortest clean run %zu (instrument %zu)\n",
                wmin, wmax, rmin, thin);

    if (wmin != wmax) {
        std::printf("  scheduler         : instruments warm up at different\n"
                    "                      points, so the opening rounds would\n"
                    "                      decide unevenly. Plain rotation kept.\n");
        return;
    }
    if (rmin < 8) {
        std::printf("  scheduler         : shortest clean run is only %zu ticks,\n"
                    "                      too short to tell quantisation from a\n"
                    "                      real defect. Plain rotation kept.\n", rmin);
        return;
    }

    // Rounds 0..wmin-1 decide for nobody; rounds wmin..wmin+rmin-1 decide for
    // everybody. Input spacing and decided spacing are both exactly NS.
    const size_t total = wmin + rmin;
    batch.updates.clear();
    for (size_t i = 0; i < total; i++)
        for (size_t a = 0; a < NS; a++)
            if (i < per[a].size()) batch.updates.push_back(per[a][i]);

    std::printf("  scheduler         : drop-free batch, %zu rounds x %zu "
                "instruments,\n"
                "                      %zu events, %zu decisions, both streams "
                "exactly %zu-spaced\n",
                total, NS, batch.updates.size(), rmin * NS, NS);
}

// ─────────────────────────────────────────────────────────────────────────────
// rr_probe — find instruments that warm up in exactly one event and then never
// drop, so a plain rotation over them satisfies BOTH spacing constraints at
// once. Pure host work: parses the feed, models each book, prints a --symbols
// line. Never touches the card.
//
// The busiest instruments are not the cleanest. Busy names like UVXY quote one
// side at a time constantly, so their book goes one-sided and lob_filter drops
// the tick. What this looks for instead is instruments whose book comes up on
// the second event and stays two-sided, because for those the input stream and
// the decided stream are the same sequence.
// ─────────────────────────────────────────────────────────────────────────────
static void rr_probe(const std::string& itch, long limit, int probe, int nasset)
{
    auto ranked = ITCHParser::scan_symbols(itch, 200 * 1024 * 1024);
    std::vector<std::string> cand;
    for (size_t i = 0; i < ranked.size() && (int)cand.size() < probe; i++)
        cand.push_back(ranked[i].first);
    std::printf("  scan returned %zu symbols\n", ranked.size());
    if (ranked.empty()) {
        std::printf("  retrying over the whole file...\n");
        ranked = ITCHParser::scan_symbols(itch, 0);
        std::printf("  scan returned %zu symbols\n", ranked.size());
        for (size_t i = 0; i < ranked.size() && (int)cand.size() < probe; i++)
            cand.push_back(ranked[i].first);
    }
    if (cand.empty()) { std::printf("  no symbols found\n"); return; }

    std::printf("  probing %zu symbols, %ld events each...\n", cand.size(), limit);

    std::vector<std::vector<upd_rec_t>> pv(cand.size());
    {
        ITCHParser p;
        p.set_symbols(cand);
        p.parse_file(itch, [&](const LOBUpdate_host& u) {
            size_t a = u.asset;
            if (a >= pv.size() || (long)pv[a].size() >= limit) return;
            upd_rec_t r{};
            r.w0    = (uint32_t)u.msg_type | ((uint32_t)u.side << 8);
            r.w1    = q16(u.price);
            r.w2    = u.qty;
            r.w3    = (uint32_t)(u.ref_num & 0xFFFFFFFFu);
            r.w4    = (uint32_t)(u.ref_num >> 32);
            r.asset = (uint32_t)a;
            pv[a].push_back(r);
        });
    }

    // (clean run length, candidate index) for every symbol that warms up on
    // exactly its second event.
    std::vector<std::pair<size_t, size_t>> good;
    for (size_t a = 0; a < pv.size(); a++) {
        golden::Book bk;
        bk.clear();
        size_t w = 0, run = 0;
        bool found = false;
        for (size_t i = 0; i < pv[a].size(); i++) {
            const upd_rec_t& r = pv[a][i];
            golden::Update u;
            u.msg_type = (uint8_t)( r.w0        & 0xFF);
            u.side     = (uint8_t)((r.w0 >> 8)  & 0xFF);
            u.price    = (double)(int32_t)r.w1 / 65536.0;
            u.qty      = r.w2;
            u.ref      = ((uint64_t)r.w4 << 32) | (uint64_t)r.w3;
            bk.apply(u);
            if (!found) { if (bk.ready()) { w = i; found = true; run = 1; } }
            else        { if (bk.ready()) run++; else break; }
        }
        if (found && w == 1) good.push_back(std::make_pair(run, a));
    }

    std::sort(good.rbegin(), good.rend());
    std::printf("  %zu of %zu warm up in exactly 1 event and then hold\n",
                good.size(), cand.size());

    std::string csv;
    const size_t take = std::min((size_t)nasset, good.size());
    for (size_t i = 0; i < take; i++) {
        std::printf("    %-8s clean run %zu\n",
                    cand[good[i].second].c_str(), good[i].first);
        if (!csv.empty()) csv += ",";
        csv += cand[good[i].second];
    }

    if ((int)good.size() >= nasset)
        std::printf("\n  --symbols %s\n", csv.c_str());
    else
        std::printf("\n  only %zu qualify, %d needed. Raise --probe or --limit.\n",
                    good.size(), nasset);
}
