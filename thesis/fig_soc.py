#!/usr/bin/env python3
"""
fig_soc.py - block diagram of the RISC-V system-on-chip build (Offer 1).

Drawn from the delivered block design: create_bd_client_final.tcl, the
build log of the as-built design (bake_run.log, which includes
axi_gpio_0), the Vivado address editor and u50_final.xdc. Writes
fig_soc.png next to this file.
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Patch
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))

INK, INK2 = "#0b0b0b", "#52514e"
BLUE, ORANGE, GREY = "#2a78d6", "#eb6834", "#8a8880"
FILL = {
    "cpu": ("#fdece3", ORANGE),
    "infra": ("#f2f2f1", GREY),
    "accel": ("#e8f0fb", BLUE),
    "mem": ("#fdf5dc", "#9a8a3a"),
    "board": ("#ffffff", GREY),
}
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 7.5,
                     "savefig.dpi": 300})

W, H = 126.0, 124.0
fig = plt.figure(figsize=(6.3, 6.2))
ax = fig.add_axes([0, 0, 1, 1])
ax.set_xlim(0, W)
ax.set_ylim(0, H)
ax.axis("off")


def box(x0, y0, x1, y1, kind, lines, bold_first=True, dashed=False, fs=7.2, lw=1.1,
        radius=0.8):
    fc, ec = FILL[kind]
    ax.add_patch(FancyBboxPatch((x0, y0), x1 - x0, y1 - y0,
                                boxstyle="round,pad=0,rounding_size=%g" % radius,
                                facecolor=fc, edgecolor=ec, linewidth=lw,
                                linestyle="--" if dashed else "-", zorder=3))
    n = len(lines)
    cy = (y0 + y1) / 2
    step = 3.1
    top = cy + step * (n - 1) / 2
    for i, t in enumerate(lines):
        mono = t.startswith("0x")
        ax.text((x0 + x1) / 2, top - i * step, t, ha="center", va="center",
                fontsize=fs - (0.4 if mono else 0) - (0 if i == 0 else 0.3),
                family="DejaVu Sans Mono" if mono else "DejaVu Sans",
                fontweight="bold" if (i == 0 and bold_first) else "normal",
                color=INK if i == 0 else INK2, zorder=4)


def line(xs, ys, color=GREY, lw=1.0, ls="-", arrow=True, z=2):
    ax.plot(xs, ys, color=color, linewidth=lw, linestyle=ls, zorder=z,
            solid_capstyle="butt")
    if arrow:
        ax.annotate("", xy=(xs[-1], ys[-1]), xytext=(xs[-2], ys[-2]),
                    arrowprops=dict(arrowstyle="-|>", color=color, lw=lw,
                                    mutation_scale=7, shrinkA=0, shrinkB=0), zorder=z)


def label(x, y, t, ha="center", va="center", color=INK2, fs=6.6, bg=True, style="normal"):
    ax.text(x, y, t, ha=ha, va=va, fontsize=fs, color=color, fontstyle=style, zorder=6,
            bbox=dict(facecolor="white", edgecolor="none", pad=0.4) if bg else None)


# ---- board-level parts, outside the FPGA ---------------------------------
box(3, 112, 35, 122, "board", ["Board clock SYSCLK2", "100 MHz LVDS, pins G17/G16"], fs=6.8)

# ---- FPGA frame -----------------------------------------------------------
ax.add_patch(FancyBboxPatch((1.2, 17), W - 2.4, 91.5,
                            boxstyle="round,pad=0,rounding_size=1.5",
                            facecolor="#fbfbfa", edgecolor="#9d9b94", linewidth=1.0, zorder=1))
label(23, 105.3, "Alveo U50 FPGA (xcu50-fsvh2104), Vivado block design, "
      "one 166.67 MHz clock domain", ha="left", color=INK, fs=7.4, bg=False)

# clocking and reset
box(3, 88, 35, 100, "infra", ["Clocking Wizard (MMCM)", "100 MHz in, 166.67 MHz out"])
box(41, 88, 64, 100, "infra", ["Processor System", "Reset"])
line([19, 19], [112, 100.2], color=GREY)
line([35, 40.8], [94, 94], color=GREY)
label(37.9, 96.2, "locked", fs=6.0, bg=False)
label(68, 94, "clock and active-low reset\nto every block", ha="left", fs=6.8, bg=False)

# processor
box(3, 66, 35, 82, "cpu", ["PicoRV32 soft processor", "32-bit RISC-V, AXI4-Lite master",
                           "firmware polls ap_done", "interrupts not used"])


# firmware sequence, as a note beside the processor
ax.add_patch(FancyBboxPatch((40, 66), 83, 16, boxstyle="round,pad=0,rounding_size=0.8",
                            facecolor="white", edgecolor=ORANGE, linewidth=0.7,
                            linestyle=(0, (2, 2)), zorder=2))
ax.text(42, 79.8, "Firmware loop, once per decision (C, 756-byte image)", ha="left",
        va="center", fontsize=6.8, color=INK, fontweight="bold", zorder=4)
steps = ["1  start inctsvd_top, poll ap_done",
         "2  write the momentum EMA back, start momentum_signal, poll ap_done",
         "3  start risk_matrix, poll ap_done;  4  start regime_classifier, poll ap_done",
         "5  2-of-3 vote on the three signals, write the result to the GPIO"]
for i, t in enumerate(steps):
    ax.text(42, 76.4 - i * 2.75, t, ha="left", va="center", fontsize=6.4, color=INK2, zorder=4)
line([35, 39.8], [74, 74], color=ORANGE, lw=0.8, arrow=False)

# interconnect bar
IC_Y0, IC_Y1 = 55.0, 60.0
box(3, IC_Y0, 123, IC_Y1, "infra", ["AXI Interconnect: 1 master, 7 slaves (AXI4-Lite control)"],
    fs=7.0, radius=0.6)
line([19, 19], [66, IC_Y1 + 0.1], color=ORANGE, lw=1.3)
label(21, 62.8, "AXI4-Lite", ha="left", fs=6.4, bg=False)

# slaves in one row
names = [
    ("infra", ["AXI BRAM", "controller", "0x0000_0000"]),
    ("infra", ["AXI GPIO", "0x4004_0000"]),
    ("accel", ["lob_filter", "0x4005_0000", "book, tick slice"]),
    ("accel", ["inctsvd_top", "0x4000_0000", "rank-4 update"]),
    ("accel", ["momentum_", "signal", "0x4001_0000"]),
    ("accel", ["risk_matrix", "0x4002_0000"]),
    ("accel", ["regime_", "classifier", "0x4003_0000"]),
]
x0, x1 = 3.0, 123.0
gap = 2.1
bw = (x1 - x0 - gap * (len(names) - 1)) / len(names)
SY0, SY1 = 37.0, 50.5
xs = []
for i, (kind, lines) in enumerate(names):
    a = x0 + i * (bw + gap)
    b = a + bw
    xs.append((a, b))
    if kind == "accel" and lines[0] in ("momentum_", "regime_"):
        # two-line identifier: keep both lines in the heading style
        box(a, SY0, b, SY1, kind, [lines[0] + "\n" + lines[1], lines[2]], fs=6.9)
    else:
        box(a, SY0, b, SY1, kind, lines, fs=6.9)
    line([(a + b) / 2, (a + b) / 2], [IC_Y0 - 0.1, SY1 + 0.1], color=GREY, lw=1.0)

(bram_a, bram_b), (gpio_a, gpio_b), (lob_a, lob_b), (svd_a, svd_b), \
    (mom_a, mom_b), (rsk_a, rsk_b), (rgm_a, rgm_b) = xs

# the accelerator group label
ax.add_patch(FancyBboxPatch((lob_a - 1.0, 19.3), rgm_b - lob_a + 2.0, SY1 - 19.3 + 1.2,
                            boxstyle="round,pad=0,rounding_size=1.0",
                            facecolor="none", edgecolor=BLUE, linewidth=0.6,
                            linestyle=(0, (1, 2)), zorder=1.5))
label(rgm_b - 0.5, 21.2, "HLS IP cores, Vitis HLS 2022.1, one instrument", ha="right",
      color=BLUE, fs=6.4, bg=False, style="italic")

# firmware memory under the BRAM controller
box(bram_a, 24.5, bram_b, 31.5, "mem", ["Block RAM", "8 KB"], fs=6.8)
line([(bram_a + bram_b) / 2, (bram_a + bram_b) / 2], [SY0 - 0.1, 31.6], color=GREY)
label((bram_a + bram_b) / 2 + 0.6, 34.3, "ports A, B", ha="left", fs=5.8, bg=False)

# GPIO to the QSFP LEDs, off chip
box(gpio_a - 3.5, 2.5, gpio_b + 14.5, 12.5, "board", ["QSFP status LEDs",
                                                  "signal_out[2:0], pins E18/E16/F17"], fs=6.6)
line([(gpio_a + gpio_b) / 2, (gpio_a + gpio_b) / 2], [SY0 - 0.1, 12.6], color=GREY)
label((gpio_a + gpio_b) / 2 + 0.8, 28.5, "final\nvote", ha="left", fs=5.8, bg=False)

# ---- AXI4-Stream links below the accelerator row --------------------------
def stream(xa, xb, depth, color, ls, text, tx=None, ty_off=0.0):
    yb = SY0 - depth
    ax.plot([xa, xa, xb], [SY0 - 0.05, yb, yb], color=color, lw=1.3, linestyle=ls, zorder=2)
    ax.annotate("", xy=(xb, SY0 - 0.05), xytext=(xb, yb),
                arrowprops=dict(arrowstyle="-|>", color=color, lw=1.3, linestyle=ls,
                                mutation_scale=7, shrinkA=0, shrinkB=0), zorder=2)
    label(tx if tx is not None else (xa + xb) / 2, yb + 1.25 + ty_off, text, fs=5.9, bg=True)


lob_c = (lob_a + lob_b) / 2
svd_c = (svd_a + svd_b) / 2
stream(lob_c + 3.0, svd_a + 2.5, 2.4, GREY, "--", "Bt: open", tx=(lob_c + svd_a) / 2 + 1.4)
stream(svd_c + 4.0, mom_a + 4.0, 2.4, BLUE, "-", "σ, 4 lanes", tx=(svd_c + mom_a) / 2 + 4.0)
stream(svd_c + 1.5, rsk_a + 4.0, 5.2, BLUE, "-", "m, 4 lanes", tx=(mom_a + mom_b) / 2 + 3)
stream(svd_c - 1.0, rgm_a + 5.0, 8.0, GREY, "--", "σ: open", tx=(rsk_b + rgm_a) / 2 + 1)
# branch of the open σ link into risk_matrix
ax.plot([rsk_b - 3.0, rsk_b - 3.0], [SY0 - 8.0, SY0 - 0.05], color=GREY, lw=1.3,
        linestyle="--", zorder=2)
ax.annotate("", xy=(rsk_b - 3.0, SY0 - 0.05), xytext=(rsk_b - 3.0, SY0 - 3.0),
            arrowprops=dict(arrowstyle="-|>", color=GREY, lw=1.3, mutation_scale=7,
                            shrinkA=0, shrinkB=0), zorder=2)
# update_in into lob_filter: no source in this build
ax.plot([lob_a + 2.5, lob_a + 2.5], [SY0 - 7.5, SY0 - 0.05], color=GREY, lw=1.3,
        linestyle="--", zorder=2)
ax.annotate("", xy=(lob_a + 2.5, SY0 - 0.05), xytext=(lob_a + 2.5, SY0 - 2.5),
            arrowprops=dict(arrowstyle="-|>", color=GREY, lw=1.3, mutation_scale=7,
                            shrinkA=0, shrinkB=0), zorder=2)
label(lob_a + 3.5, SY0 - 8.6, "update_in: no source", ha="left", fs=5.9, bg=True)

# ---- legend -----------------------------------------------------------------
handles = [
    Patch(facecolor=FILL["cpu"][0], edgecolor=FILL["cpu"][1], label="soft processor"),
    Patch(facecolor=FILL["accel"][0], edgecolor=FILL["accel"][1], label="HLS accelerator IP"),
    Patch(facecolor=FILL["infra"][0], edgecolor=FILL["infra"][1], label="AMD infrastructure IP"),
    Patch(facecolor=FILL["mem"][0], edgecolor=FILL["mem"][1], label="on-chip memory"),
    Line2D([0], [0], color=GREY, lw=1.0, label="AXI4-Lite control"),
    Line2D([0], [0], color=BLUE, lw=1.3, label="AXI4-Stream, connected"),
    Line2D([0], [0], color=GREY, lw=1.3, ls="--", label="data input left open"),
]
fig.legend(handles=handles, loc="lower left", bbox_to_anchor=(0.41, 0.0), ncol=2,
           frameon=False, fontsize=6.6, handlelength=1.8, columnspacing=1.2,
           handletextpad=0.5, labelspacing=0.35)

out = os.path.join(HERE, "fig_soc.png")
fig.savefig(out, facecolor="white", bbox_inches="tight", pad_inches=0.04)
print("wrote", out)
