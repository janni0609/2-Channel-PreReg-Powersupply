# Handoff: 2-Channel PSU — 256×64 Monochrome OLED Display

## Overview
This is the on-device readout for a **2-channel bench power supply**, shown on a **256 × 64 pixel monochrome OLED, 3.2″ diagonal, cyan phosphor**. Both channels are shown at once, side by side (128 px each). For each channel the panel shows live **measured** Voltage, Current and Power (large / primary), plus the **SET** target Voltage and Current limits (small / secondary, always visible).

The goal of the handoff is to reproduce this layout **on the physical panel, in firmware** — not to ship any HTML. Three numeral-style options are provided; pick one, then implement it with your display library.

## About the Design Files
The files in this bundle are **design references authored in HTML** (a pixel-accurate mock rendered at native 256×64, then scaled ×4 for viewing). They are **not code to compile** — they exist to communicate exact layout, hierarchy, values and formatting.

Your task: **recreate this layout on the target hardware** using your MCU's display stack — e.g. **U8g2** or **Adafruit_GFX** driving an **SSD1322 / SSD1362 / SH1122** (typical 3.2″ 256×64 controllers), or an LVGL monochrome buffer, or whatever the firmware already uses. Map the design's visual hierarchy onto the panel's capabilities; don't try to load web fonts.

## Fidelity
**High-fidelity.** Coordinates, value formatting, and the visual hierarchy below are intentional and should be matched. The exact pixel grid given under *Layout* is a solid starting point — nudge by ±1–2 px to suit the specific bitmap font metrics you choose.

## Hardware notes that change how you build this
- **A monochrome OLED has one physical color.** The "cyan" is the panel's phosphor, and the soft **glow** around digits is a physical property of the OLED — **do not** try to emulate tint or glow in firmware. Just draw pixels "on".
- **If the panel is grayscale (SSD1322 = 16 levels, SSD1362/SH1122 similar):** use grayscale to reproduce the light/dark hierarchy from the mock (bright big numbers, dimmer labels/units/SET strip, faint divider lines). Grayscale levels are suggested per element below.
- **If the panel is pure 1-bit (on/off):** you can't dim. Reproduce the hierarchy with **size and weight instead** — big bold numerals, smaller thinner labels, and dotted/short divider lines rather than dim ones.

## The Screen (single view)
- **Name:** Dual-channel main readout (the default/home screen).
- **Purpose:** At-a-glance monitoring of both outputs; set values remain visible so the operator always sees the configured limits next to the live readings.
- **Canvas:** 256 (W) × 64 (H), origin top-left, +x right, +y down. Background = off (black). Content = on (cyan).
- **Two columns:** CH1 occupies x = 0…127, CH2 occupies x = 128…255. A 1 px vertical divider sits between them.

### Layout — recommended fixed pixel grid
Coordinates are given per channel. **CH1 uses column origin `CX = 0`; CH2 uses `CX = 128`.** Inner margins: 6 px left, 6 px right (usable local x = 6…121). `y` values are global (identical for both channels). Baselines assume `setFontPosBaseline`-style drawing (y = text baseline).

| Element            | Anchor / align                    | x (local, +CX)        | baseline y | notes |
|--------------------|-----------------------------------|-----------------------|-----------|-------|
| Channel label      | left                              | x = 6                 | y = 9     | "CH1" / "CH2", small bold |
| Power readout      | right-aligned, right edge         | right edge x = 121    | y = 9     | e.g. "P 10.27W" — small |
| **Voltage (big)**  | right-aligned number, right edge  | number right edge x = 112 | y = 32 | primary; unit "V" follows |
| Voltage unit "V"   | right, right edge                 | x = 121               | y = 32    | small, dim |
| **Current (big)**  | right-aligned number, right edge  | number right edge x = 112 | y = 50 | primary; unit "A" follows |
| Current unit "A"   | right, right edge                 | x = 121               | y = 50    | small, dim |
| Separator line     | horizontal                        | x = 5 → 121           | y = 53    | 1 px, faint |
| SET label          | left                              | x = 6                 | y = 62    | "SET", small dim |
| Set voltage        | centered in strip                 | ~x = 44 (centered)    | y = 62    | e.g. "12.50V" |
| Set current        | right-aligned, right edge         | x = 121               | y = 62    | e.g. "1.000A" |
| **Center divider** | vertical line                     | global x = 127        | y = 6…58  | 1 px, faint |

Vertical rhythm summary (global y): header baseline 9 → voltage baseline 32 → current baseline 50 → separator 53 → SET baseline 62. Keep the big-number font tall enough to read but ≤ ~18 px cap height so rows don't collide; drop the current font 2–3 px smaller than voltage if needed.

### Grayscale level suggestions (for SSD1322-class panels; 0 = off, 15 = full)
- Big Voltage / Current numbers: **15**
- Channel label ("CH1"): **12**
- Power readout, unit letters V/A/W, "P": **7**
- SET label + set V / set A: **8**
- Divider + separator lines: **4–5**

On a 1-bit panel, render all of the above at level "on" and lean on font size/weight for the same hierarchy.

## Numeral-style options (pick one)
All three share the identical layout above; only the **big-number font** changes. On a 1-bit OLED everything is ultimately a bitmap glyph, so each option is really a font-file choice.

- **Option 1a — Seven-segment** (classic lab-instrument look, lit segments only).
  - U8g2: there is no small built-in 7-seg font at this height. Either **generate a custom 7-segment BDF** at ~18–20 px and add it to U8g2, or **draw the segments procedurally** (7 rectangles per digit) — procedural gives you full control of the ~18 px height and looks authentic. Off-segments should be *fully off* (do not draw ghost segments — matches OLED).
- **Option 1b — Pixel / bitmap** (native embedded look, aligns to the pixel grid).
  - U8g2: `u8g2_font_logisoso18_tn` or `u8g2_font_profont22_tn` for the big numbers; `u8g2_font_5x7_tf` / `u8g2_font_6x10_tf` for labels. This is the lowest-effort, most "native" option.
- **Option 1c — Clean digits** (modern, condensed, max legibility per pixel).
  - U8g2: `u8g2_font_helvB18_tn` or `u8g2_font_inb19_mn` for big numbers; `u8g2_font_6x10_tf` for labels.

## Values, ranges & formatting
Measured values are shown at higher resolution than the set values (matches the hardware's measurement vs. DAC-set granularity).

| Field          | Max      | Format (C `printf`) | Example  |
|----------------|----------|---------------------|----------|
| Voltage (meas) | 35.000 V | `"%.3f"`            | `12.472` |
| Current (meas) | 2.0000 A | `"%.4f"`            | `0.8231` |
| Power (meas)   | 70.00 W  | `"%.2f"`            | `10.27`  |
| Set Voltage    | 35.00 V  | `"%.2f"`            | `12.50`  |
| Set Current    | 2.000 A  | `"%.3f"`            | `1.000`  |

- Power is derived: `P = Vmeas × Imeas` (compute, don't store separately).
- **Use fixed-width / tabular formatting** so digits don't jitter as values change: pad to a constant field width and right-align (e.g. `"%6.3f"` for V). Tabular alignment is why the numbers are right-anchored at x = 112.
- Clamp/round to the resolutions above before display.

### Sample data shown in the mock (a useful test case)
- **CH1** — voltage-mode: SET 12.50 V / 1.000 A · measured 12.472 V, 0.8231 A, 10.27 W (below current limit → CV).
- **CH2** — at current limit: SET 5.00 V / 2.000 A · measured 4.021 V, 2.0000 A, 8.04 W (clamped at 2.000 A → CC).

This pair is worth reproducing as a bring-up test: it exercises both a normal reading and a limit-clamped reading, and checks that the 4-dp current field and 3-dp voltage field fit within the column without overrunning the unit letters.

## Interactions & behavior (not yet designed — flag for follow-up)
This handoff covers the **static main readout only**. Not yet specified: encoder/adjust state (highlighting the SET field being edited), output ON/OFF indication, CV/CC mode badges, over-temp/fault states, and any menu/settings screens. Ask the designer before inventing these. Refresh: redraw measured values at whatever your ADC sample/averaging rate is (typically 5–20 Hz); avoid faster than the eye needs, to reduce flicker and bus load.

## State (firmware)
Per channel: `v_set`, `i_set` (from encoders/DAC), `v_meas`, `i_meas` (from ADC), derived `p_meas`, and a mode flag (CV/CC — not yet shown but computable: CC when `i_meas ≥ i_set` within tolerance). A `dirty` flag per field to avoid full redraws helps on slow SPI buses.

## Design tokens
- Canvas: 256 × 64 px, 1 origin top-left.
- Columns: 2 × 128 px; inner margins L/R = 6 px.
- Divider: vertical @ x = 127, y 6…58; separators: horizontal @ y = 53, x 5…121.
- Number right-anchor: x = 112 (+CX); unit right edge: x = 121 (+CX).
- Baselines (global y): 9 / 32 / 50 / 62.
- "Color": monochrome — on = cyan phosphor (hardware), off = black. Mock reference cyan ≈ `#54E0FF` (for on-screen preview only).
- Grayscale hierarchy (SSD1322): 15 / 12 / 8 / 7 / 4 (see table above).

## Assets
None. No images or icons — all text/rules drawn by the display library. Fonts are chosen from your display library's built-ins (or a generated 7-seg BDF for option 1a); the web fonts in the HTML mock (DSEG7, Silkscreen, Barlow) are stand-ins to *illustrate* the three styles, not for firmware use.

## Files in this bundle
- `OLED Concept.dc.html` — the concept board: all three options (1a/1b/1c), each shown ×4 enlarged with a 1× actual-size reference. Open in a browser to see the intended result. (The tint / glow / pixel-grid controls in the mock are preview aids only — ignore for firmware.)
- `OledScreen.dc.html` — the single reusable 256×64 screen; **read this for the exact element order, spacing and formatting**.
- `support.js` — runtime needed only so the two HTML files render in a browser. Not relevant to firmware.
