#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# test_batch_limit.sh — how large a batch can this actually process?
#
# Steps the batch size up until something breaks, recording throughput and
# verdict at each size. Answers a question the client will ask, and finds the
# practical ceiling before he does.
#
# Known theoretical limits:
#   - output buffer: one HBM bank is 256 MB, each decision is 40 bytes
#     -> about 6.5 million decisions
#   - host memory: the program loads the file, packs a second copy, and runs
#     the reference model, so RAM usually binds first
#   - stream FIFOs are 512 deep, but the read-ordering fix means depth is
#     margin rather than a limit
#
#   ./test_batch_limit.sh [SYMBOL]
# ─────────────────────────────────────────────────────────────────────────────
set -u

ROOT="$HOME/quant_accel"
HOSTDIR="$ROOT/offer2_codes/host"
XCLBIN="$ROOT/offer2_codes/TB/quant_accel.xclbin"
REAL="$ROOT/20190730.BX_ITCH_50"
SYM="${1:-AAPL}"
OUT="$ROOT/validation_results"

mkdir -p "$OUT"
cd "$HOSTDIR" || exit 1
source /opt/xilinx/xrt/setup.sh > /dev/null 2>&1

[ -f "$REAL" ] || { echo "real market data not found at $REAL"; exit 1; }

echo "=== Batch scaling test — symbol $SYM ==="
echo
echo "  free RAM: $(free -m | awk '/^Mem:/{print $7}') MB"
echo

printf "  %10s %10s %11s %11s %11s %9s\n" \
       LIMIT DECISIONS "FPGA(ms)" "CPU(ms)" "M dec/s" VERDICT
printf "  %s\n" "------------------------------------------------------------------------"

prev_ok=0
#for lim in 1000 5000 20000 50000 100000 150000 190000 250000; do
for lim in 500000 1000000 2000000 4000000; do
    log="$OUT/batch_${SYM}_${lim}.txt"

    # `timeout` guards against a hang at large sizes rather than waiting forever
    timeout 600 ./host_ip "$XCLBIN" "$REAL" --symbol "$SYM" --limit "$lim" \
        > "$log" 2>&1
    rc=$?

    if [ $rc -eq 124 ]; then
        printf "  %10s %10s %11s %11s %11s %9s\n" "$lim" "-" "-" "-" "-" "TIMEOUT"
        echo
        echo "  Stopped: run exceeded 10 minutes at limit $lim."
        break
    fi

    dec=$(grep -oP 'decisions\s+:\s+\K[0-9]+' "$log" | head -1)
    fp=$(grep -oP 'FPGA : \K[0-9.]+' "$log" | head -1)
    cp=$(grep -oP 'CPU  : \K[0-9.]+' "$log" | head -1)
    th=$(grep -oP 'FPGA :.*-> \K[0-9.]+' "$log" | head -1)

    if grep -q "HARDWARE RUN PASSED" "$log"; then
        v=PASS; prev_ok=$lim
    elif grep -qE "bad_alloc|Killed|std::length_error" "$log"; then
        v="OOM"
    elif grep -q "HARDWARE RUN FAILED" "$log"; then
        v=FAIL
    else
        v="ERR"
    fi

    printf "  %10s %10s %11s %11s %11s %9s\n" \
           "$lim" "${dec:-–}" "${fp:-–}" "${cp:-–}" "${th:-–}" "$v"

    # Do NOT stop on a failure: the result is not monotonic. Very small batches
    # can fail the distribution gate because every tick lands in the SVD's
    # warm-up transient, while much larger batches pass comfortably. Stopping at
    # the first failure would report a scaling limit that does not exist.
    if [ "$v" != "PASS" ]; then
        echo "             ^ gate detail:"
        grep -E "^(PASS|FAIL):" "$log" | sed 's/^/               /'
    fi

    # Only a resource failure is a genuine ceiling — stop on those.
    if [ "$v" = "OOM" ] || [ "$v" = "ERR" ]; then
        echo
        echo "  Stopping: resource failure at limit $lim."
        tail -6 "$log" | sed 's/^/    /'
        break
    fi
done

echo
echo "  Largest batch that passed: $prev_ok updates"
echo "  Logs in $OUT"
echo
echo "  If small sizes FAIL while larger ones PASS, that is the SVD warm-up"
echo "  transient, not a scaling limit: with only a few hundred ticks every"
echo "  sample sits in the convergence phase, so the distribution gate has"
echo "  nothing steady-state to match against."
echo
echo "  Note: throughput should stay flat across sizes — that constancy is the"
echo "  determinism argument. The CPU figure will improve with size as its"
echo "  startup overhead amortises, which is why small batches flatter it less."
