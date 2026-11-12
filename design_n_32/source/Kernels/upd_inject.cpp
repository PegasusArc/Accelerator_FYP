#include "upd_inject.h"

extern "C" {

void upd_inject(
    const upd_rec_t        *updates,
    int                     n_ticks,
    hls::stream<upd_axi_t> &upd_out
) {
#pragma HLS interface m_axi     port=updates offset=slave bundle=gmem_in depth=1024
#pragma HLS interface s_axilite port=updates bundle=control
#pragma HLS interface s_axilite port=n_ticks bundle=control
#pragma HLS interface axis      port=upd_out
#pragma HLS interface s_axilite port=return  bundle=control

    for (int t = 0; t < n_ticks; t++) {
#pragma HLS pipeline II=1
        upd_rec_t r = updates[t];

        // Same bit layout Option A packed into its AXIS beat.
        ap_uint<32> w0 = r.w0, w1 = r.w1, w2 = r.w2, w3 = r.w3, w4 = r.w4;

        upd_axi_t pkt;
        pkt.data = 0;
        pkt.data.range(7, 0)     = w0.range(7, 0);     // msg_type
        pkt.data.range(15, 8)    = w0.range(15, 8);    // side
        pkt.data.range(47, 16)   = w1;                 // price raw (Q16.16)
        pkt.data.range(79, 48)   = w2;                 // qty
        pkt.data.range(111, 80)  = w3;                 // ref low
        pkt.data.range(143, 112) = w4;                 // ref high
        pkt.keep = -1;
        pkt.strb = -1;
        pkt.last = (t == n_ticks - 1) ? 1 : 0;

        upd_out.write(pkt);
    }
}

} // extern "C"
