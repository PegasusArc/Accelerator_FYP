#include "decision_sink.h"

extern "C" {

void decision_sink(
    hls::stream<res_axi_t> &mom_in,
    hls::stream<res_axi_t> &rsk_in,
    hls::stream<res_axi_t> &rgm_in,
    dec_rec_t              *decisions,
    int                     n_ticks
) {
#pragma HLS interface axis      port=mom_in
#pragma HLS interface axis      port=rsk_in
#pragma HLS interface axis      port=rgm_in
#pragma HLS interface m_axi     port=decisions offset=slave bundle=gmem_out depth=1024
#pragma HLS interface s_axilite port=decisions bundle=control
#pragma HLS interface s_axilite port=n_ticks   bundle=control
#pragma HLS interface s_axilite port=return    bundle=control

    for (int t = 0; t < n_ticks; t++) {
#pragma HLS pipeline II=1
        signal_t ms, rs, gs;
        ap_uint<1> mv, rv, gv;
        fixed_t s1, rho, r12;

        res_unpack(mom_in.read(), ms, mv, s1);
        res_unpack(rsk_in.read(), rs, rv, rho);
        res_unpack(rgm_in.read(), gs, gv, r12);

        // Pure 2-of-3 majority — identical to golden::vote. regime-valid is
        // recorded for the host trace but does NOT gate the vote.
        int b = (ms == 1) + (rs == 1) + (gs == 1);
        int s = (ms == 2) + (rs == 2) + (gs == 2);
        ap_uint<2> final_sig = (b >= 2) ? (ap_uint<2>)1
                             : (s >= 2) ? (ap_uint<2>)2
                                        : (ap_uint<2>)0;

        dec_rec_t d;
        d.ready     = 1;                       // every emitted tick is decided
        d.final_sig = (uint32_t)final_sig;
        d.mom_sig   = (uint32_t)ms;
        d.rsk_sig   = (uint32_t)rs;
        d.rgm_sig   = (uint32_t)gs;
        d.rgm_valid = (uint32_t)gv;
        d.s1        = (int32_t)(uint32_t)fx_pack(s1);
        d.rho       = (int32_t)(uint32_t)fx_pack(rho);
        d.r12       = (int32_t)(uint32_t)fx_pack(r12);
        d.pad       = 0;

        decisions[t] = d;
    }
}

} // extern "C"
