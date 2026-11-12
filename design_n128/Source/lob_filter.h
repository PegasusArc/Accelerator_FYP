// ─────────────────────────────────────────────────────────────────────────────
// lob_filter.h  (Option B revision)
//
// Book maths identical to Offer 2. Interface change: loops over the whole batch
// (n_ticks = number of UPDATES = N) in one invocation, applying every update to
// the book, but EMITS a Bt beat only on a two-sided (ready) book — exactly the
// ticks the golden model and itch_replay decide. The books are batch-local state
// carried across the loop (were file-static in Opt A), wiped once at entry, so
// the LOB_CLEAR-stuck-high class of bug cannot recur. best_bid/best_ask/
// book_ready register readbacks are dropped: the downstream kernels are sized to
// the DECIDED-tick count (M), which the host counts up front, so no per-tick
// ready flag needs to travel with the data.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "qf_types.h"

extern "C" {

void lob_filter(
    hls::stream<upd_axi_t> &upd_in,      // AXIS : packed LOBUpdate stream (N beats)
    hls::stream<bt_axi_t>  &Bt_out,      // AXIS : packed 10x2 tick matrix, ready ticks only (M beats)
    int                     n_ticks,     // AXI-L : number of updates N to consume
    const price_t          *price_ref,   // m_axi : one centring offset PER
                                         // INSTRUMENT, indexed by the beat's
                                         // identifier. Each instrument must be
                                         // centred against its own opening
                                         // price: SPY opens near 300 and IWM
                                         // near 155, so a shared reference
                                         // would displace one of them by well
                                         // over a hundred dollars and the tick
                                         // matrix would describe a fiction.
                                         //
                                         // A small memory rather than one
                                         // register per instrument, so the
                                         // interface does not grow with
                                         // NASSET.
                                         // price_t (ap_fixed<32,16>, ceiling
                                         // 32,768) NOT fixed_t: the absolute
                                         // share price has to fit here, and
                                         // fixed_t's +/-2048 excluded any
                                         // instrument above ~$2,048.
    fixed_t                 price_scale, // AXI-L : price gain
    fixed_t                 qty_scale,   // AXI-L : qty gain
    ap_uint<1>              unit_norm    // AXI-L : 1 = scale Bt to |Bt|=1
);

} // extern "C"
