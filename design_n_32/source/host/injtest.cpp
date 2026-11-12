// ─────────────────────────────────────────────────────────────────────────────
// injtest.cpp — does the n_ticks scalar actually reach the kernel?
//
// Starts ONLY upd_inject. Nothing downstream is running, so its outgoing AXIS
// FIFO (depth ~32) fills almost immediately and the kernel must block partway
// through its loop.
//
//   upd_inject ends up (IDLE)      -> it looped ZERO times -> n_ticks arrived
//                                     as 0; set_arg is not landing.
//   upd_inject stays busy/(START)  -> n_ticks arrived correctly and the kernel
//                                     is blocked writing to a full FIFO, which
//                                     is the expected, healthy behaviour here.
//
// Build: g++ -std=c++17 -O2 injtest.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib \
//        -lxrt_coreutil -pthread -o injtest
// Run:   xbutil reset --device <bdf>      (always start from a clean card)
//        ./injtest ../TB/quant_accel.xclbin
//        ... then in ANOTHER terminal, while it is paused:
//        xbutil examine --device <bdf> --report dynamic-regions
// ─────────────────────────────────────────────────────────────────────────────
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <xrt/xrt_bo.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

struct upd_rec_t {
    uint32_t w0, w1, w2, w3, w4, pad0, pad1, pad2;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <quant_accel.xclbin>\n";
        return 1;
    }
    const int N = 300;

    try {
        auto device = xrt::device(0);
        auto uuid   = device.load_xclbin(argv[1]);
        auto k_inj  = xrt::kernel(device, uuid, "upd_inject");

        auto bo_in = xrt::bo(device, (size_t)N * sizeof(upd_rec_t), k_inj.group_id(0));

        // Plausible-looking updates; content does not matter for this test.
        std::vector<upd_rec_t> host_in(N);
        for (int i = 0; i < N; i++) {
            host_in[i].w0 = 1u | ((i & 1u) << 8);   // msg_type=ADD, alternating side
            host_in[i].w1 = 150u << 16;
            host_in[i].w2 = 100;
            host_in[i].w3 = (uint32_t)i;
            host_in[i].w4 = 0;
            host_in[i].pad0 = host_in[i].pad1 = host_in[i].pad2 = 0;
        }
        std::memcpy(bo_in.map<upd_rec_t*>(), host_in.data(), (size_t)N * sizeof(upd_rec_t));
        bo_in.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        xrt::run r(k_inj);
        r.set_arg(0, bo_in);
        r.set_arg(1, N);

        std::cout << "starting upd_inject alone with n_ticks = " << N << "\n";
        r.start();

        std::cout <<
            "\n--------------------------------------------------------------\n"
            "Now, in ANOTHER terminal, run:\n"
            "  xbutil examine --device 0000:01:00.1 --report dynamic-regions\n"
            "\n"
            "Look at upd_inject's Status:\n"
            "  (IDLE)          -> looped 0 times: n_ticks did NOT reach the kernel\n"
            "  busy / (START)  -> blocked on a full FIFO: n_ticks DID reach it\n"
            "--------------------------------------------------------------\n"
            "\nPress Enter here when you have the reading...\n";
        std::cin.get();

        std::cout << "done — remember to 'xbutil reset' before the next run.\n";
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
