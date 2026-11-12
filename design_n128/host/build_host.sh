#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_host.sh - rebuild the host program. A few seconds.
#
#   ./build_host.sh           builds host/host_rr
#   cp host_rr ../bin/        to use it with run.sh
#
# The host and the bitstream are a matched pair (64-byte output record,
# NASSET 128 on both sides). Rebuild the host only against this bitstream,
# or rebuild both.
# ─────────────────────────────────────────────────────────────────────────────
set -e
cd "$(dirname "$0")"
if [ -z "${XILINX_XRT:-}" ]; then
    source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1
fi
# Same search order as the original build: the includer's own folder, then
# ../source, then the reference model's folder (tb/).
GMC=golden_model.cpp; INC=""
if [ -f tb/golden_model.cpp ]; then GMC=tb/golden_model.cpp; INC="-Itb"; fi
g++ -std=c++17 -O2 -I"$XILINX_XRT/include" -I../source $INC \
    host_rr.cpp itch_parser.cpp "$GMC" \
    -o host_rr -L"$XILINX_XRT/lib" -lxrt_coreutil -lpthread -luuid
echo "built $(pwd)/host_rr"
grep -E '^#define NASSET ' host_rr.cpp | sed 's/^/NASSET in host: /'
