// ─────────────────────────────────────────────────────────────────────────────
// botest.cpp — validate the host<->device buffer path independently of kernels.
//
// If the kernels execute but we read zeros, either (a) they wrote nothing, or
// (b) we are reading a different piece of device memory than they wrote. This
// separates the two: it prints the memory-bank group each buffer resolves to,
// then round-trips a known pattern through the output buffer with no kernel
// involved. A clean round-trip means the DMA path and bank mapping are fine and
// the problem is kernel execution.
//
// Build: g++ -std=c++17 -O2 botest.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib \
//        -lxrt_coreutil -pthread -o botest
// Run:   ./botest ../TB/quant_accel.xclbin
// ─────────────────────────────────────────────────────────────────────────────
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>
#include <xrt/xrt_bo.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <quant_accel.xclbin>\n";
        return 1;
    }

    try {
        auto device = xrt::device(0);
        auto uuid   = device.load_xclbin(argv[1]);
        std::cout << "device : " << device.get_info<xrt::info::device::name>() << "\n\n";

        auto k_inj  = xrt::kernel(device, uuid, "upd_inject");
        auto k_sink = xrt::kernel(device, uuid, "decision_sink");

        int gid_in  = k_inj.group_id(0);    // upd_inject.updates
        int gid_out = k_sink.group_id(3);   // decision_sink.decisions

        std::cout << "memory group for upd_inject.updates    (arg 0) : " << gid_in  << "\n";
        std::cout << "memory group for decision_sink.decisions(arg 3) : " << gid_out << "\n";
        std::cout << "  (these should be DIFFERENT — cfg maps them to HBM[0] and HBM[1])\n\n";

        // ── Round-trip a known pattern through the output buffer ────────────
        const size_t NBYTES = 299 * 40;   // M * sizeof(dec_rec_t)
        auto bo = xrt::bo(device, NBYTES, gid_out);

        std::vector<uint8_t> pattern(NBYTES);
        for (size_t i = 0; i < NBYTES; i++) pattern[i] = (uint8_t)(i * 7 + 13);

        std::memcpy(bo.map<uint8_t*>(), pattern.data(), NBYTES);
        bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        // wipe the host-side mapping so a stale local copy cannot fake a pass
        std::memset(bo.map<uint8_t*>(), 0, NBYTES);

        bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

        size_t bad = 0;
        for (size_t i = 0; i < NBYTES; i++)
            if (bo.map<uint8_t*>()[i] != pattern[i]) bad++;

        if (bad == 0) {
            std::cout << "PASS: buffer round-trip clean (" << NBYTES << " bytes).\n"
                         "      DMA path and bank mapping are good -> if a real run still\n"
                         "      reads zeros, the kernels are not writing.\n";
        } else {
            std::cout << "FAIL: " << bad << " / " << NBYTES << " bytes wrong after round-trip.\n"
                         "      The host<->device path itself is broken; kernel output was\n"
                         "      never going to be readable.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
