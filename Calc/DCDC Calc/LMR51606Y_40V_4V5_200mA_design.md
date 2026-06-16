# LMR51606Y Buck Converter Design — 40 V → 4.5 V @ 200 mA (1.1 MHz variant)

Synchronous step-down (buck) converter design based on the **Texas Instruments LMR51606**,
**1.1 MHz "Y" variant** (datasheet *SLUSEY1B*, Rev. December 2023, `lmr51606.pdf` in this folder).

> This is the non-"X" (= **"Y"**, 1.1 MHz) counterpart of the 400 kHz design in
> `LMR51606X_40V_4V5_200mA_design.md`. Only the switching frequency changes; all other design
> choices are kept identical (EN tied to V<sub>IN</sub>, K<sub>IND</sub> ≈ 0.5, PFM/auto-mode).
> A side-by-side comparison is in §15.

---

## 1. Design inputs & top-level decisions

| Parameter | Value | Source |
|---|---|---|
| Input voltage, V<sub>IN</sub> | **40 V** (treated as worst-case / max for ripple) | spec |
| Output voltage, V<sub>OUT</sub> | **4.5 V** | spec |
| Output current, I<sub>OUT</sub> | **200 mA** | spec |
| Switching frequency, f<sub>SW</sub> | **1.1 MHz** | fixed by "Y" suffix |
| Reference voltage, V<sub>REF</sub> | **0.8 V** (±1.5 %) | datasheet §6.5 |
| Inductor ripple ratio, K<sub>IND</sub> | **≈ 0.5** | design choice (smaller L / more ripple) |
| EN / input UVLO | **EN tied directly to V<sub>IN</sub>** | design choice |
| Light-load mode | **PFM / auto-mode** | design choice |
| **Orderable part** | **LMR51606YDBVR** | 0.6 A, 1100 kHz, PFM, adjustable, SOT-23-6 |

**Device suitability check:**
- V<sub>IN</sub> = 40 V is inside the 4 V–65 V operating range (abs. max 70 V). ✔
- 0.6 A device rating vs. 0.2 A load → **3× current headroom**. ✔
- V<sub>OUT</sub> = 4.5 V is inside the 0.8 V–28 V adjustable range. ✔

> **Note on input range:** worst-cased at V<sub>IN</sub> = 40 V. See §2 — at 1.1 MHz the headroom before
> frequency foldback is much smaller than at 400 kHz, so confirm the input cannot rise far above 40 V.

---

## 2. Operating point & frequency-foldback check ⚠️ (key difference vs 400 kHz)

Ideal buck duty cycle (frequency-independent):

```
D = VOUT / VIN = 4.5 / 40 = 0.1125  (11.25 %)

tON  = D / fSW        = 0.1125 / 1.1 MHz   = 102 ns
tOFF = (1 - D) / fSW  = 0.8875 / 1.1 MHz   = 807 ns
```

Compare against the device limits (datasheet §6.5 / §7.3.4):

| Limit | Value | Our value | Margin |
|---|---|---|---|
| Min ON time, t<sub>ON_MIN</sub> | 80 ns | **102 ns** | **only 1.28×** ⚠️ |
| Max ON time, t<sub>ON(max)</sub> | 5 µs | 102 ns | ✔ |
| Min OFF time, t<sub>OFF_MIN</sub> | 200 ns | 807 ns | ✔ (4×) |

Maximum / minimum input voltage **without** frequency foldback (Eq. 4 / Eq. 5):

```
VIN_MAX = VOUT / (fSW × tON_MIN)      = 4.5 / (1.1 MHz × 80 ns) = 51.1 V
VIN_MIN = VOUT / (1 - fSW × tOFF_MIN) = 4.5 / (1 - 0.22)        = 5.77 V
```

→ At V<sub>IN</sub> = 40 V the converter still runs at a fixed 1.1 MHz with **no foldback**, but the headroom
is now only to **≈ 51 V** (versus ≈ 141 V for the 400 kHz part). **If the input can spike above ~51 V the
device will start dropping its frequency (foldback) to hold V<sub>OUT</sub>.** At a clean 40 V rail this is a
non-issue, but it is the main reason to prefer the 400 kHz "X" part if the input is not tightly bounded.

---

## 3. Output-voltage feedback divider (R<sub>FBT</sub> / R<sub>FBB</sub>) — *unchanged*

The divider is frequency-independent, so it is identical to the 400 kHz design.

Datasheet Eq. 1 / Eq. 7:  `RFBT = (VOUT − VREF)/VREF × RFBB`, ratio = `(4.5 − 0.8)/0.8 = 4.625`.

| Resistor | Value | Notes |
|---|---|---|
| **R<sub>FBT</sub>** (V<sub>OUT</sub> → FB) | **75.0 kΩ, 1 %** | within recommended 10 k–100 kΩ |
| **R<sub>FBB</sub>** (FB → GND) | **16.2 kΩ, 1 %** | I<sub>div</sub> = 49 µA |

```
VOUT = 0.8 × (1 + 75.0k/16.2k) = 4.504 V   (+0.08 % vs target)
```

Use 1 % (or 0.5 %) low-TC resistors; place next to the FB pin.

---

## 4. Inductor (L)

Minimum inductance, datasheet Eq. 9, with K<sub>IND</sub> = 0.5:

```
LMIN = (VIN_MAX − VOUT)/(IOUT × KIND) × VOUT/(VIN_MAX × fSW)
     = (40 − 4.5)/(0.2 × 0.5) × 4.5/(40 × 1.1 MHz)
     = 36.3 µH
```

→ **Choose a standard 33 µH inductor** (nearest common value; in keeping with the "smaller L / more
ripple" choice). The higher frequency makes the inductor ~3× smaller than the 400 kHz design (100 µH).

Resulting ripple and currents (Eq. 8) with L = 33 µH:

```
ΔiL   = VOUT × (VIN_MAX − VOUT) / (VIN_MAX × L × fSW) = 110 mA   (KIND_actual = 0.55)
IL,peak   = IOUT + ΔiL/2 = 200 + 55 = 255 mA
IL,valley = IOUT − ΔiL/2 = 200 − 55 = 145 mA   (> 0 → CCM at full load)
IL,rms    ≈ √(IOUT² + ΔiL²/12) = 203 mA
```

> Pick **39 µH** instead if you want to hold K<sub>IND</sub> ≤ 0.5 exactly (Δi<sub>L</sub> = 93 mA, peak 247 mA),
> or **47 µH** for lower ripple (Δi<sub>L</sub> = 77 mA, K<sub>IND</sub> = 0.39).

**Inductor selection requirements:**

| Parameter | Requirement | Reasoning |
|---|---|---|
| Inductance | **33 µH** (±20 %) | from Eq. 9, K<sub>IND</sub> ≈ 0.5 |
| Saturation current, I<sub>sat</sub> | **≥ 1.5 A** | above device max HS peak current limit (1.4 A); ≥ 1.0 A acceptable if size-critical |
| RMS current rating | **≥ 0.3 A** | I<sub>rms</sub> ≈ 0.2 A + margin |
| DCR | **≤ ~0.3 Ω** | efficiency |
| Type | Shielded ferrite | low EMI |

Candidate series (verify exact P/N and ratings): Coilcraft MSS/XAL/XFL, Würth WE-LQS / WE-MAPI,
Bourns SRR/SRN.

---

## 5. Output capacitor (C<sub>OUT</sub>)

**Selected: 10 µF / 25 V / X7R ceramic (ESR ≈ 5 mΩ).**
At 1.1 MHz the output filter shrinks — both ripple and transient needs are met by a smaller cap than the
400 kHz design (which used 22 µF).

### 5.1 Steady-state ripple (Eq. 10 / Eq. 11), Δi<sub>L</sub> = 110 mA

```
ΔVOUT_C   = ΔiL / (8 × fSW × COUT) = 110 mA / (8 × 1.1 MHz × 10 µF) = 1.25 mV
ΔVOUT_ESR = ΔiL × ESR             = 110 mA × 5 mΩ                   = 0.55 mV
ΔVOUT(total) ≲ 1.8 mV   (terms not in phase → below their sum)
```

≈ 1.8 mV is **< 0.05 %** of 4.5 V.

### 5.2 Load-transient limit (Eq. 12), full 0 → 200 mA step

```
COUT > ½ × 6 × (IOH − IOL) / (fSW × ΔVOUT_shoot)
  ΔVOUT_shoot = ±5 % (225 mV)  → COUT > 2.4 µF
  ΔVOUT_shoot = ±3 % (135 mV)  → COUT > 4.0 µF
```

A single **10 µF** part covers a full-load step to within ±3 % with margin even after ceramic DC-bias
derating (≈ 8 µF effective at 4.5 V bias on a 25 V X7R).

> Use **22 µF** if you want extra transient margin or a single common value shared with the 400 kHz design.

---

## 6. Input capacitor (C<sub>IN</sub>) — *unchanged*

Datasheet §8.2.2.6: ≥ 2.2 µF ceramic, X7R/X5R, voltage rating **≥ 2 × V<sub>IN,max</sub> = ≥ 80 V**.

| Component | Value | Rating | Notes |
|---|---|---|---|
| C<sub>IN</sub> (bulk decoupling) | **4.7 µF** (2.2 µF min) | **100 V**, X7R | ≥ 2× margin + offsets DC-bias derating |
| C<sub>IN_HF</sub> (high-freq bypass) | **0.1 µF** | 100 V, X7R | directly at VIN/GND pins |

```
ICIN,rms ≈ IOUT × √(D × (1 − D)) = 0.2 × √(0.1125 × 0.8875) = 63 mA   (frequency-independent)
```

> Add a 10–22 µF / ≥ 63 V bulk electrolytic if the supply is more than a few inches from the device.

---

## 7. Bootstrap capacitor (C<sub>B</sub> / C<sub>BOOT</sub>) — *unchanged*

**0.1 µF (100 nF)** between **CB** and **SW**, X7R/X5R, **≥ 16 V** (25 V used for margin).

---

## 8. Enable, soft-start & compensation — *unchanged*

- **EN tied directly to V<sub>IN</sub>** → start/stop on internal V<sub>IN</sub> UVLO (rising ≈ 3.82 V, falling ≈ 3.56 V,
  hysteresis ≈ 0.25 V). EN sees 40 V ≤ V<sub>IN</sub> + 0.3 V (abs max) and ≤ V<sub>IN</sub> (recommended). ✔
- **Soft-start:** internal, fixed ≈ 2.3 ms. OCP blanking ≈ 33 ms, hiccup period ≈ 150 ms.
- **Loop compensation:** internal (peak-current-mode). No external network.

---

## 9. Protection, current limit & operating mode

| Item | Value | Comment |
|---|---|---|
| HS peak current limit, I<sub>HS_PK(OC)</sub> | 1.1 A typ (0.8 A min) | operating peak 255 mA → **>3× margin** |
| Max deliverable current (Eq. 6) | (1.1 + 0.8)/2 = **0.95 A** | vs. 0.2 A required |
| Hiccup short-circuit protection | FB < 40 % V<sub>REF</sub>, 256 cycles | auto-recovering |
| Thermal shutdown | 165 °C (20 °C hyst.) | — |
| DCM/PFM boundary (this design) | I<sub>OUT</sub> ≈ ΔiL/2 = **55 mA** | above → CCM; below → DCM/PFM |

**Mode behavior (PFM / "Y" part):** at 200 mA the inductor current is continuous (valley 145 mA) → CCM near
1.1 MHz; at light loads it enters PFM for efficiency, as selected.

> **Same caveat as the 400 kHz design:** the full-load peak (~0.26 A) is close to the device's minimum PFM
> peak-current floor (I<sub>PEAK_MIN</sub> ≈ 0.33 A), so near/below full load the effective frequency may sit at
> or below 1.1 MHz with variable ripple. For **guaranteed constant 1.1 MHz / minimum ripple at 200 mA**, use
> the FPWM variant **LMR51606YFDBVR**.

---

## 10. Efficiency & thermal estimate

From the datasheet curves (Fig. 6-9, 5 V / 1.1 MHz, Y version, interpolated to V<sub>IN</sub> ≈ 40 V,
I<sub>OUT</sub> = 0.2 A): **η ≈ 82 %** — a few points lower than the 400 kHz part because switching loss scales
with frequency.

```
POUT  = 4.5 V × 0.2 A = 0.90 W
PLOSS = POUT/η − POUT ≈ 0.20 W   (inductor DCR ≈ 10 mW)
PIC   ≈ 0.19 W dissipated in the LMR51606
```

Junction-temperature rise (SOT-23-6, DBV):

```
ΔTJ = PIC × RθJA
    = 0.19 W × 95 °C/W    ≈ 18 °C   (good 2-layer layout)
    = 0.19 W × 147.8 °C/W ≈ 28 °C   (JEDEC reference, minimal copper)
```

→ At T<sub>ambient</sub> = 85 °C, T<sub>J</sub> ≈ 103–113 °C — still well below the 150 °C limit, slightly warmer
than the 400 kHz design. **No thermal concern.**

---

## 11. Bill of materials (BOM)

| Ref | Component | Value | Rating / Package | Notes |
|---|---|---|---|---|
| U1 | Buck converter | **LMR51606YDBVR** | SOT-23-6, **1.1 MHz**, 0.6 A, PFM | TI |
| L1 | Power inductor | **33 µH** | I<sub>sat</sub> ≥ 1.5 A, I<sub>rms</sub> ≥ 0.3 A, DCR ≤ 0.3 Ω, shielded | SW → V<sub>OUT</sub> |
| C<sub>OUT</sub> | Output cap | **10 µF** | 25 V, X7R, ceramic | ~8 µF after DC-bias derate |
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
| 6 | SW | To L1 (33 µH); other end of L1 = V<sub>OUT</sub> (4.5 V) |

```
        VIN(40V) ───┬───────────────┬──────────┐
                    │               │          │
                  C_IN(4.7µF)    C_HF(0.1µF)    │
                    │               │           ├── VIN(5) ── EN(4)
                   GND             GND           │
                                                 │   CB(1) ──C_BOOT(0.1µF)──┐
                                                 │                          │
                                          [ LMR51606Y ]   SW(6) ────────────┴──┬── L1(33µH) ───┬── VOUT(4.5V)
                                                 │                              │              │
                                                GND(2)                         (CB-SW cap)    R_FBT(75k)
                                                 │                                             │
                                                GND                              FB(3) ────────┤
                                                                                               R_FBB(16.2k)
                                                                                C_OUT(10µF)    │
                                                                                  │           GND
                                                                                 GND
```

---

## 13. Layout notes (datasheet §8.4)

Layout matters **more** at 1.1 MHz (higher di/dt). Otherwise identical to the 400 kHz design:

1. **C<sub>IN</sub> / C<sub>IN_HF</sub> as close as possible to VIN and GND** — minimize the input hot loop (top EMI driver).
2. Keep the **SW node copper small**; short SW–L1 trace.
3. **C<sub>BOOT</sub> close to CB and SW pins**.
4. **R<sub>FBT</sub>/R<sub>FBB</sub> next to the FB pin**; route the FB/V<sub>OUT</sub>-sense trace away from SW/L1.
5. Solid **ground plane**, short returns, thermal vias to keep T<sub>J</sub> < 125 °C.

---

## 14. Assumptions

- V<sub>IN</sub> = 40 V = maximum operating input (worst case for ripple). **Confirm it cannot exceed ~51 V**, else
  frequency foldback begins (§2).
- K<sub>IND</sub> ≈ 0.5 → 33 µH (KIND_actual 0.55); 39 µH holds K<sub>IND</sub> ≤ 0.5.
- EN tied to V<sub>IN</sub>; PFM (auto-mode) part → LMR51606YDBVR.
- Load-transient analysis assumes a full 0 → 200 mA step (no explicit transient spec given).
- η ≈ 82 % read/interpolated from datasheet curves; T<sub>J</sub> rise for both effective and JEDEC R<sub>θJA</sub>.

---

## 15. Comparison: 1.1 MHz "Y" vs 400 kHz "X"

| Parameter | **X — 400 kHz** | **Y — 1.1 MHz** | Comment |
|---|---|---|---|
| Orderable part | LMR51606**X**DBVR | LMR51606**Y**DBVR | PFM, 0.6 A, SOT-23-6 |
| Duty cycle D | 0.1125 | 0.1125 | same |
| t<sub>ON</sub> vs 80 ns min | 281 ns (3.5×) | **102 ns (1.28×)** | Y has much less ON-time margin |
| V<sub>IN</sub> before foldback | ≤ 141 V | **≤ 51 V** | X tolerates input spikes far better |
| Inductor L | 100 µH | **33 µH** | Y inductor ~3× smaller |
| Δi<sub>L</sub> (peak / valley) | 100 mA (250/150) | 110 mA (255/145) | similar |
| Output cap C<sub>OUT</sub> | 22 µF | **10 µF** | Y filter smaller |
| Output ripple | ~2 mV | ~1.8 mV | both negligible (ceramic) |
| Transient cap need (±3 %) | 11 µF | 4 µF | Y loop faster |
| FB divider / C<sub>IN</sub> / C<sub>BOOT</sub> / EN | identical | identical | unchanged |
| Efficiency @ 0.2 A | ~85 % | **~82 %** | X more efficient |
| IC dissipation / ΔT<sub>J</sub> | 0.15 W / 14–22 °C | 0.19 W / 18–28 °C | X cooler |
| **Best for** | wide/unbounded input, efficiency | **smallest L+C footprint** | — |

**Summary:** the 1.1 MHz "Y" part buys a markedly **smaller inductor and output cap** (33 µH / 10 µF vs
100 µH / 22 µF) at the cost of **lower efficiency (~82 % vs ~85 %)**, slightly higher T<sub>J</sub>, and — most
importantly — **much less input-voltage headroom before frequency foldback (~51 V vs ~141 V)**. For a clean,
bounded 40 V rail where board area is the priority, the "Y" part is a good choice; if the input can transient
well above 40 V, prefer the "X" (400 kHz) part.

*Reference: Texas Instruments LMR51606 / LMR51610 datasheet, SLUSEY1B (Dec 2023) — `lmr51606.pdf` in this folder.*
