// ─────────────────────────────────────────────────────────────────────────────
// qf_shared.h  (Option B — NEW FILE)
//
// DMA record layouts shared between the XRT host (compiled with g++, no HLS
// headers) and the HLS kernels. Deliberately uses only <cstdint> types so the
// host can include it without pulling in ap_fixed.h / the Vitis include path.
//
// The host packs an array of upd_rec_t into the input buffer; upd_inject reads
// it over m_axi. decision_sink writes an array of dec_rec_t to the output
// buffer over m_axi; the host reads it back. Field widths are fixed so the two
// sides agree on stride and offset with no packing surprises.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>

// One market update, packed by the host exactly as Offer-2 packed its 5 AXI-L
// words (w0 = [7:0] msg_type | [15:8] side ; w1 = price_t raw Q16.16 ; w2 = qty
// ; w3/w4 = ref_num lo/hi). Padded to 32 bytes for a clean m_axi burst stride.
struct upd_rec_t {
    uint32_t w0;
    uint32_t w1;
    uint32_t w2;
    uint32_t w3;
    uint32_t w4;
    uint32_t asset;      // round robin: which instrument this update belongs to
    uint32_t pad1;
    uint32_t pad2;
};

// One decision, produced per tick by decision_sink. The fixed-point trace
// values (s1, rho, r12) are stored as their sign-extended int32 raw bits; the
// host divides by 4096.0 to recover the double, matching fx_pack() in qf_types.
//
// SIZE IS 64 BYTES AND MUST STAY A POWER OF TWO. The ten live fields are only
// 40 bytes, and 40 bytes was what this record used to be. That cost us roughly
// a factor of ten in throughput, for a reason that is invisible in every
// synthesis report:
//
//   Vitis HLS can only widen an m_axi port to 512 bits when the element it
//   carries divides the bus cleanly. 320 bits does not divide 512, so the
//   write port for `decisions` was built narrow and each record left the
//   kernel as several separate AXI beats instead of one. The kernel loop still
//   reports II=1, because the scheduler posts the write to the AXI adapter and
//   assumes the adapter accepts it immediately. It does not model the adapter
//   draining. So the report says one cycle per decision and the hardware takes
//   twenty-odd, and nothing upstream can be changed to fix it.
//
// At 64 bytes the record is exactly one 512-bit beat. The six pad words are
// written as zero by decision_sink so that a byte-for-byte memcmp between two
// runs stays meaningful. If a new field is ever needed, take it out of the pad
// rather than growing the struct.
struct dec_rec_t {
    uint32_t ready;      // 1 = both book sides present -> a real decision
    uint32_t final_sig;  // 0 HOLD / 1 BUY / 2 SELL
    uint32_t mom_sig;
    uint32_t rsk_sig;
    uint32_t rgm_sig;
    uint32_t rgm_valid;
    int32_t  s1;         // fx_pack(S1)   -> host: s1 / 4096.0
    int32_t  rho;        // fx_pack(rho)
    int32_t  r12;        // fx_pack(r12)
    int32_t  asset;      // round robin: which instrument produced this decision
    uint32_t pad[6];     // -> 64 B. Zeroed by the kernel. Do not remove.
};
