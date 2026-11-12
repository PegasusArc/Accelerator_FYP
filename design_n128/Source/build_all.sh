#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build_all.sh — recompile all nine kernels, then link.
#
# All nine, not just the edited ones. The AXI-Stream types in qf_types.h gained
# a TUSER bit, so every kernel that touches those streams has a different port
# signature now. That includes sv_bcast3 and sv_bcast2, which have no source
# change at all but whose old .xo files carry the old type and will not link
# against the new ones.
#
# Each kernel's log is kept, and only the lines worth reading are echoed. Two
# numbers matter on the way past:
#   lob_filter      should still report Final II = 1
#   decision_sink   CHUNK iteration latency should still be ~200 for 64 records
#
# Usage:  ./build_all.sh            compile everything, then link
#         ./build_all.sh nolink     compile only
# ─────────────────────────────────────────────────────────────────────────────
set -e

export PF=/opt/xilinx/platforms/xilinx_u50_gen3x16_xdma_5_202210_1/xilinx_u50_gen3x16_xdma_5_202210_1.xpfm
NASSET=128
SRC="$(cd "$(dirname "$0")" && pwd)"
# Pin the maintained config explicitly. `find | head -1` picked a copy out of
# the backups directory, which happened to be identical but is not something
# to rely on before a fifty-minute link.
CFG="$SRC/quant_accel_rr.cfg"

cd "$SRC"
[ -f "$PF" ] || { echo "platform not found: $PF"; exit 1; }
[ -n "$CFG" ] || { echo "quant_accel_rr.cfg not found"; exit 1; }
echo "platform : $PF"
echo "config   : $CFG"
echo "NASSET   : $NASSET"
echo

PAIRS="upd_inject:upd_inject.cpp
lob_filter:lob_filter.cpp
inctsvd_top:inctsvd_top.cpp
sv_bcast3:sv_bcast.cpp
sv_bcast2:sv_bcast.cpp
momentum_signal:quant_apps.cpp
risk_matrix:quant_apps.cpp
regime_classifier:quant_apps.cpp
decision_sink:decision_sink.cpp"

echo "$PAIRS" | while IFS=: read -r K F; do
    [ -n "$K" ] || continue
    echo "=== $K   (from $F) ==="
    v++ -c -t hw --platform "$PF" -k "$K" -DNASSET=$NASSET \
        --temp_dir "_x_$K" -o "$K.xo" "$F" > "build_$K.log" 2>&1 || {
            echo "  FAILED - last 30 lines:"; tail -30 "build_$K.log"; exit 1; }
    grep -E "Final II|Estimated Fmax" "build_$K.log" | sed 's/^/  /' || true
done

echo
echo "all nine compiled. .xo files:"
ls -la *.xo

if [ "$1" = "nolink" ]; then
    echo "stopping before the link as asked."
    exit 0
fi

echo
echo "=== link (about 50 minutes) ==="
grep -n freqHz "$CFG" | sed 's/^/  cfg: /'
v++ -l -t hw --platform "$PF" --config "$CFG" \
    -o quant_accel_rr.xclbin \
    upd_inject.xo lob_filter.xo inctsvd_top.xo \
    sv_bcast3.xo sv_bcast2.xo \
    momentum_signal.xo risk_matrix.xo regime_classifier.xo \
    decision_sink.xo \
    2>&1 | tee "link_$(date +%Y%m%d_%H%M).log"

echo
echo "link done. Before running anything, confirm the new register offset:"
echo "  ./argdump quant_accel_rr.xclbin 2>&1 | grep -A10 decision_sink"
echo "n_events should sit at 0x24. If it does not, change R_DEC_EVENTS in"
echo "host_rr.cpp to match and rebuild the host."
