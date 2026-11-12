#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# test_validation.sh — the three validation tests that were still open.
#
#   1. Regime classifier ground truth   (synthetic files with known conditions)
#   2. Multiple real symbols            (is 90% the floor or an outlier?)
#   3. Repeatability                    (is the hardware actually deterministic?)
#
# Writes everything to validation_results/ and prints a summary.
#   ./test_validation.sh
# ─────────────────────────────────────────────────────────────────────────────
set -u

ROOT="$HOME/quant_accel"
HOSTDIR="$ROOT/offer2_codes/host"
XCLBIN="$ROOT/offer2_codes/TB/quant_accel.xclbin"
REAL="$ROOT/20190730.BX_ITCH_50"
GEN="$ROOT/tools/gen_itch.py"
OUT="$ROOT/validation_results"
LIMIT=20000

mkdir -p "$OUT"
cd "$HOSTDIR" || { echo "cannot cd to $HOSTDIR"; exit 1; }
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1

pass=0; fail=0
note() { echo; echo "── $* ─────────────────────────────────────────"; }

# ═════════════════════════════════════════════════════════════════════════════
note "TEST 1  Regime classifier against known market conditions"
# The regime classifier is the one block with no independent validation: on real
# data there is no label saying "this hour was trending". gen_itch.py generates
# files in known modes, so the classifier's own vote can be checked against the
# condition it was generated under.
#
# Expectation, from the classifier's logic:
#   trending  -> follows the trend, so BUY should dominate over SELL
#   volatile  -> stands down, so HOLD should be the largest share
#   meanrev   -> mixed, direction set by the S2/S3 ratio
# ═════════════════════════════════════════════════════════════════════════════
if [ ! -f "$GEN" ]; then
    echo "SKIP: $GEN not found"
else
    for mode in trending meanrev volatile; do
        f="$OUT/synth_$mode.itch"
        if [ ! -f "$f" ]; then
            echo "generating $mode ..."
            python3 "$GEN" --out "$f" --messages 20000 --mode "$mode" --seed 42 \
                > /dev/null 2>&1 || { echo "  generator failed for $mode"; continue; }
        fi
        log="$OUT/regime_$mode.txt"
        ./host_ip "$XCLBIN" "$f" --limit $LIMIT > "$log" 2>&1
        line=$(grep "regime vote" "$log" || echo "regime vote: (not reported)")
        printf "  %-9s %s\n" "$mode" "$line"
    done
    echo
    echo "  Read the three lines above together: the shape should differ by mode."
    echo "  Identical distributions would mean the classifier is not responding"
    echo "  to market conditions at all."
fi

# ═════════════════════════════════════════════════════════════════════════════
note "TEST 2  Multiple real symbols"
# Two symbols gave 90.2% and 98.1% agreement. That is a wide spread from a
# sample of two — this establishes whether 90% is the floor or an outlier.
# ═════════════════════════════════════════════════════════════════════════════
if [ ! -f "$REAL" ]; then
    echo "SKIP: real market data not found at $REAL"
else
    printf "  %-8s %10s %10s %9s %8s\n" SYMBOL DECISIONS AGREEMENT "MAX|S1|" VERDICT
    for sym in SPY IWM AAPL MSFT QQQ XLB SOXL UVXY IWO MDY; do
        log="$OUT/sym_$sym.txt"
        ./host_ip "$XCLBIN" "$REAL" --symbol "$sym" --limit $LIMIT > "$log" 2>&1
        dec=$(grep -oP 'decisions\s+:\s+\K[0-9]+' "$log" | head -1)
        agr=$(grep -oP 'agreement vs golden.*\(\K[0-9.]+' "$log" | head -1)
        #s1=$(grep -oP 'max \|S1\| = \K[0-9.e+]+' "$log" | head -1)
        s1=$(grep -oP 'max \|S1\| = \K[0-9.e+]+' "$log" | head -1)
        s1=$(printf "%.1f" "$s1" 2>/dev/null || echo "$s1")
        if grep -q "HARDWARE RUN PASSED" "$log"; then v=PASS; pass=$((pass+1))
        else v=FAIL; fail=$((fail+1)); fi
        printf "  %-8s %10s %9s%% %9s %8s\n" "$sym" "${dec:-–}" "${agr:-–}" "${s1:-–}" "$v"
    done
fi

# ═════════════════════════════════════════════════════════════════════════════
note "TEST 3  Repeatability"
# The determinism claim currently rests on throughput being stable. Identical
# decisions across repeated runs is the direct evidence.
# ═════════════════════════════════════════════════════════════════════════════
if [ ! -f "$REAL" ]; then
    echo "SKIP: real market data not found"
else
    for i in 1 2 3; do
        ./host_ip "$XCLBIN" "$REAL" --symbol AAPL --limit $LIMIT --trace \
            > "$OUT/repeat_$i.txt" 2>&1
    done
    # compare everything except the timing lines, which legitimately vary
    strip() { grep -v -E "ms   ->|M decisions/s|wall clock" "$1"; }
    strip "$OUT/repeat_1.txt" > "$OUT/.r1"
    strip "$OUT/repeat_2.txt" > "$OUT/.r2"
    strip "$OUT/repeat_3.txt" > "$OUT/.r3"
    if diff -q "$OUT/.r1" "$OUT/.r2" > /dev/null && \
       diff -q "$OUT/.r1" "$OUT/.r3" > /dev/null; then
        echo "  PASS: three runs produced byte-identical decisions."
        pass=$((pass+1))
        echo -n "  timing across runs: "
        grep -h "FPGA" "$OUT"/repeat_*.txt | grep -oP 'FPGA : \K[0-9.]+' | tr '\n' ' '
        echo "ms"
    else
        echo "  FAIL: runs differ — the hardware is not reproducing itself."
        diff "$OUT/.r1" "$OUT/.r2" | head -20
        fail=$((fail+1))
    fi
    rm -f "$OUT/.r1" "$OUT/.r2" "$OUT/.r3"
fi

note "SUMMARY"
echo "  passed: $pass    failed: $fail"
echo "  full logs in $OUT"
[ "$fail" -eq 0 ] && echo "  ALL GOOD" || echo "  REVIEW THE FAILURES ABOVE"
