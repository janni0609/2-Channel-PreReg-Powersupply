# LMR51606Y Buck Converter Design — 40 V → 4.5 V @ **500 mA** (1.1 MHz variant)

Synchronous step-down (buck) converter, **LMR51606 1.1 MHz "Y" variant**, at the **higher 500 mA** load
(datasheet *SLUSEY1B*, Rev. December 2023, `lmr51606.pdf` in this folder).

> Same part and choices as `LMR51606Y_40V_4V5_200mA_design.md` (EN→V<sub>IN</sub>, K<sub>IND</sub> ≈ 0.5, PFM),
> only **I<sub>OUT</sub> = 500 mA** instead of 200 mA. Most of the design is unchanged; what changes is the
> **inductor, the output/input caps, the current-limit margin, and — most importantly — the thermals.**
> §15 lists exactly what changed; §10 is the one to read carefully. ⚠️

> **⚠️ Headroom warning:** 500 mA is **83 % of the LMR51606's 0.6 A rating** and **53 % of its typical
> maximum deliverable current** (Eq. 6 = 0.95 A). The part *can* do it, but margins are no longer generous.
> At this load the **1 A pin-compatible sibling LMR51610Y** (same datasheet/footprint) or the cooler-running
> **400 kHz LMR51606X** are worth considering — see §10 and §15.

---

## 1. Design inputs

| Parameter | Value | Note |
|---|---|---|
| Input voltage, V<sub>IN</sub> | **40 V** (worst case for ripple) | spec |
| Output voltage, V<sub>OUT</sub> | **4.5 V** | spec |
| Output current, I<sub>OUT</sub> | **500 mA** | **changed** (was 200 mA) |
| Switching frequency, f<sub>SW</sub> | **1.1 MHz** | "Y" suffix |
| Reference voltage, V<sub>REF</sub> | **0.8 V** (±1.5 %) | datasheet §6.5 |
| Inductor ripple ratio, K<sub>IND</sub> | **≈ 0.5** | design choice |
| EN / mode | EN→V<sub>IN</sub>, PFM | unchanged |
| **Orderable part** | **LMR51606YDBVR** | 0.6 A, 1100 kHz, PFM, SOT-23-6 |

---

## 2. Operating point — *unchanged* (duty cycle is load-independent)

```
D = VOUT / VIN = 0.1125    tON = 102 ns (> 80 ns min)    tOFF = 807 ns
VIN before frequency foldback ≤ 51 V   (still fine at 40 V; see 200 mA doc §2)
```

**One thing improves at 500 mA:** the converter is now **solidly in CCM** — the DCM/PFM boundary is
I<sub>OUT</sub> ≈ Δi<sub>L</sub>/2 ≈ 121 mA, and the peak current (621 mA) is far above the device's PFM
peak-current floor (~0.33 A). So the "near-PFM-boundary" caveat that applied at 200 mA **does not apply
here** — the part runs at a constant 1.1 MHz at this load.

---

## 3. Feedback divider — *unchanged* (frequency- and load-independent)

```
RFBT = 75.0 kΩ (1 %),  RFBB = 16.2 kΩ (1 %)
VOUT = 0.8 × (1 + 75.0k/16.2k) = 4.504 V   (+0.08 %)
```

---

## 4. Inductor (L) — **changed**

Minimum inductance (Eq. 9), K<sub>IND</sub> = 0.5, now with I<sub>OUT</sub> = 0.5 A:

```
LMIN = (40 − 4.5)/(0.5 × 0.5) × 4.5/(40 × 1.1 MHz) = 14.5 µH
```

→ **Choose 15 µH** (nearest standard, K<sub>IND</sub> ≈ 0.5). Currents (Eq. 8):

```
ΔiL   = 242 mA  (KIND_actual = 0.48)
IL,peak   = 500 + 121 = 621 mA
IL,valley = 500 − 121 = 379 mA
IL,rms    = 505 mA
```

> **⚠️ Current-limit margin:** peak 621 mA vs. the device's **worst-case minimum HS current limit of 0.8 A
> → only 1.29× margin.** That is acceptable but no longer comfortable. **Recommended at this load: use a
> 22 µH inductor** (Δi<sub>L</sub> = 165 mA, peak **583 mA**, margin **1.37×**, lower ripple, lower cap RMS).
> The slightly larger inductor is the safer choice for a 500 mA design and is what I'd build.

**Inductor requirements (for 15 µH or 22 µH):**

| Parameter | Requirement | Note |
|---|---|---|
| Inductance | **15 µH** (22 µH recommended) | K<sub>IND</sub> ≈ 0.5 / 0.33 |
| Saturation current, I<sub>sat</sub> | **≥ 1.5 A** | above device max HS peak current limit (1.4 A) |
| RMS current rating | **≥ 0.7 A** | I<sub>rms</sub> ≈ 0.5 A + margin |
| DCR | **≤ ~0.15 Ω** | at 0.5 A, DCR loss matters: 0.15 Ω → 38 mW |
| Type | Shielded ferrite | low EMI |

---

## 5. Output capacitor (C<sub>OUT</sub>) — **changed to 22 µF**

The bigger load step drives this. With Δi<sub>L</sub> = 242 mA (15 µH):

```
Ripple (Eq. 11), 22 µF:  ΔVOUT_C = ΔiL/(8·fSW·COUT) = 1.25 mV   (negligible, ceramic)
Transient (Eq. 12), full 0 → 500 mA step:
    ±5 % (225 mV) → COUT > 6.1 µF
    ±3 % (135 mV) → COUT > 10.1 µF
```

→ **Choose 22 µF / 25 V / X7R** (was 10 µF at 200 mA). Covers a full 0→500 mA step within ±3 % with margin
after DC-bias derating (~18 µF effective). 10 µF is only adequate for ±5 %.

---

## 6. Input capacitor (C<sub>IN</sub>) — **higher ripple current**

```
ICIN,rms ≈ IOUT × √(D·(1−D)) = 0.5 × √(0.1125·0.8875) = 158 mA   (was 63 mA at 200 mA)
```

| Component | Value | Rating | Note |
|---|---|---|---|
| C<sub>IN</sub> | **10 µF** (or 2 × 4.7 µF) | **100 V**, X7R | bumped up for the 158 mA RMS + lower input ripple |
| C<sub>IN_HF</sub> | **0.1 µF** | 100 V, X7R | at VIN/GND pins |

A single 4.7 µF/100 V still survives 158 mA, but 10 µF (or two 4.7 µF) keeps input ripple and cap heating low.

---

## 7. Bootstrap, enable, soft-start, compensation — *unchanged*

- **C<sub>BOOT</sub> = 0.1 µF / 25 V X7R** (CB→SW).
- **EN tied to V<sub>IN</sub>** (internal V<sub>IN</sub> UVLO ≈ 3.8 V / 3.56 V).
- **Soft-start internal (2.3 ms); compensation internal.** No external parts.

---

## 8. Protection & current limit ⚠️

| Item | Value | At 500 mA |
|---|---|---|
| HS peak current limit | 1.1 A typ (**0.8 A min**, 1.4 A max) | peak 621 mA → **1.29× to worst-case min** (1.5× typ) |
| Max deliverable (Eq. 6) | (1.1 + 0.8)/2 = **0.95 A** | 500 mA = 53 % of it |
| Hiccup short-circuit | FB < 40 % V<sub>REF</sub>, 256 cyc | auto-recovering |
| Thermal shutdown | 165 °C | see §10 |

The peak current stays below the minimum current limit, so no false tripping at room temperature — but with
only 1.29× margin (15 µH), a load transient or a low-tolerance inductor could approach it. 22 µH restores
margin to 1.37×.

---

## 9. — (see §10)

---

## 10. ⚠️ Efficiency & thermal — the limiting factor at 500 mA

At 6.25× the power of the 200 mA design, dissipation is the real constraint. Estimated IC loss budget at
V<sub>IN</sub> = 40 V, 1.1 MHz:

```
High-side conduction (0.7 Ω)   ≈ 20 mW
Low-side conduction  (0.36 Ω)  ≈ 80 mW
Switching loss (∝ VIN·IOUT·fSW) ≈ 110 mW   (large at 40 V / 1.1 MHz)
Gate / quiescent               ≈ 12 mW
──────────────────────────────────────────
IC dissipation                 ≈ 0.22 – 0.44 W   (range = optimistic vs. conservative switching loss)
```

Total system loss ≈ 0.46 W at η ≈ 83 % (P<sub>OUT</sub> = 2.25 W). Junction-temperature rise:

| IC loss | R<sub>θJA</sub> = 95 °C/W (good 2-layer) | R<sub>θJA</sub> = 147.8 °C/W (JEDEC, min. copper) |
|---|---|---|
| 0.30 W | ΔT<sub>J</sub> = 29 °C → **T<sub>J</sub> ≈ 114 °C** @ 85 °C ambient | 44 °C → **129 °C** |
| 0.44 W | 42 °C → **127 °C** @ 85 °C ambient | 65 °C → **150 °C → thermal-shutdown risk** ⚠️ |

**Conclusion:**
- At **moderate ambient (≤ 50–60 °C) with a good 2-layer-or-better layout and thermal vias**, T<sub>J</sub> stays
  comfortably below 150 °C — the design is fine.
- At **high ambient (≥ 85 °C) and/or minimal copper**, T<sub>J</sub> approaches the 150 °C limit. The datasheet
  explicitly notes the 0.6 A rating **derates at high f<sub>SW</sub> and high ambient** — 1.1 MHz at 40 V/500 mA is
  exactly that corner.

**Ways to relieve it (recommended for a robust 500 mA design):**
1. **Use the 400 kHz LMR51606X instead of the 1.1 MHz "Y"** — lower switching loss → cooler (at the cost of a
   larger inductor, see the X-variant doc). Best efficiency option.
2. **Use the pin-compatible 1 A LMR51610Y** (same datasheet/footprint, just re-spec L for ~1 A) — its higher
   current limit (1.6 A typ / 1.25 A min) gives ~2× peak-current margin and headroom at 500 mA.
3. Maximize copper / add thermal vias under the device to push R<sub>θJA</sub> toward 95 °C/W.

---

## 11. Bill of materials (BOM)

| Ref | Component | Value | Rating / Package | Notes |
|---|---|---|---|---|
| U1 | Buck converter | **LMR51606YDBVR** | SOT-23-6, 1.1 MHz, 0.6 A, PFM | see §10 re: 1 A LMR51610Y |
| L1 | Power inductor | **15 µH** (22 µH recommended) | I<sub>sat</sub> ≥ 1.5 A, I<sub>rms</sub> ≥ 0.7 A, DCR ≤ 0.15 Ω, shielded | SW → V<sub>OUT</sub> |
| C<sub>OUT</sub> | Output cap | **22 µF** | 25 V, X7R, ceramic | up from 10 µF |
| C<sub>IN</sub> | Input cap | **10 µF** (or 2×4.7 µF) | 100 V, X7R, ceramic | 158 mA RMS |
| C<sub>IN_HF</sub> | Input HF bypass | **0.1 µF** | 100 V, X7R | at VIN/GND pins |
| C<sub>BOOT</sub> | Bootstrap cap | **0.1 µF** | 25 V, X7R | CB → SW |
| R<sub>FBT</sub> | Feedback top | **75.0 kΩ** | 1 %, low-TC | V<sub>OUT</sub> → FB |
| R<sub>FBB</sub> | Feedback bottom | **16.2 kΩ** | 1 %, low-TC | FB → GND |
| — | EN | **wire to V<sub>IN</sub>** | — | no UVLO divider |

---

## 12. Connection summary (per pin)

| Pin | Name | Connection |
|---|---|---|
| 1 | CB | C<sub>BOOT</sub> (0.1 µF) to SW (pin 6) |
| 2 | GND | System ground; ground side of C<sub>IN</sub>, C<sub>OUT</sub>, R<sub>FBB</sub> |
| 3 | FB | Junction of R<sub>FBT</sub> / R<sub>FBB</sub> |
| 4 | EN | Tied to VIN (pin 5) |
| 5 | VIN | 40 V input; C<sub>IN</sub> (10 µF) + C<sub>IN_HF</sub> (0.1 µF) to GND, directly at pin |
| 6 | SW | To L1 (15/22 µH); other end of L1 = V<sub>OUT</sub> (4.5 V) |

(Topology identical to the 200 mA "Y" design — only L1, C<sub>OUT</sub>, C<sub>IN</sub> values differ.)

---

## 13. Layout notes

Same as the 200 mA design (datasheet §8.4), **with extra emphasis on thermal management at 500 mA:**
- Maximize GND/V<sub>IN</sub>/SW copper and use an **array of thermal vias under the device** to the inner/bottom
  ground planes — this directly sets R<sub>θJA</sub> and therefore T<sub>J</sub> (§10).
- C<sub>IN</sub>/C<sub>IN_HF</sub> right at VIN/GND; SW node small; C<sub>BOOT</sub> near CB/SW; FB resistors at the FB pin.

---

## 14. Assumptions

- V<sub>IN</sub> = 40 V = max operating input; confirm it cannot exceed ~51 V (foldback) — see 200 mA doc §2.
- K<sub>IND</sub> ≈ 0.5 → 15 µH; **22 µH recommended** for current-limit margin at this load.
- Load-transient analysis assumes a full 0 → 500 mA step.
- η ≈ 83 % and the switching-loss term are **estimates** (datasheet curves are for the LMR51610 sibling and
  stop short of this exact corner); the T<sub>J</sub> table brackets optimistic and conservative loss.

---

## 15. What changed vs the 200 mA "Y" design

| Parameter | 200 mA | **500 mA** | Comment |
|---|---|---|---|
| Inductor L | 33 µH | **15 µH** (or 22 µH) | scales ~1/I<sub>OUT</sub> at fixed K<sub>IND</sub> |
| Δi<sub>L</sub> / peak | 110 mA / 255 mA | 242 mA / **621 mA** | — |
| Current-limit margin (to 0.8 A min) | 3.1× | **1.29×** (15 µH) / 1.37× (22 µH) | ⚠️ much tighter |
| Output cap | 10 µF | **22 µF** | bigger load step |
| Input cap / RMS | 4.7 µF / 63 mA | **10 µF / 158 mA** | — |
| IC dissipation | ~0.19 W | **~0.22–0.44 W** | — |
| T<sub>J</sub> rise | 18–28 °C | **29–65 °C** | ⚠️ marginal at high ambient |
| Operating mode @ load | near PFM boundary | **solidly CCM** | improvement |
| FB divider / C<sub>BOOT</sub> / EN | identical | identical | unchanged |

**Bottom line:** 500 mA works on the LMR51606Y at moderate ambient with a good thermal layout, but it pushes
the 0.6 A / 1.1 MHz part into its derating corner and leaves modest current-limit margin. Prefer **22 µH** over
15 µH, and for a robust design consider the **400 kHz LMR51606X** (cooler) or the **1 A LMR51610Y**
(pin-compatible, far more margin).

*Reference: Texas Instruments LMR51606 / LMR51610 datasheet, SLUSEY1B (Dec 2023) — `lmr51606.pdf` in this folder.*
