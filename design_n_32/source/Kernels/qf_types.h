// ─────────────────────────────────────────────────────────────────────────────
// qf_types.h — shared types for the Quant Finance Accelerator
// Option B revision: adds the per-tick result / meta AXIS beats and pulls in the
// shared DMA record layouts (qf_shared.h). Everything above the "Option B" band
// is identical to the Offer-2 file.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "ap_fixed.h"
#include "ap_int.h"
#include "ap_axi_sdata.h"
#include "hls_stream.h"
#include "qf_shared.h"      // upd_rec_t / dec_rec_t (POD, host-safe)

// ─── Design parameters ───────────────────────────────────────────────────────
#define N1        10
#define K          4
#define N3         2
#define LOB_DEPTH 10

// ─── Core numeric type ───────────────────────────────────────────────────────
typedef ap_fixed<24, 12, AP_RND, AP_SAT> fixed_t;
typedef ap_fixed<40, 20, AP_RND, AP_SAT> acc_t;

typedef ap_fixed<32, 16, AP_RND, AP_SAT> price_t;
typedef ap_uint<32> qty_t;
typedef ap_uint<64> ref_t;

typedef ap_uint<2> signal_t;

// ─── Message / regime codes ──────────────────────────────────────────────────
#define MSG_ADD     0x01
#define MSG_DELETE  0x02
#define MSG_EXECUTE 0x03
#define MSG_REPLACE 0x04

#define SIDE_BID    0x00
#define SIDE_ASK    0x01

#define REGIME_TRENDING 0
#define REGIME_MEANREV  1
#define REGIME_VOLATILE 2

// ─── Packed AXI-Stream payloads ──────────────────────────────────────────────
#define SV_BUS_W  (K * 32)
#define BT_BUS_W  (N1 * N3 * 32)
#define UPD_BUS_W 192

typedef ap_axiu<SV_BUS_W,  0, 0, 0> sv_axi_t;
typedef ap_axiu<BT_BUS_W,  0, 0, 0> bt_axi_t;
typedef ap_axiu<UPD_BUS_W, 0, 0, 0> upd_axi_t;

// ─── Bit-level pack / unpack helpers ─────────────────────────────────────────
static inline ap_uint<32> fx_pack(fixed_t v) {
#pragma HLS inline
    ap_int<24> raw = v.range(23, 0);
    ap_int<32> ext = raw;
    return (ap_uint<32>)ext;
}
static inline fixed_t fx_unpack(ap_uint<32> w) {
#pragma HLS inline
    fixed_t v;
    v.range(23, 0) = w.range(23, 0);
    return v;
}
static inline void sv_pack(const fixed_t v[K], sv_axi_t &pkt) {
#pragma HLS inline
    for (int j = 0; j < K; j++) {
#pragma HLS unroll
        pkt.data.range(32 * j + 31, 32 * j) = fx_pack(v[j]);
    }
    pkt.keep = -1; pkt.strb = -1; pkt.last = 1;
}
static inline void sv_unpack(const sv_axi_t &pkt, fixed_t v[K]) {
#pragma HLS inline
    for (int j = 0; j < K; j++) {
#pragma HLS unroll
        v[j] = fx_unpack(pkt.data.range(32 * j + 31, 32 * j));
    }
}
static inline void bt_pack(const fixed_t m[N1][N3], bt_axi_t &pkt) {
#pragma HLS inline
    for (int i = 0; i < N1; i++) {
#pragma HLS unroll
        for (int s = 0; s < N3; s++) {
#pragma HLS unroll
            int lane = i * N3 + s;
            pkt.data.range(32 * lane + 31, 32 * lane) = fx_pack(m[i][s]);
        }
    }
    pkt.keep = -1; pkt.strb = -1; pkt.last = 1;
}
static inline void bt_unpack(const bt_axi_t &pkt, fixed_t m[N1][N3]) {
#pragma HLS inline
    for (int i = 0; i < N1; i++) {
#pragma HLS unroll
        for (int s = 0; s < N3; s++) {
#pragma HLS unroll
            int lane = i * N3 + s;
            m[i][s] = fx_unpack(pkt.data.range(32 * lane + 31, 32 * lane));
        }
    }
}

// ─── LOB update, packed into 192 bits ────────────────────────────────────────
struct LOBUpdate {
    ap_uint<8> msg_type;
    ap_uint<8> side;
    price_t    price;
    qty_t      qty;
    ref_t      ref_num;
};
static inline void upd_pack(const LOBUpdate &u, upd_axi_t &pkt) {
#pragma HLS inline
    pkt.data = 0;
    pkt.data.range(7, 0)     = u.msg_type;
    pkt.data.range(15, 8)    = u.side;
    pkt.data.range(47, 16)   = u.price.range(31, 0);
    pkt.data.range(79, 48)   = u.qty;
    pkt.data.range(143, 80)  = u.ref_num;
    pkt.keep = -1; pkt.strb = -1; pkt.last = 1;
}
static inline LOBUpdate upd_unpack(const upd_axi_t &pkt) {
#pragma HLS inline
    LOBUpdate u;
    u.msg_type = pkt.data.range(7, 0);
    u.side     = pkt.data.range(15, 8);
    u.price.range(31, 0) = pkt.data.range(47, 16);
    u.qty      = pkt.data.range(79, 48);
    u.ref_num  = pkt.data.range(143, 80);
    return u;
}

// ═════════════════════════════════════════════════════════════════════════════
// Option B additions
// ═════════════════════════════════════════════════════════════════════════════
//
// res_axi_t  — one per tick, produced by momentum/risk/regime, read by the sink.
//   [1:0]   = signal (0 HOLD / 1 BUY / 2 SELL)
//   [2]     = valid  (regime only; momentum/risk set 1)
//   [63:32] = value  (fx_pack: S1 for momentum, rho for risk, r12 for regime)
#define RES_BUS_W 64
typedef ap_axiu<RES_BUS_W, 0, 0, 0> res_axi_t;

static inline void res_pack(signal_t sig, ap_uint<1> valid, fixed_t value, res_axi_t &pkt) {
#pragma HLS inline
    pkt.data = 0;
    pkt.data.range(1, 0)   = sig;
    pkt.data.range(2, 2)   = valid;
    pkt.data.range(63, 32) = fx_pack(value);
    pkt.keep = -1; pkt.strb = -1; pkt.last = 1;
}
static inline void res_unpack(const res_axi_t &pkt, signal_t &sig, ap_uint<1> &valid, fixed_t &value) {
#pragma HLS inline
    sig   = pkt.data.range(1, 0);
    valid = pkt.data.range(2, 2);
    value = fx_unpack(pkt.data.range(63, 32));
}

// meta_axi_t — one per tick, produced by lob_filter, read by the sink.
//   [0] = book_ready (both sides present)
#define META_BUS_W 32
typedef ap_axiu<META_BUS_W, 0, 0, 0> meta_axi_t;

static inline void meta_pack(ap_uint<1> ready, meta_axi_t &pkt) {
#pragma HLS inline
    pkt.data = 0;
    pkt.data.range(0, 0) = ready;
    pkt.keep = -1; pkt.strb = -1; pkt.last = 1;
}
