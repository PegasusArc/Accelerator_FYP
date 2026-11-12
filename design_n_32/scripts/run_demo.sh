#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_demo.sh — one command to run the accelerator on the U50.
#
# Replays real NASDAQ BX market data through the card. Sets up the environment
# and checks everything is in place first, so a live demonstration cannot fail
# because a shell was missing a variable.
#
#   ./run_demo.sh                 XLB, 20,000 messages
#   ./run_demo.sh AAPL            a different stock
#   ./run_demo.sh AAPL 50000      and a different number of messages
#   ./run_demo.sh --reset         clear the card first (only after a killed run)
#
# This always uses the real market data. To run the small synthetic sample
# instead - useful only as a quick functional check - call the host directly:
#   cd offer2_codes/host
#   ./host_ip ../TB/quant_accel.xclbin ~/quant_accel/emu_test.itch --trace
# ─────────────────────────────────────────────────────────────────────────────
set -e

ROOT="${HOME}/quant_accel"
BDF="0000:01:00.1"
XCLBIN="$ROOT/offer2_codes/TB/quant_accel.xclbin"
DATA="$ROOT/20190730.BX_ITCH_50"
HOST="$ROOT/offer2_codes/host/host_ip"

SYMBOL="XLB"
LIMIT=20000
DO_RESET=0

for a in "$@"; do
    case "$a" in
        --reset) DO_RESET=1 ;;
        [0-9]*)  LIMIT="$a" ;;
        *)       SYMBOL="$a" ;;
    esac
done

echo "=== Quant Finance Accelerator — Alveo U50 ==="
echo

# ── environment ──────────────────────────────────────────────────────────────
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1
echo "XRT        : $(xbutil --version 2>/dev/null | grep -i version | head -1 || echo "$XILINX_XRT")"

# ── sanity checks, with useful messages instead of cryptic failures ──────────
[ -f "$XCLBIN" ] || { echo "ERROR: bitstream not found at $XCLBIN"; exit 1; }
[ -x "$HOST" ]   || { echo "ERROR: host program not built at $HOST"; exit 1; }
if [ ! -f "$DATA" ]; then
    echo "ERROR: market data not found at $DATA"
    echo "       This demonstration runs on the real NASDAQ BX file. It is"
    echo "       licensed exchange data and is not distributed with the code."
    exit 1
fi

if ! xbutil examine --device "$BDF" > /dev/null 2>&1; then
    echo "ERROR: card $BDF not visible. Check it is installed and XRT is loaded."
    exit 1
fi

echo "Card       : $BDF  (detected)"
echo "Bitstream  : $(basename "$XCLBIN")  ($(du -h "$XCLBIN" | cut -f1))"
echo "Market data: $(basename "$DATA")  — real NASDAQ BX, 30 July 2019"
echo "Stock      : $SYMBOL   (first $LIMIT messages)"
echo

# ── clear anything left from an interrupted run ─────────────────────────────
if pgrep -x host_ip > /dev/null 2>&1; then
    echo "A previous run is still active — stopping it."
    pkill -x host_ip || true
    sleep 1
fi

# The card only needs resetting after a run was killed mid-flight. It prompts
# for confirmation, which is awkward mid-demonstration, so it is off by default.
if [ "$DO_RESET" -eq 1 ]; then
    echo "Resetting card..."
    yes | xbutil reset --device "$BDF" > /dev/null 2>&1 || true
    echo
fi

# ── run ──────────────────────────────────────────────────────────────────────
cd "$ROOT/offer2_codes/host"
./host_ip "$XCLBIN" "$DATA" --symbol "$SYMBOL" --limit "$LIMIT" --trace
