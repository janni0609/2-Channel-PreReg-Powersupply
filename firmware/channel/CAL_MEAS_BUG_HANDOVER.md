# Handover — calibrating the measure paths (`VMEAS` / `IMEAS`) has no effect

**Date:** 2026-08-04
**Symptom:** `CAL:POIN` + `CAL:COMM` on `VMEAS`/`IMEAS` changes nothing, while the
same sequence on `VSET`/`ISET` demonstrably works.
**Impact:** both channels' current readback carries a real **~0.17 % gain error**
that calibration ought to remove. Voltage readback is unaffected in practice
(already at the ADC quantum), so this mainly costs current-measurement accuracy.
**Status:** reproduced on both channels; cause not yet identified.

---

## 1. The evidence

Measured with `Calc/evaluation/psu_accuracy.py` against a Siglent SDM3065X.
Full data in `Calc/evaluation/results/`, summarised in the project README.

### Set paths work

| path | gain error before → after |
|---|---|
| CH1 `ISET` | +0.176 % → **−0.023 %** |
| CH2 `ISET` | +0.080 % → **+0.029 %** |

CH1's current-setpoint rms went 0.89 → 0.29 mA and the pre-cal *tilt*
(−1.2 mA at 0.1 A rising to +1.8 mA at 2 A) flattened to ±0.3 mA. So
`CMD_CAL_POINT` → `CMD_CAL_COMMIT` → EEPROM → `cal_apply()` is fundamentally
functional.

### Measure paths do not

| path | gain error before → after |
|---|---|
| CH1 `IMEAS` | +0.139 % → +0.111 % |
| CH2 `IMEAS` | +0.173 % → **+0.176 %** |
| CH2 `VMEAS` | unchanged (offset stuck at −1.3 mV class) |

**The decisive observation:** a 2-point fit *must* pass through its own two
points. After calibrating CH2's `IMEAS` at 0.2 A and 1.8 A, the readback error
**at the 0.2 A cal point itself** was **+0.42 mA**, versus +0.39 mA before. It did
not move, and it is ~4 `IMEAS` LSBs (0.1 mA each) away from zero — far outside
noise. Whatever line got stored, it is not the line fitted to those two points.

### Ruled out

- **Not the PGA autoscaler.** `ADC_PGA_{START,MIN,MAX}_INDEX` are all `1` in
  `firmware/channel/include/config.h`, so `autoscale()` is a no-op and the range
  is pinned to ±4.096 V. (This was an early guess; it is wrong.)
- **Not a physical shunt-vs-DMM difference.** That was the first hypothesis for
  CH1's stuck −1.3 mA offset, but CH2 kills it: CH2's `IMEAS` error is a clean
  linear tilt (0.173 % gain, ~0 offset) — precisely the shape a 2-point fit
  removes — and it still did not move.
- **Not a broken commit/EEPROM path in general** — `ISET` proves it works.

## 2. Why the "it committed fine" reports mean nothing

This is the most important thing to know before re-testing.

`channel_cal_point()` and `channel_cal_commit()`
(`firmware/brain/main/src/channel_link.cpp:118` and `:127`) are **fire-and-forget
`link_send()` calls**. Nothing waits for the ACK/NACK. A `CMD_NACK` only lands
asynchronously in `st.lastNackCmd` / `st.lastNackReason`
(`channel_link.cpp:176`), and the SCPI `CALibration` handler never looks at it.

So the measurement harness printing `commit IMEAS stored`, and `CAL:VALid?`
returning `1`, are **not evidence that anything was stored**. They only mean the
SCPI error queue was empty. On the channel side `cal_commit()` returns `false`
and NACKs if either point is missing (`calibration.cpp:81`) — and that failure is
currently invisible to the host.

`CALibration:DATA?` returns SCPI NaN (documented Known issue), so the stored
gain/offset cannot be read back over the link either. **The whole measure-path
calibration is currently unobservable from the host.** Fixing that is arguably
the first task regardless of the root cause.

## 3. The code path

| step | location |
|---|---|
| SCPI parse, unit scaling | `firmware/brain/main/src/scpi.cpp` `cmdCal()` — `actual * 1000` for V paths, `* 10000` for I paths |
| brain → channel | `firmware/brain/main/src/channel_link.cpp:118,127` (fire-and-forget) |
| channel receive | `firmware/channel/src/app/comms.cpp` `CMD_CAL_POINT` (~line 118) |
| point capture | `calibration.cpp:69` `cal_record_point()` |
| solve + persist | `calibration.cpp:77` `cal_commit()` |
| apply | `calibration.cpp:63` `cal_apply()` |
| measure path | `measure.cpp:93` `process()`, `measure.cpp:150` `measure_last_vadc()` |

The asymmetry to focus on is in `comms.cpp` `CMD_CAL_POINT`:

```c
if (target == CAL_VSET || target == CAL_ISET) {
    /* x = externally measured value, y = the code we drove */
    cal_record_point(target, index, eng, (float)setpoint_last_code(target));
} else {
    /* x = our averaged raw ADC volts, y = externally measured value */
    cal_record_point(target, index, measure_last_vadc(target), eng);
}
```

For set paths `y` is an exact integer the firmware chose. For measure paths `x`
is a float sampled from hardware state — `s_vadc_avg[]`, an EWMA (alpha 1/16,
~1.5 s to settle). That is the only structural difference between the working
and non-working cases.

Worth checking specifically:

1. **Is `x` what `process()` actually feeds `cal_apply()`?** `process()` applies
   the calibration to the *instantaneous* `volts`, whereas the capture uses the
   *EWMA* `s_vadc_avg[]`. At steady state these should converge — confirm they
   really do, and that the EWMA is warm (the harness waits 3 s, `--cal-settle`).
2. **Are the two captured `x` values distinct and sane?** If `x0 ≈ x1`,
   `cal_commit()` bails on the degenerate check (`calibration.cpp:84`) and NACKs
   — invisibly.
3. **Units.** `eng` is mV for V paths and mA for I paths
   (`comms.cpp` scales the 0.1 mA wire units by `0.1f`); `cal_apply(CAL_IMEAS, …)`
   is expected to return mA and `process()` then multiplies by 10 for the 0.1 mA
   telemetry field. Confirm nothing is off by 10 in a way that lands near the
   old line.

## 4. Suggested order of attack

1. **Read the stored coefficients from the front panel.** *Settings → Channel n*
   exposes stored calibration values. Note `VMEAS`/`IMEAS` gain+offset, run a
   calibration, look again. If they did not change, the problem is upstream of
   the fit (point capture or NACK). If they did change but behaviour did not, the
   problem is in `cal_apply()`/`process()`. **This needs no code change and
   splits the search space in half.**
2. **Deliberate-wrong-value test.** Calibrate `IMEAS` claiming an `actual` far
   from truth (e.g. report 1.000 A while the DMM reads 0.500 A). If the readback
   moves, commits land and the bug is in the captured `x`; if nothing moves, the
   commit or apply path for measure targets is dead. Re-calibrate properly
   afterwards. Decisive, ~2 minutes.
3. **Surface the NACK / `EVT_CAL_STORED`.** Have the SCPI `CAL:COMM` handler
   check `lastNackCmd`/`lastEvent` and push a real error. Removes the blindfold
   permanently.
4. **Implement `CALibration:DATA?`** (already a Known issue) so gain/offset are
   readable over the link.

## 5. Reproducing

DMM in series with the channel output, current input, range pinned:

```powershell
cd Calc/evaluation
# baseline
py psu_accuracy.py --psu 192.168.2.128 --dmm 192.168.2.222 --channel 1 `
   --quantity current --dmm-range 2 --compliance 2.0 --tag before
# calibrate + immediately re-measure
py psu_accuracy.py --psu 192.168.2.128 --dmm 192.168.2.222 --channel 1 `
   --quantity current --dmm-range 2 --compliance 2.0 --calibrate --verify --tag after
```

Compare the `readback` gain error and, more tellingly, the readback error at
0.200 A and 1.800 A (the two cal points) in the two CSVs. A working calibration
must drive both toward zero.

Notes for whoever runs this:

- Pin `--dmm-range 2`. On AUTO the SDM3065X reads ~1.2 % high around 250 mA
  (overranged on its 200 mA range before it steps up).
- Exclude the 0 A point — after the calibration leaves the channel at 1.8 A it
  has not decayed within the 0.6 s settle and reads ~18 mA, which otherwise
  dominates the statistics.
- Exclude the clipped top of the range: neither channel reaches 2.000 A (ISET DAC
  clips at ~1.99 A on CH1, ~1.95 A on CH2 — separate Known issue).
- A DMM in series with an *off* output reads a steady **−7.77 mA**. That is the
  normal output pre-load for this design, not a stuck output.

## 6. Expected result once fixed

Both channels' `IMEAS` gain error should drop from ~0.17 % to the same order as
`ISET` after calibration (<0.03 %), taking CH2's current-readback rms from
2.0 mA to well under 1 mA and its worst-case from 3.7 mA toward the ~0.1 mA ADC
quantum. Re-run §5 to confirm, and update the README "Measured accuracy" table
and the corresponding Known issue.
