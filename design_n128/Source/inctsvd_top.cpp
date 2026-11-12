#include "inctsvd_top.h"

typedef ap_fixed<36, 14, AP_RND, AP_SAT> nr_t;

// ─── Compute helpers (unchanged from Offer 2) ────────────────────────────────
static void fft2_tensor(fixed_t in[N1][K][N3], fixed_t out[N1][K][N3]) {
#pragma HLS inline
    for (int i = 0; i < N1; i++) {
#pragma HLS unroll
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            out[i][j][0] = in[i][j][0] + in[i][j][1];
            out[i][j][1] = in[i][j][0] - in[i][j][1];
        }
    }
}
static void fft2_tick(fixed_t in[N1][N3], fixed_t out[N1][N3]) {
#pragma HLS inline
    for (int i = 0; i < N1; i++) {
#pragma HLS unroll
        out[i][0] = in[i][0] + in[i][1];
        out[i][1] = in[i][0] - in[i][1];
    }
}
static void project(fixed_t Ut_f[N1][K][N3], fixed_t Bt_f[N1][N3], fixed_t P[K][N3]) {
#pragma HLS inline
    for (int s = 0; s < N3; s++) {
#pragma HLS unroll
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            acc_t acc = 0;
            for (int i = 0; i < N1; i++) {
#pragma HLS unroll
                acc += (acc_t)Ut_f[i][j][s] * (acc_t)Bt_f[i][s];
            }
            P[j][s] = (fixed_t)acc;
        }
    }
}
static void residual(fixed_t Ut_f[N1][K][N3], fixed_t Bt_f[N1][N3],
                     fixed_t P[K][N3], fixed_t R[N1][N3]) {
#pragma HLS inline
    for (int s = 0; s < N3; s++) {
#pragma HLS unroll
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            acc_t acc = (acc_t)Bt_f[i][s];
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                acc -= (acc_t)Ut_f[i][j][s] * (acc_t)P[j][s];
            }
            R[i][s] = (fixed_t)acc;
        }
    }
}
static void normalise(fixed_t R[N1][N3], fixed_t Q[N1][N3]) {
#pragma HLS inline
    for (int s = 0; s < N3; s++) {
#pragma HLS unroll
        acc_t norm2_w = 0;
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            norm2_w += (acc_t)R[i][s] * (acc_t)R[i][s];
        }
        bool  degenerate = (norm2_w < (acc_t)1e-3);
        nr_t  safe       = degenerate ? (nr_t)1.0 : (nr_t)norm2_w;
        nr_t  half_n     = (nr_t)0.5 * safe;
        nr_t x;
        if      (safe > (nr_t)1024.0)   x = (nr_t)0.015625;
        else if (safe > (nr_t)256.0)    x = (nr_t)0.031250;
        else if (safe > (nr_t)64.0)     x = (nr_t)0.062500;
        else if (safe > (nr_t)16.0)     x = (nr_t)0.125000;
        else if (safe > (nr_t)4.0)      x = (nr_t)0.250000;
        else if (safe > (nr_t)1.0)      x = (nr_t)0.500000;
        else if (safe > (nr_t)0.25)     x = (nr_t)1.000000;
        else if (safe > (nr_t)0.0625)   x = (nr_t)2.000000;
        else if (safe > (nr_t)0.015625) x = (nr_t)4.000000;
        else                            x = (nr_t)8.000000;
        for (int iter = 0; iter < 5; iter++) {
#pragma HLS unroll
            x = x * ((nr_t)1.5 - half_n * x * x);
        }
        fixed_t inv = degenerate ? (fixed_t)0.0 : (fixed_t)x;
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            Q[i][s] = (fixed_t)((acc_t)R[i][s] * (acc_t)inv);
        }
    }
}
static void update_sv(fixed_t St_state[K], fixed_t P[K][N3], fixed_t lambda,
                      fixed_t St_new[K], fixed_t proj_mag[K]) {
#pragma HLS inline
    for (int j = 0; j < K; j++) {
#pragma HLS unroll
        acc_t mag = 0;
        for (int s = 0; s < N3; s++) {
#pragma HLS unroll
            mag += (acc_t)P[j][s] * (acc_t)P[j][s];
        }
        proj_mag[j] = (fixed_t)mag;
        St_new[j]   = (fixed_t)((acc_t)lambda * (acc_t)St_state[j] + mag);
    }
}

extern "C" {

void inctsvd_top(
    hls::stream<bt_axi_t> &Bt_in,
    hls::stream<sv_axi_t> &St_out,
    hls::stream<sv_axi_t> &proj_out,
    int                    n_ticks,
    fixed_t                lambda
) {
#pragma HLS interface axis      port=Bt_in
#pragma HLS interface axis      port=St_out
#pragma HLS interface axis      port=proj_out
#pragma HLS interface s_axilite port=n_ticks bundle=control
#pragma HLS interface s_axilite port=lambda  bundle=control
#pragma HLS interface s_axilite port=return  bundle=control

    // ROUND ROBIN. One basis and one set of singular values per instrument.
    // This is the state that creates the 116-cycle dependency: tick t+1 of an
    // instrument needs tick t's updated basis. Giving each instrument its own
    // copy means consecutive beats in the stream are independent, so the
    // pipeline can stay full instead of waiting.
    //
    // The inner dimensions are partitioned completely, so a whole basis is
    // readable in one cycle. The asset dimension stays in BRAM and is addressed
    // by the id arriving on the beat. Partitioning that dimension too would ask
    // for NASSET x N1 x K x N3 registers and will not place.
    fixed_t Ut_all[NASSET][N1][K][N3];

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
    fixed_t St_all[NASSET][K];
#pragma HLS array_partition variable=Ut_all complete dim=2
#pragma HLS array_partition variable=Ut_all complete dim=3
#pragma HLS array_partition variable=Ut_all complete dim=4
#pragma HLS array_partition variable=St_all complete dim=2
    HLS_DEP_DISTANCE(Ut_all, NASSET)
    HLS_DEP_DISTANCE(St_all, NASSET)
#pragma HLS bind_storage variable=Ut_all type=RAM_2P impl=BRAM
#pragma HLS bind_storage variable=St_all type=RAM_2P impl=BRAM

    for (int a = 0; a < NASSET; a++) {
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    Ut_all[a][i][j][s] = (i == j) ? (fixed_t)1.0 : (fixed_t)0.0;
                }
            }
        }
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            St_all[a][j] = (fixed_t)8.0;
        }
    }

    for (int t = 0; t < n_ticks; t++) {
        bt_axi_t pkt = Bt_in.read();

        ap_uint<8> aid = pkt.id;
        ap_uint<1> vld = pkt.user;    // was this event's book two-sided
        int        a   = (NASSET == 1) ? 0 : (int)(aid % NASSET);

        // Pull this instrument's state into registers for the tick, then write
        // it back at the end. One BRAM read and one write per tick per array.
        fixed_t Ut[N1][K][N3];
        fixed_t St[K];
#pragma HLS array_partition variable=Ut complete dim=0
#pragma HLS array_partition variable=St complete dim=0
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    Ut[i][j][s] = Ut_all[a][i][j][s];
                }
            }
        }
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            St[j] = St_all[a][j];
        }

        fixed_t Bt[N1][N3], Ut_f[N1][K][N3], Bt_f[N1][N3];
        fixed_t P[K][N3], R[N1][N3], Q[N1][N3];
        fixed_t St_new[K], proj_mag[K];
#pragma HLS array_partition variable=Bt       complete dim=0
#pragma HLS array_partition variable=Ut_f     complete dim=0
#pragma HLS array_partition variable=Bt_f     complete dim=0
#pragma HLS array_partition variable=P        complete dim=0
#pragma HLS array_partition variable=R        complete dim=0
#pragma HLS array_partition variable=Q        complete dim=0
#pragma HLS array_partition variable=St_new   complete dim=0
#pragma HLS array_partition variable=proj_mag complete dim=0

        bt_unpack(pkt, Bt);

        fft2_tensor(Ut, Ut_f);
        fft2_tick(Bt, Bt_f);
        project(Ut_f, Bt_f, P);
        residual(Ut_f, Bt_f, P, R);
        normalise(R, Q);
        update_sv(St, P, lambda, St_new, proj_mag);

        // Window shift: drop oldest basis vector, append Q
        fixed_t Ut_next[N1][K][N3];
#pragma HLS array_partition variable=Ut_next complete dim=0
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            for (int j = 0; j < K - 1; j++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    Ut_next[i][j][s] = Ut[i][j + 1][s];
                }
            }
            for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                Ut_next[i][K - 1][s] = Q[i][s];
            }
        }
        for (int i = 0; i < N1; i++) {
#pragma HLS unroll
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                    Ut[i][j][s] = Ut_next[i][j][s];
                }
            }
        }
        for (int j = 0; j < K; j++) {
#pragma HLS unroll
            St[j] = St_new[j];
        }

        // Write this instrument's updated state back before moving on. The
        // next beat may well belong to a different instrument.
        //
        // A beat whose book was one-sided occupies its slot in the rotation -
        // which is the whole point, it keeps the spacing exact - but must not
        // advance the decomposition, because no tick matrix was formed for it.
        // The arithmetic above ran on a zero matrix and its result is discarded
        // here. Predicated, so the initiation interval is unchanged.
        if (vld) {
            for (int i = 0; i < N1; i++) {
#pragma HLS unroll
                for (int j = 0; j < K; j++) {
#pragma HLS unroll
                    for (int s = 0; s < N3; s++) {
#pragma HLS unroll
                        Ut_all[a][i][j][s] = Ut[i][j][s];
                    }
                }
            }
            for (int j = 0; j < K; j++) {
#pragma HLS unroll
                St_all[a][j] = St[j];
            }
        }

        sv_axi_t st_pkt, pm_pkt;
        sv_pack(St_new,   st_pkt);
        sv_pack(proj_mag, pm_pkt);
        st_pkt.id   = aid;            // carry the instrument forward
        pm_pkt.id   = aid;
        st_pkt.user = vld;            // and whether it is a real decision
        pm_pkt.user = vld;
        st_pkt.last = (t == n_ticks - 1) ? 1 : 0;
        pm_pkt.last = (t == n_ticks - 1) ? 1 : 0;
        St_out.write(st_pkt);
        proj_out.write(pm_pkt);
    }
}

} // extern "C"
