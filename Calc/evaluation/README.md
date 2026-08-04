# Evaluation — bench measurements against a reference instrument

Scripts that measure the *built* power supply and compare it against a
calibrated reference. This is the empirical counterpart to the paper analyses in
[Calc/DAC_ADC/](../DAC_ADC/) and [Calc/Stability/](../Stability/).

| Script | What it does |
|---|---|
| [psu_accuracy.py](psu_accuracy.py) | Ramps one channel 0 → 36 V (or 0 → 2 A) and plots setpoint- and readback-error against a Siglent SDM3065X |
| [cal_probe.py](cal_probe.py) | Recovers the `IMEAS` gain/offset actually stored in the channel and compares it with the fit it should have produced — stands in for the missing `CAL:DATA?` |
| [net_stress.py](net_stress.py) | SCPI-over-Ethernet stress and liveness harness (see the brain's network-hang notes) |

Results (CSV + report + PNGs) are written to `results/`, one timestamped set per
run.

---

## psu_accuracy.py

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

### Two panels

1. **Absolute deviation (mV)** — the headline. Flat-and-nonzero means offset,
   sloped means gain error.
2. **Relative deviation (% of reading)** — the spec-sheet view. Points below
   `--rel-floor` (default 1 V) are dropped, because dividing a fixed offset by
   a near-zero reading produces a meaningless spike.

The text report additionally prints gain error, offset, worst/rms error, a
`worst non-linearity` row, and a ready-to-paste `CAL:POIN` sequence built from
the two points nearest 10 % and 90 % of full scale.

**`worst non-linearity`** is the residual left after a least-squares gain/offset
line is removed — the part a 2-point calibration **cannot** fix, so it is the
real accuracy floor of the hardware. Compare it against the quantisation floor
printed directly beneath it, and against the ±0.05 % predicted in
[DAC_ADC_Accuracy_Analysis.md](../DAC_ADC/DAC_ADC_Accuracy_Analysis.md); 12-bit
DAC quantisation alone is ~8.6 mV, so a sawtooth of that amplitude on the
setpoint trace is expected, not a defect. (This used to be a third plot panel;
it is analysis of the calibration scheme rather than of the instrument, so it now
lives only in the report.)

### Excluded endpoints

The report and plots leave out the two ramp endpoints the channel physically
cannot reach, because each one dominates every statistic it appears in:

- the **0 V / 0 A setpoint**, which sits on the output floor (CH1 measures
  31.6 mV for a 0 V request), and
- the **clipped top** of a current ramp, above the ISET DAC ceiling — one such
  point took CH2's worst setpoint error from ~1 mA to 49 mA.

Clipping is only ever detected as a contiguous tail exceeding `--clip-tol`
(default 5 setpoint LSB), so a genuinely poor calibration is never silently
discarded as "clipping". Both endpoints stay in the CSV, which is the raw
measurement record. `--keep-endpoints` reports every point.

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
py psu_accuracy.py --psu 192.168.1.50 --dmm 192.168.1.51

# CH2, coarser and faster (per-point markers appear at <=40 points)
py psu_accuracy.py --psu 192.168.1.50 --dmm 192.168.1.51 `
    --channel 2 --vstep 2.0 --nplc 1 --dmm-samples 1 --settle 0.3

# verify the post-calibration residual only over the useful span
py psu_accuracy.py --psu ... --dmm ... --vmin 1 --vmax 36 --tag after_cal

# no hardware: modelled instruments, exercises the whole pipeline
py psu_accuracy.py --simulate

# re-draw plots/report from a stored run
py psu_accuracy.py --replot results/ramp_i_ch1_20260804_224335_ch1_imeas_fix.csv
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
py psu_accuracy.py --psu ... --dmm ... --channel 1 --calibrate --verify
```

Drives `--cal-low` (3.5 V) and `--cal-high` (32.5 V), waits `--cal-settle`,
reports the averaged DMM reading with `CAL:POIN`, then commits. `--verify`
re-runs the ramp afterwards, and the residual at each cal point is checked and
reported either way — a 2-point fit must pass through its own two points, so
that residual is the one check the missing `CAL:DATA?` cannot hide.

**`--cal-settle` is the whole ballgame for the measure paths.** The channel
captures `x` from an average of the raw ADC volts taken right after the channel
steps to the point, so any lag left in that average is stored as the point's
`x` and comes straight back as gain/offset error. This is what caused the
measure-path calibration bug — the old 3 s default stored a 2.2 % `IMEAS` gain
error while reporting success. The default is now 12 s, which is comfortable on
firmware carrying the `measure.cpp` alpha = 1/4 + snap fix (the reasoning is in
that file's comment); on older firmware use 20 s or more.

Calibrate a channel that has been running, not one straight from idle: CH2
calibrated cold came out at +0.033 % gain, and re-running it once thermally
soaked gave +0.019 %.

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
your previous values. Two ways round that: [cal_probe.py](cal_probe.py) recovers
the stored `IMEAS` line from SCPI readbacks alone (good to ~0.02 % on gain), and
a UPDI adapter can read the whole EEPROM out byte-for-byte — see the flash-status
section of the handover for the procedure.

> **Measured outcome on this unit (2026-08-03):** recalibrating VSET+VMEAS on
> both channels changed nothing beyond noise. Both were already at their floor:
> the setpoint residual is DAC INL (a bow between the cal points) plus ±½ LSB
> rounding, and the readback residual is ~1 ADC LSB. **A 2-point linear fit pins
> the endpoints; it cannot remove a bow between them.** Before calibrating,
> check the `worst non-linearity` row — if the residual is a bow rather than a
> tilt or a step, calibration has nothing to grip.

### Interpreting a first, uncalibrated run

Expect a **gain error of a few percent** dominated by the DAC's internal band
gap (±3.3 %) — that is the known consequence of the unconnected `Vref` pin and
is exactly what the 2-point calibration exists to remove. What matters on a
first run is `worst non-linearity`: if the residual is within a few mV, the
hardware is good and calibration will do the rest. A residual that grows with
voltage, or a kink, points at something real (post-reg loop, sense-amp resistor
matching, thermal drift) rather than at a missing calibration.

Re-confirmed on 2026-08-04: CH1's voltage paths were measured again a day later
and had not moved (VSET 8.78 mV rms / −0.0005 % gain, VMEAS 0.74 mV rms /
+0.0025 % gain, both within ~0.1 mV of the previous run). Still nothing for a
calibration to correct, so none was run.

### Current ramps

`--quantity current` ramps 0 → 2 A instead, with the DMM's own current input as
the load and `--compliance` setting the voltage limit. Pin the DMM range
(`--dmm-range 2`): on AUTO the SDM3065X reads ~1.2 % high around 250 mA, having
overranged its 200 mA range before stepping up. Unlike the voltage paths, the
current paths *do* respond to calibration — see the README accuracy section.
