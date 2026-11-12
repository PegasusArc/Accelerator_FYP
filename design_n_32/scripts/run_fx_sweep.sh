#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_fx_sweep.sh — find the right fixed-point format, across the whole market.
#
# WHY THIS EXISTS IN THIS FORM
#
# The first version of this sweep tested four symbols and concluded that range
# was irrelevant and only precision mattered. That conclusion was wrong, and it
# was wrong because all four symbols happened to trade well below the format's
# ceiling. Adding AMZN — which reached $2,195 against a ceiling of 2048 —
# reversed it: the delivered format is 200x worse on AMZN than a format with
# more range and LESS precision.
#
# The lesson is that a format cannot be judged on symbols that never stress it.
# So this version:
#
#   * picks its own symbol list, deliberately including the high-priced
#     instruments that expose range limits
#   * reads the 799 MB data file once per format instead of once per
#     symbol-format pair
#   * judges formats on their WORST case, not their average, because a design
#     that is excellent on nine instruments and broken on the tenth is not a
#     good design
#   * states the recommendation rather than leaving it to be eyeballed
#
#   ./run_fx_sweep.sh                    # auto-select symbols
#   ./run_fx_sweep.sh "AMZN SPY XLB"     # explicit list
#   ./run_fx_sweep.sh auto 30000 400     # auto, 30k messages, scan 400 MB
# ─────────────────────────────────────────────────────────────────────────────
set -u

ROOT="$HOME/quant_accel"
SRC="$ROOT/offer2_codes/source"
TB="$ROOT/offer2_codes/TB"
HOSTD="$ROOT/offer2_codes/host"
DATA="$ROOT/20190730.BX_ITCH_50"
WORK="$ROOT/fx_sweep_work"
OUT="$ROOT/validation_results"

ARG_SYMS="${1:-auto}"
LIMIT="${2:-20000}"
MAXMB="${3:-300}"

# Range must clear the priciest instrument; precision uses whatever is left.
# 24,12 is the delivered format and stays in as the baseline.
FORMATS="24:12 24:13 24:14 32:12 32:13 32:14 32:15 32:16"

mkdir -p "$OUT"
rm -f "$OUT/fx_matrix.txt"

[ -f "$DATA" ] || { echo "market data not found: $DATA"; exit 1; }
[ -f "$ROOT/fx_sweep.cpp" ] || { echo "fx_sweep.cpp must be in $ROOT"; exit 1; }

HLS_INC=""
for cand in "$HOME/xilinx/Vitis_HLS/2022.1/include" \
            "$HOME/xilinx/Vitis/2022.1/include" \
            "${XILINX_HLS:-}/include" "${XILINX_VITIS:-}/include" ; do
    if [ -f "$cand/ap_fixed.h" ] && [ -f "$cand/etc/ap_private.h" ]; then
        HLS_INC="$cand"; break
    fi
done
[ -n "$HLS_INC" ] || { echo "no usable HLS include dir (needs ap_fixed.h AND etc/ap_private.h)"; exit 1; }

# ── Symbol selection ────────────────────────────────────────────────────────
if [ "$ARG_SYMS" = "auto" ]; then
    # Always rebuild. A stale binary from before --select existed silently
    # printed its full human report instead of a symbol list, and the script
    # then treated every word of that report as a ticker.
    echo "Building price_probe..."
    g++ -std=c++17 -O2 "$ROOT/price_probe.cpp" -o "$ROOT/price_probe" || exit 1
    echo "Selecting symbols (busiest, plus everything that stresses the range)..."
    SYMBOLS=$("$ROOT/price_probe" "$DATA" "$MAXMB" --select 12)
    [ -n "$SYMBOLS" ] || { echo "symbol selection failed"; exit 1; }
else
    SYMBOLS="$ARG_SYMS"
fi

# Guard against a malformed selection. Tickers are short and uppercase; if
# anything long or lowercase appears, the list is really captured prose.
for tk in $SYMBOLS; do
    if [ ${#tk} -gt 8 ] || echo "$tk" | grep -q '[a-z]'; then
        echo "Symbol list looks wrong - got '$tk'."
        echo "Check: ./price_probe $DATA $MAXMB --select 12"
        exit 1
    fi
done

nsym=$(echo "$SYMBOLS" | wc -w)
echo
echo "=== Fixed-point format sweep ==="
echo "  data    : $(basename "$DATA")  (first ${MAXMB} MB)"
echo "  symbols : $nsym  —  $SYMBOLS"
echo "  limit   : $LIMIT messages each"
echo "  formats : $(echo $FORMATS | tr ':' ',')"
echo

# ── Sweep ───────────────────────────────────────────────────────────────────
for f in $FORMATS; do
    W="${f%%:*}"; I="${f##*:}"
    printf "  building %s,%s ... " "$W" "$I"

    rm -rf "$WORK"; mkdir -p "$WORK"
    cp "$SRC"/*.h "$SRC"/*.cpp "$WORK"/ 2>/dev/null
    cp "$TB"/golden_model.* "$WORK"/ 2>/dev/null
    cp "$HOSTD"/itch_parser.* "$WORK"/ 2>/dev/null
    cp "$ROOT/fx_sweep.cpp" "$WORK"/

    sed -i "s/ap_fixed<24, *12, *AP_RND, *AP_SAT> fixed_t/ap_fixed<$W, $I, AP_RND, AP_SAT> fixed_t/" "$WORK/qf_types.h"
    sed -i "s/ap_int<24> raw = v.range(23, *0)/ap_int<$W> raw = v.range($((W-1)), 0)/" "$WORK/qf_types.h"
    sed -i "s/v.range(23, *0) = w.range(23, *0)/v.range($((W-1)), 0) = w.range($((W-1)), 0)/" "$WORK/qf_types.h"

    ( cd "$WORK" && g++ -std=c++17 -O2 -I. -I"$HLS_INC" \
        -DFIXED_W=$W -DFIXED_I=$I \
        fx_sweep.cpp lob_filter.cpp inctsvd_top.cpp quant_apps.cpp \
        upd_inject.cpp decision_sink.cpp golden_model.cpp itch_parser.cpp \
        -o fx_sweep ) > "$OUT/fxbuild_${W}_${I}.log" 2>&1

    if [ ! -x "$WORK/fx_sweep" ]; then
        echo "BUILD FAILED (see $OUT/fxbuild_${W}_${I}.log)"
        continue
    fi

    printf "running %s symbols ... " "$nsym"
    "$WORK/fx_sweep" "$DATA" "$LIMIT" "$MAXMB" $SYMBOLS 2>/dev/null \
        | grep '^RESULT' | sed "s/^/W=$W I=$I /" >> "$OUT/fx_matrix.txt"
    echo "done"
done
rm -rf "$WORK"

[ -s "$OUT/fx_matrix.txt" ] || { echo; echo "No results collected."; exit 1; }

# ── Matrix + recommendation ─────────────────────────────────────────────────
echo
awk -v syms="$SYMBOLS" '
function val(f,s) { return (f SUBSEP s) in E ? E[f,s] : -1 }
{
  for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
  if ("skipped" in v) { nskip++; delete v; next }
  f = v["W"] "," v["I"]
  E[f, v["sym"]] = v["medErr"] + 0
  A[f, v["sym"]] = v["agree"] + 0
  S[f] += v["satS1"] + 0
  FR[f] = v["frac"]; RG[f] = v["range"]
  if (!(f in seen)) { order[++nf] = f; seen[f] = 1 }
  delete v
}
END {
  n = split(syms, sym, " ")

  printf "  %-8s %5s %9s %6s", "FORMAT", "FRAC", "RANGE", "SAT"
  for (j = 1; j <= n; j++) printf " %8s", sym[j]
  printf "  %9s\n", "WORST"
  printf "  ------------------------------------------------------------"
  for (j = 1; j <= n; j++) printf "---------"
  printf "\n"

  best = ""; bestw = 1e18
  for (k = 1; k <= nf; k++) {
    f = order[k]
    worst = -1; worst_sym = "-"
    for (j = 1; j <= n; j++) {
      e = val(f, sym[j])
      if (e >= 0 && e > worst) { worst = e; worst_sym = sym[j] }
    }
    W[f] = worst; WS[f] = worst_sym
    if (worst >= 0 && worst < bestw) { bestw = worst; best = f }

    printf "  %-8s %5s %9s %6s", f, FR[f], RG[f], S[f]
    for (j = 1; j <= n; j++) {
      e = val(f, sym[j])
      if (e < 0)          printf " %8s", "-"
      else if (e >= 1000) printf " %8.0f", e
      else                printf " %8.3f", e
    }
    mark = (f == "24,12") ? "  <- delivered" : ""
    if (S[f] + 0 > 0) mark = mark "  ** SATURATING **"
    printf "  %8.3f%s\n", worst, mark
  }

  printf "\n  === Recommendation ===\n"
  if (nskip > 0)
    printf "  (%d symbol/format pairs skipped: too few ticks to clear warm-up)\n", nskip
  printf "  Judged on WORST case across all %d symbols, not average.\n", n
  printf "  A format that is excellent on nine instruments and broken on the\n"
  printf "  tenth is not a good format.\n\n"
  printf "  Best worst-case : %s   (%.3f%%, driven by %s)\n", best, bestw, WS[best]
  printf "  Delivered       : 24,12   (%.3f%%, driven by %s)\n", W["24,12"], WS["24,12"]
  if (W["24,12"] > 0 && bestw > 0)
    printf "  Improvement     : %.0fx better in the worst case\n", W["24,12"]/bestw
  if (bestw > 25)
    printf "\n  WARNING: even the best format exceeds the 25%% gate somewhere.\n"
  printf "\n  Raw data: validation_results/fx_matrix.txt\n"
}' "$OUT/fx_matrix.txt"
