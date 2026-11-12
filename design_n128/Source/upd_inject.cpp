#include "upd_inject.h"

extern "C" {

void upd_inject(
    const upd_rec_t        *updates,
    int                     n_ticks,
    hls::stream<upd_axi_t> &upd_out
) {
// The read side was never the bottleneck - cost in this design attaches to
// decisions, not events, and only the write side is per-decision. These two
// options are margin, not a fix: reads carry no byte strobes and consecutive
// upd_rec_t share a 512-bit beat harmlessly, so the adapter already pipelines
// them. Once decision_sink stops being the drain the feeder becomes the next
// thing to look at, and a deeper read pipeline costs nothing to have in place.
#pragma HLS interface m_axi     port=updates offset=slave bundle=gmem_in depth=1024 \
    max_read_burst_length=64 num_read_outstanding=32
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
        // Which instrument this update belongs to. The host interleaves the
        // records in round-robin order and stamps this field; the feeder just
        // carries it through to the stream.
        pkt.id   = (ap_uint<8>)(r.asset & 0xFF);
        pkt.keep = -1;
        pkt.strb = -1;
        pkt.last = (t == n_ticks - 1) ? 1 : 0;

        upd_out.write(pkt);
    }
}

} // extern "C"
