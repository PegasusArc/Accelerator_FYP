#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_decisions.sh — the trading view.
#
# Replays a session on the card and shows what it decided: one row per market
# tick with the price, the structure measure, and what each of the three
# strategy blocks voted before the majority was taken. Ends with per-block
# totals for the whole session.
#
# Same program and same bitstream as run_demo.sh — this asks for the decision
# view rather than the correctness report.
#
#   ./run_decisions.sh                 XLB, 20,000 messages
#   ./run_decisions.sh AAPL            a different stock
#   ./run_decisions.sh AAPL 50000      and more messages
#   ./run_decisions.sh AAPL 20000 12000   start the table at tick 12000
# ─────────────────────────────────────────────────────────────────────────────
set -e

ROOT="${HOME}/quant_accel"
BDF="0000:01:00.1"
XCLBIN="$ROOT/offer2_codes/TB/quant_accel.xclbin"
DATA="$ROOT/20190730.BX_ITCH_50"
HOST="$ROOT/offer2_codes/host/host_ip"

SYMBOL="${1:-XLB}"
LIMIT="${2:-20000}"
FROM="${3:-}"

source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1

[ -f "$XCLBIN" ] || { echo "ERROR: bitstream not found at $XCLBIN"; exit 1; }
[ -x "$HOST" ]   || { echo "ERROR: host program not built at $HOST"; exit 1; }
if [ ! -f "$DATA" ]; then
    echo "ERROR: market data not found at $DATA"
    echo "       This runs on the real NASDAQ BX file. It is licensed exchange"
    echo "       data and is not distributed with the code."
    exit 1
fi

if ! xbutil examine --device "$BDF" > /dev/null 2>&1; then
    echo "ERROR: card $BDF not visible."
    exit 1
fi

if pgrep -x host_ip > /dev/null 2>&1; then
    pkill -x host_ip || true
    sleep 1
fi

# Without a starting tick, jump to the most eventful stretch. Long parts of a
# session are entirely HOLD - correct behaviour, but it shows nothing happening.
# Both options skip the warm-up, where the risk block's slow reference has not
# yet converged and its value reads far above its steady-state range.
if [ -n "$FROM" ]; then
    WINDOW="--from $FROM"
else
    WINDOW="--busy"
fi

clear
cd "$ROOT/offer2_codes/host"
./host_ip "$XCLBIN" "$DATA" --symbol "$SYMBOL" --limit "$LIMIT" --table 30 $WINDOW
