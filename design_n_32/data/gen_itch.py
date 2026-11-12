#!/usr/bin/env python3
"""
gen_itch.py — synthesise a NASDAQ ITCH 5.0 binary file.

WHY: end-to-end validation is an Offer 2 deliverable, but real NASDAQ ITCH
sample data is licensed and not redistributable. This generator produces a
byte-accurate ITCH 5.0 stream (length-prefixed, big-endian, prices as int/1e4)
that exercises every message type the parser handles, so the whole chain can be
validated today. If the client obtains a real 01302019.NASDAQ_ITCH50 sample,
the same replay path consumes it unchanged.

Message layouts implemented (offsets match itch_parser.cpp exactly):
  'A' Add Order        36 B   ref@11 side@19 shares@20 stock@24 price@32
  'E' Order Executed   31 B   ref@11 exec_shares@19 match@23
  'X' Order Cancel     23 B   ref@11 cancelled@19
  'D' Order Delete     19 B   ref@11
  'U' Order Replace    35 B   orig_ref@11 new_ref@19 shares@27 price@31

Usage:
  python3 gen_itch.py --out ticks.itch --messages 20000 --seed 42
  python3 gen_itch.py --out trend.itch --messages 20000 --mode trending
  python3 gen_itch.py --out chop.itch  --messages 20000 --mode meanrev
  python3 gen_itch.py --out vol.itch   --messages 20000 --mode volatile

The three modes exist so the Regime Classifier can be checked against ground
truth: a file generated in `trending` mode SHOULD be labelled TRENDING. That is
the only honest way to validate a regime label without a labelled market
dataset.
"""

import argparse
import random
import struct

STOCK = b"AAPL    "          # 8 bytes, space padded


def _hdr(msg_type: bytes, ts: int) -> bytes:
    """msg_type(1) + stock_locate(2) + tracking(2) + timestamp(6)"""
    return (msg_type
            + struct.pack(">H", 1)          # stock_locate
            + struct.pack(">H", 0)          # tracking number
            + ts.to_bytes(6, "big"))        # nanoseconds since midnight


def _frame(body: bytes) -> bytes:
    return struct.pack(">H", len(body)) + body


def add_order(ts, ref, side, shares, price):
    b = (_hdr(b"A", ts)
         + struct.pack(">Q", ref)
         + (b"B" if side == 0 else b"S")
         + struct.pack(">I", shares)
         + STOCK
         + struct.pack(">I", int(round(price * 10000))))
    assert len(b) == 36, len(b)
    return _frame(b)


def order_executed(ts, ref, shares, match):
    b = (_hdr(b"E", ts)
         + struct.pack(">Q", ref)
         + struct.pack(">I", shares)
         + struct.pack(">Q", match))
    assert len(b) == 31, len(b)
    return _frame(b)


def order_cancel(ts, ref, shares):
    b = (_hdr(b"X", ts)
         + struct.pack(">Q", ref)
         + struct.pack(">I", shares))
    assert len(b) == 23, len(b)
    return _frame(b)


def order_delete(ts, ref):
    b = _hdr(b"D", ts) + struct.pack(">Q", ref)
    assert len(b) == 19, len(b)
    return _frame(b)


def order_replace(ts, orig_ref, new_ref, shares, price):
    b = (_hdr(b"U", ts)
         + struct.pack(">Q", orig_ref)
         + struct.pack(">Q", new_ref)
         + struct.pack(">I", shares)
         + struct.pack(">I", int(round(price * 10000))))
    assert len(b) == 35, len(b)
    return _frame(b)


def system_event(ts, code=b"O"):
    b = _hdr(b"S", ts) + code
    return _frame(b)


def generate(n_msgs, seed, mode, mid0=150.00):
    rng = random.Random(seed)
    out = bytearray()
    ts = 9 * 3600 * 10**9                    # 09:00:00.000000000
    out += system_event(ts, b"O")

    mid = mid0
    live = {}          # ref -> (side, price, shares)
    next_ref = 1000

    # Mode shapes the mid-price process. This is the ground truth the regime
    # classifier is scored against.
    for _ in range(n_msgs):
        ts += rng.randint(1_000, 60_000)     # 1-60 us between messages

        if mode == "trending":
            mid += rng.gauss(0.0008, 0.0015)             # persistent drift
        elif mode == "meanrev":
            mid += 0.05 * (mid0 - mid) + rng.gauss(0, 0.004)
        elif mode == "volatile":
            mid += rng.gauss(0, 0.02)
            if rng.random() < 0.03:
                mid += rng.choice([-1, 1]) * rng.uniform(0.05, 0.20)
        else:                                             # "mixed"
            mid += 0.02 * (mid0 - mid) + rng.gauss(0, 0.006)
            if rng.random() < 0.01:
                mid += rng.choice([-1, 1]) * rng.uniform(0.03, 0.12)
        mid = max(1.0, mid)

        r = rng.random()

        # 55% add, 15% execute, 12% cancel, 12% delete, 6% replace
        if r < 0.55 or not live:
            side = rng.randint(0, 1)
            tick = rng.randint(1, 5) * 0.01
            price = round(mid - tick if side == 0 else mid + tick, 4)
            shares = rng.choice([100, 200, 300, 500, 800, 1000, 1500])
            ref = next_ref; next_ref += 1
            live[ref] = (side, price, shares)
            out += add_order(ts, ref, side, shares, price)

        elif r < 0.70:
            ref = rng.choice(list(live))
            side, price, shares = live[ref]
            ex = min(shares, rng.choice([100, 200, shares]))
            out += order_executed(ts, ref, ex, next_ref)
            if ex >= shares: del live[ref]
            else: live[ref] = (side, price, shares - ex)

        elif r < 0.82:
            ref = rng.choice(list(live))
            side, price, shares = live[ref]
            cx = min(shares, rng.choice([100, shares]))
            out += order_cancel(ts, ref, cx)
            if cx >= shares: del live[ref]
            else: live[ref] = (side, price, shares - cx)

        elif r < 0.94:
            ref = rng.choice(list(live))
            out += order_delete(ts, ref)
            del live[ref]

        else:
            orig = rng.choice(list(live))
            side, price, shares = live[orig]
            new_ref = next_ref; next_ref += 1
            tick = rng.randint(1, 5) * 0.01
            new_px = round(mid - tick if side == 0 else mid + tick, 4)
            new_sh = rng.choice([100, 200, 500, 1000])
            out += order_replace(ts, orig, new_ref, new_sh, new_px)
            del live[orig]
            live[new_ref] = (side, new_px, new_sh)

    out += system_event(ts + 1000, b"C")
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--messages", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--mode", default="mixed",
                    choices=["mixed", "trending", "meanrev", "volatile"])
    ap.add_argument("--mid", type=float, default=150.00)
    a = ap.parse_args()

    data = generate(a.messages, a.seed, a.mode, a.mid)
    with open(a.out, "wb") as f:
        f.write(data)
    print(f"wrote {a.out}: {len(data)} bytes, {a.messages} messages, mode={a.mode}")


if __name__ == "__main__":
    main()
