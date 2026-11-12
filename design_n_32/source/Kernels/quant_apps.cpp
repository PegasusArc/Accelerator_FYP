#include "quant_apps.h"

extern "C" {

// ─────────────────────────────────────────────────────────────────────────────
// App 1: Momentum Signal
//   ema carried across the batch loop (was externalised via the host in Opt A)
// ─────────────────────────────────────────────────────────────────────────────
void momentum_signal(
    hls::stream<sv_axi_t>  &St_in,
    hls::stream<sv_axi_t>  &proj_in,
    int                     n_ticks,
    fixed_t                 alpha,
    fixed_t                 dev_hi,
    fixed_t                 dev_lo,
    fixed_t                 energy_min,
    hls::stream<res_axi_t> &res_out
) {
#pragma HLS interface axis      port=St_in
#pragma HLS interface axis      port=proj_in
#pragma HLS interface axis      port=res_out
#pragma HLS interface s_axilite port=n_ticks    bundle=control
#pragma HLS interface s_axilite port=alpha      bundle=control
#pragma HLS interface s_axilite port=dev_hi     bundle=control
#pragma HLS interface s_axilite port=dev_lo     bundle=control
#pragma HLS interface s_axilite port=energy_min bundle=control
#pragma HLS interface s_axilite port=return     bundle=control

    fixed_t ema = (fixed_t)0;                 // batch-persistent EMA state

    for (int t = 0; t < n_ticks; t++) {
        fixed_t St[K], pm[K];
#pragma HLS array_partition variable=St complete dim=1
#pragma HLS array_partition variable=pm complete dim=1

        sv_unpack(St_in.read(),   St);
        sv_unpack(proj_in.read(), pm);

        fixed_t S1 = St[0];

        acc_t e = (acc_t)alpha * (acc_t)S1
                + ((acc_t)1.0 - (acc_t)alpha) * (acc_t)ema;
        ema = (fixed_t)e;

        acc_t energy = 0;
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            energy += (acc_t)pm[j];
        }

        fixed_t dev = (fixed_t)((acc_t)S1 - (acc_t)ema);

        signal_t sig;
        if (dev > dev_hi && energy > (acc_t)energy_min) sig = 1;   // BUY
        else if (dev < dev_lo)                          sig = 2;   // SELL
        else                                            sig = 0;   // HOLD

        res_axi_t pkt;
        res_pack(sig, (ap_uint<1>)1, S1, pkt);
        res_out.write(pkt);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// App 2: Portfolio Risk Matrix
//   mbar (slow EMA reference) carried across the batch loop; seeded on t==0
// ─────────────────────────────────────────────────────────────────────────────
void risk_matrix(
    hls::stream<sv_axi_t>  &proj_in,
    hls::stream<sv_axi_t>  &St_in,
    int                     n_ticks,
    fixed_t                 risk_thresh,
    fixed_t                 eps_in,
    ap_uint<1>              ref_mode,
    fixed_t                 lambda_slow,
    hls::stream<res_axi_t> &res_out
) {
#pragma HLS interface axis      port=proj_in
#pragma HLS interface axis      port=St_in
#pragma HLS interface axis      port=res_out
#pragma HLS interface s_axilite port=n_ticks     bundle=control
#pragma HLS interface s_axilite port=risk_thresh bundle=control
#pragma HLS interface s_axilite port=eps_in      bundle=control
#pragma HLS interface s_axilite port=ref_mode    bundle=control
#pragma HLS interface s_axilite port=lambda_slow bundle=control
#pragma HLS interface s_axilite port=return      bundle=control

    fixed_t mbar[K];
#pragma HLS array_partition variable=mbar complete dim=1

    for (int t = 0; t < n_ticks; t++) {
        fixed_t pm[K], St[K];
#pragma HLS array_partition variable=pm complete dim=1
#pragma HLS array_partition variable=St complete dim=1

        // ORDER MATTERS: read St BEFORE proj, matching momentum_signal and the
        // order inctsvd_top produces them (St_out then proj_out each tick).
        // Reading proj first deadlocks on hardware: risk would not drain its St
        // FIFO, sv_bcast3 would block, inctsvd_top would stall on its St_out
        // write and never emit proj — so risk would wait forever for the proj
        // beat it needs. C-simulation cannot see this because hls::stream is
        // unbounded there; it only bites with real, bounded FIFOs.
        sv_unpack(St_in.read(),   St);
        sv_unpack(proj_in.read(), pm);

        acc_t eps   = (acc_t)eps_in;
        acc_t trace = 0;

        if (t == 0) {                          // seed slow EMA from the first tick
            for (int i = 0; i < K; i++) {
#pragma HLS unroll
                mbar[i] = pm[i];
            }
        }

        fixed_t ref[K];
#pragma HLS array_partition variable=ref complete dim=1
        for (int i = 0; i < K; i++) {
#pragma HLS unroll
            ref[i] = ref_mode ? mbar[i] : St[i];
        }

        for (int i = 0; i < K; i++) {
#pragma HLS unroll
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                acc_t denom = (acc_t)ref[i] * (acc_t)ref[j];
                acc_t safe  = (denom < eps && denom > -eps) ? eps : denom;
                acc_t c     = ((acc_t)pm[i] * (acc_t)pm[j]) / safe;
                if (i == j) trace += c;
            }
        }

        for (int i = 0; i < K; i++) {
#pragma HLS unroll
            mbar[i] = (fixed_t)((acc_t)lambda_slow * (acc_t)mbar[i]
                              + ((acc_t)1.0 - (acc_t)lambda_slow) * (acc_t)pm[i]);
        }

        fixed_t rho = (fixed_t)trace;

        signal_t sig;
        if (rho > risk_thresh)                                     sig = 2;  // SELL
        else if (rho < (fixed_t)((acc_t)risk_thresh * (acc_t)0.3)) sig = 1;  // BUY
        else                                                       sig = 0;  // HOLD

        res_axi_t pkt;
        res_pack(sig, (ap_uint<1>)1, rho, pkt);
        res_out.write(pkt);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// App 3: Market Regime Classifier  (stateless per tick)
// ─────────────────────────────────────────────────────────────────────────────
void regime_classifier(
    hls::stream<sv_axi_t>  &St_in,
    int                     n_ticks,
    fixed_t                 trend_ratio_thresh,
    fixed_t                 vol_thresh,
    fixed_t                 eps_in,
    hls::stream<res_axi_t> &res_out
) {
#pragma HLS interface axis      port=St_in
#pragma HLS interface axis      port=res_out
#pragma HLS interface s_axilite port=n_ticks            bundle=control
#pragma HLS interface s_axilite port=trend_ratio_thresh bundle=control
#pragma HLS interface s_axilite port=vol_thresh         bundle=control
#pragma HLS interface s_axilite port=eps_in             bundle=control
#pragma HLS interface s_axilite port=return             bundle=control

    for (int t = 0; t < n_ticks; t++) {
        fixed_t St[K];
#pragma HLS array_partition variable=St complete dim=1
        sv_unpack(St_in.read(), St);

        acc_t eps = (acc_t)eps_in;

        bool ok = ((acc_t)St[1] > eps) && ((acc_t)St[2] > eps);

        acc_t d1 = ((acc_t)St[1] > eps) ? (acc_t)St[1] : eps;
        acc_t d2 = ((acc_t)St[2] > eps) ? (acc_t)St[2] : eps;

        acc_t r12 = (acc_t)St[0] / d1;
        acc_t r23 = (acc_t)St[1] / d2;

        acc_t total = 0;
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            total += (acc_t)St[j];
        }

        signal_t sig;
        if (r12 > (acc_t)trend_ratio_thresh) {
            sig = 1;                            // TRENDING -> follow
        } else if (total > (acc_t)vol_thresh) {
            sig = 0;                            // VOLATILE -> stand down
        } else {
            sig = (r23 < (acc_t)1.0) ? (signal_t)2 : (signal_t)1;  // MEAN-REV
        }

        res_axi_t pkt;
        res_pack(sig, ok ? (ap_uint<1>)1 : (ap_uint<1>)0, (fixed_t)r12, pkt);
        res_out.write(pkt);
    }
}

} // extern "C"
