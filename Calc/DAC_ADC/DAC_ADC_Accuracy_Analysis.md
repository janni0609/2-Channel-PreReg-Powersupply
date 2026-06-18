# DAC / ADC Accuracy Analysis — 2-Channel Pre-Reg Power Supply

**Date:** 2026-06-18
**Scope:** Setpoint (DAC) and measurement (ADC) accuracy of one channel, including the analog sense/scaling network that feeds them.
**Sources:** `schaltplan.pdf` sheet 8 (`DAC_ADC.kicad_sch`) and sheet 10 (`Linear_PostReg.kicad_sch`); datasheets `20005466A.pdf` (MCP48FVBxx), `20005440A.pdf` (MCP48FxBxx curves), `ads1118.pdf`, `tlv2387 (1).pdf`.

> **Headline:** The silicon can *measure* to ~14 bits (≈1.8 mV / 0.10 mA) and *set* to 12 bits (≈8.6 mV / 0.49 mA). Uncalibrated absolute accuracy is poor (~±3–4 %), dominated by the DAC internal band gap (±3.3 %) and 1 % resistors/shunt. **A multimeter-based 2-point calibration is mandatory** and removes all of that, leaving a temperature/INL/noise floor of ~±0.05 % (V) and ~±0.1–0.2 % (I).

---

## 1. Circuit as drawn

### 1.1 DAC/ADC sheet (sheet 8, `DAC_ADC.kicad_sch`)

| Device | Part | Configuration |
|---|---|---|
| **DAC** | U702 = **MCP48FVB22** | 12-bit dual, VDD = +3V3. Outputs **VA→Vset**, **VB→Iset**. SPI. `LAT/HVC`←DAC_LATCH. |
| **ADC** | U701 = **ADS1118IDGS** | 16-bit ΔΣ, VDD = +3V3. **Vsense→AIN0**, **Isense→AIN2**; AIN1/AIN3→GND (two pseudo-differential single-ended measurements). SPI. |

**Critical finding — DAC reference:** the **Vref pin (3) is left unconnected**. The DAC therefore must use the **internal 1.22 V band gap** (×2 output buffer → **2.44 V full scale**) or VDD as reference. *Decision (confirmed): internal band gap is used.* The unrouted Vref pin forecloses using an external precision reference without a board change.

**ADC input filters R701/R702/C701/C702 are unpopulated ("##").**

### 1.2 Sense / scaling network (sheet 10, `Linear_PostReg.kicad_sch`)

| Path | Topology | Resistors | Gain | Full-scale |
|---|---|---|---|---|
| **Voltage sense** (U501D, TLV4387) | **4-resistor difference amp** across OUT+ / OUT− | R313, R314 = 62k (inputs); R306, R309 = 4.3k (fb/gnd) | 4.3k/62k = **0.06935** | 36 V → 2.4957 V |
| **Current sense** (U501C, TLV4387) | **non-inverting** amp on low-side shunt | R318 = 0.1 Ω shunt; R316 = 18k, R317 = 1.6k | 0.1 Ω × (1+18k/1.6k) = **×12.25** | 2 A → 2.45 V |

The voltage sense amp sees a **common-mode voltage ≈ Vout/2 (up to ~18 V)**, so its four resistors' *matching* sets CMRR. The current sense is referenced to GND (no CMRR concern). The shunt (PCS2512, 0.1 Ω, "F" = ±1 %) dissipates **0.4 W at 2 A**.

---

## 2. System scaling (confirmed full-scale: 36 V / 2 A)

| | Factor | DAC FS (2.44 V) maps to | ADC usable (2.5 V) maps to |
|---|---|---|---|
| Voltage | 0.06935 V_adc/V_out → ×14.42 V_out/V_adc | 35.2 V | 36.0 V |
| Current | 1.225 V_adc/A_out → ÷1.225 | 1.99 A | 2.04 A |

---

## 3. Key datasheet specs

### 3.1 MCP48FVB22 DAC (band gap, G=0, 12-bit) — `20005466A.pdf`
| Parameter | Typ | Max | Note |
|---|---|---|---|
| Resolution | 12-bit / 4096 taps | — | no missing codes |
| **Internal band gap V_BG** | 1.22 V | 1.18–1.26 V = **±3.3 %** | dominant uncalibrated error |
| Output FS (×2 buffer) | 2.44 V | — | LSB = 595.7 µV |
| Offset error | ±1.5 mV | ±15 mV | G=0 |
| Offset tempco | ±10 µV/°C | — | |
| Gain error (ext-ref basis) | ±0.1 % | ±1.0 % | band gap tol adds on top |
| Gain-error drift | −3 ppm/°C | — | |
| **V_OUT tempco (midscale)** | **15 ppm/°C** | — | practical output drift |
| INL (band gap mode) | ≈ ±2 LSb | — | from curves doc p14 |
| DNL | small, monotonic | — | guaranteed no missing codes |

### 3.2 ADS1118 ADC — `ads1118.pdf`
| Parameter | Typ | Max | Note |
|---|---|---|---|
| Resolution | 16-bit | — | no missing codes |
| **Required FSR for 0–2.5 V** | **±4.096 V** | — | 2.5 V > 2.048 V → next range up; **LSB = 125 µV** |
| Effective bits over 0–2.5 V | ~14.3 | — | only ~20000 of 65536 codes used |
| INL | — | 1 LSb | best-fit, 99 % FS |
| Offset error (single-ended) | ±0.25 LSb | — | drift 0.002 LSb/°C |
| **Gain error (incl. PGA + ref)** | ±0.01 % | **±0.15 %** | internal ref error included |
| Gain drift (incl. ref) | 7 ppm/°C | 40 ppm/°C | |
| Noise @ 8 SPS, ±4.096 V | ≈1 LSb RMS | — | quantization-limited; ~16-bit ENOB over full ±FSR |

---

## 4. Resolution (the hard floor)

| | DAC setpoint (12-bit, 2.44 V) | ADC measurement (±4.096 V FSR) |
|---|---|---|
| Step at chip | 595.7 µV | 125 µV |
| **Voltage** | **8.6 mV** (0.024 % FS) | **1.8 mV** (0.005 % FS) |
| **Current** | **0.49 mA** (0.024 % FS) | **0.10 mA** (0.005 % FS) |
| Effective bits | 12.0 | ~14.3 |

➡ **Measurement is ~5× finer than setpoint.** You can read finer than you can command.

---

## 5. Accuracy budget — uncalibrated vs. calibrated

| Error source | Uncalibrated | After multimeter 2-pt cal |
|---|---|---|
| DAC band gap tol (1.18–1.26 V) | **±3.3 %** | removed |
| DAC gain / offset | ±1 % / ±15 mV | removed |
| ADC gain error (incl. ref) | ±0.15 % | removed |
| Resistor divider tol (1 %) | ±1.4 % | removed |
| Shunt tol (±1 %) | ±1 % (I) | removed |
| DAC INL + ADC INL | included | ~±1.8 mV (V) / ~±0.1 mA (I) |
| Temp drift (band gap/V_OUT 15 ppm/°C, ADC 7–40 ppm/°C, resistor & shunt TCR) | — | dominant residual |
| ADC noise @ 8 SPS | ~1 LSb | ~1 LSb (averages out) |
| **Net** | **~±3–4 %** | **~±0.05 % (V), ~±0.1–0.2 % (I)** |

Calibration removes every *absolute* term. What survives = **INL + noise + drift from cal point**.

---

## 6. Tolerance recommendation — per location

> **Because you calibrate, absolute tolerance is nearly irrelevant — use 1 % initial value almost everywhere.** Spend the budget on **TCR**, **matching** (V-sense diff-amp), and **shunt self-heating**.

| Ref | Value | Role | Absolute | **TCR** | **Matching** | Suggested part |
|---|---|---|---|---|---|---|
| **R313, R314** | 62k | V-sense diff-amp inputs (OUT+/OUT−) | 1 % OK | ≤10–25 ppm/°C | **critical (CMRR)** | thin-film, matched |
| **R306, R309** | 4.3k | V-sense diff-amp fb/gnd | 1 % OK | ≤10–25 ppm/°C | **critical (ratio tracks R313/R314)** | thin-film, matched |
| → all four | | set V gain **and** CMRR | | **best: single matched network, tracking ≤5 ppm/°C, ratio match ≤0.1 %** | | 4-element divider array |
| **R318** | 0.1 Ω | current shunt | ≤0.5 % | **≤50 ppm/°C (critical), ≤25 better** | n/a | 4-terminal/Kelvin metal-foil, **≥1 W headroom** |
| **R316, R317** | 18k / 1.6k | I-sense amp gain (×12.25) | 1 % OK | ≤25 ppm/°C | ratio only (0.1 % pair) | thin-film pair |
| R301–R305 | 1k–10k | CV/CC loop summing | 1 % OK | not critical *if closed-loop setpoint* | matched only if open-loop | std 1 % |
| R701, R702 | ~1k (populate) | ADC anti-alias R | 1 % OK | not critical | — | std 1 % (≤1 kΩ) |
| C701, C702 | ~100n (populate) | ADC anti-alias C | — | **C0G/NP0** | — | NP0 |
| C703, C704 | 100n | decoupling | — | — | — | X7R |

**Why V-sense matching dominates:** the diff-amp rejects ≤18 V common mode. At 1 % resistors, worst-case CMRR ≈ 28 dB → ~0.67 V CM-induced gain error. Cal absorbs the static part, but **differential TCR re-introduces it**: uncorrelated 25 ppm/°C → ~±36 mV/20 °C at 36 V; a matched network (≤5 ppm/°C tracking) → <5 mV. Matching/tracking — not absolute value — is the real V-accuracy lever.

**Why shunt TCR/self-heating:** 0.4 W at 2 A → load-dependent self-heating that **cannot be calibrated out** (it varies with current). A 2512 (~1 W) at 50 ppm/°C gives a few mA drift. Kelvin foil shunt with ≥2× power margin and ≤25 ppm/°C removes most of it.

---

## 7. Achievable accuracy (band gap + multimeter cal, recommended parts, 36 V / 2 A)

| | Resolution | After cal @ fixed temp | Over 0–50 °C |
|---|---|---|---|
| **V setpoint** | 8.6 mV | ≈ measurement (closed-loop) | ±0.05 % + few mV |
| **V measurement** | 1.8 mV | ±2–3 mV | ~±15–20 mV (~±0.05 %), diff-amp tracking-limited |
| **I setpoint** | 0.49 mA | ≈ measurement (closed-loop) | ±0.1 % |
| **I measurement** | 0.10 mA | ±2–3 mA | ±0.1–0.2 %, shunt-limited |

---

## 8. Recommendations / action items

1. **Closed-loop setpoint in firmware** (set DAC → read ADC → trim to target). Servos out the DAC band gap error, DAC INL, and the CV/CC loop resistor ratios → setpoint accuracy becomes *measurement* accuracy, limited only by the 8.6 mV / 0.49 mA DAC step. (This is why R301–R305 can stay 1 %.)
2. **Buy tolerance only where it counts:** a matched 4-resistor network for R313/R314/R306/R309, and a low-TCR Kelvin shunt for R318. Everything else 1 %.
3. **Populate the ADC input filters** R701/C701 & R702/C702 (≈1 kΩ + 100 nF NP0, ~1.6 kHz). Keep series R ≤1 kΩ (ADC Z_in ≈15 MΩ → ~67 ppm loading, calibrates out).
4. **Shunt:** size for ≥2× power margin or use a dedicated current-sense element; the 0.4 W self-heating is the current channel's accuracy limit.
5. **Grounding:** the pseudo-differential ADC inputs (AIN1/AIN3 = GND) make any sense-ground IR drop a direct error — Kelvin the ADC analog ground to the sense reference.
6. **Run the ADC at 8 SPS** (or average) for ~16-bit ENOB / lowest noise; trade against update rate.

### Optional improvement — reclaim ADC resolution
The ADC is forced to ±4.096 V because the sense maxes at ~2.5 V (>2.048 V), wasting ~0.7 bit. **Rescaling both sense paths to ≤2.0 V max** lets the ADC use **±2.048 V FSR → 62.5 µV LSB**, doubling measurement resolution to **~0.9 mV / 0.05 mA (~15 bit)**. Requires changing the divider/gain resistors (and watch headroom so transients don't clip the 2.048 V range).

---

## 9. Open items / assumptions

- **Confirmed:** internal band gap reference; firmware multimeter calibration; 36 V / 2 A full scale.
- **Assumed:** firmware does *closed-loop* setpoint (recommended). If open-loop, R301–R305 ratios need 0.1 %.
- DAC INL taken as ≈±2 LSb (band gap mode) from `20005440A.pdf` p14 (typical curves; not a guaranteed limit).
- Resistor/shunt tolerances are **not specified in the schematic** — assumed 1 % unless changed per §6.

---

## Appendix — key formulas

```
DAC LSB            = 2.44 V / 4096                = 595.7 µV
ADC LSB (±4.096 V) = 4.096 V / 32768              = 125 µV
V scale            = R306/R313 = 4.3k/62k         = 0.06935  (×14.42 to output)
I scale            = R318·(1+R316/R317)=0.1·12.25 = 1.225 V/A
Diff-amp CMRR (tol t) ≈ (1 + Rf/Rin) / (4·t)
ENOB               = ln(FSR / V_RMS_noise) / ln(2)
```
