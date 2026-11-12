# Market data

## The real data — not in this package

Every result in the documentation comes from **`20190730.BX_ITCH_50`**, a
complete NASDAQ BX trading day, 799 MB. It is already on the build machine at
`~/quant_accel/`.

It is not included here: it is licensed exchange data and is not ours to
redistribute. It does not need to be included, because it is already there.

`run_demo.sh` and all the test scripts point at it by default, so nothing needs
changing to reproduce the published results:

    cd ~/quant_accel
    ./run_demo.sh

**Always pass `--symbol` when using a real file.** It contains every listed
company interleaved; without a filter they all merge into one order book and the
output is meaningless.

## The synthetic data — included

- `emu_test.itch` — 300 generated messages in byte-accurate ITCH 5.0 format.
  Included so this package runs standalone on a machine without licensed data.
  The code path is identical.
- `gen_itch.py` — the generator. `--mode trending`, `--mode meanrev` and
  `--mode volatile` produce known market conditions, which is how the regime
  classifier is checked against ground truth.

Use synthetic data for quick functional checks. Use the real file for anything
that will be quoted or demonstrated.

One caution: the generator's risk-value distribution is unrealistic — roughly
37 times the real one — so it must not be used to calibrate thresholds.
