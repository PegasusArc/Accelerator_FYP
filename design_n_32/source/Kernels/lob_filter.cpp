#include "lob_filter.h"

#define BOOK_N 5

struct OBEntry {
    price_t price;
    qty_t   qty;
    ref_t   ref_num;
    bool    valid;
};

// ─── Lookup / order helpers (unchanged from Offer 2) ─────────────────────────
static int find_ref(OBEntry book[BOOK_N], ref_t ref) {
#pragma HLS inline
    int hit = -1;
    for (int i = 0; i < BOOK_N; i++) {
#pragma HLS unroll
        if (book[i].valid && book[i].ref_num == ref) hit = i;
    }
    return hit;
}
static int find_empty(OBEntry book[BOOK_N]) {
#pragma HLS inline
    int slot = -1;
    for (int i = BOOK_N - 1; i >= 0; i--) {
#pragma HLS unroll
        if (!book[i].valid) slot = i;
    }
    return slot;
}
static bool bid_ahead(const OBEntry &a, const OBEntry &b) {
#pragma HLS inline
    if (!a.valid) return false;
    if (!b.valid) return true;
    return a.price > b.price;
}
static bool ask_ahead(const OBEntry &a, const OBEntry &b) {
#pragma HLS inline
    if (!a.valid) return false;
    if (!b.valid) return true;
    return a.price < b.price;
}
static void sort_bids(OBEntry book[BOOK_N]) {
#pragma HLS inline
    for (int i = 0; i < BOOK_N - 1; i++) {
#pragma HLS unroll
        for (int j = i + 1; j < BOOK_N; j++) {
#pragma HLS unroll
            if (bid_ahead(book[j], book[i])) { OBEntry t = book[i]; book[i] = book[j]; book[j] = t; }
        }
    }
}
static void sort_asks(OBEntry book[BOOK_N]) {
#pragma HLS inline
    for (int i = 0; i < BOOK_N - 1; i++) {
#pragma HLS unroll
        for (int j = i + 1; j < BOOK_N; j++) {
#pragma HLS unroll
            if (ask_ahead(book[j], book[i])) { OBEntry t = book[i]; book[i] = book[j]; book[j] = t; }
        }
    }
}
static int slot_for_insert(OBEntry book[BOOK_N]) {
#pragma HLS inline
    int e = find_empty(book);
    return (e >= 0) ? e : (BOOK_N - 1);
}
static void wipe(OBEntry book[BOOK_N]) {
#pragma HLS inline
    for (int i = 0; i < BOOK_N; i++) {
#pragma HLS unroll
        book[i].valid = false; book[i].price = (price_t)0;
        book[i].qty = (qty_t)0; book[i].ref_num = (ref_t)0;
    }
}

extern "C" {

void lob_filter(
    hls::stream<upd_axi_t> &upd_in,
    hls::stream<bt_axi_t>  &Bt_out,
    int                     n_ticks,
    price_t                 price_ref,
    fixed_t                 price_scale,
    fixed_t                 qty_scale,
    ap_uint<1>              unit_norm
) {
#pragma HLS interface axis      port=upd_in
#pragma HLS interface axis      port=Bt_out
#pragma HLS interface s_axilite port=n_ticks     bundle=control
#pragma HLS interface s_axilite port=price_ref   bundle=control
#pragma HLS interface s_axilite port=price_scale bundle=control
#pragma HLS interface s_axilite port=qty_scale   bundle=control
#pragma HLS interface s_axilite port=unit_norm   bundle=control
#pragma HLS interface s_axilite port=return      bundle=control

    // Batch-local books (were file-static in Option A). Wiped once at entry.
    OBEntry bid[BOOK_N], ask[BOOK_N];
#pragma HLS array_partition variable=bid complete dim=1
#pragma HLS array_partition variable=ask complete dim=1
    wipe(bid);
    wipe(ask);

    for (int t = 0; t < n_ticks; t++) {
        upd_axi_t pkt = upd_in.read();       // consume every update
        LOBUpdate u   = upd_unpack(pkt);
        bool is_bid   = (u.side == SIDE_BID);

        OBEntry work[BOOK_N];
#pragma HLS array_partition variable=work complete dim=1
        for (int i = 0; i < BOOK_N; i++) {
#pragma HLS unroll
            work[i] = is_bid ? bid[i] : ask[i];
        }

        switch (u.msg_type) {
            case MSG_ADD: {
                int slot = slot_for_insert(work);
                work[slot].price = u.price; work[slot].qty = u.qty;
                work[slot].ref_num = u.ref_num; work[slot].valid = true;
                break;
            }
            case MSG_DELETE: {
                int idx = find_ref(work, u.ref_num);
                if (idx >= 0) work[idx].valid = false;
                break;
            }
            case MSG_EXECUTE: {
                int idx = find_ref(work, u.ref_num);
                if (idx >= 0) {
                    if (work[idx].qty <= u.qty) work[idx].valid = false;
                    else                        work[idx].qty  -= u.qty;
                }
                break;
            }
            case MSG_REPLACE: {
                int idx = find_ref(work, u.ref_num);
                if (idx >= 0) work[idx].valid = false;
                int slot = slot_for_insert(work);
                work[slot].price = u.price; work[slot].qty = u.qty;
                work[slot].ref_num = u.ref_num; work[slot].valid = true;
                break;
            }
            default: break;
        }

        if (is_bid) sort_bids(work); else sort_asks(work);

        for (int i = 0; i < BOOK_N; i++) {
#pragma HLS unroll
            if (is_bid) bid[i] = work[i]; else ask[i] = work[i];
        }

        // Emit a tick matrix ONLY when the book is two-sided — the decided ticks.
        bool ready = (bid[0].valid && ask[0].valid);
        if (!ready) continue;

        // ── Build the tick matrix (centre + scale) ──────────────────────────
        fixed_t Bt[N1][N3];
#pragma HLS array_partition variable=Bt complete dim=0
        for (int i = 0; i < BOOK_N; i++) {
#pragma HLS unroll
            if (bid[i].valid) {
                // Centre FIRST, narrow SECOND. The previous form cast the
                // price to fixed_t before subtracting, so the ABSOLUTE price
                // had to fit fixed_t's +/-2048 - not just its movement. Any
                // instrument above ~$2,048 clamped, price and price_ref both
                // collapsed to 2048, their difference became zero and the
                // price half of the tick matrix went blank. Real cases on the
                // BX file alone: AMZN $2,196, BKNG $2,230, NVR $3,912,
                // SEB $4,060; and a reverse-split penny stock can go far
                // higher. Subtracting in acc_t (+/-524,288) first means only
                // the small centred result is ever narrowed, so the absolute
                // price stops mattering.
                Bt[i][0] = (fixed_t)(((acc_t)bid[i].price - (acc_t)price_ref) * (acc_t)price_scale);
                Bt[i][1] = (fixed_t)((acc_t)(int)bid[i].qty * (acc_t)qty_scale);
            } else { Bt[i][0] = (fixed_t)0; Bt[i][1] = (fixed_t)0; }
            if (ask[i].valid) {
                Bt[BOOK_N + i][0] = (fixed_t)(((acc_t)ask[i].price - (acc_t)price_ref) * (acc_t)price_scale);
                Bt[BOOK_N + i][1] = (fixed_t)((acc_t)(int)ask[i].qty * (acc_t)qty_scale);
            } else { Bt[BOOK_N + i][0] = (fixed_t)0; Bt[BOOK_N + i][1] = (fixed_t)0; }
        }

        // ── Unit-norm normalisation (Newton-Raphson inverse sqrt) ───────────
        if (unit_norm) {
            // n2 needs a WIDER accumulator than acc_t. acc_t is
            // ap_fixed<40,20> and saturates at +/-524,288, but the sum of
            // squares reaches ~913,000 on a high-priced instrument like AMZN:
            // the centred price can be ~600 after scaling, squared is 360,000,
            // and there are twenty terms. Clamping n2 makes 1/sqrt(n2) come out
            // too large, leaves the tick matrix under-shrunk and pushes S1 high
            // - the same symptom as the guess-table defect, a different cause.
            //
            // ap_fixed<56,32> holds up to +/-2.1e9, roughly 2,300x the observed
            // maximum. It is local to this accumulation, so acc_t is unchanged
            // everywhere else.
            typedef ap_fixed<56, 32, AP_RND, AP_SAT> n2_t;
            n2_t n2 = 0;
            for (int i = 0; i < N1; i++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    n2 += (n2_t)Bt[i][s] * (n2_t)Bt[i][s];
                }
            }

            // ── Inverse square root ────────────────────────────────────────
            // Newton-Raphson for 1/sqrt(a) converges only when the initial
            // guess x0 satisfies x0 < sqrt(3/a). The original guess table
            // stopped at "a > 1024 -> 0.015625", which breaks that condition
            // once a exceeds about 12,288: the iteration then DIVERGES rather
            // than converging, the gain comes out far too large, the tick
            // matrix is left under-shrunk and S1 reads high.
            //
            // That is not hypothetical. Measured on real NASDAQ BX data, IWO
            // and MDY spend ~70% of their ticks above that limit and the
            // computed 1/sqrt is wrong by up to 260%, which put S1 1.7x above
            // the reference and failed the distribution gate. It happens to any
            // instrument that trends away from its opening price, because
            // price_ref is fixed at the first price seen and the centred values
            // - and therefore n2 - grow with the square of that drift.
            //
            // Two changes fix it:
            //   1. the guess table now covers the full range, and every entry
            //      is 1/sqrt(interval upper bound). That makes the guess always
            //      <= the true value, so the iteration converges from below,
            //      which is unconditionally stable.
            //   2. nrl_t is wide enough that hn = n2/2 cannot saturate. At
            //      ap_fixed<36,14> it clamped at 8192, so any n2 above 16,384
            //      was solving for the wrong number entirely.
            typedef ap_fixed<48, 24, AP_RND, AP_SAT> nrl_t;   // was <36,14>
            bool  deg  = (n2 < (n2_t)1e-3);
            nrl_t safe = deg ? (nrl_t)1.0 : (nrl_t)n2;
            nrl_t hn   = (nrl_t)0.5 * safe;
            nrl_t x;
            // Each entry is 1/sqrt(the interval's UPPER bound), so the guess
            // is always <= the true value and the iteration converges from
            // below - unconditionally stable. The entries from 1024 downward
            // are the originals, unchanged, so every symbol that passes today
            // takes a bit-identical path. Only the five entries above 1024 are
            // new, and they are what was missing.
            if      (safe > (nrl_t)1048576.0) x = (nrl_t)0.00048828125;
            else if (safe > (nrl_t)262144.0)  x = (nrl_t)0.0009765625;
            else if (safe > (nrl_t)65536.0)   x = (nrl_t)0.001953125;
            else if (safe > (nrl_t)16384.0)   x = (nrl_t)0.00390625;
            else if (safe > (nrl_t)4096.0)    x = (nrl_t)0.0078125;
            else if (safe > (nrl_t)1024.0)    x = (nrl_t)0.015625;
            else if (safe > (nrl_t)256.0)     x = (nrl_t)0.031250;
            else if (safe > (nrl_t)64.0)      x = (nrl_t)0.062500;
            else if (safe > (nrl_t)16.0)      x = (nrl_t)0.125000;
            else if (safe > (nrl_t)4.0)       x = (nrl_t)0.250000;
            else if (safe > (nrl_t)1.0)       x = (nrl_t)0.500000;
            else if (safe > (nrl_t)0.25)      x = (nrl_t)1.000000;
            else if (safe > (nrl_t)0.0625)    x = (nrl_t)2.000000;
            else if (safe > (nrl_t)0.015625)  x = (nrl_t)4.000000;
            else                              x = (nrl_t)8.000000;

            // Starting within a factor of two and converging quadratically,
            // five iterations reach the limit of the type.
            for (int it = 0; it < 5; it++) {
#pragma HLS unroll
                x = x * ((nrl_t)1.5 - hn * x * x);
            }

            fixed_t g = deg ? (fixed_t)0.0 : (fixed_t)x;
            for (int i = 0; i < N1; i++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    Bt[i][s] = (fixed_t)((acc_t)Bt[i][s] * (acc_t)g);
                }
            }
        }

        bt_axi_t out;
        bt_pack(Bt, out);
        Bt_out.write(out);
    }
}

} // extern "C"
