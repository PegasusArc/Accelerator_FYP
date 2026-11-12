// ─────────────────────────────────────────────────────────────────────────────
// argdump.cpp — print the kernel argument table exactly as XRT indexes it.
//
// The host sets scalars with run.set_arg(index, value). Those indices must match
// what XRT believes, which is NOT necessarily the arg `id` in the xclbin XML —
// AXIS stream ports may or may not occupy an index. If they don't, every scalar
// index is shifted and n_ticks silently lands in the wrong register (or nowhere),
// which makes each kernel loop zero times and produce no output.
//
// Build: g++ -std=c++17 -O2 argdump.cpp -I$XILINX_XRT/include -L$XILINX_XRT/lib \
//        -lxrt_coreutil -pthread -o argdump
// Run:   ./argdump ../TB/quant_accel.xclbin
// ─────────────────────────────────────────────────────────────────────────────
#include <experimental/xrt_xclbin.h>

#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <quant_accel.xclbin>\n";
        return 1;
    }

    try {
        xrt::xclbin xb(argv[1]);

        std::cout << "xclbin: " << argv[1] << "\n\n";

        for (const auto& k : xb.get_kernels()) {
            std::cout << "=== " << k.get_name() << " ===\n";
            std::cout << "  idx  offset  size  name\n";
            for (const auto& a : k.get_args()) {
                std::cout << "  " << std::setw(3) << a.get_index()
                          << "  " << std::setw(6) << ("0x" + [&]{
                                 char b[16]; snprintf(b, sizeof b, "%x", (unsigned)a.get_offset());
                                 return std::string(b); }())
                          << "  " << std::setw(4) << a.get_size()
                          << "  " << a.get_name()
                          << "\n";
            }
            std::cout << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
