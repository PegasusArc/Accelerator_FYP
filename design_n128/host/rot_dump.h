// ─────────────────────────────────────────────────────────────────────────────
// rot_dump.h — rotation invariance, run as two separate processes.
//
// The in-process version of this test could not work. Running a second batch
// inside one process means the host sees the completion flags the first batch
// left set, returns in a few microseconds and reads whatever was still in the
// output buffer. The acknowledge-by-reading block in run_on_card does not clear
// them; only an external reset does. A second run reporting 20,758 M/s is that
// failure, not a measurement.
//
// So each rotation is its own invocation, each preceded by the wrapper's reset.
//
// --shift N   places instrument a at slot (a + N) mod NASSET, so every
//             instrument is processed at a different point in the round while
//             still appearing exactly once per round. Both orderings are
//             therefore legitimate, which is precisely what the old solo-versus-
//             interleaved test never was.
//
// --dump F    writes the decisions keyed by ORIGINAL instrument, undoing the
//             shift, with the slot number zeroed. If the design is rotation
//             invariant the two files are byte-identical, so the comparison is
//             `cmp` and needs no code at all.
//
//   ~/quant_accel/rr --limit 5000 --dump /tmp/r0.bin
//   ~/quant_accel/rr --limit 5000 --shift 16 --dump /tmp/r16.bin
//   cmp /tmp/r0.bin /tmp/r16.bin && echo "ROTATION INVARIANT"
//
// What this catches that the reference comparison cannot: state leaking between
// instrument slots, an off-by-one in the state indexing, and any behaviour that
// depends on where in the round an instrument sits. The reference comparison
// checks the arithmetic; this checks the interleaving. Neither covers the other.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdio>

// Rebuild the batch with instrument a at slot (a + shift) mod NS.
static void apply_shift(const std::vector<std::vector<upd_rec_t>>& per,
                        const std::vector<double>& pref,
                        int shift, Batch& batch)
{
    const int NS = (int)per.size();
    if (NS < 2) return;
    const int S = ((shift % NS) + NS) % NS;
    if (S == 0) return;

    batch.price_ref.assign((size_t)NS, 0.0);
    for (int a = 0; a < NS; a++)
        batch.price_ref[(size_t)((a + S) % NS)] = pref[(size_t)a];

    batch.updates.clear();
    std::vector<size_t> pos((size_t)NS, 0);
    bool any = true;
    while (any) {
        any = false;
        for (int slot = 0; slot < NS; slot++) {
            const int a = ((slot - S) % NS + NS) % NS;   // who sits in this slot
            if (pos[(size_t)a] < per[(size_t)a].size()) {
                upd_rec_t r = per[(size_t)a][pos[(size_t)a]++];
                r.asset = (uint32_t)slot;
                batch.updates.push_back(r);
                any = true;
            }
        }
    }
    std::printf("  rotation          : shifted by %d slots\n", S);
}

// Write decisions keyed by ORIGINAL instrument index, so two runs at different
// shifts produce byte-identical files when the design is invariant.
static int dump_decisions(const char* path, int shift,
                          const std::vector<std::vector<dec_rec_t>>& by_slot,
                          const std::vector<std::string>& want)
{
    const int NS = (int)by_slot.size();
    const int S  = NS ? (((shift % NS) + NS) % NS) : 0;

    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("  cannot open %s for writing\n", path); return 1; }

    long total = 0;
    for (int a = 0; a < NS; a++) {
        const auto& v = by_slot[(size_t)((a + S) % NS)];
        // Length first, so a count difference shows up as a byte difference
        // rather than silently shifting everything after it.
        uint32_t n = (uint32_t)v.size();
        std::fwrite(&n, sizeof(n), 1, f);
        for (const auto& d : v) {
            dec_rec_t c = d;
            c.asset = 0;                 // the slot number is meant to differ
            std::fwrite(&c, sizeof(c), 1, f);
        }
        total += (long)v.size();
    }
    std::fclose(f);
    std::printf("  dumped            : %ld decisions for %d instruments -> %s\n"
                "                      (keyed by original instrument, slot zeroed)\n",
                total, NS, path);
    (void)want;
    return 0;
}
