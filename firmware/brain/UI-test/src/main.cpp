#include <Arduino.h>
#include <SSD1322.h>
#include <stdio.h>
#include <string.h>
#include "BigDigits.h"

// ── Front-panel OLED wiring (SSD1322 256x64) on the RP2350-Tiny brain ──────────
//   RES  -> GPIO9     reset          (manual GPIO)
//   SCK  -> GPIO10    SPI1 SCK
//   MOSI -> GPIO11    SPI1 TX
//   DC   -> GPIO12    data/command   (manual GPIO)
//   CS   -> GPIO13    chip select    (manual GPIO)
// GPIO10/GPIO11 are the SPI1 clock/data functions, so the driver uses SPI1.
#define PIN_RES  9
#define PIN_SCK  10
#define PIN_MOSI 11
#define PIN_DC   12
#define PIN_CS   13

SSD1322 display(SPI1, PIN_CS, PIN_DC, PIN_RES, PIN_SCK, PIN_MOSI);

// ── Dual-channel main readout — layout from design_handoff_oled_psu ────────────
// Numeral style: Option 1c ("Clean digits") — the big V/I numbers use the
// condensed BigDigits font; labels/units/SET strip use the driver's 5x7 font.
//
// Two 128px columns (CH1: CX=0, CH2: CX=128) split by a 1px centre divider.
// Per-channel element grid (local x, add CX; y is global). Baselines follow the
// handoff; the 5x7 font draws from its top row (baseline ≈ top+6), and BigDigits
// draws from its cell top (baseline = top + BigDigits::BASELINE).
//
//   Channel label "CHx"   left   x=6           baseline y=9   (5x7)
//   Power  "xx.xxW"        right  right-edge=121 baseline y=9   (5x7, dim)
//   Voltage (big)          right  right-edge=112 baseline y=32  (BigDigits)
//   unit "V"               right  right-edge=121 baseline y=32  (5x7, dim)
//   Current (big)          right  right-edge=112 baseline y=50  (BigDigits)
//   unit "A"               right  right-edge=121 baseline y=50  (5x7, dim)
//   separator (h)          x=5..121  y=52   (faint)
//   "SET"                  left   x=6           baseline y=62  (5x7)
//   set V "xx.xxV"         centred ~x=44        baseline y=62  (5x7)
//   set I "x.xxxA"         right  right-edge=121 baseline y=62  (5x7)
//   centre divider (v)     global x=127  y=6..57 (faint)

// Grey levels (0..15) — hierarchy per the handoff grayscale table.
static const uint8_t G_BIG   = 15;  // big voltage / current numbers
static const uint8_t G_CHLBL = 12;  // "CH1" / "CH2"
static const uint8_t G_NUM   = 8;   // power number, set values
static const uint8_t G_DIM   = 7;   // "P", "W", unit letters V/A
static const uint8_t G_SET   = 8;   // "SET" label
static const uint8_t G_LINE  = 4;   // divider + separator lines

// Baselines / row tops.
static const int Y_HDR_TOP = 3;   // 5x7 top for baseline 9  (header row)
static const int Y_V_TOP   = 14;  // BigDigits top for baseline 29 (voltage) — nudged up for gap
static const int Y_I_TOP   = 35;  // BigDigits top for baseline 50 (current)
static const int Y_UNIT_V  = 23;  // 5x7 top for baseline 29 (tracks voltage)
static const int Y_UNIT_A  = 44;  // 5x7 top for baseline 50
static const int Y_SEP     = 52;  // separator line
static const int Y_SET_TOP = 56;  // 5x7 top for baseline 62 (SET strip)

// Number field cleared/redrawn each frame (holds both big numbers, x=40..113).
// Height spans the raised voltage row (Y_V_TOP) down through the current row.
static const int NUM_X = 40, NUM_W = 74, NUM_Y = Y_V_TOP, NUM_H = Y_I_TOP + 16 - Y_V_TOP;
// Power readout region (top-right, cleared each frame).
static const int PWR_X = 40, PWR_W = 82, PWR_Y = 2, PWR_H = 8;
// Fixed anchor for the power unit so "W" never shifts as the value width changes.
static const int PWR_W_X = 116;  // local x of the "W" unit (right edge ≈ 121)

struct Channel {
    float measV;
    float measI;
    float setV;
    float setI;
};

// Right-align a 5x7 string so its rightmost pixel sits near local x=rightX (+CX).
static void drawText5x7Right(int rightX, int y, const char *s, uint8_t fg) {
    display.drawText(rightX + 1 - (int)strlen(s) * 6, y, s, fg, 0);
}

// Thin gap (px) inserted before the least-significant current digit so the 4th
// decimal reads slightly apart, e.g. "0.823 1". Normal inter-digit gap is 1 px.
static const int I_TAIL_GAP = 6;

// Draw a BigDigits number right-anchored at rightX, but split off the last
// character with I_TAIL_GAP px of extra space before it. Used for the measured
// current ("%.4f") to set its 4th decimal apart from the first three.
static void drawBigCurrent(int rightX, int yTop, const char *s, uint8_t color) {
    int n = (int)strlen(s);
    if (n < 2) { BigDigits::drawRight(display, rightX, yTop, s, color); return; }

    char head[16];
    memcpy(head, s, (size_t)(n - 1));
    head[n - 1] = '\0';
    const char *tail = s + (n - 1);        // final digit (the 4th decimal)

    int headW  = BigDigits::textWidth(head);
    int total  = headW + I_TAIL_GAP + BigDigits::textWidth(tail);
    int x      = rightX + 1 - total;
    BigDigits::draw(display, x, yTop, head, color);
    BigDigits::draw(display, x + headW + I_TAIL_GAP, yTop, tail, color);
}

// ── Startup self-test ──────────────────────────────────────────────────────────
// Full-screen pattern so a wiring/init fault is obvious: border (extents),
// grayscale ramp (4-bit path), and text (addressing). Held briefly, then the
// readout takes over.
void selfTest() {
    display.clear(0);
    display.drawRect(0, 0, SSD1322::WIDTH, SSD1322::HEIGHT, 0xF);
    for (int i = 0; i < 16; i++)
        display.fillRect(4 + i * 15, 12, 14, 20, (uint8_t)i);
    display.drawText(4, 2, "SSD1322 SELFTEST  RP2350 SPI1", 0xF, 0);
    display.drawText(4, 40, "0", 0xF, 0);
    display.drawText(SSD1322::WIDTH - 5 - 12, 40, "15", 0xF, 0);
    display.drawTextScaled(80, 44, "BRAIN UI", 0xF, 0, 2);
    display.flush();
    delay(2500);
}

// Elements that never change while a channel is displayed. Called once per panel.
void drawStatic(const Channel &ch, int xOff) {
    char buf[12];

    // Channel label (top-left).
    display.drawText(xOff + 6, Y_HDR_TOP, xOff == 0 ? "CH1" : "CH2", G_CHLBL, 0);

    // Unit letters beside the big numbers (right-aligned to x=121, dim).
    display.drawText(xOff + 116, Y_UNIT_V, "V", G_DIM, 0);
    display.drawText(xOff + 116, Y_UNIT_A, "A", G_DIM, 0);

    // Separator above the SET strip.
    display.drawLine(xOff + 5, Y_SEP, xOff + 121, Y_SEP, G_LINE);

    // SET strip: "SET"  |  set V (centred ~x=44)  |  set I (right).
    display.drawText(xOff + 6, Y_SET_TOP, "SET", G_SET, 0);

    snprintf(buf, sizeof(buf), "%.2fV", ch.setV);          // e.g. "12.50V"
    // Centred at x=54: midway between the "SET" label and the set-current value.
    display.drawText(xOff + 54 - (int)strlen(buf) * 3, Y_SET_TOP, buf, G_SET, 0);

    snprintf(buf, sizeof(buf), "%.3fA", ch.setI);          // e.g. "1.000A"
    drawText5x7Right(xOff + 121, Y_SET_TOP, buf, G_SET);
}

// Redraw the parts that track the measured values: power readout + big V/I.
// Each region is cleared first (values change width), so no stale pixels remain.
void drawDynamic(const Channel &ch, int xOff) {
    char buf[16];

    // Power readout "xx.xxW". "W" is pinned at fixed x; the value is right-aligned
    // against it so its decimal stays put (tabular) as the width changes.
    display.fillRect(xOff + PWR_X, PWR_Y, PWR_W, PWR_H, 0);
    display.drawText(xOff + PWR_W_X, Y_HDR_TOP, "W", G_DIM, 0);   // fixed right unit
    snprintf(buf, sizeof(buf), "%.2f", ch.measV * ch.measI);
    drawText5x7Right(xOff + PWR_W_X - 1, Y_HDR_TOP, buf, G_NUM);  // ends just left of "W"

    // Big voltage / current numbers (right-anchored at x=112).
    display.fillRect(xOff + NUM_X, NUM_Y, NUM_W, NUM_H, 0);
    snprintf(buf, sizeof(buf), "%.3f", ch.measV);          // e.g. "12.472"
    BigDigits::drawRight(display, xOff + 112, Y_V_TOP, buf, G_BIG);
    snprintf(buf, sizeof(buf), "%.4f", ch.measI);          // e.g. "0.823 1"
    drawBigCurrent(xOff + 112, Y_I_TOP, buf, G_BIG);
}

// Push both dynamic regions of one channel to the panel (power + numbers).
void flushDynamic(int xOff) {
    display.flushRect(xOff + PWR_X, PWR_Y, PWR_W, PWR_H);
    display.flushRect(xOff + NUM_X, NUM_Y, NUM_W, NUM_H);
}

Channel ch1 = { 0, 0, 12.50f, 1.000f };
Channel ch2 = { 0, 0,  5.00f, 2.000f };

void setup() {
    Serial.begin(115200);
    display.begin();

    selfTest();

    display.clear(0);
    display.drawLine(127, 6, 127, 57, G_LINE);   // centre divider

    drawStatic(ch1,   0);
    drawStatic(ch2, 128);
    drawDynamic(ch1,   0);
    drawDynamic(ch2, 128);

    display.flush();  // one full flush to paint the static background
}

void loop() {
    // Demo sweep until real ADC data is wired in: CH1 in CV, CH2 clamped at CC.
    const uint32_t PERIOD = 5000;
    uint32_t now = millis();
    float t1 = (float)(now % PERIOD) / (float)PERIOD;
    float t2 = (float)((now + PERIOD / 2) % PERIOD) / (float)PERIOD;

    ch1.measV = t1 * 30.0f;
    ch1.measI = t1 *  1.0f;
    ch2.measV = t2 * 30.0f;
    ch2.measI = t2 *  1.0f;

    uint32_t t0 = micros();
    drawDynamic(ch1,   0);
    drawDynamic(ch2, 128);
    flushDynamic(0);
    flushDynamic(128);
    uint32_t dt = micros() - t0;

    Serial.print("update: ");
    Serial.print(dt);
    Serial.println(" us");

    delay(50);  // ~20 fps; reduce or remove once timing is confirmed
}
