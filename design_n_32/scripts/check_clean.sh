#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# check_clean.sh — pre-delivery hygiene check.
#
# Looks for anything that should not go out: backup copies, editor leftovers,
# superseded sources, temp working directories, and build artifacts from the
# wrong toolchain. Reports only; deletes nothing. Anything it flags, you decide
# on.
#
#   ./check_clean.sh
# ─────────────────────────────────────────────────────────────────────────────

ROOT="$HOME/quant_accel"
found=0

hdr() { echo; echo "── $* ──────────────────────────────────────────"; }
hit() { echo "  $*"; found=$((found+1)); }

echo "=== Pre-delivery check: $ROOT ==="

# ── 1. Backup and editor leftovers ──────────────────────────────────────────
hdr "Backup / editor leftovers"
while IFS= read -r f; do hit "$f"; done < <(
    find "$ROOT" \( \
        -name "*.bak" -o -name "*~" -o -name "*.orig" -o -name "*.rej" -o \
        -name "*backup*" -o -name "*_old*" -o -name "*.save" -o \
        -name "*.swp" -o -name "*.tmp" -o -name "#*#" \
    \) -not -path "*/_x/*" -not -path "*/.git/*" 2>/dev/null | sort
)
[ "$found" -eq 0 ] && echo "  none"

# ── 2. Superseded sources ───────────────────────────────────────────────────
hdr "Superseded sources (kept only if deliberate)"
before=$found
for f in \
    "$ROOT/offer2_codes/host/host_xrt.cpp" \
    "$ROOT/offer2_codes/host/host_xrt_1.cpp" \
    "$ROOT/offer2_codes/host/host_xrt_2.cpp" \
    "$ROOT/host/host_xrt.cpp" \
    "$ROOT/host/host_xrt_1.cpp" \
    "$ROOT/host/host_xrt_2.cpp" ; do
    [ -f "$f" ] && hit "$f   (Option A host — superseded by host_xrt_ip.cpp)"
done
# the whole legacy host dir, if it still exists alongside offer2_codes/host
if [ -d "$ROOT/host" ] && [ -d "$ROOT/offer2_codes/host" ]; then
    hit "$ROOT/host/   (legacy Option A host directory — offer2_codes/host is the live one)"
fi
[ "$found" -eq "$before" ] && echo "  none"

# ── 3. Temp working directories ─────────────────────────────────────────────
hdr "Temp / working directories"
before=$found
for d in "$ROOT/fx_sweep_work" "$ROOT/offer2_codes/TB/.Xil" \
         "$ROOT/offer2_codes/host/.Xil" "$ROOT/.Xil" \
         "$ROOT/offer2_codes/TB/.ipcache" "$ROOT/offer2_codes/host/.run" ; do
    [ -d "$d" ] && hit "$d   ($(du -sh "$d" 2>/dev/null | cut -f1))"
done
[ "$found" -eq "$before" ] && echo "  none"

# ── 4. Odd filenames from mistyped commands ─────────────────────────────────
hdr "Odd filenames (mistyped redirects, stray flags)"
before=$found
while IFS= read -r f; do hit "$f"; done < <(
    find "$ROOT" -maxdepth 3 \( -name "-*" -o -name "*.o" \) \
        -not -path "*/_x/*" 2>/dev/null | sort
)
[ "$found" -eq "$before" ] && echo "  none"

# ── 5. Build artifacts — check they came from the right toolchain ───────────
hdr "Build artifacts"
xo_count=$(ls "$ROOT/offer2_codes/TB"/*.xo 2>/dev/null | wc -l)
if [ "$xo_count" -gt 0 ]; then
    echo "  $xo_count .xo files in offer2_codes/TB (9 expected)"
    [ "$xo_count" -ne 9 ] && hit "expected exactly 9 .xo — check for stale extras"
fi
if [ -f "$ROOT/offer2_codes/TB/quant_accel.xclbin" ]; then
    echo "  xclbin present: $(du -h "$ROOT/offer2_codes/TB/quant_accel.xclbin" | cut -f1), built $(date -r "$ROOT/offer2_codes/TB/quant_accel.xclbin" '+%Y-%m-%d %H:%M')"
else
    hit "quant_accel.xclbin MISSING — the package needs it"
fi
if [ -d "$ROOT/offer2_codes/TB/_x" ]; then
    echo "  _x/ build tree: $(du -sh "$ROOT/offer2_codes/TB/_x" | cut -f1) — not packaged, safe to keep or delete"
fi

# ── 6. Files the package expects ────────────────────────────────────────────
hdr "Files package_delivery.sh will look for"
before=$found
for f in README.md GUIDE.pdf guide.tex run_demo.sh \
         test_validation.sh test_batch_limit.sh \
         fx_sweep.cpp run_fx_sweep.sh emu_test.itch tools/gen_itch.py ; do
    if [ -e "$ROOT/$f" ]; then echo "  ok   $f"; else hit "MISSING  $f"; fi
done
for f in offer2_codes/host/host_xrt_ip.cpp offer2_codes/host/itch_parser.cpp \
         offer2_codes/TB/tb_system_b.cpp offer2_codes/TB/golden_model.cpp \
         offer2_codes/source/qf_types.h offer2_codes/source/qf_shared.h \
         hls/sv_bcast.cpp vitis/quant_accel.cfg ; do
    if [ -e "$ROOT/$f" ]; then echo "  ok   $f"; else hit "MISSING  $f"; fi
done

# ── 7. Should NOT be in the package ─────────────────────────────────────────
hdr "Deliberately excluded from the package"
[ -f "$ROOT/20190730.BX_ITCH_50" ] && \
    echo "  BX market data present ($(du -h "$ROOT/20190730.BX_ITCH_50" | cut -f1)) — stays on this machine, not zipped"
[ -f "$ROOT/CALL_BRIEF.md" ] && \
    echo "  CALL_BRIEF.md present — internal notes, must NOT be zipped"
for f in "$ROOT"/big*.itch; do
    [ -e "$f" ] && echo "  $(basename "$f") ($(du -h "$f" | cut -f1)) — scaling test file, not needed in the package"
done

# ── Summary ─────────────────────────────────────────────────────────────────
echo
echo "═══════════════════════════════════════════════════════"
if [ "$found" -eq 0 ]; then
    echo "  Clean. Nothing flagged."
else
    echo "  $found item(s) flagged above. Review each, then run package_delivery.sh."
fi
echo "═══════════════════════════════════════════════════════"
