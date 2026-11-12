// ─────────────────────────────────────────────────────────────────────────────
// upd_inject.h  (Option B revision)
//
// Was: register-fed shim, one AXIS beat per ap_start (Option A).
// Now: DMA feeder. Reads n_ticks packed updates from a host buffer over m_axi
// and streams them out as AXIS beats. Runs once per batch; no per-tick host
// interaction, which is what removes the CU-restart dependency entirely.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "qf_types.h"

extern "C" {

void upd_inject(
    const upd_rec_t        *updates,   // m_axi : host input buffer, n_ticks records
    int                     n_ticks,   // AXI-L : batch length
    hls::stream<upd_axi_t> &upd_out    // AXIS  : to lob_filter
);

} // extern "C"
