#include <Arduino.h>
#include <SSD1322.h>
#include <stdio.h>
#include <string.h>

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

// ── Rotary encoder (temporary direct wiring; button moves to the IO expander
// later). The ROT signals are active-low, so the inputs use pull-ups; a closed
// contact pulls the pin down. GPIO17 is just held high for the current hookup.
//   ROT_A  -> GPIO14   quadrature A   (PIO in-base; A/B must be consecutive)
//   ROT_B  -> GPIO15   quadrature B
//   ROT_BT -> GPIO16   push button (digit select), low = pressed
//   GPIO17 -> driven high
#define PIN_ENC_A   14
#define PIN_ENC_B   15
#define PIN_ENC_BT  16
#define PIN_ENC_PWR 17

SSD1322 display(SPI1, PIN_CS, PIN_DC, PIN_RES, PIN_SCK, PIN_MOSI);

// ── Quadrature decoder (PIO) ────────────────────────────────────────────────────
// A PIO state machine (quadrature_encoder.pio, from pico-examples) samples A/B
// every ~10 sys clocks, walks the quadrature state table in hardware and keeps
// the signed edge count in its Y register, streaming it to the RX FIFO. The CPU
// takes no interrupts; serviceEncoder() just fetches the latest count. One
// mechanical detent = 4 edge counts.
#include "hardware/pio.h"
#include "quadrature_encoder.pio.h"

static PIO     encPio       = pio0;
static int     encSm        = -1;
static int32_t encLastCount = 0;   // PIO count at the previous serviceEncoder()
static int     encAccum     = 0;   // edges accumulated toward the next detent
static const int ENC_SIGN   = -1;  // flip sign if rotation goes the wrong way

static void encoderInit() {
    // Pads: inputs with pull-ups (signals are active-low). The PIO IN path
    // sees the pad regardless of function select, so plain pinMode() is enough.
    pinMode(PIN_ENC_PWR, OUTPUT);
    digitalWrite(PIN_ENC_PWR, HIGH);
    pinMode(PIN_ENC_A,  INPUT_PULLUP);
    pinMode(PIN_ENC_B,  INPUT_PULLUP);
    pinMode(PIN_ENC_BT, INPUT_PULLUP);

    encSm = pio_claim_unused_sm(encPio, true);
    pio_add_program(encPio, &quadrature_encoder_program);  // needs .origin 0
    quadrature_encoder_program_init(encPio, encSm, PIN_ENC_A, 0);
    encLastCount = quadrature_encoder_get_count(encPio, encSm);
}

// Whole detents turned since the last call (sign per ENC_SIGN), remainder kept.
static int encoderReadSteps() {
    int32_t count = quadrature_encoder_get_count(encPio, encSm);
    encAccum += (int)(count - encLastCount) * ENC_SIGN;
    encLastCount = count;
    int steps = encAccum / 4;
    encAccum -= steps * 4;
    return steps;
}

// ── Dual-channel main readout — layout from design_handoff_oled_psu ────────────
// Numeral style: the big V/I numbers are the driver's built-in 5x7 font scaled
// x2 (drawTextScaled); labels/units/SET strip use the same 5x7 at x1. One font
// for the whole panel — no custom glyph data.
//
// Two 128px columns (CH1: CX=0, CH2: CX=128) split by a 1px centre divider.
// Per-channel element grid (local x, add CX; y is global). Baselines follow the
// handoff; the 5x7 font draws from its top row (baseline ≈ top+6). The big
// numbers are that same 5x7 scaled x2 (drawTextScaled), so their digit bottom
// sits at top+13.
//
//   Channel label "CHx"   left   x=6           baseline y=9   (5x7)
//   Power  "xx.xxW"        right  right-edge=121 baseline y=9   (5x7, dim)
//   Voltage (big)          right  right-edge=112 baseline y=32  (5x7 ×2)
//   unit "V"               right  right-edge=121 baseline y=32  (5x7, dim)
//   Current (big)          right  right-edge=112 baseline y=50  (5x7 ×2)
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

// Big-number font: the driver's 5x7 magnified x2 via drawTextScaled().
static const int BIG_SCALE = 2;              // magnification
static const int BIG_CW    = 6 * BIG_SCALE;  // char pitch  = 12 px (monospace)
static const int BIG_H     = 7 * BIG_SCALE;  // glyph height = 14 px

// Baselines / row tops.
static const int Y_HDR_TOP = 3;   // 5x7 top for baseline 9  (header row)
static const int Y_V_TOP   = 16;  // scaled-5x7 top; digit bottom on baseline 29 (voltage)
static const int Y_I_TOP   = 37;  // scaled-5x7 top; digit bottom on baseline 50 (current)
static const int Y_UNIT_V  = 23;  // 5x7 top for baseline 29 (tracks voltage)
static const int Y_UNIT_A  = 44;  // 5x7 top for baseline 50
static const int Y_SEP     = 52;  // separator line
static const int Y_SET_TOP = 56;  // 5x7 top for baseline 62 (SET strip)

// Number field cleared/redrawn each frame (holds both big numbers). Wide enough
// for the current row: monospace "2.0000" + the 4th-decimal gap starts at x=35.
static const int NUM_X = 34, NUM_W = 80, NUM_Y = Y_V_TOP, NUM_H = Y_I_TOP + BIG_H - Y_V_TOP;
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

// ── Set-value editing (CH1 voltage only until the channel/V-I select buttons
// exist) ────────────────────────────────────────────────────────────────────────
// The set voltage is edited in centivolts so digit steps are exact. Display is
// zero-padded "05.00V" so every digit has a fixed column for the cursor.
// selDigit indexes the four editable digits: 0=tens 1=ones 2=tenths 3=hundredths.
static const int32_t SETV_MAX_CV = 3000;                     // 30.00 V
static const int32_t DIGIT_STEP_CV[4] = { 1000, 100, 10, 1 };
static int32_t setV1_cV = 1250;                              // mirrors ch1.setV
static int     selDigit = 3;                                 // start on hundredths

// Set-voltage field geometry: 6 chars "05.00V" at 6 px pitch, centred on local
// x=54 like the original SET strip. Row is 8 px: 7 font rows + 1 underline row.
static const int SETV_X0 = 54 - 3 * 6;
static const int SETV_W  = 6 * 6;
static const int SETV_H  = 8;
// Char cell of each editable digit inside "05.00V" (skips the decimal point).
static const uint8_t DIGIT_POS[4] = { 0, 1, 3, 4 };

// Right-align a 5x7 string so its rightmost pixel sits near local x=rightX (+CX).
static void drawText5x7Right(int rightX, int y, const char *s, uint8_t fg) {
    display.drawText(rightX + 1 - (int)strlen(s) * 6, y, s, fg, 0);
}

// Right-align a big (scaled-x2) 5x7 number so its rightmost column sits at x=rightX.
static void drawBigScaledRight(int rightX, int y, const char *s, uint8_t fg) {
    display.drawTextScaled(rightX + 1 - (int)strlen(s) * BIG_CW, y, s, fg, 0, BIG_SCALE);
}

// Thin gap (px) inserted before the least-significant current digit so the 4th
// decimal reads slightly apart, e.g. "0.823 1". Normal inter-digit gap is 1 px.
static const int I_TAIL_GAP = 6;

// Draw a big current number right-anchored at rightX, but split off the last
// character with I_TAIL_GAP px of extra space before it. Used for the measured
// current ("%.4f") to set its 4th decimal apart from the first three.
static void drawBigCurrent(int rightX, int yTop, const char *s, uint8_t color) {
    int n = (int)strlen(s);
    if (n < 2) { drawBigScaledRight(rightX, yTop, s, color); return; }

    char head[16];
    memcpy(head, s, (size_t)(n - 1));
    head[n - 1] = '\0';
    const char *tail = s + (n - 1);        // final digit (the 4th decimal)

    int headW = (n - 1) * BIG_CW;          // monospace: 5x7 x2 = BIG_CW per char
    int total = headW + I_TAIL_GAP + BIG_CW;
    int x     = rightX + 1 - total;
    display.drawTextScaled(x, yTop, head, color, 0, BIG_SCALE);
    display.drawTextScaled(x + headW + I_TAIL_GAP, yTop, tail, color, 0, BIG_SCALE);
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

    // SET strip: "SET"  |  set V (drawn by drawSetVolt)  |  set I (right).
    display.drawText(xOff + 6, Y_SET_TOP, "SET", G_SET, 0);

    snprintf(buf, sizeof(buf), "%.3fA", ch.setI);          // e.g. "1.000A"
    drawText5x7Right(xOff + 121, Y_SET_TOP, buf, G_SET);
}

// Draw the set-voltage field "05.00V" with an optional digit cursor: the digit
// DIGIT_POS[cursor] is drawn bright with an underline; cursor=-1 draws the plain
// field (used for the non-editable channel). Framebuffer only — caller flushes.
static void drawSetVolt(int xOff, float setV, int cursor) {
    char buf[8], cs[2] = { 0, 0 };
    snprintf(buf, sizeof(buf), "%05.2fV", setV);           // e.g. "05.00V"

    display.fillRect(xOff + SETV_X0, Y_SET_TOP, SETV_W, SETV_H, 0);
    int curPos = (cursor >= 0) ? DIGIT_POS[cursor] : -1;
    for (int i = 0; buf[i]; i++) {
        int x = xOff + SETV_X0 + i * 6;
        cs[0] = buf[i];
        display.drawText(x, Y_SET_TOP, cs, i == curPos ? G_BIG : G_SET, 0);
        if (i == curPos)
            display.drawLine(x, Y_SET_TOP + 7, x + 4, Y_SET_TOP + 7, G_BIG);
    }
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
    drawBigScaledRight(xOff + 112, Y_V_TOP, buf, G_BIG);
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

// ── Settings menu ───────────────────────────────────────────────────────────────
// Long-pressing ROT_BT on the main page opens a settings overview with four
// submenus. Rotation moves the highlight, a short press enters the selected
// submenu, a long press backs out (submenu -> overview -> main page). Submenu
// values are dummies until the real configuration store exists.
enum UiPage : uint8_t { PAGE_MAIN, PAGE_SETTINGS, PAGE_SUBMENU };
static UiPage uiPage = PAGE_MAIN;

struct MenuItem { const char *name; const char *value; };

static const MenuItem CHANNEL_ITEMS[] = {
    { "OVP CH1",     "31.0 V"  },
    { "OVP CH2",     "31.0 V"  },
    { "OCP CH1",     "2.100 A" },
    { "OCP CH2",     "2.100 A" },
};
static const MenuItem FAN_ITEMS[] = {
    { "Fan mode",    "Auto"    },
    { "Min speed",   "20 %"    },
    { "Start temp",  "45 C"    },
    { "Full temp",   "70 C"    },
};
static const MenuItem NETWORK_ITEMS[] = {
    { "DHCP",        "On"             },
    { "IP address",  "192.168.1.50"   },
    { "Netmask",     "255.255.255.0"  },
    { "Hostname",    "psu-2ch"        },
};
static const MenuItem UI_ITEMS[] = {
    { "Brightness",  "12/15"   },
    { "Dim after",   "5 min"   },
    { "Encoder dir", "Normal"  },
    { "Key beep",    "Off"     },
};

struct Submenu {
    const char     *title;   // header inside the submenu
    const char     *label;   // entry text in the overview list
    const MenuItem *items;
    int             count;
};
static const Submenu SUBMENUS[] = {
    { "CHANNEL SETTINGS", "Channel Settings", CHANNEL_ITEMS, 4 },
    { "FAN SETTINGS",     "Fan Settings",     FAN_ITEMS,     4 },
    { "NETWORK SETTINGS", "Network Settings", NETWORK_ITEMS, 4 },
    { "UI SETTINGS",      "UI Settings",      UI_ITEMS,      4 },
};
static const int SUBMENU_COUNT = (int)(sizeof(SUBMENUS) / sizeof(SUBMENUS[0]));

static int settingsSel = 0;  // highlighted entry in the overview
static int submenuIdx  = 0;  // which submenu is open
static int submenuSel  = 0;  // highlighted row inside the open submenu

// Menu layout: header baseline 9 like the main page, rule under it, then up to
// four 11 px rows. Values are right-aligned near the panel's right edge.
static const int MENU_Y0    = 17;   // first item row top
static const int MENU_DY    = 11;   // row pitch
static const int MENU_VAL_X = 246;  // right edge of the value column

// Header + horizontal rule shared by the overview and every submenu.
static void drawMenuHeader(const char *title) {
    display.clear(0);
    display.drawText(6, Y_HDR_TOP, title, G_CHLBL, 0);
    display.drawLine(5, 13, 250, 13, G_LINE);
}

// One menu row: ">" marker + name (and optional right-aligned value) with the
// selected row drawn bright.
static void drawMenuRow(int row, bool sel, const char *name, const char *value) {
    int y = MENU_Y0 + row * MENU_DY;
    if (sel) display.drawText(10, y, ">", G_BIG, 0);
    display.drawText(20, y, name, sel ? G_BIG : G_NUM, 0);
    if (value) drawText5x7Right(MENU_VAL_X, y, value, sel ? G_BIG : G_DIM);
}

static void drawSettingsOverview() {
    drawMenuHeader("SETTINGS");
    for (int i = 0; i < SUBMENU_COUNT; i++)
        drawMenuRow(i, i == settingsSel, SUBMENUS[i].label, nullptr);
    display.flush();
}

static void drawSubmenu() {
    const Submenu &m = SUBMENUS[submenuIdx];
    drawMenuHeader(m.title);
    for (int i = 0; i < m.count; i++)
        drawMenuRow(i, i == submenuSel, m.items[i].name, m.items[i].value);
    display.flush();
}

// Full repaint of the dual-channel readout, used when leaving the menus.
static void drawMainPage() {
    display.clear(0);
    display.drawLine(127, 6, 127, 57, G_LINE);   // centre divider
    drawStatic(ch1,   0);
    drawStatic(ch2, 128);
    drawSetVolt(  0, ch1.setV, selDigit);
    drawSetVolt(128, ch2.setV, -1);
    drawDynamic(ch1,   0);
    drawDynamic(ch2, 128);
    display.flush();
}

// Wrap sel + steps into [0, count).
static int wrapSel(int sel, int steps, int count) {
    sel = (sel + steps) % count;
    return sel < 0 ? sel + count : sel;
}

void setup() {
    Serial.begin(115200);

    encoderInit();

    display.begin();

    selfTest();

    ch1.setV = setV1_cV / 100.0f;
    drawMainPage();
}

// ── Encoder input dispatch ──────────────────────────────────────────────────────
// Short press: main page = advance the digit cursor; overview = enter the
// selected submenu; submenu = nothing yet (values are dummies).
static void onShortPress() {
    switch (uiPage) {
    case PAGE_MAIN:
        selDigit = (selDigit + 1) & 3;
        drawSetVolt(0, ch1.setV, selDigit);
        display.flushRect(SETV_X0, Y_SET_TOP, SETV_W, SETV_H);
        break;
    case PAGE_SETTINGS:
        submenuIdx = settingsSel;
        submenuSel = 0;
        uiPage     = PAGE_SUBMENU;
        drawSubmenu();
        break;
    case PAGE_SUBMENU:
        break;  // dummy values — nothing to edit yet
    }
}

// Long press: main page = open settings; menus = go back one level.
static void onLongPress() {
    switch (uiPage) {
    case PAGE_MAIN:
        settingsSel = 0;
        uiPage      = PAGE_SETTINGS;
        drawSettingsOverview();
        break;
    case PAGE_SETTINGS:
        uiPage = PAGE_MAIN;
        drawMainPage();
        break;
    case PAGE_SUBMENU:
        uiPage = PAGE_SETTINGS;
        drawSettingsOverview();
        break;
    }
}

// Rotation: main page = step the selected digit of the CH1 set voltage
// (clamped to 0..30.00 V); menus = move the highlight (wraps around).
static void onRotate(int steps) {
    switch (uiPage) {
    case PAGE_MAIN:
        setV1_cV += (int32_t)steps * DIGIT_STEP_CV[selDigit];
        if (setV1_cV < 0)           setV1_cV = 0;
        if (setV1_cV > SETV_MAX_CV) setV1_cV = SETV_MAX_CV;
        ch1.setV = setV1_cV / 100.0f;
        drawSetVolt(0, ch1.setV, selDigit);
        display.flushRect(SETV_X0, Y_SET_TOP, SETV_W, SETV_H);
        break;
    case PAGE_SETTINGS:
        settingsSel = wrapSel(settingsSel, steps, SUBMENU_COUNT);
        drawSettingsOverview();
        break;
    case PAGE_SUBMENU:
        submenuSel = wrapSel(submenuSel, steps, SUBMENUS[submenuIdx].count);
        drawSubmenu();
        break;
    }
}

// Poll the encoder button and drain the encoder step counter, dispatching to
// the handlers above. The button (active-low) is edge-detected with a debounce
// lockout; holding it past LONGPRESS_MS fires the long-press action once, and
// the following release is then swallowed so it doesn't also short-press.
static const uint32_t LONGPRESS_MS = 600;

static void serviceEncoder() {
    static bool     btPrev    = false;
    static uint32_t btEdgeMs  = 0;
    static uint32_t btDownMs  = 0;
    static bool     longFired = false;

    uint32_t now = millis();
    bool bt = !digitalRead(PIN_ENC_BT);            // true while pressed
    if (bt != btPrev && now - btEdgeMs > 30) {
        btEdgeMs = now;
        btPrev   = bt;
        if (bt) {                                  // press edge: arm the timer
            btDownMs  = now;
            longFired = false;
        } else if (!longFired) {                   // release before threshold
            onShortPress();
        }
    }
    if (btPrev && !longFired && now - btDownMs >= LONGPRESS_MS) {
        longFired = true;                          // fires while still held
        onLongPress();
    }

    int steps = encoderReadSteps();
    if (steps != 0)
        onRotate(steps);
}

void loop() {
    serviceEncoder();

    // Demo sweep until real ADC data is wired in: CH1 in CV, CH2 clamped at CC.
    const uint32_t PERIOD = 5000;
    uint32_t now = millis();
    float t1 = (float)(now % PERIOD) / (float)PERIOD;
    float t2 = (float)((now + PERIOD / 2) % PERIOD) / (float)PERIOD;

    ch1.measV = t1 * 30.0f;
    ch1.measI = t1 *  1.0f;
    ch2.measV = t2 * 30.0f;
    ch2.measI = t2 *  1.0f;

    // Only repaint the readout while it is on screen; the menus own the panel
    // otherwise (measurements keep updating above so the values stay current).
    if (uiPage == PAGE_MAIN) {
        uint32_t t0 = micros();
        drawDynamic(ch1,   0);
        drawDynamic(ch2, 128);
        flushDynamic(0);
        flushDynamic(128);
        uint32_t dt = micros() - t0;

        Serial.print("update: ");
        Serial.print(dt);
        Serial.println(" us");
    }

    delay(50);  // ~20 fps; reduce or remove once timing is confirmed
}
