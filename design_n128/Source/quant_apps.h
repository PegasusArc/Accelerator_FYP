// ─────────────────────────────────────────────────────────────────────────────
// quant_apps.h  (Option B revision)
//
// Maths identical to Offer 2. Interface change only: each app now loops over the
// whole batch (n_ticks) in one invocation and emits one res_axi_t per tick to
// the decision_sink instead of writing s_axilite readback registers. EMA / slow-
// EMA state that Option A externalised through the host now lives in a local
// carried across the batch loop — correct because the app runs once per batch.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "qf_types.h"

extern "C" {

void momentum_signal(
    hls::stream<sv_axi_t>  &St_in,
    hls::stream<sv_axi_t>  &proj_in,
    int                     n_ticks,
    fixed_t                 alpha,
    fixed_t                 dev_hi,
    fixed_t                 dev_lo,
    fixed_t                 energy_min,
    hls::stream<res_axi_t> &res_out      // {signal, S1}
);

void risk_matrix(
    hls::stream<sv_axi_t>  &proj_in,
    hls::stream<sv_axi_t>  &St_in,
    int                     n_ticks,
    fixed_t                 risk_thresh,
    fixed_t                 eps_in,
    ap_uint<1>              ref_mode,
    fixed_t                 lambda_slow,
    hls::stream<res_axi_t> &res_out      // {signal, rho}
);

void regime_classifier(
    hls::stream<sv_axi_t>  &St_in,
    int                     n_ticks,
    fixed_t                 trend_ratio_thresh,
    fixed_t                 vol_thresh,
    fixed_t                 eps_in,
    hls::stream<res_axi_t> &res_out      // {signal, valid, r12}
);

} // extern "C"
