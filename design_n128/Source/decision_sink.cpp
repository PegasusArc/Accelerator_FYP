#include "decision_sink.h"

// How many decisions are staged on-chip before being pushed to HBM as one
// burst. 64 records at 64 bytes is a 64-beat 512-bit burst, which is the
// longest single AXI burst the protocol allows and also the default cap on
// max_write_burst_length. Raising it further does nothing.
#define DS_BURST 64

extern "C" {

void decision_sink(
    hls::stream<res_axi_t> &mom_in,
    hls::stream<res_axi_t> &rsk_in,
    hls::stream<res_axi_t> &rgm_in,
    dec_rec_t              *decisions,
    int                     n_ticks,       // records to WRITE  (decided ticks)
    int                     n_events       // beats to READ     (every event)
) {
#pragma HLS interface axis      port=mom_in
#pragma HLS interface axis      port=rsk_in
#pragma HLS interface axis      port=rgm_in
#pragma HLS interface m_axi     port=decisions offset=slave bundle=gmem_out depth=1024 \
    max_write_burst_length=64 num_write_outstanding=32
#pragma HLS interface s_axilite port=decisions bundle=control
#pragma HLS interface s_axilite port=n_ticks   bundle=control
#pragma HLS interface s_axilite port=n_events  bundle=control
#pragma HLS interface s_axilite port=return    bundle=control

    // Every event now reaches this block, because lob_filter emits on all of
    // them to keep the rotation exact. Only the ones whose book was two-sided
    // carry TUSER set, and only those become records. So the loop reads
    // n_events beats and writes at most n_ticks records, and the two differ by
    // however many ticks the feed left one-sided.
    dec_rec_t buf[DS_BURST];
#pragma HLS bind_storage variable=buf type=RAM_2P impl=BRAM

    int base = 0;      // beats consumed
    int wr   = 0;      // records written

CHUNK:
    while (base < n_events) {
#pragma HLS loop_tripcount min=1 max=4096
        int chunk = (n_events - base < DS_BURST) ? (n_events - base) : DS_BURST;
        int nval  = 0;

    FILL:
        for (int i = 0; i < chunk; i++) {
#pragma HLS loop_tripcount min=1 max=64
#pragma HLS pipeline II=1
            signal_t ms, rs, gs;
            ap_uint<1> mv, rv, gv;
            fixed_t s1, rho, r12;

            // All three beats for a given tick carry the same instrument id and
            // the same validity flag, since they came from the same broadcast.
            // Take both from the first.
            res_axi_t mom_beat = mom_in.read();
            res_unpack(mom_beat,      ms, mv, s1);
            res_unpack(rsk_in.read(), rs, rv, rho);
            res_unpack(rgm_in.read(), gs, gv, r12);
            ap_uint<8> aid = mom_beat.id;
            ap_uint<1> vld = mom_beat.user;

            // Pure 2-of-3 majority — identical to golden::vote. regime-valid is
            // recorded for the host trace but does NOT gate the vote.
            int b = (ms == 1) + (rs == 1) + (gs == 1);
            int s = (ms == 2) + (rs == 2) + (gs == 2);
            ap_uint<2> final_sig = (b >= 2) ? (ap_uint<2>)1
                                 : (s >= 2) ? (ap_uint<2>)2
                                            : (ap_uint<2>)0;

            dec_rec_t d;
            d.ready     = 1;                       // every RECORDED tick is decided
            d.final_sig = (uint32_t)final_sig;
            d.mom_sig   = (uint32_t)ms;
            d.rsk_sig   = (uint32_t)rs;
            d.rgm_sig   = (uint32_t)gs;
            d.rgm_valid = (uint32_t)gv;
            d.s1        = (int32_t)(uint32_t)fx_pack(s1);
            d.rho       = (int32_t)(uint32_t)fx_pack(rho);
            d.r12       = (int32_t)(uint32_t)fx_pack(r12);
            d.asset     = (int32_t)(uint32_t)aid;
            // Zero the padding so a byte-for-byte memcmp between two runs
            // compares decisions and not leftover BRAM contents.
            for (int p = 0; p < 6; p++) {
#pragma HLS unroll
                d.pad[p] = 0;
            }

            // Spacing beats are consumed and dropped here. They have done their
            // job by occupying a slot in the rotation upstream.
            if (vld) { buf[nval] = d; nval++; }
        }

        // Never write past the host's buffer, whatever the counts say. An
        // earlier version of this pipeline trusted a host-side model of the
        // book, and when that model was off by a few hundred the kernel wrote
        // beyond the allocation and the whole design deadlocked with no
        // indication of why. Clamping here turns that failure into short output
        // the host can see and report.
        int room = n_ticks - wr;
        int emit = (nval < room) ? nval : room;

        // Contiguous, monotonically increasing index over a power-of-two
        // record: this is the shape Vitis HLS needs in order to infer a burst.
    DRAIN:
        for (int i = 0; i < emit; i++) {
#pragma HLS loop_tripcount min=1 max=64
#pragma HLS pipeline II=1
            decisions[wr + i] = buf[i];
        }

        wr   += emit;
        base += chunk;
    }
}

} // extern "C"
