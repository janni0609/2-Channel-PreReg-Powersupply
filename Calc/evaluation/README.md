# Evaluation — bench measurements against a reference instrument

Scripts that measure the *built* power supply and compare it against a
calibrated reference. This is the empirical counterpart to the paper analyses in
[Calc/DAC_ADC/](../DAC_ADC/) and [Calc/Stability/](../Stability/).

| Script | What it does |
|---|---|
| [psu_voltage_accuracy.py](psu_voltage_accuracy.py) | Ramps one channel 0 → 36 V in 500 mV steps and plots setpoint- and readback-error against a Siglent SDM3065X |

Results (CSV + report + PNGs) are written to `results/`, one timestamped set per
run.

---

## psu_voltage_accuracy.py

### What it measures

At each of the 73 ramp steps the script collects three numbers:

| Symbol | Source | Meaning |
|---|---|---|
| `v_set` | what we wrote with `VOLT <x>` | the request |
| `v_psu` | `MEAS:VOLT?` | what the PSU *thinks* it is doing |
| `v_dmm` | SDM3065X `READ?` | ground truth |

and reports the two deviations:

- **setpoint error** = `v_set − v_dmm` — exercises the **DAC → Vset** path
  (MCP48FVB22 band gap, the 12-bit code grid, the post-reg loop). This is what
  `CAL:POIN VSET,…` corrects.
- **readback error** = `v_psu − v_dmm` — exercises the **Vsense → ADC** path
  (difference amp, ADS1118). This is what `CAL:POIN VMEAS,…` corrects.

Both paths are independent, which is why they are plotted as two separate
series rather than being folded into one "accuracy" number.

### Three panels

1. **Absolute deviation (mV)** — the headline. Flat-and-nonzero means offset,
   sloped means gain error.
2. **Relative deviation (% of reading)** — the spec-sheet view. Points below
   `--rel-floor` (default 1 V) are dropped, because dividing a fixed offset by
   a near-zero reading produces a meaningless spike.
3. **Linearity** — residual after a least-squares gain/offset line is removed.
   This is the part a 2-point calibration **cannot** fix, so it is the real
   accuracy floor of the hardware. Compare it against the ±0.05 % predicted in
   [DAC_ADC_Accuracy_Analysis.md](../DAC_ADC/DAC_ADC_Accuracy_Analysis.md);
   12-bit DAC quantisation alone is ~8.6 mV, so a sawtooth of that amplitude on
   the setpoint trace is expected, not a defect.

The text report additionally prints gain error, offset, worst/rms error and a
ready-to-paste `CAL:POIN` sequence built from the two points nearest 10 % and
90 % of full scale.

### Wiring

```
PSU CH<n> OUT+ ────────────────► SDM3065X  HI (DC V)
PSU CH<n> OUT− ────────────────► SDM3065X  LO
```

Run it **open-circuit** apart from the DMM. The SDM3065X draws >10 GΩ on its
2 V/20 V ranges and 10 MΩ on the 200 V range (≈3.6 µA at 36 V), so the current
reading should stay at zero; `--abort-current` (default 20 mA) stops the ramp if
it does not, which catches a shorted or loaded output.

> **Settling vs step size.** 500 mV steps settle well inside the 0.6 s default.
> A large step does not: measured on this unit, 0 V → 15 V in one jump is still
> slewing 0.6 s later (the DMM read 1.87 V low). If you raise `--vstep` much
> above ~1 V, raise `--settle` with it.

> With `--dmm-range AUTO` the DMM switches 2 V → 20 V → 200 V during the ramp,
> and its input impedance changes with it. That is the most accurate choice and
> is harmless here, but if you see a step in the error curve right at 20 V, pin
> the range with `--dmm-range 200` and re-run to confirm it is the DMM and not
> the PSU.

### Prerequisites

- Python 3 with `numpy` and `matplotlib` (already present on this machine).
  **No pyvisa/VISA runtime needed** — both instruments are driven over raw SCPI
  on TCP port 5025.
- PSU firmware ≥ 0.2.0 with the SCPI server (see
  [firmware/SCPI/commands.md](../../firmware/SCPI/commands.md)); the PSU must
  have an IP (front panel → *Settings → Network*).
- SDM3065X on the same subnet, LAN enabled.

### Usage

```powershell
# full 0..36 V ramp on CH1
py psu_voltage_accuracy.py --psu 192.168.1.50 --dmm 192.168.1.51

# CH2, coarser and faster (per-point markers appear at <=40 points)
py psu_voltage_accuracy.py --psu 192.168.1.50 --dmm 192.168.1.51 `
    --channel 2 --vstep 2.0 --nplc 1 --dmm-samples 1 --settle 0.3

# verify the post-calibration residual only over the useful span
py psu_voltage_accuracy.py --psu ... --dmm ... --vmin 1 --vmax 36 --tag after_cal

# no hardware: modelled instruments, exercises the whole pipeline
py psu_voltage_accuracy.py --simulate

# re-draw plots/report from a stored run
py psu_voltage_accuracy.py --replot results/ramp_ch1_20260803_120000.csv
```

Key options (`--help` lists them all):

| Option | Default | Notes |
|---|---|---|
| `--vmin / --vmax / --vstep` | 0 / 36 / 0.5 V | the setpoint grid is 10 mV, so keep steps a multiple of that |
| `--ilim` | 0.050 A | current limit held during the ramp |
| `--settle` | 0.6 s | dwell after each step before measuring |
| `--dmm-samples` / `--psu-samples` | 3 / 3 | readings averaged per step |
| `--nplc` | 10 | DMM integration time; 10 NPLC ≈ 0.2 s/reading at 50 Hz |
| `--dmm-range` | AUTO | or `2`, `20`, `200` |
| `--abort-current` | 0.020 A | safety abort |
| `--max-deviation` | 3.0 V | aborts on a grossly wrong reading (wrong leads/range) |
| `--theme` | both | light and dark PNGs |

A full 73-point run at the defaults takes roughly 1½–2 minutes.

### Safety

The output is set to 0 V and switched **off** in a `finally` block, so Ctrl-C,
a timeout or an instrument error all leave the supply in a safe state. The PSU
error queue is drained before and after the ramp and anything it reports is
printed.

### Calibration (`--calibrate`) — and why it probably won't help

```powershell
py psu_voltage_accuracy.py --psu ... --dmm ... --channel 1 --calibrate --verify
```

Drives `--cal-low` (3.5 V) and `--cal-high` (32.5 V), waits `--cal-settle`
(3 s — the VMEAS capture is an EWMA needing ~1.5 s per
[measure.h](../../firmware/channel/src/app/measure.h)), reports the averaged DMM
reading with `CAL:POIN`, then commits. `--verify` re-runs the ramp afterwards.

Two properties of the firmware make this safe:

- The fit is **absolute**, not incremental — `x` = the true value, `y` = the raw
  hardware quantity (the DAC code for VSET, raw ADC volts for VMEAS). Calibrating
  an already-calibrated channel converges rather than compounding.
- Each point is captured **live**, from whatever the channel is doing when
  `CAL:POIN` arrives. Values from a stored run cannot be replayed — hence a
  script rather than a paste-in command list.

One thing it is **not**: reversible. `CALibration:DATA?` returns NaN (the
constants can't be read back over the channel link), so the present calibration
cannot be saved first, and `CAL:RESet` restores compile-time defaults rather than
your previous values.

> **Measured outcome on this unit (2026-08-03):** recalibrating VSET+VMEAS on
> both channels changed nothing beyond noise. Both were already at their floor:
> the setpoint residual is DAC INL (a bow between the cal points) plus ±½ LSB
> rounding, and the readback residual is ~1 ADC LSB. **A 2-point linear fit pins
> the endpoints; it cannot remove a bow between them.** Before calibrating,
> check the linearity panel — if the residual is a bow rather than a tilt or a
> step, calibration has nothing to grip.

### Interpreting a first, uncalibrated run

Expect a **gain error of a few percent** dominated by the DAC's internal band
gap (±3.3 %) — that is the known consequence of the unconnected `Vref` pin and
is exactly what the 2-point calibration exists to remove. What matters on a
first run is the *linearity* panel: if the residual is within a few mV, the
hardware is good and calibration will do the rest. A residual that grows with
voltage, or a kink, points at something real (post-reg loop, sense-amp resistor
matching, thermal drift) rather than at a missing calibration.

### Not covered yet

Current-setpoint / current-readback accuracy (`ISET`/`IMEAS`) needs an
electronic load or a power resistor bank plus the DMM in current mode, and is
not part of this script.
