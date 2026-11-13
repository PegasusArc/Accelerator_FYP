#!/usr/bin/env python3
"""
dataflow_model.py - software baseline analysis and data-path model.

Reproduces the analysis behind the data-path choices of the thesis
(Section 3.3) and draws its two figures:

  fig_sw_profile.png     where the Python baseline spends its time per
                         update, and what the hardware does with each stage
  fig_datapath_model.png (a) cycles per event against the interleaving
                         factor N: the recurrence bound ceil(L/N), the
                         C-synthesis sweep and the card measurements;
                         (b) share of one-sided events per instrument on
                         the 128-instrument run

It also prints the three trade-offs as tables: interleaving factor,
output-record width, and per-event emission, plus the latency summary.

    python3 dataflow_model.py [path/to/tableA1.csv]

Inputs are measurements, each listed below with where it comes from. The
model adds arithmetic to them; it does not invent any figure. Needs
matplotlib.
"""
import csv
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Measured inputs
# ---------------------------------------------------------------------------

# Software baseline: the Phase 1 NumPy implementation of IncTSVD
# Algorithm 3, re-timed on a desktop core.
# 0.54 ms per update without the profiler, 0.56 ms with it; stage shares
# from per-stage timers over the same run.
SW_MS_PER_UPDATE = 0.54
SW_STAGES = [
    # (stage, share %, hardware treatment, what the hardware does with it)
    ("Mode-3 transforms\n(FFT and inverse FFT)", 25.3,
     "forward: two-point butterfly, adders only;\ninverse: not applied in hardware", "partial"),
    ("Projection, residual\nand QR", 49.0,
     "unrolled multiply-accumulate;\nQR of one column = norm + scale", "full"),
    ("Kernel SVD", 25.7,
     "not computed: replaced by the\nforgetting-factor recurrence", "replaced"),
]
# The same code profiled on Google Colab in Phase 1 (thesis Figure 3.3):
COLAB_TICKS = 10_000
COLAB_CALLS = 3_800_292
COLAB_SECONDS = 37.915

# Decomposition unit (inctsvd_top), C-synthesis initiation interval against
# the interleaving factor N, development builds (thesis Table 4.8).
SWEEP = {1: 116, 4: 30, 8: 15, 16: 8, 32: 4, 64: 2, 128: 1}
L_FINAL = 122          # dependent path of the final build, cycles

# Card measurements (end-to-end cycles per event of the whole pipeline).
F_MHZ = 300.0
CARD = {
    # N: (cycles per event, decisions per second, note)
    32: (7.215, 40.449e6, "N = 32 build, 300 MHz"),
    128: (5.784, 48.827e6, "N = 128 build, 300 MHz"),
}
# Single-instrument design (Offer 2): 1.436 M decisions/s at 166.67 MHz,
# decomposition II 116 on decided ticks only.
SINGLE = (166.67e6 / 1.436e6, 1.436e6)

# Output record: 10 live fields = 40 bytes; memory beat = 512 bits.
BEAT_BYTES = 64
LIVE_BYTES = 40
SINK_CYCLES_40B = 52.2     # measured, 300 MHz builds, cycles per decision

# Pipeline depth of each unit's per-event loop, N = 128 build (HLS logs).
DEPTH_N128 = [("upd_inject", 2), ("lob_filter", 124), ("inctsvd_top", 122),
              ("sv_bcast3 / sv_bcast2", 2),
              ("risk_matrix (longest of the three estimators)", 102),
              ("decision_sink fill", 3)]
# SoC build, per-module HLS latency (Offer 1, nominal part, 166.67 MHz).
SOC_LAT = [("inctsvd_top", 23), ("momentum_signal", 2),
           ("risk_matrix", 69), ("regime_classifier", 39)]

# ---------------------------------------------------------------------------
# Style: one axis per chart, recessive grid, ink-coloured text
# ---------------------------------------------------------------------------
BLUE, ORANGE, BLUE_L = "#2a78d6", "#eb6834", "#9ec2ef"
GREY, GREY_D = "#b9b7b0", "#8a8880"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e5e4e0"
plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9,
    "axes.edgecolor": GREY_D, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": INK2, "ytick.color": INK2,
    "axes.spines.top": False, "axes.spines.right": False,
    "figure.dpi": 100, "savefig.dpi": 300,
})


def ceil_div(a, b):
    return -(-a // b)


def table(title, header, rows):
    print("\n" + title)
    w = [max(len(str(x)) for x in col) for col in zip(header, *rows)]
    line = "  " + "  ".join(str(h).ljust(n) for h, n in zip(header, w))
    print(line)
    print("  " + "  ".join("-" * n for n in w))
    for r in rows:
        print("  " + "  ".join(str(x).ljust(n) for x, n in zip(r, w)))


# ---------------------------------------------------------------------------
# 1. Software baseline: where the time goes
# ---------------------------------------------------------------------------
def software_profile():
    rows = []
    for name, share, hw, kept in SW_STAGES:
        us = SW_MS_PER_UPDATE * 1000 * share / 100
        rows.append((name.replace("\n", " "), "%.1f%%" % share, "%.0f us" % us,
                     {"full": "computed", "partial": "forward only",
                      "replaced": "replaced"}[kept], hw.replace("\n", " ")))
    table("Software baseline, %.2f ms per update (desktop core)" % SW_MS_PER_UPDATE,
          ("Stage", "Share", "Time", "Hardware", "How"), rows)
    kept = sum(s for _, s, _, k in SW_STAGES if k != "replaced")
    print("  replaced by a recurrence: %.1f%% (kernel SVD); the other %.1f%% maps to fixed-cost"
          " arithmetic (of the transforms, the forward one only)" % (100 - kept, kept))
    print("  Colab profile: %d calls in %.3f s for %d updates = %.2f ms per update"
          % (COLAB_CALLS, COLAB_SECONDS, COLAB_TICKS, COLAB_SECONDS / COLAB_TICKS * 1e3))

    fig = plt.figure(figsize=(6.3, 2.5))
    ax = fig.add_axes([0.25, 0.22, 0.36, 0.62])
    y = list(range(len(SW_STAGES)))[::-1]
    for yi, (name, share, hw, kept) in zip(y, SW_STAGES):
        fill = {"full": BLUE, "partial": BLUE_L, "replaced": "white"}[kept]
        edge = {"full": BLUE, "partial": BLUE_L, "replaced": GREY_D}[kept]
        ax.barh(yi, share, height=0.56, color=fill, edgecolor=edge,
                hatch="///" if kept == "replaced" else None, linewidth=1.0)
        us = SW_MS_PER_UPDATE * 1000 * share / 100
        ax.text(share + 1.0, yi, "%.1f%% (%.0f µs)" % (share, us), va="center",
                ha="left", color=INK, fontsize=8.5)
        ax.text(1.45, yi, hw, va="center", ha="left", color=INK2, fontsize=8,
                transform=ax.get_yaxis_transform(), clip_on=False)
    ax.set_yticks(y)
    ax.set_yticklabels([s[0] for s in SW_STAGES], fontsize=8.5, color=INK)
    ax.set_xlim(0, 50)
    ax.set_xticks([0, 10, 20, 30, 40, 50])
    ax.set_xlabel("Share of the software time per update, %", color=INK2, fontsize=8.5)
    ax.grid(axis="x", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)
    ax.text(1.45, len(SW_STAGES) - 0.45, "What the hardware data path does", ha="left",
            va="bottom", fontsize=8.5, color=INK, fontweight="bold",
            transform=ax.get_yaxis_transform(), clip_on=False)
    fig.legend(handles=[Patch(facecolor=BLUE, edgecolor=BLUE, label="fixed-cost logic on the card"),
                        Patch(facecolor=BLUE_L, edgecolor=BLUE_L,
                              label="forward transform on the card, inverse not applied"),
                        Patch(facecolor="white", edgecolor=GREY_D, hatch="///",
                              label="replaced by a recurrence")],
               loc="upper left", bbox_to_anchor=(0.02, 1.02), ncol=3, frameon=False,
               fontsize=7.5, handlelength=1.4, columnspacing=1.0)
    out = os.path.join(HERE, "fig_sw_profile.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("  wrote", out)


# ---------------------------------------------------------------------------
# 2. Interleaving factor, record width, per-event emission
# ---------------------------------------------------------------------------
def interleaving():
    rows = []
    for n in (1, 2, 4, 8, 16, 32, 64, 128, 256):
        ii = ceil_div(L_FINAL, n)
        bound = F_MHZ * 1e6 / ii
        rows.append((n, ii, SWEEP.get(n, "-"), "%.1f" % (bound / 1e6),
                     "%.3f" % CARD[n][0] if n in CARD else "-"))
    table("Interleaving factor: recurrence bound II = ceil(%d/N) at %.0f MHz" % (L_FINAL, F_MHZ),
          ("N", "II model", "II synthesis", "bound, M updates/s", "card, cycles/event"), rows)
    best = min(n for n in (1, 2, 4, 8, 16, 32, 64, 128, 256) if ceil_div(L_FINAL, n) == 1)
    print("  smallest power of two with II = 1: N = %d (L = %d)" % (best, L_FINAL))
    return best


def record_width():
    rows = []
    for size in (32, 40, 48, 56, 64, 128):
        tiles = (BEAT_BYTES % size == 0) or (size % BEAT_BYTES == 0)
        beats = ("%g" % (size / BEAT_BYTES)) if tiles else "straddles beats"
        pad = size - LIVE_BYTES
        rows.append((size, "yes" if tiles else "no", beats,
                     ("%d B" % pad) if size >= LIVE_BYTES else "too small"))
    table("Output record against the %d-byte memory beat (%d bytes of live fields)"
          % (BEAT_BYTES, LIVE_BYTES), ("Bytes", "Tiles the beat", "Beats per record", "Padding"),
          rows)
    print("  smallest size >= %d B that tiles the beat: 64 B (measured sink cost at 40 B: "
          "%.1f cycles per decision)" % (LIVE_BYTES, SINK_CYCLES_40B))


def one_sided(csv_path):
    fr = []
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            e, d = int(r["events"]), int(r["decisions"])
            fr.append((100.0 * (e - d) / e, r["symbol"], e - d, e))
    fr.sort(reverse=True)
    tot_e = sum(x[3] for x in fr)
    tot_1 = sum(x[2] for x in fr)
    vals = sorted(x[0] for x in fr)
    med = (vals[len(vals) // 2 - 1] + vals[len(vals) // 2]) / 2
    n_any = sum(1 for x in fr if x[2] >= 1)
    n_one = sum(1 for x in fr if x[2] == 1)
    print("\nPer-event emission: one-sided events on the 128-instrument run")
    print("  %d of %d events one-sided (%.2f%%); instruments with at least one: %d of %d "
          "(%d with exactly one); median share %.2f%%; largest %s %.1f%%"
          % (tot_1, tot_e, 100.0 * tot_1 / tot_e, n_any, len(fr), n_one, med, fr[0][1], fr[0][0]))
    print("  dropping them would break the N-event spacing for %d of %d instruments -> emit "
          "every event and flag validity" % (n_any, len(fr)))
    return fr, med, n_any, n_one


def figure_model(fr, med, n_any, n_one):
    fig, (a, b) = plt.subplots(2, 1, figsize=(6.3, 6.4),
                               gridspec_kw={"height_ratios": [1.15, 1.0], "hspace": 0.55})

    # (a) cycles per event against N, log-log
    ns = [1, 2, 4, 8, 16, 32, 64, 128, 256]
    a.plot(ns, [ceil_div(L_FINAL, n) for n in ns], color=GREY_D, linewidth=2,
           drawstyle="steps-mid", label="recurrence bound, ⌈%d/N⌉" % L_FINAL, zorder=2)
    sx = sorted(SWEEP)
    a.plot(sx, [SWEEP[n] for n in sx], linestyle="none", marker="o", markersize=7,
           markerfacecolor=BLUE, markeredgecolor="white", markeredgewidth=1.2,
           label="decomposition II, C-synthesis", zorder=4)
    cx = sorted(CARD)
    a.plot([1] + cx, [SINGLE[0]] + [CARD[n][0] for n in cx], linestyle="none",
           marker="D", markersize=7, markerfacecolor=ORANGE, markeredgecolor="white",
           markeredgewidth=1.2, label="whole pipeline, measured on the card", zorder=5)
    a.set_xscale("log", base=2)
    a.set_yscale("log")
    a.set_xticks(ns)
    a.set_xticklabels([str(n) for n in ns])
    a.set_yticks([1, 2, 4, 8, 16, 32, 64, 128])
    a.set_yticklabels(["1", "2", "4", "8", "16", "32", "64", "128"])
    a.set_ylim(0.7, 520)
    a.set_xlabel("Interleaving factor N (instruments in the rotation)", color=INK2)
    a.set_ylabel("Cycles per event", color=INK2)
    a.grid(color=GRID, linewidth=0.6)
    a.set_axisbelow(True)
    a.annotate("N = 1: 116 cycles, 1.44 M decisions/s", xy=(1, SINGLE[0]), xytext=(1.45, 250),
               fontsize=8, color=INK2, arrowprops=dict(arrowstyle="-", color=GREY_D, lw=0.8))
    a.annotate("N = 32: 7.2 cycles,\n40.45 M decisions/s", xy=(32, CARD[32][0]), xytext=(9, 26),
               fontsize=8, color=INK2, arrowprops=dict(arrowstyle="-", color=GREY_D, lw=0.8))
    a.annotate("N = 128: 5.8 cycles,\n48.83 M decisions/s", xy=(128, CARD[128][0]),
               xytext=(40, 13), fontsize=8, color=INK2, ha="left",
               arrowprops=dict(arrowstyle="-", color=GREY_D, lw=0.8))
    a.annotate("II reaches 1 at N = 128", xy=(128, 1), xytext=(128, 2.9), fontsize=8,
               color=INK2, ha="center", va="center",
               arrowprops=dict(arrowstyle="-", color=GREY_D, lw=0.8))
    a.legend(loc="lower left", frameon=False, fontsize=8)
    a.set_title("(a) Interleaving factor: decomposition interval and pipeline cost",
                loc="left", fontsize=9, color=INK)

    # (b) one-sided share per instrument, sorted
    vals = [x[0] for x in fr]
    xs = range(1, len(vals) + 1)
    b.bar(xs, vals, width=0.8, color=BLUE, linewidth=0)
    b.set_yscale("log")
    b.set_ylim(0.01, 100)
    b.set_yticks([0.01, 0.1, 1, 10, 100])
    b.set_yticklabels(["0.01", "0.1", "1", "10", "100"])
    b.set_xlim(0, len(vals) + 1)
    b.set_xticks([1, 32, 64, 96, 128])
    b.set_xlabel("Instruments, ranked by share of one-sided events", color=INK2)
    b.set_ylabel("One-sided events, %", color=INK2)
    b.axhline(med, color=GREY_D, linewidth=1, linestyle="--")
    b.text(len(vals) - 1, med * 1.25, "median %.2f%%" % med, ha="right", va="bottom",
           fontsize=8, color=INK2)
    b.text(3, fr[0][0] * 1.08, "%s %.1f%%" % (fr[0][1], fr[0][0]), fontsize=8, color=INK2,
           va="bottom")
    b.text(64, 22, "every instrument has at least one one-sided event\n"
           "(%d have exactly one); dropping them would break the\n"
           "128-event spacing for all %d" % (n_one, n_any),
           fontsize=8, color=INK2, ha="center")
    b.grid(axis="y", color=GRID, linewidth=0.6)
    b.set_axisbelow(True)
    b.set_title("(b) Per-event emission: events with a one-sided book, 128-instrument run",
                loc="left", fontsize=9, color=INK)

    out = os.path.join(HERE, "fig_datapath_model.png")
    fig.savefig(out, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("  wrote", out)


def latency():
    cyc = sum(c for _, c in DEPTH_N128)
    table("Per-event compute latency, 128-instrument build (pipeline depths, %.0f MHz)" % F_MHZ,
          ("Unit", "Depth, cycles"), DEPTH_N128)
    print("  sum %d cycles = %.2f us unloaded (synthesis estimate; excludes stream FIFOs,"
          " queueing and the sink's 64-record burst)" % (cyc, cyc / F_MHZ))
    print("  amortised: %.3f cycles per event = %.1f ns per event"
          % (CARD[128][0], CARD[128][0] / F_MHZ * 1e3))
    seq = sum(c for _, c in SOC_LAT)
    par = SOC_LAT[0][1] + max(c for _, c in SOC_LAT[1:])
    print("  SoC build, firmware-sequenced compute: %d cycles = %.0f ns at 166.67 MHz "
          "(apps in parallel: %d cycles); firmware polling not included" % (seq, seq * 6.0, par))


if __name__ == "__main__":
    csv_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "tableA1.csv")
    software_profile()
    interleaving()
    record_width()
    fr, med, n_any, n_one = one_sided(csv_path)
    figure_model(fr, med, n_any, n_one)
    latency()
