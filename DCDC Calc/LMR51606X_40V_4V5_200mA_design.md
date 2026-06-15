# LMR51606X Buck Converter Design — 40 V → 4.5 V @ 200 mA

Synchronous step-down (buck) converter design based on the **Texas Instruments LMR51606**
(datasheet *SLUSEY1B*, Rev. December 2023, located in this folder as `lmr51606.pdf`).

---

## 1. Design inputs & top-level decisions

| Parameter | Value | Source |
|---|---|---|
| Input voltage, V<sub>IN</sub> | **40 V** (treated as worst-case / max for ripple) | spec |
| Output voltage, V<sub>OUT</sub> | **4.5 V** | spec |
| Output current, I<sub>OUT</sub> | **200 mA** | spec |
| Switching frequency, f<sub>SW</sub> | **400 kHz** | fixed by "X" suffix |
| Reference voltage, V<sub>REF</sub> | **0.8 V** (±1.5 %) | datasheet §6.5 |
| Inductor ripple ratio, K<sub>IND</sub> | **≈ 0.5** | design choice (smaller L / more ripple) |
| EN / input UVLO | **EN tied directly to V<sub>IN</sub>** | design choice |
| Light-load mode | **PFM / auto-mode** | design choice |
| **Orderable part** | **LMR51606XDBVR** | 0.6 A, 400 kHz, PFM, adjustable, SOT-23-6 |

**Device suitability check:**
- V<sub>IN</sub> = 40 V is inside the 4 V–65 V operating range (abs. max 70 V). ✔
- 0.6 A device rating vs. 0.2 A load → **3× current headroom**. ✔
- V<sub>OUT</sub> = 4.5 V is inside the 0.8 V–28 V adjustable range. ✔

> **Note on input range:** the design is worst-cased at V<sub>IN</sub> = 40 V (the point of highest
> ripple current). It stays valid for any lower or varying input down to ≈ 4.9 V (see §2). If 40 V is
> actually the *minimum* of a higher range, the inductor and ripple numbers must be recomputed at the
> true maximum input.

---

## 2. Operating point & frequency-foldback check

Ideal buck duty cycle:

```
D = VOUT / VIN = 4.5 / 40 = 0.1125  (11.25 %)

tON  = D / fSW        = 0.1125 / 400 kHz   = 281 ns
tOFF = (1 - D) / fSW  = 0.8875 / 400 kHz   = 2219 ns
```

Compare against the device limits (datasheet §6.5 / §7.3.4):

| Limit | Value | Our value | OK? |
|---|---|---|---|
| Min ON time, t<sub>ON_MIN</sub> | 80 ns | 281 ns | ✔ (3.5×) |
| Max ON time, t<sub>ON(max)</sub> | 5 µs | 281 ns | ✔ |
| Min OFF time, t<sub>OFF_MIN</sub> | 200 ns | 2219 ns | ✔ (11×) |

Maximum / minimum input voltage **without** frequency foldback (Eq. 4 / Eq. 5):

```
VIN_MAX = VOUT / (fSW × tON_MIN)   = 4.5 / (400 kHz × 80 ns) = 140.6 V
VIN_MIN = VOUT / (1 - fSW × tOFF_MIN) = 4.5 / (1 - 0.08)     = 4.89 V
```

→ At V<sub>IN</sub> = 40 V the converter runs at a **clean fixed 400 kHz with no frequency foldback**.

---

## 3. Output-voltage feedback divider (R<sub>FBT</sub> / R<sub>FBB</sub>)

Datasheet Eq. 1 / Eq. 7:  `RFBT = (VOUT − VREF)/VREF × RFBB`

Required ratio: `RFBT/RFBB = (4.5 − 0.8)/0.8 = 4.625`

Chosen standard E96 (1 %) pair:

| Resistor | Value | Notes |
|---|---|---|
| **R<sub>FBT</sub>** (V<sub>OUT</sub> → FB) | **75.0 kΩ, 1 %** | within recommended 10 k–100 kΩ range |
| **R<sub>FBB</sub>** (FB → GND) | **16.2 kΩ, 1 %** | sets divider current |

Resulting output voltage:

```
VOUT = VREF × (1 + RFBT/RFBB) = 0.8 × (1 + 75.0k/16.2k) = 4.504 V   (+0.08 % vs target)
Idivider = VREF / RFBB = 0.8 / 16.2k = 49 µA
```

> Nominal set-point error is +0.08 %. The dominant error is component tolerance: ±1.5 % (V<sub>REF</sub>)
> plus ≈ ±1.4 % (two 1 % resistors, RSS) → use 1 % (or 0.5 %) resistors with a low temperature
> coefficient as recommended by TI. Place both resistors next to the FB pin (§9).

---

## 4. Inductor (L)

Minimum inductance, datasheet Eq. 9, with K<sub>IND</sub> = 0.5:

```
LMIN = (VIN_MAX − VOUT)/(IOUT × KIND) × VOUT/(VIN_MAX × fSW)
     = (40 − 4.5)/(0.2 × 0.5) × 4.5/(40 × 400 kHz)
     = 99.8 µH
```

→ **Choose a standard 100 µH inductor.**

Resulting ripple and currents (Eq. 8) with L = 100 µH:

```
ΔiL   = VOUT × (VIN_MAX − VOUT) / (VIN_MAX × L × fSW) = 99.8 mA   (KIND_actual = 0.50)
IL,peak   = IOUT + ΔiL/2 = 200 + 50 = 250 mA
IL,valley = IOUT − ΔiL/2 = 200 − 50 = 150 mA   (> 0 → continuous conduction at full load)
IL,rms    ≈ √(IOUT² + ΔiL²/12) = 202 mA
```

**Inductor selection requirements:**

| Parameter | Requirement | Reasoning |
|---|---|---|
| Inductance | **100 µH** (±20 %) | from Eq. 9, K<sub>IND</sub> = 0.5 |
| Saturation current, I<sub>sat</sub> | **≥ 1.5 A** | above the device max HS peak current limit (1.4 A) so the core does not saturate during an overload/short before cycle-by-cycle limiting acts |
| RMS current rating | **≥ 0.3 A** | operating I<sub>rms</sub> ≈ 0.2 A + margin |
| DCR | **≤ ~0.3 Ω** (lower = better) | efficiency; 0.3 Ω → ~12 mW loss |
| Type | Shielded ferrite | low EMI |

> If board size is critical, an inductor with I<sub>sat</sub> ≥ ~1.0 A still operates safely (operating peak
> is only 250 mA, and the IC's cycle-by-cycle current limit + hiccup protect the device); the ≥ 1.5 A
> recommendation is purely for full ruggedness against a hard short.

Candidate series (verify exact P/N, size and ratings against the requirements above): Coilcraft MSS-series /
XAL-series, Würth WE-LQS / WE-PD, Bourns SRR/SRN.

---

## 5. Output capacitor (C<sub>OUT</sub>)

**Selected: 22 µF / 25 V / X7R ceramic (ESR ≈ 5 mΩ).**

### 5.1 Steady-state ripple (Eq. 10 / Eq. 11)

```
ΔVOUT_C   = ΔiL / (8 × fSW × COUT) = 99.8 mA / (8 × 400 kHz × 22 µF) = 1.42 mV
ΔVOUT_ESR = ΔiL × ESR             = 99.8 mA × 5 mΩ                  = 0.50 mV
ΔVOUT(total) ≲ 2 mV   (the two terms are not in phase, so the real value is below their sum)
```

≈ 2 mV is **< 0.05 %** of 4.5 V — ripple is dominated by, and easily met by, a ceramic cap.

### 5.2 Load-transient limit (Eq. 12), full 0 → 200 mA step

```
COUT > ½ × 6 × (IOH − IOL) / (fSW × ΔVOUT_shoot)
  ΔVOUT_shoot = ±5 % (225 mV)  → COUT > 6.7 µF
  ΔVOUT_shoot = ±3 % (135 mV)  → COUT > 11.1 µF
```

A single **22 µF** part covers a full-load step to within ±3 % with margin, even after ceramic DC-bias
derating (a 25 V X7R at 4.5 V bias retains roughly 18–20 µF effective).

> A 10 µF / 25 V X7R is acceptable if minimizing size and only ±5 % transient is required.
> No load-transient spec was provided — values above assume a full 0 → 200 mA step; tighten C<sub>OUT</sub>
> if your overshoot/undershoot budget is smaller.

---

## 6. Input capacitor (C<sub>IN</sub>)

Datasheet §8.2.2.6: ≥ 2.2 µF ceramic, X7R/X5R, voltage rating **≥ 2 × V<sub>IN,max</sub>** = **≥ 80 V**.

| Component | Value | Rating | Notes |
|---|---|---|---|
| C<sub>IN</sub> (bulk decoupling) | **4.7 µF** (2.2 µF min) | **100 V**, X7R | 100 V rating gives ≥ 2× margin and offsets DC-bias derating of a 2.2 µF part |
| C<sub>IN_HF</sub> (high-freq bypass) | **0.1 µF** | 100 V, X7R | placed **directly** at the VIN/GND pins |

Input-cap RMS current:

```
ICIN,rms ≈ IOUT × √(D × (1 − D)) = 0.2 × √(0.1125 × 0.8875) = 63 mA
```

A 4.7 µF / 100 V X7R (1210) easily handles 63 mA.

> If the supply is more than a few inches from the device, add a bulk capacitor
> (10 µF or 22 µF electrolytic, ≥ 63 V) per datasheet §8.3.

---

## 7. Bootstrap capacitor (C<sub>B</sub> / C<sub>BOOT</sub>)

Fixed by the datasheet (§7.3.5, pin table): **0.1 µF (100 nF)** between **CB** and **SW**, X7R/X5R,
**≥ 16 V** rating.

| Component | Value | Rating | Notes |
|---|---|---|---|
| C<sub>BOOT</sub> | **0.1 µF (100 nF)** | 25 V, X7R | high-side gate-drive supply; abs-max CBOOT-to-SW is 5.5 V, so 25 V is ample |

---

## 8. Enable, soft-start & compensation

### 8.1 Enable / input UVLO — **EN tied directly to V<sub>IN</sub>**

No external divider. The converter then starts/stops on the device's **internal V<sub>IN</sub> UVLO**:

```
VIN UVLO rising  ≈ 3.82 V (typ),  max 4.0 V
VIN UVLO falling ≈ 3.56 V (typ),  min 3.4 V
Hysteresis       ≈ 0.25 V
```

At 40 V the device is always enabled. With EN = V<sub>IN</sub>, the EN pin sees 40 V, which is within both
the recommended range (0 to V<sub>IN</sub>) and the absolute maximum (V<sub>IN</sub> + 0.3 V). ✔

> A programmable turn-on threshold (e.g. start at 30 V / 36 V) would require an R<sub>ENT</sub>/R<sub>ENB</sub>
> divider (datasheet Eq. 13–15). Not used here per the chosen configuration.

### 8.2 Soft-start — **internal, fixed ≈ 2.3 ms** (no external component).
Also note: overcurrent-protection blanking time T<sub>OCP_BLK</sub> ≈ 33 ms at start-up, hiccup period ≈ 150 ms.

### 8.3 Loop compensation — **internal** (peak-current-mode, internally compensated). No external R/C network.

---

## 9. Protection, current limit & operating mode

| Item | Value | Comment |
|---|---|---|
| HS peak current limit, I<sub>HS_PK(OC)</sub> | 1.1 A typ (0.8 A min) | operating peak is 250 mA → **>3× margin**, no false trips |
| LS valley current limit, I<sub>LS_V(OC)</sub> | 0.8 A typ | — |
| Max deliverable current (Eq. 6) | (1.1 + 0.8)/2 = **0.95 A** | vs. 0.2 A required |
| Hiccup short-circuit protection | FB < 40 % V<sub>REF</sub> for 256 cycles | auto-recovering |
| Thermal shutdown | 165 °C (20 °C hyst.) | — |
| DCM/PFM boundary (this design) | I<sub>OUT</sub> ≈ ΔiL/2 = **50 mA** | above 50 mA → CCM; below → DCM/PFM |

**Mode behavior (PFM / "X" part):** at the rated 200 mA load the inductor current is continuous
(valley = 150 mA), so the converter operates in CCM near 400 kHz. At light loads it enters PFM (frequency
drops) for high efficiency, as selected.

> **Caveat:** because this is a low-current design, the full-load peak inductor current (~0.25 A) is close to
> the device's minimum PFM peak-current floor (I<sub>PEAK_MIN</sub> ≈ 0.33 A). Near and below full load the
> effective switching frequency may sit at or somewhat below 400 kHz, with correspondingly variable ripple.
> If a **guaranteed constant 400 kHz and minimum ripple at 200 mA** are required, use the pin-compatible
> **FPWM** variant **LMR51606XFDBVR** instead.

---

## 10. Efficiency & thermal estimate

From the datasheet efficiency curves (Fig. 6-3, 5 V / 400 kHz, interpolated to V<sub>IN</sub> ≈ 40 V,
I<sub>OUT</sub> = 0.2 A): **η ≈ 85 %**.

```
POUT  = 4.5 V × 0.2 A = 0.90 W
PLOSS = POUT/η − POUT ≈ 0.16 W   (of which inductor DCR ≈ 12 mW)
PIC   ≈ 0.15 W dissipated in the LMR51606
```

Junction-temperature rise in the SOT-23-6 (DBV):

```
ΔTJ = PIC × RθJA
    = 0.15 W × 95 °C/W  ≈ 14 °C   (good 2-layer layout, datasheet effective value)
    = 0.15 W × 147.8 °C/W ≈ 22 °C (JEDEC reference, minimal copper)
```

→ At T<sub>ambient</sub> = 85 °C, T<sub>J</sub> ≈ 99–107 °C — comfortably below the 150 °C limit. **No thermal concern.**

---

## 11. Bill of materials (BOM)

| Ref | Component | Value | Rating / Package | Notes |
|---|---|---|---|---|
| U1 | Buck converter | **LMR51606XDBVR** | SOT-23-6, 400 kHz, 0.6 A, PFM | TI |
| L1 | Power inductor | **100 µH** | I<sub>sat</sub> ≥ 1.5 A, I<sub>rms</sub> ≥ 0.3 A, DCR ≤ 0.3 Ω, shielded | SW → V<sub>OUT</sub> |
| C<sub>OUT</sub> | Output cap | **22 µF** | 25 V, X7R, ceramic | ~18–20 µF after DC-bias derate |
| C<sub>IN</sub> | Input cap | **4.7 µF** (≥ 2.2 µF) | 100 V, X7R, ceramic | bulk input decoupling |
| C<sub>IN_HF</sub> | Input HF bypass | **0.1 µF** | 100 V, X7R | right at VIN/GND pins |
| C<sub>BOOT</sub> | Bootstrap cap | **0.1 µF** | 25 V, X7R | CB → SW |
| R<sub>FBT</sub> | Feedback top | **75.0 kΩ** | 1 %, low-TC | V<sub>OUT</sub> → FB |
| R<sub>FBB</sub> | Feedback bottom | **16.2 kΩ** | 1 %, low-TC | FB → GND |
| — | EN | **wire to V<sub>IN</sub>** | — | no UVLO divider |
| C<sub>BULK</sub> *(optional)* | Bulk input cap | 10–22 µF | ≥ 63 V, electrolytic | only if supply is far from U1 |

---

## 12. Connection summary (per pin)

| Pin | Name | Connection |
|---|---|---|
| 1 | CB | C<sub>BOOT</sub> (0.1 µF) to SW (pin 6) |
| 2 | GND | System ground; ground side of C<sub>IN</sub>, C<sub>OUT</sub>, R<sub>FBB</sub> |
| 3 | FB | Junction of R<sub>FBT</sub> / R<sub>FBB</sub> |
| 4 | EN | Tied to VIN (pin 5) |
| 5 | VIN | 40 V input; C<sub>IN</sub> (4.7 µF) + C<sub>IN_HF</sub> (0.1 µF) to GND, directly at pin |
| 6 | SW | To L1 (100 µH); other end of L1 = V<sub>OUT</sub> (4.5 V) |

```
        VIN(40V) ───┬───────────────┬──────────┐
                    │               │          │
                  C_IN(4.7µF)    C_HF(0.1µF)    │
                    │               │           ├── VIN(5) ── EN(4)
                   GND             GND           │
                                                 │   CB(1) ──C_BOOT(0.1µF)──┐
                                                 │                          │
                                          [ LMR51606X ]   SW(6) ────────────┴──┬── L1(100µH) ──┬── VOUT(4.5V)
                                                 │                              │              │
                                                GND(2)                         (CB-SW cap)    R_FBT(75k)
                                                 │                                             │
                                                GND                              FB(3) ────────┤
                                                                                               R_FBB(16.2k)
                                                                                C_OUT(22µF)    │
                                                                                  │           GND
                                                                                 GND
```

---

## 13. Layout notes (datasheet §8.4)

1. Place **C<sub>IN</sub> / C<sub>IN_HF</sub> as close as possible to VIN and GND pins** — this is the #1 EMI driver
   (high di/dt loop).
2. Keep the **SW node copper small** but thick enough for the load current; keep the SW–L1 trace short.
3. Place **C<sub>BOOT</sub> close to CB and SW pins**.
4. Place **R<sub>FBT</sub>/R<sub>FBB</sub> next to the FB pin**; route the FB/V<sub>OUT</sub>-sense trace away from
   the SW node and inductor (FB is a high-impedance node). Sense V<sub>OUT</sub> at the load if accuracy matters.
5. Use a **solid ground plane**; connect the IC GND and C<sub>IN</sub>/C<sub>OUT</sub> grounds to it with short
   paths and use thermal vias under/around the device to spread heat and keep T<sub>J</sub> < 125 °C.

---

## 14. Assumptions

- V<sub>IN</sub> = 40 V treated as the maximum operating input (worst case for ripple current).
- Inductor ripple ratio K<sub>IND</sub> = 0.5 (chosen for a smaller ~100 µH inductor; more ripple accepted).
- EN tied directly to V<sub>IN</sub>; turn-on/off governed by the internal V<sub>IN</sub> UVLO (≈ 3.8 V / 3.56 V).
- PFM (auto-mode) part selected → LMR51606XDBVR.
- Load-transient analysis assumes a full 0 → 200 mA step (no explicit transient spec was given).
- Efficiency ≈ 85 % read/interpolated from the datasheet curves; T<sub>J</sub> rise computed for both the 2-layer
  effective and JEDEC R<sub>θJA</sub> values.

*Reference: Texas Instruments LMR51606 / LMR51610 datasheet, SLUSEY1B (Dec 2023) — `lmr51606.pdf` in this folder.*
