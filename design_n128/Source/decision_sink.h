// ─────────────────────────────────────────────────────────────────────────────
// decision_sink.h  (Option B — NEW FILE)
//
// Convergence point of the three app streams. Reads one result beat per decided
// tick from momentum / risk / regime, runs the 2-of-3 vote on-chip (identical to
// golden::vote — pure majority on the three raw signals, regime-valid is carried
// for trace but NOT gated), and DMAs one dec_rec_t per decided tick to a host
// buffer. n_ticks = number of DECIDED (two-sided-book) ticks, which the host
// counts up front and passes to every downstream kernel. Runs once per batch.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "qf_types.h"

extern "C" {

void decision_sink(
    hls::stream<res_axi_t> &mom_in,     // AXIS : {signal, S1}
    hls::stream<res_axi_t> &rsk_in,     // AXIS : {signal, rho}
    hls::stream<res_axi_t> &rgm_in,     // AXIS : {signal, valid, r12}
    dec_rec_t              *decisions,  // m_axi : host output buffer
    int                     n_ticks,    // AXI-L : records to write  (decided)
    int                     n_events    // AXI-L : beats to read     (all events)
);

// n_events is NEW and is appended deliberately. Adding an argument anywhere
// earlier would shift every later register offset, and the host writes those
// offsets by hand - a silent shift of that kind once produced a run that looked
// plausible and was entirely wrong. Appended, every existing offset is
// unchanged and only the new one has to be confirmed against argdump.

} // extern "C"
