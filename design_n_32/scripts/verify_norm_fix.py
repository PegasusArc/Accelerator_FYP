#!/usr/bin/env python3
"""
verify_norm_fix.py — check the corrected inverse-square-root before spending
two hours building it.

Replays both the old and new algorithms in Python across the n2 range actually
observed on real market data (from about 1e-3 up to 1e6) and reports the error
against a true 1/sqrt. No hardware, no HLS, runs in a second.
"""
import math

def old_nr(a):
    """The delivered version: guess table stops at 1024, hn saturates at 8192."""
    safe = min(a, 8191.99)
    hn   = min(0.5 * a, 8191.99)      # ap_fixed<36,14> ceiling
    if   safe > 1024.0:   x = 0.015625
    elif safe > 256.0:    x = 0.031250
    elif safe > 64.0:     x = 0.062500
    elif safe > 16.0:     x = 0.125000
    elif safe > 4.0:      x = 0.250000
    elif safe > 1.0:      x = 0.500000
    elif safe > 0.25:     x = 1.000000
    elif safe > 0.0625:   x = 2.000000
    elif safe > 0.015625: x = 4.000000
    else:                 x = 8.000000
    for _ in range(5):
        x = x * (1.5 - hn * x * x)
        if not math.isfinite(x) or abs(x) > 1e12:
            return float('inf')
    return x

def new_nr(a):
    """Corrected: full table, every guess <= true value, no saturation."""
    safe = a
    hn   = 0.5 * a
    if   safe > 1048576.0: x = 0.00048828125
    elif safe > 262144.0:  x = 0.0009765625
    elif safe > 65536.0:   x = 0.001953125
    elif safe > 16384.0:   x = 0.00390625
    elif safe > 4096.0:    x = 0.0078125
    elif safe > 1024.0:    x = 0.015625
    elif safe > 256.0:     x = 0.031250
    elif safe > 64.0:      x = 0.062500
    elif safe > 16.0:      x = 0.125000
    elif safe > 4.0:       x = 0.250000
    elif safe > 1.0:       x = 0.500000
    elif safe > 0.25:      x = 1.000000
    elif safe > 0.0625:    x = 2.000000
    elif safe > 0.015625:  x = 4.000000
    else:                  x = 8.000000
    for _ in range(5):
        x = x * (1.5 - hn * x * x)
        if not math.isfinite(x) or abs(x) > 1e12:
            return float('inf')
    return x

print("  n2 values span roughly 1e-3 to 4e4 on real data; tested wider for margin.\n")
print(f"  {'n2':>12} {'exact 1/sqrt':>14} {'old err %':>12} {'new err %':>12}")
print("  " + "-" * 54)

worst_old = worst_new = 0.0
a = 1e-3
while a <= 4e6:
    exact = 1.0 / math.sqrt(a)
    o, n = old_nr(a), new_nr(a)
    eo = float('inf') if not math.isfinite(o) else abs(o - exact) / exact * 100
    en = float('inf') if not math.isfinite(n) else abs(n - exact) / exact * 100
    if math.isfinite(eo): worst_old = max(worst_old, eo)
    if math.isfinite(en): worst_new = max(worst_new, en)
    so = "DIVERGED" if not math.isfinite(eo) else f"{eo:12.4f}"
    sn = "DIVERGED" if not math.isfinite(en) else f"{en:12.4f}"
    print(f"  {a:12.4g} {exact:14.8f} {so:>12} {sn:>12}")
    a *= 10

print()
print(f"  worst old error : {worst_old:.4f}%   (excluding outright divergence)")
print(f"  worst new error : {worst_new:.6f}%")
print()
print("  The measured cases that failed on hardware:")
for a, sym in [(14037.7, "IWO median"), (31291.9, "IWO max"),
               (13695.3, "MDY median"), (41854.7, "MDY max"),
               (11007.6, "AAPL max"),   (95.5,    "XLB median")]:
    exact = 1.0 / math.sqrt(a)
    o, n = old_nr(a), new_nr(a)
    eo = float('inf') if not math.isfinite(o) else abs(o - exact) / exact * 100
    en = float('inf') if not math.isfinite(n) else abs(n - exact) / exact * 100
    so = "DIVERGED" if not math.isfinite(eo) else f"{eo:9.3f}%"
    sn = "DIVERGED" if not math.isfinite(en) else f"{en:9.6f}%"
    print(f"    {sym:<14} n2={a:10.1f}   old {so}   new {sn}")
