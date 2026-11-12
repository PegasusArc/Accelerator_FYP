#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run.sh - run the 128-instrument design on the card.
#
#   ./run.sh --limit 5000 --table 30
#   ./run.sh --limit 5000 --dump /tmp/r0.bin
#   ./run.sh --limit 5000 --shift 64 --dump /tmp/r64.bin
#
# Resets the card first, every time. Without the reset the host sees the
# previous run's done flag, returns in microseconds and prints the old output
# buffer as a result. A rate in the thousands of M/s means that happened.
#
# ITCH=/path/to/20190730.BX_ITCH_50 ./run.sh ...   if the file is elsewhere
# BDF=0000:xx:00.1 ./run.sh ...                    if the card is elsewhere
# ─────────────────────────────────────────────────────────────────────────────
HERE="$(cd "$(dirname "$0")" && pwd)"
BDF="${BDF:-0000:01:00.1}"
ITCH="${ITCH:-$HOME/quant_accel/20190730.BX_ITCH_50}"

if [ -z "${XILINX_XRT:-}" ]; then
    source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1 || { echo "XRT not found at /opt/xilinx/xrt"; exit 1; }
fi
[ -f "$ITCH" ] || { echo "ITCH file not found: $ITCH  (set ITCH=/path/to/file)"; exit 1; }

xbutil reset --device "$BDF" --force > /dev/null 2>&1 || \
    yes | xbutil reset --device "$BDF" > /dev/null 2>&1

cd "$HERE/bin"
exec ./host_rr "$HERE/bin/quant_accel_rr.xclbin" "$ITCH" "$@"
