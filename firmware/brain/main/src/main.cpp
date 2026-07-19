#include <Arduino.h>
#include <EEPROM.h>
#include <SSD1322.h>
#include <Wire.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "channel_link.h"   // UART link to the two ATtiny1614 channel boards
#include "protocol.h"       // ST_* / FLAG_* telemetry decode constants

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

// ── Rotary encoder quadrature (direct wiring). The ROT signals are active-low,
// so the inputs use pull-ups; a closed contact pulls the pin down. GPIO17 is just
// held high for the current hookup. The encoder PUSH button no longer lives here —
// it moved to the MCP23008 I/O expander (GP0), read over I2C below.
//   ROT_A  -> GPIO14   quadrature A   (PIO in-base; A/B must be consecutive)
//   ROT_B  -> GPIO15   quadrature B
//   GPIO17 -> driven high
#define PIN_ENC_A   14
#define PIN_ENC_B   15
#define PIN_ENC_PWR 17

// ── Front-panel buttons + buzzer on an MCP23008 I/O expander (I2C1) ────────────
// The RP2350 exposes I2C1 SDA/SCL on GPIO6/GPIO7 (I2C0 cannot reach those pins),
// so the expander lives on the Wire1 peripheral. Its INT pin flags any button
// change on GPIO8; we read GPIO to identify the change and clear the interrupt.
//   SDA -> GPIO6   (I2C1 SDA)
//   SCL -> GPIO7   (I2C1 SCL)
//   INT -> GPIO8   (active-low, open-drain w/ external pull-up; low on change)
#define PIN_MCP_SDA 6
#define PIN_MCP_SCL 7
#define PIN_MCP_INT 8

// A2:A1:A0 strapped low → 7-bit slave address 0x20.
static const uint8_t MCP_ADDR = 0x20;

// MCP23008 register addresses (Table 1-2).
#define MCP_IODIR   0x00
#define MCP_IPOL    0x01
#define MCP_GPINTEN 0x02
#define MCP_DEFVAL  0x03
#define MCP_INTCON  0x04
#define MCP_IOCON   0x05
#define MCP_GPPU    0x06
#define MCP_INTF    0x07
#define MCP_INTCAP  0x08
#define MCP_GPIO    0x09
#define MCP_OLAT    0x0A

// Expander pin map (bit position = GPn). All buttons are active-low with pull-ups;
// GP4 is the active-high buzzer output. Bits are named by front-panel function.
#define MCP_ENC_BT (1 << 0)   // GP0  encoder push button (digit select / menu)
#define MCP_CH2_I  (1 << 1)   // GP1  select CH2 current setpoint for editing
#define MCP_CH2_ON (1 << 2)   // GP2  CH2 output toggle
#define MCP_CH2_V  (1 << 3)   // GP3  select CH2 voltage setpoint for editing
#define MCP_BUZZER (1 << 4)   // GP4  buzzer (active-high output)
#define MCP_CH1_I  (1 << 5)   // GP5  select CH1 current setpoint for editing
#define MCP_CH1_V  (1 << 6)   // GP6  select CH1 voltage setpoint for editing
#define MCP_CH1_ON (1 << 7)   // GP7  CH1 output toggle

// All pins except the buzzer (GP4) are button inputs.
static const uint8_t MCP_BTN_MASK = (uint8_t)~MCP_BUZZER;   // 0xEF

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
static const int ENC_SIGN   = 1;  // flip sign if rotation goes the wrong way

static void encoderInit() {
    // Pads: inputs with pull-ups (signals are active-low). The PIO IN path
    // sees the pad regardless of function select, so plain pinMode() is enough.
    pinMode(PIN_ENC_PWR, OUTPUT);
    digitalWrite(PIN_ENC_PWR, HIGH);
    pinMode(PIN_ENC_A,  INPUT_PULLUP);
    pinMode(PIN_ENC_B,  INPUT_PULLUP);

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

// ── MCP23008 I/O expander (front-panel buttons + buzzer) ───────────────────────
// Single-register reads/writes over I2C1. The expander is configured for
// interrupt-on-change against the previous pin state (INTCON = 0), so any button
// edge pulses INT; serviceMcpButtons() reads GPIO to see which pins changed and to
// clear the interrupt, then edge-detects presses in software with a debounce.

static uint8_t mcpOlat      = 0;   // shadow of the output latch (only GP4 matters)
static uint8_t g_btnPressed = 0;   // debounced pressed bitmask (bit set = pressed)

static void mcpWrite(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(MCP_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static uint8_t mcpRead(uint8_t reg) {
    Wire1.beginTransmission(MCP_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);                 // repeated start (no bus release)
    Wire1.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1);
    return Wire1.available() ? (uint8_t)Wire1.read() : 0xFF;
}

// Drive the buzzer (GP4). Kept as a shadow write so the input pins are untouched.
static void mcpSetBuzzer(bool on) {
    if (on) mcpOlat |= MCP_BUZZER;
    else    mcpOlat &= (uint8_t)~MCP_BUZZER;
    mcpWrite(MCP_OLAT, mcpOlat);
}

static void mcpInit() {
    Wire1.setSDA(PIN_MCP_SDA);
    Wire1.setSCL(PIN_MCP_SCL);
    Wire1.begin();
    Wire1.setClock(400000);                       // 400 kHz (MCP23008 fast mode)

    mcpWrite(MCP_IODIR,   MCP_BTN_MASK);          // buttons in, GP4 (buzzer) out
    mcpWrite(MCP_GPPU,    MCP_BTN_MASK);          // pull-ups on the button inputs
    mcpWrite(MCP_IPOL,    0x00);                  // no inversion; low = pressed
    mcpWrite(MCP_INTCON,  0x00);                  // compare against previous value
    mcpWrite(MCP_DEFVAL,  0x00);
    mcpWrite(MCP_IOCON,   0x04);                  // INT open-drain (ODR=1); ext. pull-up
    mcpWrite(MCP_GPINTEN, MCP_BTN_MASK);          // interrupt-on-change for buttons
    mcpOlat = 0x00;
    mcpWrite(MCP_OLAT,    mcpOlat);               // buzzer off

    pinMode(PIN_MCP_INT, INPUT);                  // hardware pull-up holds INT high
    (void)mcpRead(MCP_GPIO);                      // clear any power-on interrupt

    uint8_t back = mcpRead(MCP_IODIR);            // bring-up sanity check
    Serial.print("MCP23008 IODIR readback: 0x");
    Serial.println(back, HEX);
    if (back != MCP_BTN_MASK)
        Serial.println("MCP23008 not responding as expected!");
}

// ── Buzzer (non-blocking) ──────────────────────────────────────────────────────
// beep() arms a timed tone; serviceBuzzer() turns it off when the deadline passes.
static uint32_t buzzerOffMs = 0;
static bool     beeperEnabled = true;   // gated by the UI "Beeper" setting

static void beep(uint16_t ms) {
    if (!beeperEnabled) return;
    mcpSetBuzzer(true);
    buzzerOffMs = millis() + ms;
}

static void serviceBuzzer() {
    if (buzzerOffMs && (int32_t)(millis() - buzzerOffMs) >= 0) {
        mcpSetBuzzer(false);
        buzzerOffMs = 0;
    }
}

// ── Heatsink NTC thermistor (brain ADC) ────────────────────────────────────────
// A 10k B3435 NTC on GPIO29 (ADC3) is the low leg of a divider with a 10k pull-up
// to 3V3; the same 3V3 rail is the ADC reference, so the reading is ratiometric and
// the exact rail voltage cancels. Node voltage falls as the heatsink heats up.
//   3V3 ── 10k ──┬── GPIO29 (ADC3)
//                └── NTC(10k @25C) ── GND
#define PIN_NTC 29
static const float NTC_PULLUP_OHM = 10000.0f;   // top resistor to 3V3
static const float NTC_R25_OHM    = 10000.0f;   // NTC nominal resistance at 25 C
static const float NTC_BETA       = 3435.0f;    // B25/85 constant (B3435)
static const float NTC_T0_K       = 298.15f;    // 25 C in kelvin
static const int   NTC_ADC_BITS   = 12;         // analogReadResolution() set in setup()
static const int   NTC_OVERSAMPLE = 16;         // raw samples averaged per read

// Read the heatsink NTC and convert to degrees C via the beta model. Returns NAN
// when the divider sits at either rail (open or shorted thermistor / bad wiring).
static float readHeatsinkTempC() {
    uint32_t acc = 0;
    for (int i = 0; i < NTC_OVERSAMPLE; i++) acc += (uint32_t)analogRead(PIN_NTC);
    const int   maxCount = (1 << NTC_ADC_BITS) - 1;
    const float ratio    = (float)acc / ((float)NTC_OVERSAMPLE * (float)maxCount);
    if (ratio <= 0.01f || ratio >= 0.99f) return NAN;   // rail: open/short, no valid temp

    // Divider: Vnode = Vcc * Rntc/(Rntc + Rpull)  ->  Rntc = Rpull * ratio/(1 - ratio).
    const float rNtc = NTC_PULLUP_OHM * ratio / (1.0f - ratio);
    // Beta equation: 1/T = 1/T0 + ln(Rntc/R25)/B.
    const float tK = 1.0f / (1.0f / NTC_T0_K + logf(rNtc / NTC_R25_OHM) / NTC_BETA);
    return tK - 273.15f;
}

// ── Chassis fan (12V 4-wire PWM) ───────────────────────────────────────────────
// A PC fan's PWM control input is on GPIO26. Speed follows the hottest measured
// temperature (Ch1 / Ch2 / heatsink) using the curve configured in the Temp and Fan
// submenu: off below "Fan start temp", jumping to "Fan min speed" there and ramping
// linearly to 100% at "Fan max temp". Standard 4-wire fan PWM runs at 25 kHz.
#define PIN_FAN 26
static const uint32_t FAN_PWM_HZ    = 25000;   // Intel 4-wire spec target (21-28 kHz)
static const uint16_t FAN_PWM_RANGE = 1000;    // analogWrite() full-scale (0.1% steps)
static const float    FAN_HYST_C    = 3.0f;    // switch-on hysteresis (anti-chatter)
static const uint32_t FAN_UPDATE_MS = 500;     // control-loop period

// Thermal setpoints, also shown (and edited) in the Temp and Fan submenu. Single
// source of truth: resolveMenuValue() renders these, the thermal service acts on them.
static uint8_t g_fanMinPct = 20;   // fan duty when it first switches on
static int     g_fanStartC = 45;   // fan switch-on temperature
static int     g_fanMaxC   = 70;   // temperature at which the fan reaches 100%
static int     g_sysOtpC   = 85;   // system over-temp trip (Temp max >= this -> outputs off)

static const float SYS_OTP_HYST_C = 5.0f;   // clear the trip this far below the setpoint

// Shared thermal state, updated by serviceThermal(): the hottest measured
// temperature (Ch1 / Ch2 / heatsink) and the latched over-temperature trip.
static float g_tempMaxC   = NAN;   // NAN until the first sample / when no sensor is valid
static bool  g_otpTripped = false; // true while the OTP latch holds the outputs off

// ── Persistent thermal setpoints (flash-backed EEPROM emulation) ───────────────
// The four Temp-and-Fan setpoints survive reboots. Layout mirrors the channel's
// storage: magic + version gate the blob and a CRC16 guards it. Saved when a
// Temp-and-Fan edit is committed; loaded once at boot (defaults stand if the blob is
// absent or corrupt).
#define BRAIN_EEPROM_SIZE     256
#define THERMAL_STORE_ADDR    0
#define THERMAL_STORE_MAGIC   0x54484D31u   /* 'THM1' */
#define THERMAL_STORE_VERSION 1

struct ThermalStore {
    uint32_t magic;
    uint16_t version;
    uint8_t  fanMinPct;
    uint8_t  fanStartC;
    uint8_t  fanMaxC;
    uint8_t  sysOtpC;
    uint16_t crc;
};

// CRC16-CCITT (poly 0x1021, init 0xFFFF), same routine as the channel storage.
static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

// Persist the current thermal setpoints to flash. Called after a committed edit.
static void thermalSettingsSave() {
    ThermalStore s;
    s.magic     = THERMAL_STORE_MAGIC;
    s.version   = THERMAL_STORE_VERSION;
    s.fanMinPct = g_fanMinPct;
    s.fanStartC = (uint8_t)g_fanStartC;
    s.fanMaxC   = (uint8_t)g_fanMaxC;
    s.sysOtpC   = (uint8_t)g_sysOtpC;
    s.crc       = crc16((const uint8_t *)&s, sizeof(s) - sizeof(s.crc));
    EEPROM.put(THERMAL_STORE_ADDR, s);
    EEPROM.commit();   // RP2350 EEPROM emulation is a RAM shadow; commit flushes it
}

// Load persisted setpoints at boot, clamped to the editor ranges. Leaves the
// compiled-in defaults in place if the blob is absent or fails validation.
static void thermalSettingsLoad() {
    ThermalStore s;
    EEPROM.get(THERMAL_STORE_ADDR, s);
    if (s.magic != THERMAL_STORE_MAGIC || s.version != THERMAL_STORE_VERSION) return;
    if (s.crc != crc16((const uint8_t *)&s, sizeof(s) - sizeof(s.crc)))        return;
    g_fanMinPct = s.fanMinPct > 100 ? 100 : s.fanMinPct;
    g_fanStartC = s.fanStartC > 100 ? 100 : s.fanStartC;
    g_fanMaxC   = s.fanMaxC   > 100 ? 100 : s.fanMaxC;
    g_sysOtpC   = s.sysOtpC   < 40 ? 40 : (s.sysOtpC > 120 ? 120 : s.sysOtpC);
}

// ── Brain operating-hours meter (flash-backed EEPROM emulation) ────────────────
// The brain's own hour-meter: accumulated powered-on seconds, persisted so the
// "Brain runtime" system row survives reboots. Its own blob (magic/version/CRC)
// in a separate EEPROM slot from the thermal store. RP2350 EEPROM.commit() flushes
// the whole 256 B shadow to one flash sector, so we save on a slow cadence to
// bound sector-erase wear; up to BRAIN_RUNTIME_SAVE_S of the current session is
// lost on an unexpected power-off, which never changes the whole-hours readout.
#define BRAIN_RUNTIME_STORE_ADDR    32
#define BRAIN_RUNTIME_STORE_MAGIC   0x42525431u   /* 'BRT1' */
#define BRAIN_RUNTIME_STORE_VERSION 1
#define BRAIN_RUNTIME_SAVE_S        600u          /* flush every 10 min of runtime */

struct BrainRuntimeStore {
    uint32_t magic;
    uint16_t version;
    uint32_t seconds;
    uint16_t crc;
};

static uint32_t g_brainRuntimeS = 0;   // total accumulated powered-on seconds

static void brainRuntimeSave() {
    BrainRuntimeStore s;
    s.magic   = BRAIN_RUNTIME_STORE_MAGIC;
    s.version = BRAIN_RUNTIME_STORE_VERSION;
    s.seconds = g_brainRuntimeS;
    s.crc     = crc16((const uint8_t *)&s, sizeof(s) - sizeof(s.crc));
    EEPROM.put(BRAIN_RUNTIME_STORE_ADDR, s);
    EEPROM.commit();
}

static void brainRuntimeLoad() {
    BrainRuntimeStore s;
    EEPROM.get(BRAIN_RUNTIME_STORE_ADDR, s);
    if (s.magic != BRAIN_RUNTIME_STORE_MAGIC || s.version != BRAIN_RUNTIME_STORE_VERSION) {
        brainRuntimeSave();   // seed a zeroed, valid blob
        return;
    }
    if (s.crc != crc16((const uint8_t *)&s, sizeof(s) - sizeof(s.crc))) return;
    g_brainRuntimeS = s.seconds;
}

// Advance the brain hour-meter from elapsed millis() and flush to flash once a
// full BRAIN_RUNTIME_SAVE_S has accumulated since the last save. Called every loop.
static void serviceBrainRuntime() {
    static uint32_t lastMs   = 0;
    static uint32_t fracMs   = 0;
    static uint32_t savedS   = 0;
    static bool     inited   = false;
    if (!inited) { lastMs = millis(); savedS = g_brainRuntimeS; inited = true; }

    uint32_t now = millis();
    fracMs += (uint32_t)(now - lastMs);
    lastMs  = now;
    if (fracMs >= 1000u) { g_brainRuntimeS += fracMs / 1000u; fracMs %= 1000u; }

    if (g_brainRuntimeS - savedS >= BRAIN_RUNTIME_SAVE_S) {
        brainRuntimeSave();
        savedS = g_brainRuntimeS;
    }
}

static void fanInit() {
    analogWriteFreq(FAN_PWM_HZ);
    analogWriteRange(FAN_PWM_RANGE);
    analogWrite(PIN_FAN, FAN_PWM_RANGE);   // fail-safe: full speed until the first temp read
}

// Hottest currently-valid temperature (Ch1 / Ch2 telemetry + heatsink NTC), or NAN
// if none is available.
static float governingTempC() {
    float best = NAN;
    const ChannelStatus &s1 = channel_status(0);
    const ChannelStatus &s2 = channel_status(1);
    if (s1.linkUp) { float t = s1.temp_cC / 100.0f; if (isnan(best) || t > best) best = t; }
    if (s2.linkUp) { float t = s2.temp_cC / 100.0f; if (isnan(best) || t > best) best = t; }
    float ths = readHeatsinkTempC();
    if (!isnan(ths) && (isnan(best) || ths > best)) best = ths;
    return best;
}

// Fan duty (%) on the ramp for temperature t, ignoring the on/off latch: min% at or
// below the start temp, 100% at or above the max temp, linear in between.
static uint8_t fanRampPct(float t) {
    if (g_fanMaxC <= g_fanStartC) return 100;   // guard a degenerate/inverted curve
    if (t <= g_fanStartC) return g_fanMinPct;
    if (t >= g_fanMaxC)   return 100;
    float frac = (t - (float)g_fanStartC) / (float)(g_fanMaxC - g_fanStartC);
    int pct = (int)g_fanMinPct + (int)(frac * (100 - (int)g_fanMinPct) + 0.5f);
    return (uint8_t)(pct > 100 ? 100 : pct);
}

// Apply the fan curve for governing temperature t (no timer of its own). The fan
// latches on at "Fan start temp" and off only after dropping FAN_HYST_C below it, so
// it doesn't chatter around the threshold. Unknown temperature -> full (fail-safe).
static void fanApply(float t) {
    static bool fanOn    = true;   // fail-safe until the first evaluation
    static int  lastDuty = -1;
    uint8_t pct;
    if (isnan(t)) {
        pct   = 100;                   // no valid sensor: full speed
        fanOn = true;
    } else {
        if (fanOn) { if (t < (float)g_fanStartC - FAN_HYST_C) fanOn = false; }
        else       { if (t >= (float)g_fanStartC)             fanOn = true;  }
        pct = fanOn ? fanRampPct(t) : 0;
    }

    int duty = (int)pct * FAN_PWM_RANGE / 100;
    if (duty != lastDuty) {
        analogWrite(PIN_FAN, duty);
        lastDuty = duty;
    }
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
// Channel-label cell (redrawn each frame so its colour can track link/output state).
static const int HDR_LBL_X = 6, HDR_LBL_W = 18, HDR_LBL_H = 8;

struct Channel {
    float measV;
    float measI;
    float setV;
    float setI;
};

// ── Set-value editing (per-channel voltage + current) ──────────────────────────
// Setpoints are held as integers so digit steps stay exact: voltage in centivolts
// (0..3600 = 36.00 V), current in milliamps (0..2000 = 2.000 A). The front-panel
// CHx_V / CHx_I buttons pick which of the four setpoints the encoder edits; the
// encoder push selects the digit and rotation steps it. The digit index (0..3)
// selects one of the four editable digits — voltage: tens/ones/tenths/hundredths;
// current: ones and the three decimals — and is remembered per field (see
// selDigitFor). Displays are zero-padded so each digit has a fixed cursor column.
static const int32_t SETV_MAX_CV = 3600;                     // 36.00 V
static const int32_t SETI_MAX_MA = 2000;                     // 2.000 A
static const int32_t V_DIGIT_STEP_CV[4] = { 1000, 100, 10, 1 };
static const int32_t I_DIGIT_STEP_MA[4] = { 1000, 100, 10, 1 };

static int32_t setV_cV[2] = { 1250, 500 };                   // CH1 12.50 V, CH2 5.00 V
static int32_t setI_mA[2] = { 1000, 2000 };                  // CH1 1.000 A, CH2 2.000 A

// Desired output state per channel (toggled by the CHx_ON buttons, mirrored to the
// channel over the link). The channel's *actual* state comes back in telemetry.
static bool outDesired[2] = { false, false };

enum EditParam : uint8_t { EDIT_V, EDIT_I };
static int       editCh    = 0;                              // 0 = CH1, 1 = CH2
static EditParam editParam = EDIT_V;                         // V or I setpoint

// Active digit (0..3), remembered independently for each CH / V-or-I target so
// switching between fields restores the digit that was last edited there.
// Indexed [channel][EditParam]. Defaults: voltage on the ones digit (index 1,
// just before the decimal); current on the tenths digit (index 1, just after
// the decimal).
static int selDigitFor[2][2] = { { 1, 1 }, { 1, 1 } };
static inline int &selDigit() { return selDigitFor[editCh][editParam]; }

// UI page state (the settings menu handlers live further down; declared here so
// the set-strip drawing knows to show the digit cursor only on the main page).
enum UiPage : uint8_t { PAGE_MAIN, PAGE_SETTINGS, PAGE_SUBMENU, PAGE_CAL };
static UiPage uiPage = PAGE_MAIN;

// ── Channel link push helpers ───────────────────────────────────────────────────
// Setpoints are edited in centivolts / milliamps; the protocol carries millivolts
// / milliamps, so voltage scales x10 on the way out.
static void pushSetVoltage(int ch) { channel_set_voltage((uint8_t)ch, setV_cV[ch] * 10); }
static void pushSetCurrent(int ch) { channel_set_current((uint8_t)ch, setI_mA[ch]); }

// Set-field geometry: 6 chars ("05.00V" / "2.000A") at 6 px pitch, 8 px row (7 font
// rows + 1 underline). The voltage field is centred on local x=54 as before; the
// current field is right-anchored to x=121 like the original SET strip.
static const int SETV_X0 = 54 - 3 * 6;    // 36
static const int SETV_W  = 6 * 6;         // 36
static const int SETI_X0 = 121 + 1 - 6 * 6;   // 86 (right edge ≈ 121)
static const int SETI_W  = 6 * 6;         // 36
static const int SETV_H  = 8;
// Char cell of each editable digit (skips the '.' and the trailing unit letter).
static const uint8_t DIGIT_POS_V[4] = { 0, 1, 3, 4 };   // "05.00V"
static const uint8_t DIGIT_POS_I[4] = { 0, 2, 3, 4 };   // "2.000A"

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
// (The "CHx" label is drawn here for the first paint but is refreshed every frame
// by drawChannelHeader() so its colour can track link/output state.)
void drawStatic(const Channel &ch, int xOff) {
    (void)ch;

    // Channel label (top-left).
    display.drawText(xOff + HDR_LBL_X, Y_HDR_TOP, xOff == 0 ? "CH1" : "CH2", G_CHLBL, 0);

    // Unit letters beside the big numbers (right-aligned to x=121, dim).
    display.drawText(xOff + 116, Y_UNIT_V, "V", G_DIM, 0);
    display.drawText(xOff + 116, Y_UNIT_A, "A", G_DIM, 0);

    // Separator above the SET strip.
    display.drawLine(xOff + 5, Y_SEP, xOff + 121, Y_SEP, G_LINE);

    // SET strip label; the V and I fields are drawn by drawSetStrip().
    display.drawText(xOff + 6, Y_SET_TOP, "SET", G_SET, 0);
}

// Refresh the "CHx" label with a colour that encodes link + output state:
//   no link  -> very dim (greyed out)   output on -> bright   output off -> normal
// Framebuffer only; the main-page redraw flushes the header cell.
void drawChannelHeader(int ch) {
    int xOff = ch * 128;
    const ChannelStatus &s = channel_status((uint8_t)ch);
    uint8_t col;
    if (!s.linkUp)                         col = G_LINE;    // no telemetry: greyed
    else if (s.flags & FLAG_OUTPUT_ON)     col = G_BIG;     // output live: bright
    else                                   col = G_CHLBL;   // idle: normal

    display.fillRect(xOff + HDR_LBL_X, Y_HDR_TOP, HDR_LBL_W, HDR_LBL_H, 0);
    display.drawText(xOff + HDR_LBL_X, Y_HDR_TOP, ch == 0 ? "CH1" : "CH2", col, 0);
}

// Draw one editable set field ("05.00V" or "2.000A") at local x0, clearing the
// cell first. When cursor >= 0 the digit at digitPos[cursor] is drawn bright with
// an underline; cursor = -1 draws the plain field. Framebuffer only — caller flushes.
static void drawSetField(int xOff, int x0, const char *buf,
                         const uint8_t *digitPos, int cursor) {
    char cs[2] = { 0, 0 };
    display.fillRect(xOff + x0, Y_SET_TOP, SETV_W, SETV_H, 0);
    int curPos = (cursor >= 0) ? digitPos[cursor] : -1;
    for (int i = 0; buf[i]; i++) {
        int x = xOff + x0 + i * 6;
        cs[0] = buf[i];
        display.drawText(x, Y_SET_TOP, cs, i == curPos ? G_BIG : G_SET, 0);
        if (i == curPos)
            display.drawLine(x, Y_SET_TOP + 7, x + 4, Y_SET_TOP + 7, G_BIG);
    }
}

// Draw the V and I set fields for channel `ch` (0/1). The digit cursor is shown on
// whichever field is the active edit target, and only while the main page is up.
static void drawSetStrip(int ch) {
    int xOff = ch * 128;
    char buf[8];
    bool active = (uiPage == PAGE_MAIN);
    int vCur = (active && editCh == ch && editParam == EDIT_V) ? selDigitFor[ch][EDIT_V] : -1;
    int iCur = (active && editCh == ch && editParam == EDIT_I) ? selDigitFor[ch][EDIT_I] : -1;

    snprintf(buf, sizeof(buf), "%05.2fV", setV_cV[ch] / 100.0f);   // "05.00V"
    drawSetField(xOff, SETV_X0, buf, DIGIT_POS_V, vCur);
    snprintf(buf, sizeof(buf), "%.3fA", setI_mA[ch] / 1000.0f);    // "2.000A"
    drawSetField(xOff, SETI_X0, buf, DIGIT_POS_I, iCur);
}

// Push both set fields of one channel to the panel.
static void flushSetStrip(int ch) {
    int xOff = ch * 128;
    display.flushRect(xOff + SETV_X0, Y_SET_TOP,
                      (SETI_X0 + SETI_W) - SETV_X0, SETV_H);
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

// Push both dynamic regions of one channel to the panel (header label + power + numbers).
void flushDynamic(int xOff) {
    display.flushRect(xOff + HDR_LBL_X, Y_HDR_TOP, HDR_LBL_W, HDR_LBL_H);
    display.flushRect(xOff + PWR_X, PWR_Y, PWR_W, PWR_H);
    display.flushRect(xOff + NUM_X, NUM_Y, NUM_W, NUM_H);
}

Channel ch1 = { 0, 0, 12.50f, 1.000f };
Channel ch2 = { 0, 0,  5.00f, 2.000f };

// Pull the latest telemetry for both channels into the display Channel structs.
// A down link shows zeros (the greyed "CHx" header already flags the loss).
static void updateMeasFromTelemetry() {
    const ChannelStatus &s1 = channel_status(0);
    const ChannelStatus &s2 = channel_status(1);
    ch1.measV = s1.linkUp ? s1.v_mV / 1000.0f : 0.0f;
    ch1.measI = s1.linkUp ? s1.i_dmA / 10000.0f : 0.0f;
    ch2.measV = s2.linkUp ? s2.v_mV / 1000.0f : 0.0f;
    ch2.measI = s2.linkUp ? s2.i_dmA / 10000.0f : 0.0f;
}

// ── Settings menu ───────────────────────────────────────────────────────────────
// Long-pressing the encoder button on the main page opens a settings overview with
// several submenus. Rotation moves the highlight, a short press enters the selected
// submenu, a long press backs out (submenu -> overview -> main page). Submenu
// values marked below as "live" are filled from channel telemetry by
// resolveMenuValue(); the rest are still placeholders until their stores exist.
struct MenuItem { const char *name; const char *value; };

// Per-channel settings. Channel 2 reuses the same list (the layout is identical;
// only the underlying data source differs — see resolveMenuValue()).
// Row indices are referenced by resolveMenuValue(), keep them in sync.
enum {
    CHITEM_TEMP = 0, CHITEM_VOLT, CHITEM_CURR, CHITEM_AVGV, CHITEM_AVGI,
    CHITEM_OTP, CHITEM_DAC, CHITEM_ADC, CHITEM_COMM, CHITEM_RUNTIME,
    CHITEM_MANV, CHITEM_MANI
};
static const MenuItem CHANNEL_ITEMS[] = {
    { "Temp",         "-- C"     },   // live: measured
    { "Volt",         "-- V"     },   // live: measured
    { "Curr",         "-- A"     },   // live: measured
    { "Avg V",        "--"       },   // editable: V measurement averaging (1..32)
    { "Avg I",        "--"       },   // editable: I measurement averaging (1..32)
    { "OTP",          "--"       },   // editable: per-channel over-temp trip (deg C)
    { "DAC state",    "--"       },   // live: telemetry flag
    { "ADC state",    "--"       },   // live: telemetry flag
    { "Comm",         "--"       },   // live: link up/down
    { "Runtime",      "0 h"      },   // live: channel operating-hours (s.runtimeS)
    { "Manual Cal V", "Start"    },   // enter cal wizard (confirmed w/ CHx V button)
    { "Manual Cal I", "Start"    },
};
enum { FANITEM_T1 = 0, FANITEM_T2, FANITEM_THS, FANITEM_TMAX,
       FANITEM_OTP, FANITEM_FANMIN, FANITEM_FANSTART, FANITEM_FANMAX };
static const MenuItem FAN_ITEMS[] = {
    { "Temp Ch1",     "-- C"  },   // live: measured
    { "Temp Ch2",     "-- C"  },   // live: measured
    { "Temp heatsink","-- C"  },   // live: brain NTC on GPIO29 (readHeatsinkTempC)
    { "Temp max",     "-- C"  },   // live: max of Ch1/Ch2
    { "SYS OTP",      "85 C"  },   // brain OTP setpoint vs. max temp
    { "Fan min speed","20 %"  },   // live: g_fanMinPct (drives serviceFan)
    { "Fan start temp","45 C" },   // live: g_fanStartC
    { "Fan max temp", "70 C"  },   // live: g_fanMaxC
};
static const MenuItem UI_ITEMS[] = {
    { "Beeper",      "On"      },
};
static const MenuItem NETWORK_ITEMS[] = {
    { "Status",      "Up"             },   // link + IP (read only)
    { "MAC address", "A8:61:0A:.."    },   // read only
    { "DHCP",        "On"             },   // when On, IP/mask/gw are read only
    { "IP address",  "192.168.1.50"   },
    { "Netmask",     "255.255.255.0"  },   // CIDR /24
    { "Gateway",     "192.168.1.1"    },
    { "Hostname",    "psu-2ch"        },
    { "Apply",       ""               },   // applies + restarts the interface
};
enum { SYSITEM_FWVER = 0, SYSITEM_REMEMBER, SYSITEM_RUNTIME };
static const MenuItem SYSTEM_ITEMS[] = {
    { "FW version",   "0.1.0" },
    { "Remember set", "Yes"   },   // remember set values on restart
    { "Brain runtime","0 h"   },   // live: g_brainRuntimeS (see resolveMenuValue)
};

struct Submenu {
    const char     *title;   // header inside the submenu
    const char     *label;   // entry text in the overview list
    const MenuItem *items;
    int             count;
};
#define ITEM_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
// Submenu indices used by resolveMenuValue().
enum { SUB_CH1 = 0, SUB_CH2, SUB_FAN, SUB_UI, SUB_NET, SUB_SYS };
static const Submenu SUBMENUS[] = {
    { "CHANNEL 1",    "Channel 1",    CHANNEL_ITEMS, ITEM_COUNT(CHANNEL_ITEMS) },
    { "CHANNEL 2",    "Channel 2",    CHANNEL_ITEMS, ITEM_COUNT(CHANNEL_ITEMS) },
    { "TEMP AND FAN", "Temp and Fan", FAN_ITEMS,     ITEM_COUNT(FAN_ITEMS)     },
    { "UI",           "UI",           UI_ITEMS,      ITEM_COUNT(UI_ITEMS)      },
    { "NETWORK",      "Network",      NETWORK_ITEMS, ITEM_COUNT(NETWORK_ITEMS) },
    { "SYSTEM",       "System",       SYSTEM_ITEMS,  ITEM_COUNT(SYSTEM_ITEMS)  },
};
static const int SUBMENU_COUNT = (int)(sizeof(SUBMENUS) / sizeof(SUBMENUS[0]));

static int settingsSel = 0;  // highlighted entry in the overview
static int submenuIdx  = 0;  // which submenu is open
static int submenuSel  = 0;  // highlighted row inside the open submenu

// Inline editing of a numeric submenu row (Avg V/I on the channels; the fan curve +
// SYS OTP on Temp and Fan). While editing, rotation changes editValue within
// [editMin, editMax] instead of moving the highlight; a short press commits, a long
// press cancels.
static bool submenuEditing = false;
static int  editValue      = 0;
static int  editMin        = 0;
static int  editMax        = 0;

// Is the given channel-submenu row an editable averaging row? If so, report the
// AVG_* selector it edits. Only the CH1/CH2 submenus have them.
static bool avgRowSelector(int subIdx, int row, uint8_t *which) {
    if (subIdx != SUB_CH1 && subIdx != SUB_CH2) return false;
    if (row == CHITEM_AVGV) { if (which) *which = AVG_V; return true; }
    if (row == CHITEM_AVGI) { if (which) *which = AVG_I; return true; }
    return false;
}

// If (subIdx, row) is an inline-editable row, report its current value and the
// [lo, hi] range the editor clamps to. Two kinds: the channel averaging rows (seeded
// from telemetry, committed over the link) and the Temp-and-Fan curve + OTP rows
// (local setpoint variables).
static bool editableRow(int subIdx, int row, int *cur, int *lo, int *hi) {
    uint8_t which;
    if (avgRowSelector(subIdx, row, &which)) {
        const ChannelStatus &s = channel_status((uint8_t)subIdx);   // SUB_CH1/2 == ch 0/1
        int v = (which == AVG_V) ? s.avgV : s.avgI;
        if (!s.avgValid || v < (int)AVG_MIN || v > (int)AVG_MAX) v = (int)AVG_MIN;
        *cur = v; *lo = (int)AVG_MIN; *hi = (int)AVG_MAX;
        return true;
    }
    if ((subIdx == SUB_CH1 || subIdx == SUB_CH2) && row == CHITEM_OTP) {
        const ChannelStatus &s = channel_status((uint8_t)subIdx);   // SUB_CH1/2 == ch 0/1
        int v = s.otpC;
        if (!s.avgValid || v < (int)OTP_MIN_C || v > (int)OTP_MAX_C) v = (int)OTP_MIN_C;
        *cur = v; *lo = (int)OTP_MIN_C; *hi = (int)OTP_MAX_C;
        return true;
    }
    if (subIdx == SUB_FAN) {
        switch (row) {
        case FANITEM_OTP:      *cur = g_sysOtpC;   *lo = 40; *hi = 120; return true;
        case FANITEM_FANMIN:   *cur = g_fanMinPct; *lo = 0;  *hi = 100; return true;
        case FANITEM_FANSTART: *cur = g_fanStartC; *lo = 0;  *hi = 100; return true;
        case FANITEM_FANMAX:   *cur = g_fanMaxC;   *lo = 0;  *hi = 100; return true;
        default: break;
        }
    }
    return false;
}

// Commit an edited value to its backing store: channel averaging goes out over the
// link; the fan/OTP setpoints are local variables the thermal service reads.
static void commitEdit(int subIdx, int row, int value) {
    uint8_t which;
    if (avgRowSelector(subIdx, row, &which)) {
        channel_set_avg((uint8_t)subIdx, which, (uint8_t)value);
        return;
    }
    if ((subIdx == SUB_CH1 || subIdx == SUB_CH2) && row == CHITEM_OTP) {
        channel_set_otp((uint8_t)subIdx, (uint8_t)value);
        return;
    }
    if (subIdx == SUB_FAN) {
        switch (row) {
        case FANITEM_OTP:      g_sysOtpC   = value;          break;
        case FANITEM_FANMIN:   g_fanMinPct = (uint8_t)value; break;
        case FANITEM_FANSTART: g_fanStartC = value;          break;
        case FANITEM_FANMAX:   g_fanMaxC   = value;          break;
        default: break;
        }
        thermalSettingsSave();   // persist the new setpoint to flash
    }
}

// Resolve the value string for a menu row. Channel / fan rows that mirror live
// telemetry are formatted into `buf`; every other row returns its static value.
// Returns a pointer valid until the next call (either `buf` or the static string).
static const char *resolveMenuValue(int subIdx, int row, const MenuItem &item, char *buf, size_t buflen) {
    if (subIdx == SUB_CH1 || subIdx == SUB_CH2) {
        const ChannelStatus &s = channel_status((uint8_t)subIdx);   // SUB_CH1/2 == ch 0/1
        bool up = s.linkUp;
        switch (row) {
        case CHITEM_TEMP:
            if (!up) break;
            snprintf(buf, buflen, "%.1f C", s.temp_cC / 100.0f); return buf;
        case CHITEM_VOLT:
            if (!up) break;
            snprintf(buf, buflen, "%.3f V", s.v_mV / 1000.0f); return buf;
        case CHITEM_CURR:
            if (!up) break;
            snprintf(buf, buflen, "%.4f A", s.i_dmA / 10000.0f); return buf;
        case CHITEM_DAC:
            return up ? ((s.flags & FLAG_DAC_FAULT) ? "FAULT" : "OK") : "--";
        case CHITEM_ADC:
            return up ? ((s.flags & FLAG_ADC_FAULT) ? "FAULT" : "OK") : "--";
        case CHITEM_COMM:
            return up ? "OK" : "FAIL";
        case CHITEM_AVGV:
            if (!s.avgValid) break;
            snprintf(buf, buflen, "%u", (unsigned)s.avgV); return buf;
        case CHITEM_AVGI:
            if (!s.avgValid) break;
            snprintf(buf, buflen, "%u", (unsigned)s.avgI); return buf;
        case CHITEM_OTP:
            if (!s.avgValid) break;
            snprintf(buf, buflen, "%u C", (unsigned)s.otpC); return buf;
        case CHITEM_RUNTIME:
            if (!up || !s.avgValid) break;
            snprintf(buf, buflen, "%lu h", (unsigned long)(s.runtimeS / 3600u)); return buf;
        default:
            break;
        }
        if (!up && (row == CHITEM_TEMP || row == CHITEM_VOLT || row == CHITEM_CURR))
            return "--";
        return item.value;
    }

    if (subIdx == SUB_FAN) {
        const ChannelStatus &s1 = channel_status(0);
        const ChannelStatus &s2 = channel_status(1);
        switch (row) {
        case FANITEM_T1:
            if (!s1.linkUp) return "--";
            snprintf(buf, buflen, "%.0f C", s1.temp_cC / 100.0f); return buf;
        case FANITEM_T2:
            if (!s2.linkUp) return "--";
            snprintf(buf, buflen, "%.0f C", s2.temp_cC / 100.0f); return buf;
        case FANITEM_THS: {
            float ths = readHeatsinkTempC();
            if (isnan(ths)) return "--";
            snprintf(buf, buflen, "%.0f C", ths); return buf;
        }
        case FANITEM_TMAX:                       // cached hottest of Ch1 / Ch2 / heatsink
            if (isnan(g_tempMaxC)) return "--";
            snprintf(buf, buflen, "%.0f C", g_tempMaxC); return buf;
        case FANITEM_OTP:
            snprintf(buf, buflen, "%d C", g_sysOtpC); return buf;
        case FANITEM_FANMIN:
            snprintf(buf, buflen, "%u %%", (unsigned)g_fanMinPct); return buf;
        case FANITEM_FANSTART:
            snprintf(buf, buflen, "%d C", g_fanStartC); return buf;
        case FANITEM_FANMAX:
            snprintf(buf, buflen, "%d C", g_fanMaxC); return buf;
        default:
            break;
        }
        return item.value;
    }

    if (subIdx == SUB_SYS && row == SYSITEM_RUNTIME) {
        snprintf(buf, buflen, "%lu h", (unsigned long)(g_brainRuntimeS / 3600u));
        return buf;
    }

    return item.value;
}

// Menu layout: header baseline 9 like the main page, rule under it, then up to
// four 11 px rows. Values are right-aligned near the panel's right edge.
static const int MENU_Y0    = 17;   // first item row top
static const int MENU_DY    = 11;   // row pitch
static const int MENU_VAL_X = 246;  // right edge of the value column
static const int MENU_VISIBLE = 4;  // rows that fit on the 64 px panel

// Scroll offset that keeps the selected row inside the visible window. Lists
// with <= MENU_VISIBLE items never scroll (top stays 0).
static int menuTop(int sel, int count) {
    if (count <= MENU_VISIBLE) return 0;
    int top = sel - MENU_VISIBLE / 2;
    if (top < 0) top = 0;
    if (top > count - MENU_VISIBLE) top = count - MENU_VISIBLE;
    return top;
}

// Up/down markers at the far right edge when the list scrolls off-window.
static void drawScrollHints(int top, int count) {
    if (top > 0)
        display.drawText(250, MENU_Y0, "^", G_DIM, 0);
    if (top + MENU_VISIBLE < count)
        display.drawText(250, MENU_Y0 + (MENU_VISIBLE - 1) * MENU_DY, "v", G_DIM, 0);
}

// Header + horizontal rule shared by the overview and every submenu.
static void drawMenuHeader(const char *title) {
    display.clear(0);
    display.drawText(6, Y_HDR_TOP, title, G_CHLBL, 0);
    display.drawLine(5, 13, 250, 13, G_LINE);
}

// One menu row at screen slot `slot` (0..MENU_VISIBLE-1): ">" marker + name
// (and optional right-aligned value) with the selected row drawn bright.
static void drawMenuRow(int slot, bool sel, const char *name, const char *value) {
    int y = MENU_Y0 + slot * MENU_DY;
    if (sel) display.drawText(10, y, ">", G_BIG, 0);
    display.drawText(20, y, name, sel ? G_BIG : G_NUM, 0);
    if (value && value[0]) drawText5x7Right(MENU_VAL_X, y, value, sel ? G_BIG : G_DIM);
}

static void drawSettingsOverview() {
    drawMenuHeader("SETTINGS");
    int top = menuTop(settingsSel, SUBMENU_COUNT);
    for (int slot = 0; slot < MENU_VISIBLE && top + slot < SUBMENU_COUNT; slot++) {
        int i = top + slot;
        drawMenuRow(slot, i == settingsSel, SUBMENUS[i].label, nullptr);
    }
    drawScrollHints(top, SUBMENU_COUNT);
    display.flush();
}

static void drawSubmenu() {
    const Submenu &m = SUBMENUS[submenuIdx];
    drawMenuHeader(m.title);
    int top = menuTop(submenuSel, m.count);
    for (int slot = 0; slot < MENU_VISIBLE && top + slot < m.count; slot++) {
        int i = top + slot;
        char vbuf[20];
        const char *val;
        if (submenuEditing && i == submenuSel) {
            // Brackets flag the row as being actively edited.
            snprintf(vbuf, sizeof(vbuf), "[%d]", editValue);
            val = vbuf;
        } else {
            val = resolveMenuValue(submenuIdx, i, m.items[i], vbuf, sizeof(vbuf));
        }
        drawMenuRow(slot, i == submenuSel, m.items[i].name, val);
    }
    drawScrollHints(top, m.count);
    display.flush();
}

// ── Manual calibration wizard (PAGE_CAL) ───────────────────────────────────────
// Guided 2-point calibration of one channel's voltage or current paths. One run
// drives the output to two known operating points; at each, the operator reads an
// external multimeter and dials the reading in with the encoder. Every captured
// point is sent for BOTH the set path and the measure path (the channel pairs the
// entered value with the DAC code it drove resp. its averaged raw ADC voltage),
// so a single run calibrates CAL_xSET and CAL_xMEAS together.
//
// Controls: the wizard is started from the channel submenu and must be confirmed
// by pressing the VOLTAGE button of the channel being calibrated; the same button
// then advances every step (continue / capture / done). Encoder short press moves
// the entry digit, rotation steps it, and a long press aborts at any time (output
// off, user setpoints restored). All other front-panel buttons are inert.
enum CalStep : uint8_t {
    CAL_S_CONFIRM,   // safety gate: waiting for the CHx V button
    CAL_S_CONNECT,   // multimeter hook-up instructions shown
    CAL_S_ENTRY,     // output driven to a point; operator enters the DMM reading
    CAL_S_DONE,      // both points captured + committed (ACK-verified), output off
    CAL_S_ERROR      // the channel never acknowledged a point/commit frame
};
static int       calCh    = 0;         // channel being calibrated (0/1)
static EditParam calParam = EDIT_V;    // EDIT_V = voltage paths, EDIT_I = current paths
static CalStep   calStep  = CAL_S_CONFIRM;
static int       calPoint = 0;         // which of the two cal points is active (0/1)
static int32_t   calEntry = 0;         // dialled-in DMM reading (mV or 0.1 mA units)
static int       calDigit = 0;         // digit cursor in the entry field (0..4)
static uint32_t  calEntryMs = 0;       // entry-step start; gates capture until settled

// Driven operating points. Voltage cal runs open-circuit (current limit only as a
// safety net), spanning most of the 0..36 V range; current cal shorts the output
// through the DMM's ammeter input, with a few volts of compliance, spanning most
// of the 0..2 A range while staying inside a typical 2 A DMM range.
static const int32_t CALV_POINT_MV[2]  = { 2000, 30000 };   // 2 V / 30 V
static const int32_t CALV_ILIM_MA      = 200;               // I limit during V cal
static const int32_t CALI_POINT_DMA[2] = { 2000, 18000 };   // 0.2 A / 1.8 A (0.1 mA)
static const int32_t CALI_VSET_MV      = 5000;              // compliance V during I cal

// Capture is refused until the channel's averaged raw-ADC value (EWMA, ~1.5 s
// step settling) and the output itself have settled at the new point.
static const uint32_t CAL_SETTLE_MS    = 3000;

// Entry field: 5 editable digits. V is dialled in mV as "02.000", I in 0.1 mA
// as "0.2000" — the per-digit step is 10^(4-digit) base units for both.
static const int32_t CAL_ENTRY_STEP[5]  = { 10000, 1000, 100, 10, 1 };
static const uint8_t CAL_DIGIT_POS_V[5] = { 0, 1, 3, 4, 5 };   // "02.000"
static const uint8_t CAL_DIGIT_POS_I[5] = { 0, 2, 3, 4, 5 };   // "0.2000"

// The expander bit of the confirm key: the V button of the calibrated channel.
static inline uint8_t calConfirmBit() { return calCh == 0 ? MCP_CH1_V : MCP_CH2_V; }

// ── Verified (ACK-checked) delivery of the cal transaction ──
// Cal frames must not be fire-and-forget: CMD_CAL_COMMIT makes the channel do a
// blocking EEPROM write, and a frame sent right behind it can be dropped while
// the write runs. That silently lost the VMEAS commit in the field — set path
// calibrated, measure path left on defaults, wizard still claiming success.
// Waiting for each ACK both verifies delivery and paces the next frame past the
// write window (the channel only ACKs a commit after its EEPROM write is done).
static bool calAwaitAck(uint8_t ackCmd, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < timeoutMs) {
        channel_link_task();
        if (channel_status((uint8_t)calCh).lastAckCmd == ackCmd) return true;
    }
    return false;
}

static bool calSendPoint(uint8_t target, uint8_t index, int32_t actual) {
    for (int attempt = 0; attempt < 3; attempt++) {   // idempotent: retry freely
        channel_clear_last_ack((uint8_t)calCh);
        channel_cal_point((uint8_t)calCh, target, index, actual);
        if (calAwaitAck(CMD_CAL_POINT, 100)) return true;
    }
    return false;
}

static bool calSendCommit(uint8_t target) {
    for (int attempt = 0; attempt < 3; attempt++) {   // commit keeps its points, so
        channel_clear_last_ack((uint8_t)calCh);       // a re-send is idempotent too
        channel_cal_commit((uint8_t)calCh, target);
        if (calAwaitAck(CMD_CAL_COMMIT, 300)) return true;  // EEPROM write inside
    }
    return false;
}

static bool calSettled() {
    return (uint32_t)(millis() - calEntryMs) >= CAL_SETTLE_MS;
}

static void drawCalPage() {
    char title[24], buf[44];
    const bool isV = (calParam == EDIT_V);
    snprintf(title, sizeof(title), "MANUAL CAL %s  CH%d", isV ? "V" : "I", calCh + 1);
    drawMenuHeader(title);

    const int y0 = MENU_Y0, dy = MENU_DY;
    switch (calStep) {
    case CAL_S_CONFIRM:
        snprintf(buf, sizeof(buf), "Calibrates CH%d %s set + meas paths.",
                 calCh + 1, isV ? "voltage" : "current");
        display.drawText(10, y0,          buf, G_NUM, 0);
        display.drawText(10, y0 + dy,     "The output will be driven ON.", G_NUM, 0);
        snprintf(buf, sizeof(buf), "Press the CH%d V button to start", calCh + 1);
        display.drawText(10, y0 + 2 * dy, buf, G_BIG, 0);
        display.drawText(10, y0 + 3 * dy, "Hold encoder: cancel", G_DIM, 0);
        break;

    case CAL_S_CONNECT:
        if (isV) {
            display.drawText(10, y0,      "Multimeter: DC VOLTAGE mode, probes", G_NUM, 0);
            snprintf(buf, sizeof(buf), "across the CH%d output terminals.", calCh + 1);
            display.drawText(10, y0 + dy, buf, G_NUM, 0);
            display.drawText(10, y0 + 2 * dy, "Disconnect any other load.", G_NUM, 0);
        } else {
            display.drawText(10, y0,      "Multimeter: CURRENT mode (2A range),", G_NUM, 0);
            snprintf(buf, sizeof(buf), "directly across the CH%d output.", calCh + 1);
            display.drawText(10, y0 + dy, buf, G_NUM, 0);
            display.drawText(10, y0 + 2 * dy, "No other load: the DMM carries 1.8A.", G_NUM, 0);
        }
        snprintf(buf, sizeof(buf), "CH%d V: output ON + continue", calCh + 1);
        display.drawText(10, y0 + 3 * dy, buf, G_BIG, 0);
        break;

    case CAL_S_ENTRY: {
        const ChannelStatus &s = channel_status((uint8_t)calCh);
        if (isV)
            snprintf(buf, sizeof(buf), "Point %d/2   set %.3fV   meas %.3fV",
                     calPoint + 1, CALV_POINT_MV[calPoint] / 1000.0f, s.v_mV / 1000.0f);
        else
            snprintf(buf, sizeof(buf), "Point %d/2   set %.4fA   meas %.4fA",
                     calPoint + 1, CALI_POINT_DMA[calPoint] / 10000.0f, s.i_dmA / 10000.0f);
        display.drawText(10, y0,      buf, G_NUM, 0);
        display.drawText(10, y0 + dy, "Enter the multimeter reading:", G_NUM, 0);

        // Entry field with the active digit bright + underlined (like the SET strip).
        char field[8];
        if (isV) snprintf(field, sizeof(field), "%06.3f", calEntry / 1000.0f);
        else     snprintf(field, sizeof(field), "%.4f",   calEntry / 10000.0f);
        const uint8_t *pos = isV ? CAL_DIGIT_POS_V : CAL_DIGIT_POS_I;
        const int curPos = pos[calDigit];
        const int yf = y0 + 2 * dy;
        for (int i = 0; field[i]; i++) {
            char cs[2] = { field[i], 0 };
            int x = 10 + i * 6;
            display.drawText(x, yf, cs, i == curPos ? G_BIG : G_SET, 0);
            if (i == curPos)
                display.drawLine(x, yf + 7, x + 4, yf + 7, G_BIG);
        }
        display.drawText(10 + 7 * 6, yf, isV ? "V" : "A", G_DIM, 0);

        if (!calSettled())
            display.drawText(10, y0 + 3 * dy, "Settling / averaging...", G_DIM, 0);
        else {
            snprintf(buf, sizeof(buf), "CH%d V: capture  Enc: digit  Hold: abort", calCh + 1);
            display.drawText(10, y0 + 3 * dy, buf, G_DIM, 0);
        }
        break;
    }

    case CAL_S_DONE:
        display.drawText(10, y0,      "Calibration stored in the channel.", G_NUM, 0);
        display.drawText(10, y0 + dy, "Output is OFF.", G_NUM, 0);
        snprintf(buf, sizeof(buf), "CH%d V: done", calCh + 1);
        display.drawText(10, y0 + 2 * dy, buf, G_BIG, 0);
        break;

    case CAL_S_ERROR:
        display.drawText(10, y0,      "Channel did not confirm storing!", G_NUM, 0);
        display.drawText(10, y0 + dy, "Calibration may be partly applied.", G_NUM, 0);
        display.drawText(10, y0 + 2 * dy, "Re-run the procedure.", G_NUM, 0);
        snprintf(buf, sizeof(buf), "CH%d V: exit", calCh + 1);
        display.drawText(10, y0 + 3 * dy, buf, G_BIG, 0);
        break;
    }
    display.flush();
}

// Command the output to one of the two cal points and (re)open the entry step,
// seeding the entry field with the nominal value so only the error is dialled in.
static void calApplyPoint(int point) {
    calPoint = point;
    if (calParam == EDIT_V) {
        channel_set_current((uint8_t)calCh, CALV_ILIM_MA);
        channel_set_voltage((uint8_t)calCh, CALV_POINT_MV[point]);
        calEntry = CALV_POINT_MV[point];
    } else {
        channel_set_voltage((uint8_t)calCh, CALI_VSET_MV);
        channel_set_current((uint8_t)calCh, CALI_POINT_DMA[point] / 10);
        calEntry = CALI_POINT_DMA[point];
    }
    calDigit   = 3;              // start on a mid-significance digit
    calEntryMs = millis();
    calStep    = CAL_S_ENTRY;
}

// Leave the wizard from any step: output off, user setpoints restored, back to
// the channel submenu. Serves both abort (long press) and the normal exit.
static void calExit() {
    channel_set_output((uint8_t)calCh, false);
    pushSetVoltage(calCh);
    pushSetCurrent(calCh);
    uiPage = PAGE_SUBMENU;
    drawSubmenu();
}

// Enter the wizard from the channel submenu. The output is forced off first so
// calibration never starts on a live output; outDesired stays cleared so the
// exit path leaves the channel off as well.
static void calStart(int ch, EditParam p) {
    calCh    = ch;
    calParam = p;
    calStep  = CAL_S_CONFIRM;
    outDesired[ch] = false;
    channel_set_output((uint8_t)ch, false);
    uiPage = PAGE_CAL;
    drawCalPage();
}

// The confirm key (CHx V button) advances the wizard one step.
static void calConfirmPress() {
    switch (calStep) {
    case CAL_S_CONFIRM:
        calStep = CAL_S_CONNECT;
        beep(20);
        drawCalPage();
        break;

    case CAL_S_CONNECT:
        channel_set_output((uint8_t)calCh, true);
        calApplyPoint(0);
        beep(20);
        drawCalPage();
        break;

    case CAL_S_ENTRY: {
        if (!calSettled()) { beep(100); break; }   // averaging still warming up
        const uint8_t tSet  = (calParam == EDIT_V) ? CAL_VSET  : CAL_ISET;
        const uint8_t tMeas = (calParam == EDIT_V) ? CAL_VMEAS : CAL_IMEAS;

        bool ok = calSendPoint(tSet,  (uint8_t)calPoint, calEntry) &&
                  calSendPoint(tMeas, (uint8_t)calPoint, calEntry);
        if (ok && calPoint == 0) {
            beep(20);
            calApplyPoint(1);
            drawCalPage();
            break;
        }
        if (ok)
            ok = calSendCommit(tSet) && calSendCommit(tMeas);

        channel_set_output((uint8_t)calCh, false);
        pushSetVoltage(calCh);         // re-apply user setpoints through the new cal
        pushSetCurrent(calCh);
        calStep = ok ? CAL_S_DONE : CAL_S_ERROR;
        beep(ok ? 20 : 200);
        drawCalPage();
        break;
    }

    case CAL_S_DONE:
    case CAL_S_ERROR:
        calExit();
        break;
    }
}

// Full repaint of the dual-channel readout, used when leaving the menus.
static void drawMainPage() {
    display.clear(0);
    display.drawLine(127, 6, 127, 57, G_LINE);   // centre divider
    drawStatic(ch1,   0);
    drawStatic(ch2, 128);
    drawChannelHeader(0);
    drawChannelHeader(1);
    drawSetStrip(0);
    drawSetStrip(1);
    drawDynamic(ch1,   0);
    drawDynamic(ch2, 128);
    display.flush();
}

// Wrap sel + steps into [0, count).
static int wrapSel(int sel, int steps, int count) {
    sel = (sel + steps) % count;
    return sel < 0 ? sel + count : sel;
}

// Mirror the integer setpoint stores into the Channel float fields (kept for any
// consumer that wants the values as volts/amps).
static void applySetpoints() {
    ch1.setV = setV_cV[0] / 100.0f;   ch1.setI = setI_mA[0] / 1000.0f;
    ch2.setV = setV_cV[1] / 100.0f;   ch2.setI = setI_mA[1] / 1000.0f;
}

void setup() {
    Serial.begin(115200);          // USB CDC debug console (separate from the links)

    EEPROM.begin(BRAIN_EEPROM_SIZE);      // flash-backed settings store
    thermalSettingsLoad();                // restore persisted fan curve + SYS OTP
    brainRuntimeLoad();                   // restore the brain's operating-hours meter

    analogReadResolution(NTC_ADC_BITS);   // 12-bit ADC for the heatsink NTC on GPIO29
    fanInit();                            // 25 kHz PWM on GPIO26 (starts at full speed)

    encoderInit();
    mcpInit();
    channel_link_init();           // bring up the two channel UARTs (UART0/UART1)

    display.begin();

    selfTest();

    applySetpoints();
    drawMainPage();
}

// ── Encoder input dispatch ──────────────────────────────────────────────────────
// Short press: main page = advance the digit cursor; overview = enter the
// selected submenu; submenu = nothing yet (values are dummies).
static void onShortPress() {
    switch (uiPage) {
    case PAGE_MAIN:
        selDigit() = (selDigit() + 1) & 3;         // step the active field's digit cursor
        drawSetStrip(editCh);
        flushSetStrip(editCh);
        break;
    case PAGE_SETTINGS:
        submenuIdx     = settingsSel;
        submenuSel     = 0;
        submenuEditing = false;
        uiPage         = PAGE_SUBMENU;
        drawSubmenu();
        break;
    case PAGE_SUBMENU: {
        // The Manual Cal rows launch the calibration wizard (link must be up;
        // the channel is the one whose submenu is open).
        if ((submenuIdx == SUB_CH1 || submenuIdx == SUB_CH2) && !submenuEditing &&
            (submenuSel == CHITEM_MANV || submenuSel == CHITEM_MANI)) {
            if (!channel_status((uint8_t)submenuIdx).linkUp) { beep(200); break; }
            calStart(submenuIdx, submenuSel == CHITEM_MANV ? EDIT_V : EDIT_I);
            break;
        }
        int cur, lo, hi;
        if (!editableRow(submenuIdx, submenuSel, &cur, &lo, &hi)) break;  // not editable
        if (!submenuEditing) {
            editValue = cur; editMin = lo; editMax = hi;   // seed the editor + its range
            submenuEditing = true;
        } else {
            commitEdit(submenuIdx, submenuSel, editValue); // channel link or local store
            submenuEditing = false;
            beep(20);
        }
        drawSubmenu();
        break;
    }
    case PAGE_CAL:
        if (calStep == CAL_S_ENTRY) {          // cycle the entry-field digit cursor
            calDigit = (calDigit + 1) % 5;
            drawCalPage();
        }
        break;
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
        if (submenuEditing) {
            submenuEditing = false;   // cancel edit, discard the pending value
            drawSubmenu();
        } else {
            uiPage = PAGE_SETTINGS;
            drawSettingsOverview();
        }
        break;
    case PAGE_CAL:
        calExit();                    // abort: output off, setpoints restored
        beep(20);
        break;
    }
}

// Rotation: main page = step the selected digit of the active setpoint (the V/I
// button picks channel + parameter), clamped to its range, and push the new value
// to the channel; menus = move the highlight (wraps around).
static void onRotate(int steps) {
    switch (uiPage) {
    case PAGE_MAIN:
        if (editParam == EDIT_V) {
            setV_cV[editCh] += (int32_t)steps * V_DIGIT_STEP_CV[selDigit()];
            if (setV_cV[editCh] < 0)           setV_cV[editCh] = 0;
            if (setV_cV[editCh] > SETV_MAX_CV) setV_cV[editCh] = SETV_MAX_CV;
            pushSetVoltage(editCh);
        } else {
            setI_mA[editCh] += (int32_t)steps * I_DIGIT_STEP_MA[selDigit()];
            if (setI_mA[editCh] < 0)           setI_mA[editCh] = 0;
            if (setI_mA[editCh] > SETI_MAX_MA) setI_mA[editCh] = SETI_MAX_MA;
            pushSetCurrent(editCh);
        }
        applySetpoints();
        drawSetStrip(editCh);
        flushSetStrip(editCh);
        break;
    case PAGE_SETTINGS:
        settingsSel = wrapSel(settingsSel, steps, SUBMENU_COUNT);
        drawSettingsOverview();
        break;
    case PAGE_SUBMENU:
        if (submenuEditing) {
            int v = editValue + steps;
            if (v < editMin) v = editMin;
            if (v > editMax) v = editMax;
            editValue = v;
        } else {
            submenuSel = wrapSel(submenuSel, steps, SUBMENUS[submenuIdx].count);
        }
        drawSubmenu();
        break;
    case PAGE_CAL:
        if (calStep == CAL_S_ENTRY) {          // step the selected entry digit
            const int32_t max = (calParam == EDIT_V) ? SETV_MAX_CV * 10   // 36000 mV
                                                     : SETI_MAX_MA * 10;  // 20000 dmA
            calEntry += (int32_t)steps * CAL_ENTRY_STEP[calDigit];
            if (calEntry < 0)   calEntry = 0;
            if (calEntry > max) calEntry = max;
            drawCalPage();
        }
        break;
    }
}

// ── Front-panel button dispatch (from the expander) ────────────────────────────
// A CHx_V / CHx_I button selects which setpoint the encoder edits; the cursor moves
// to that field. Only meaningful on the main page.
static void setEditTarget(int ch, EditParam p) {
    if (uiPage != PAGE_MAIN) return;
    editCh    = ch;
    editParam = p;
    drawSetStrip(0); flushSetStrip(0);             // move the cursor to the new field
    drawSetStrip(1); flushSetStrip(1);
}

// Toggle a channel's output and command it over the link. The header colour will
// follow the channel's actual state once telemetry confirms it. While the OTP latch
// holds, turning an output *on* is refused (a long beep flags the block).
static void toggleOutput(int ch) {
    if (g_otpTripped && !outDesired[ch]) {   // over-temp: don't re-energize
        beep(200);
        return;
    }
    outDesired[ch] = !outDesired[ch];
    channel_set_output((uint8_t)ch, outDesired[ch]);
    beep(20);
    if (uiPage == PAGE_MAIN) { drawChannelHeader(ch); flushDynamic(ch * 128); }
}

// Handle a fresh press of a non-encoder expander button.
static void onButtonPress(uint8_t bit) {
    if (uiPage == PAGE_CAL) {
        // Only the calibrated channel's V button (the confirm key) acts during
        // the wizard; every other front-panel button is inert.
        if (bit == calConfirmBit()) calConfirmPress();
        return;
    }
    switch (bit) {
    case MCP_CH1_V:  setEditTarget(0, EDIT_V); break;
    case MCP_CH1_I:  setEditTarget(0, EDIT_I); break;
    case MCP_CH2_V:  setEditTarget(1, EDIT_V); break;
    case MCP_CH2_I:  setEditTarget(1, EDIT_I); break;
    case MCP_CH1_ON: toggleOutput(0); break;
    case MCP_CH2_ON: toggleOutput(1); break;
    default: break;                                // encoder button handled elsewhere
    }
}

// Read the expander when its INT line signals a change, edge-detect button presses
// with a per-pin debounce, and keep g_btnPressed as the debounced pressed bitmask
// (used by serviceEncoder for the encoder button on GP0). Reading GPIO clears INT.
static const uint32_t BTN_DEBOUNCE_MS = 20;

static void serviceMcpButtons() {
    static bool     inited        = false;
    static uint8_t  prevPressed   = 0;
    static uint32_t lastChange[8] = { 0 };

    if (!inited) {                                 // baseline; also clears stray INT
        uint8_t raw  = mcpRead(MCP_GPIO);
        prevPressed  = (uint8_t)(~raw) & MCP_BTN_MASK;
        g_btnPressed = prevPressed;
        inited = true;
        return;
    }

    if (digitalRead(PIN_MCP_INT)) return;          // INT idle (high) → no change

    uint8_t raw     = mcpRead(MCP_GPIO);           // read + clear the interrupt
    uint8_t pressed = (uint8_t)(~raw) & MCP_BTN_MASK;   // active-low: low = pressed
    uint8_t changed = pressed ^ prevPressed;
    uint32_t now    = millis();

    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1 << i);
        if (!(changed & bit)) continue;
        if (now - lastChange[i] < BTN_DEBOUNCE_MS) continue;   // swallow bounce
        lastChange[i] = now;
        prevPressed   = (uint8_t)((prevPressed & ~bit) | (pressed & bit));
        g_btnPressed  = prevPressed;
        if ((pressed & bit) && bit != MCP_ENC_BT)  // fresh press (encoder handled below)
            onButtonPress(bit);
    }
}

// Poll the encoder button and drain the encoder step counter, dispatching to
// the handlers above. The button (active-low, now on expander GP0) is edge-detected
// with a debounce lockout; holding it past LONGPRESS_MS fires the long-press action
// once, and the following release is then swallowed so it doesn't also short-press.
static const uint32_t LONGPRESS_MS = 600;

static void serviceEncoder() {
    static bool     btPrev    = false;
    static uint32_t btEdgeMs  = 0;
    static uint32_t btDownMs  = 0;
    static bool     longFired = false;

    uint32_t now = millis();
    bool bt = (g_btnPressed & MCP_ENC_BT) != 0;    // encoder button via the expander
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

// System over-temperature protection. When the governing temperature reaches the
// SYS OTP setpoint, force both outputs off and clear the desired-on state so the link
// resync won't re-energize them. The trip latches until the temperature falls
// SYS_OTP_HYST_C below the setpoint; while latched, toggleOutput() refuses to turn an
// output back on. A valid reading is required to trip (a dropped sensor won't).
static void otpApply(float t) {
    if (isnan(t)) return;
    if (!g_otpTripped) {
        if (t >= (float)g_sysOtpC) {
            g_otpTripped = true;
            for (int ch = 0; ch < 2; ch++) {
                outDesired[ch] = false;
                channel_set_output((uint8_t)ch, false);
            }
            beep(200);
            if (uiPage == PAGE_MAIN) {
                drawChannelHeader(0); drawChannelHeader(1);
                flushDynamic(0); flushDynamic(128);
            }
        }
    } else if (t < (float)g_sysOtpC - SYS_OTP_HYST_C) {
        g_otpTripped = false;   // cooled down: manual re-enable allowed again
    }
}

// Thermal service: sample the hottest temperature on a fixed cadence, cache it for
// the menu, then run over-temperature protection and the fan curve from it.
static void serviceThermal() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - last) < FAN_UPDATE_MS) return;
    last = now;

    float t = governingTempC();
    g_tempMaxC = t;
    otpApply(t);   // trip first, so a trip also forces the fan to full via fanApply
    fanApply(t);
}

// Track link up/down edges to keep the channel and panel in sync across drops:
//  - up edge:   push setpoints + desired output so the channel matches the panel
//               even if the brain booted after the channel (or the link glitched).
//  - down edge: clear the desired-on state. On a comms drop the channel trips its
//               output off (its own watchdog); clearing outDesired here means the
//               resync on reconnect commands the output OFF, so a live output never
//               silently re-energizes -- the user must press CHx_ON again.
static void serviceLinkResync() {
    static bool prevUp[2] = { false, false };
    for (int ch = 0; ch < 2; ch++) {
        bool up = channel_status((uint8_t)ch).linkUp;
        if (up && !prevUp[ch]) {
            pushSetVoltage(ch);
            pushSetCurrent(ch);
            channel_set_output((uint8_t)ch, outDesired[ch]);
        } else if (!up && prevUp[ch]) {
            outDesired[ch] = false;
        }
        prevUp[ch] = up;
    }
}

// Ping each channel on a fixed cadence. This heartbeat feeds the channel-side
// comms-loss watchdog: with no other periodic Brain->channel traffic, a channel
// would otherwise trip its output off during quiet operation. Any valid frame
// resets the channel's watchdog, so an idle link stays alive on these pings alone.
static void serviceLinkHeartbeat() {
    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - lastPing) >= LINK_HEARTBEAT_MS) {
        lastPing = now;
        channel_ping(0);
        channel_ping(1);
    }
}

// Re-poll each channel's settings on a slow cadence. Settings are otherwise
// only fetched on link-up, but the channel's operating-hours meter (runtimeS)
// rides the CMD_SETTINGS reply and keeps climbing, so a periodic refresh keeps
// the Runtime menu row current (and reconciles avg/OTP if they ever drift).
// Hours-granularity data needs nothing fast; 10 s is plenty.
static void serviceLinkSettingsPoll() {
    static uint32_t lastPoll = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - lastPoll) >= 10000u) {
        lastPoll = now;
        channel_get_settings(0);
        channel_get_settings(1);
    }
}

void loop() {
    channel_link_task();     // drain both channel UARTs, update telemetry + link state
    serviceLinkResync();     // re-send setpoints/output on link-up; disarm on link-down
    serviceLinkHeartbeat();  // ping both channels so their comms-loss watchdog stays fed
    serviceLinkSettingsPoll(); // slow re-poll of settings (keeps Runtime row fresh)
    serviceBrainRuntime();   // accumulate the brain's own operating hours
    serviceMcpButtons();   // refresh button state (incl. encoder GP0) before dispatch
    serviceEncoder();
    serviceBuzzer();
    serviceThermal();      // hottest temp -> over-temp trip + chassis fan PWM

    const uint32_t now = millis();

    if (uiPage == PAGE_MAIN) {
        // ~20 fps readout refresh from live telemetry.
        static uint32_t lastReadout = 0;
        if ((uint32_t)(now - lastReadout) >= 50) {
            lastReadout = now;
            updateMeasFromTelemetry();
            drawChannelHeader(0);
            drawChannelHeader(1);
            drawDynamic(ch1,   0);
            drawDynamic(ch2, 128);
            flushDynamic(0);
            flushDynamic(128);
        }
    } else if (uiPage == PAGE_CAL) {
        if (!channel_status((uint8_t)calCh).linkUp || g_otpTripped) {
            // Lost the channel or over-temperature: abandon the calibration.
            beep(200);
            calExit();
        } else if (calStep == CAL_S_ENTRY) {
            // Refresh the entry page so the live measurement and the
            // settling banner keep tracking.
            static uint32_t lastCal = 0;
            if ((uint32_t)(now - lastCal) >= 300) {
                lastCal = now;
                drawCalPage();
            }
        }
    } else if (uiPage == PAGE_SUBMENU && submenuIdx <= SUB_FAN) {
        // Channel / fan submenus mirror live telemetry; refresh them a few times a
        // second so the measured values keep moving while the page is open.
        static uint32_t lastMenu = 0;
        if ((uint32_t)(now - lastMenu) >= 300) {
            lastMenu = now;
            drawSubmenu();
        }
    }
}
