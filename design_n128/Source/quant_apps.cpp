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

    // ROUND ROBIN: one EMA per instrument.
    fixed_t ema_all[NASSET];
#pragma HLS bind_storage variable=ema_all type=RAM_2P impl=BRAM

// Consecutive ticks belong to DIFFERENT instruments, so the state one tick
// writes is not the state the next tick reads. Instrument a is only revisited
// every NASSET ticks. The compiler cannot work this out on its own, because the
// index arrives at runtime, so it assumes the worst case and refuses to overlap
// the ticks - which is why the initiation interval stayed at 116 regardless of
// NASSET.
//
// This pragma states the fact. IT IS A PROMISE, NOT A HINT: it holds only while
// the host feeds instruments in strict round-robin order. If that ordering is
// broken the design will read stale state and report nothing wrong.
    HLS_DEP_DISTANCE(ema_all, NASSET)
    for (int a = 0; a < NASSET; a++) ema_all[a] = (fixed_t)0;

    for (int t = 0; t < n_ticks; t++) {
        fixed_t St[K], pm[K];
#pragma HLS array_partition variable=St complete dim=1
#pragma HLS array_partition variable=pm complete dim=1

        sv_axi_t st_beat = St_in.read();
        sv_unpack(st_beat,        St);
        sv_unpack(proj_in.read(), pm);

        ap_uint<8> aid = st_beat.id;
        ap_uint<1> vld = st_beat.user;   // real decision, or a spacing beat
        int        a   = (NASSET == 1) ? 0 : (int)(aid % NASSET);
        fixed_t    ema = ema_all[a];

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

        // Write the moving average back for this instrument. Without this the
        // stored value stays at its initial zero and every tick computes
        // alpha*S1 instead of a running average, so the deviation - and the
        // vote - is wrong from the second tick onward. The other two stateful
        // blocks (risk's mbar, the SVD's Ut/St) already do this; momentum was
        // the one that did not.
        //
        // Only on a real decision. lob_filter now emits every event so the
        // rotation reaching this block stays exactly NASSET-spaced whatever
        // the feed drops; a beat that carried no tick matrix takes its slot
        // but must not move the average.
        if (vld) ema_all[a] = ema;

        res_axi_t pkt;
        res_pack(sig, (ap_uint<1>)1, S1, pkt);
        pkt.id   = aid;                   // carry the instrument forward
        pkt.user = vld;
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

    // ROUND ROBIN: one slow reference per instrument. Inner dimension is
    // partitioned for single-cycle access; the asset dimension stays in BRAM.
    fixed_t mbar_all[NASSET][K];
#pragma HLS array_partition variable=mbar_all complete dim=2
#pragma HLS bind_storage variable=mbar_all type=RAM_2P impl=BRAM

// Consecutive ticks belong to DIFFERENT instruments, so the state one tick
// writes is not the state the next tick reads. Instrument a is only revisited
// every NASSET ticks. The compiler cannot work this out on its own, because the
// index arrives at runtime, so it assumes the worst case and refuses to overlap
// the ticks - which is why the initiation interval stayed at 116 regardless of
// NASSET.
//
// This pragma states the fact. IT IS A PROMISE, NOT A HINT: it holds only while
// the host feeds instruments in strict round-robin order. If that ordering is
// broken the design will read stale state and report nothing wrong.
    HLS_DEP_DISTANCE(mbar_all, NASSET)
    ap_uint<1> seeded[NASSET];
    for (int a = 0; a < NASSET; a++) seeded[a] = 0;

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
        sv_axi_t st_beat = St_in.read();
        sv_unpack(st_beat,        St);
        sv_unpack(proj_in.read(), pm);

        ap_uint<8> aid = st_beat.id;
        ap_uint<1> vld = st_beat.user;   // real decision, or a spacing beat
        int        a   = (NASSET == 1) ? 0 : (int)(aid % NASSET);
        fixed_t mbar[K];
#pragma HLS array_partition variable=mbar complete dim=1
        for (int i2 = 0; i2 < K; i2++) {
#pragma HLS unroll
            mbar[i2] = mbar_all[a][i2];
        }

        acc_t eps   = (acc_t)eps_in;
        acc_t trace = 0;

        if (!seeded[a]) {                      // seed this instrument's reference
            if (vld) seeded[a] = 1;            // but only off a real decision
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

        // OPTIMISED. The original walked the full 4x4 outer product and then
        // discarded everything except the diagonal:
        //
        //     for i, for j:  c = pm[i]*pm[j] / (ref[i]*ref[j])
        //                    if (i == j) trace += c
        //
        // Sixteen divisions were issued and twelve of the results thrown away.
        // Division is the most expensive operator here and it is what set the
        // initiation interval, so removing three quarters of them is the whole
        // optimisation. Only the diagonal is ever used.
        //
        // Note the guard is applied to the SQUARED denominator, exactly as the
        // original did. Guarding ref[i] and then squaring is NOT the same thing:
        // it makes the effective floor eps^2 rather than eps, and the two forms
        // then differ by a factor of 1/eps wherever the guard fires. That was
        // caught by verify_equiv.py before this was built - worth keeping in
        // mind if anyone is tempted to simplify further.
        //
        // As written, the result is bit-identical to the original for every
        // input, including inside the guard region.
        for (int i = 0; i < K; i++) {
#pragma HLS unroll
            acc_t denom = (acc_t)ref[i] * (acc_t)ref[i];
            acc_t safe  = (denom < eps && denom > -eps) ? eps : denom;
            trace += ((acc_t)pm[i] * (acc_t)pm[i]) / safe;
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
        if (vld) {                            // hand the state back, only on a
            for (int i2 = 0; i2 < K; i2++) {  // real decision
#pragma HLS unroll
                mbar_all[a][i2] = mbar[i2];
            }
        }
        res_pack(sig, (ap_uint<1>)1, rho, pkt);
        pkt.id   = aid;
        pkt.user = vld;
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
        sv_axi_t st_beat = St_in.read();
        sv_unpack(st_beat, St);
        ap_uint<8> aid = st_beat.id;     // stateless block, just forwards the id

        acc_t eps = (acc_t)eps_in;

        bool ok = ((acc_t)St[1] > eps) && ((acc_t)St[2] > eps);

        acc_t d1 = ((acc_t)St[1] > eps) ? (acc_t)St[1] : eps;
        acc_t d2 = ((acc_t)St[2] > eps) ? (acc_t)St[2] : eps;

        // r12 is needed as a value because it is reported in the result packet.
        // r23 is only ever compared against 1.0, and a > b is the same test as
        // a/b > 1 for positive b - so that division is removed entirely. St is
        // non-negative by construction (they are singular values, confirmed
        // 0.0% negative across all validation runs), so the comparison is safe.
        acc_t r12 = (acc_t)St[0] / d1;
        bool  r23_below_one = ((acc_t)St[1] < d2);

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
            sig = r23_below_one ? (signal_t)2 : (signal_t)1;       // MEAN-REV
        }

        res_axi_t pkt;
        res_pack(sig, ok ? (ap_uint<1>)1 : (ap_uint<1>)0, (fixed_t)r12, pkt);
        pkt.id   = aid;
        pkt.user = st_beat.user;          // stateless: nothing to hold back
        res_out.write(pkt);
    }
}

} // extern "C"
