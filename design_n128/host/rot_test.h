// ─────────────────────────────────────────────────────────────────────────────
// rot_test.h — rotation invariance.
//
// This replaces what `--verify` was reaching for. That check ran instrument 0
// alone and compared it against instrument 0 taken out of the interleaved run,
// on the theory that interleaving changes WHEN a tick is processed and not WHAT
// is computed. The theory is right; the test was not, because feeding a single
// instrument into a design built for NASSET of them violates the dependence
// distance on every tick, so the solo run is the least trustworthy thing in the
// system and it can never pass.
//
// The same idea works if both runs are legitimate. Run the identical thirty-two
// instruments twice, with the rotation cyclically shifted the second time, so
// each instrument occupies a different slot and is processed at a different
// point in each round. Every instrument still appears exactly once per round,
// so both runs honour the spacing promise. If the design is correct, each
// instrument's decisions must come back byte-identical regardless of which slot
// it sat in.
//
// What it catches that the reference comparison does not: cross-contamination
// between instrument slots, an off-by-one in the state indexing, and any
// position-dependent behaviour in the rotation. The reference comparison checks
// the arithmetic; this checks the interleaving itself. They are complementary
// and neither subsumes the other.
//
// It needs no reference model and no golden data - only the hardware, twice.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

static int rotation_test(xrt::device& dev, const xrt::uuid& uuid,
                         const std::vector<std::vector<upd_rec_t>>& per,
                         const std::vector<double>& pref,
                         const std::vector<std::vector<dec_rec_t>>& by_asset,
                         const std::vector<std::string>& want)
{
    const int NS = (int)per.size();
    if (NS < 2) { std::printf("\n  rotation test needs at least 2 instruments\n"); return 0; }
    const int S = NS / 2;                       // shift; half a round is as far as it goes

    std::printf("\n=== Rotation invariance ===\n");
    std::printf("  Same %d instruments, rotation shifted by %d slots. Each one is\n"
                "  processed at a different point in the round, but still exactly\n"
                "  once per round, so both runs honour the spacing the kernels\n"
                "  were built on. Every instrument's decisions must be identical.\n\n",
                NS, S);

    // Instrument a moves to slot (a + S) mod NS.
    Batch b2;
    b2.price_ref.assign((size_t)NS, 0.0);
    for (int a = 0; a < NS; a++) b2.price_ref[(size_t)((a + S) % NS)] = pref[(size_t)a];

    {
        std::vector<size_t> pos((size_t)NS, 0);
        bool any = true;
        while (any) {
            any = false;
            for (int slot = 0; slot < NS; slot++) {
                const int a = ((slot - S) % NS + NS) % NS;   // who sits in this slot
                if (pos[(size_t)a] < per[(size_t)a].size()) {
                    upd_rec_t r = per[(size_t)a][pos[(size_t)a]++];
                    r.asset = (uint32_t)slot;                // retag to the new slot
                    b2.updates.push_back(r);
                    any = true;
                }
            }
        }
    }
    b2.decided = count_decided(b2.updates, NS);

    std::printf("  running shifted...\n");
    auto out2 = run_on_card(dev, uuid, b2);
    if (out2.empty()) { std::printf("*** FAIL: shifted run returned nothing.\n"); return 1; }

    std::vector<std::vector<dec_rec_t>> by_slot((size_t)NS);
    for (const auto& d : out2)
        if (d.asset >= 0 && d.asset < NS) by_slot[(size_t)d.asset].push_back(d);

    long bad_inst = 0, first_bad = -1, total = 0, diffs = 0;
    for (int a = 0; a < NS; a++) {
        const auto& x = by_asset[(size_t)a];               // original run
        const auto& y = by_slot[(size_t)((a + S) % NS)];   // shifted run
        total += (long)x.size();

        if (x.size() != y.size()) {
            std::printf("    %-6s count differs: %zu original vs %zu shifted\n",
                        want[(size_t)a].c_str(), x.size(), y.size());
            bad_inst++;
            if (first_bad < 0) first_bad = a;
            continue;
        }
        long d_here = 0;
        for (size_t i = 0; i < x.size(); i++) {
            dec_rec_t p = x[i], q = y[i];
            p.asset = q.asset = 0;              // the slot number is meant to differ
            if (std::memcmp(&p, &q, sizeof(dec_rec_t)) != 0) d_here++;
        }
        if (d_here) {
            std::printf("    %-6s %ld of %zu decisions differ\n",
                        want[(size_t)a].c_str(), d_here, x.size());
            bad_inst++;
            if (first_bad < 0) first_bad = a;
        }
        diffs += d_here;
    }

    if (bad_inst == 0) {
        std::printf("\n*** PASS: all %d instruments byte-identical across %ld decisions.\n"
                    "    Where an instrument sits in the rotation changes nothing about\n"
                    "    what it computes, which is the property interleaving depends on.\n",
                    NS, total);
        return 0;
    }
    std::printf("\n*** FAIL: %ld of %d instruments differ (%ld decisions), first is %s.\n"
                "    Position in the rotation is affecting the result, which means state\n"
                "    is leaking between slots or being indexed wrongly.\n",
                bad_inst, NS, diffs, want[(size_t)first_bad].c_str());
    return 1;
}
