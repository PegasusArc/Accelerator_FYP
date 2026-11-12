// ─────────────────────────────────────────────────────────────────────────────
// sv_bcast.cpp — 1-to-N stream broadcasters, Vitis kernel flow
//
// v++ --connectivity.stream_connect is strictly point-to-point, so nothing in
// the Vitis link step can fan one stream out to several consumers. These two
// tiny kernels do that replication in fabric. The SVD's St_out feeds all three
// apps (bcast3) and its proj_out feeds momentum + risk (bcast2).
//
// Each is one beat in, N beats out, no state. They add one cycle of latency
// per branch and cost ~200 LUT total.
//
// NOTE the ap_ctrl_chain requirement: in the Vitis kernel flow a free-running
// stream kernel must be declared ap_ctrl_none so it does not need the host to
// ap_start it every tick. Both are declared that way; the host never touches
// them, and they simply pump whatever arrives.
//
// ON CONTEXT INTERLEAVING: these kernels need no modification. Under round-robin
// each beat carries its instrument in the AXI-Stream TID field, and reading a
// beat into sv_axi_t copies the whole ap_axiu struct - payload, keep, strb, last
// AND id. Writing it out again carries the identity with it, so the broadcasters
// are transparent to instrument identity by construction. Do not "fix" this by
// adding explicit id assignments; there is nothing to fix.
//
// They MUST however be recompiled whenever qf_types.h changes. sv_axi_t was
// ap_axiu<SV_BUS_W,0,0,0> before interleaving and is ap_axiu<SV_BUS_W,0,8,0>
// after, which is eight bits wider on the sideband. A stale .xo built against
// the old header will not agree with its producer or consumer at link time.
// ─────────────────────────────────────────────────────────────────────────────
#include "qf_types.h"

extern "C" {

void sv_bcast3(
    hls::stream<sv_axi_t> &in,
    hls::stream<sv_axi_t> &out0,
    hls::stream<sv_axi_t> &out1,
    hls::stream<sv_axi_t> &out2
) {
#pragma HLS interface axis port=in
#pragma HLS interface axis port=out0
#pragma HLS interface axis port=out1
#pragma HLS interface axis port=out2
#pragma HLS interface ap_ctrl_none port=return
#pragma HLS pipeline II=1

    sv_axi_t b = in.read();
    out0.write(b);
    out1.write(b);
    out2.write(b);
}

void sv_bcast2(
    hls::stream<sv_axi_t> &in,
    hls::stream<sv_axi_t> &out0,
    hls::stream<sv_axi_t> &out1
) {
#pragma HLS interface axis port=in
#pragma HLS interface axis port=out0
#pragma HLS interface axis port=out1
#pragma HLS interface ap_ctrl_none port=return
#pragma HLS pipeline II=1

    sv_axi_t b = in.read();
    out0.write(b);
    out1.write(b);
}

} // extern "C"
