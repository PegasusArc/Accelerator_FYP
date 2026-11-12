// ─────────────────────────────────────────────────────────────────────────────
// inctsvd_top.h  (Option B revision)
//
// SVD maths identical to Offer 2. Interface change: loops over the whole batch
// (n_ticks) in one invocation, emitting St + proj per tick. The Ut / St model
// state is local, carried across the batch loop and seeded once at entry (was
// file-static + init strobe in Opt A). threshold / St0 / qnorm / trade_signal
// debug readbacks are dropped — the vote never used trade_signal, and the host
// reads S1 from the decision records.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "qf_types.h"

extern "C" {

void inctsvd_top(
    hls::stream<bt_axi_t> &Bt_in,     // AXIS : tick matrix 10x2, packed / tick
    hls::stream<sv_axi_t> &St_out,    // AXIS : singular values S1..S4 / tick
    hls::stream<sv_axi_t> &proj_out,  // AXIS : projection magnitudes m1..m4 / tick
    int                    n_ticks,   // AXI-L : batch length
    fixed_t                lambda     // AXI-L : forgetting factor (0..1)
);

} // extern "C"
