/**
 * scpi.cpp - SCPI-1999 / IEEE-488.2 subset parser for the RP2350 brain.
 *
 * Implements firmware/SCPI/commands.md: a table-light, hand-structured header
 * matcher with a per-instrument error queue, the SCPI status model (Questionable
 * / Operation registers, ESR/ESE/SRE/STB), channel-list ((@1,2)) addressing and
 * MIN|MAX|DEF numeric arguments. Accepted commands are translated into the binary
 * channel-link protocol and the shared front-panel state (brain_api.h).
 *
 * Deviations (documented for the reader):
 *  - Voltage setpoints resolve to 10 mV (the panel's finest digit); current keeps
 *    1 mA. VOLT/CURR readback reflect the stored, quantized setpoint.
 *  - Compound relative headers ("VOLT 5;CURR 1" continuing a subsystem) are not
 *    supported; every ';'-separated command is parsed from the root, matching the
 *    ':'-prefixed examples in the spec.
 *  - CALibration:DATA? cannot read gain/offset back over the link, so it returns
 *    SCPI NaN (9.91E37,9.91E37).
 *  - One shared instrument error queue / status model is used for all sessions.
 */
#include <Arduino.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "scpi.h"
#include "channel_link.h"
#include "protocol.h"
#include "brain_api.h"
#include "netcfg.h"

// ── Limits / sizes ──────────────────────────────────────────────────────────────
#define SCPI_MAX_TOK    10     // header colon-separated tokens
#define SCPI_MAX_ARGV   6      // comma-separated parameter fields
#define SCPI_ARG_MAX    200    // residual parameter text length
#define SCPI_RESP_MAX   480    // assembled response line
#define SCPI_ERR_DEPTH  16     // error-queue depth
#define SCPI_ERR_MSGLEN 56

// SCPI Questionable-status bit weights (per commands.md §8).
#define QUES_OVERTEMP   (1u << 4)   // 16
#define QUES_CALINVALID (1u << 8)   // 256
#define QUES_ADCFAULT   (1u << 9)   // 512
#define QUES_DACFAULT   (1u << 10)  // 1024
#define QUES_FAULT      (1u << 11)  // 2048
// Operation-status bits: output on per channel.
#define OPER_CH1_ON     (1u << 8)   // 256
#define OPER_CH2_ON     (1u << 9)   // 512

// ── Instrument state ────────────────────────────────────────────────────────────
static int      s_sel = 0;               // selected channel (0/1)
static uint16_t s_quesEnable = 0, s_quesEvent = 0;
static uint16_t s_operEnable = 0, s_operEvent = 0;
static uint8_t  s_esr = 0, s_ese = 0, s_sre = 0;
static bool     s_calUnlocked = false;
static uint8_t  s_prevChEvent[LINK_COUNT] = { 0, 0 };
static uint16_t s_prevQuesCond = 0, s_prevOperCond = 0;

struct ErrEntry { int code; char msg[SCPI_ERR_MSGLEN]; };
static ErrEntry s_err[SCPI_ERR_DEPTH];
static int      s_errHead = 0, s_errCount = 0;

// *SAV/*RCL slots (volatile RAM; recall applies to the shared state).
struct SaveSlot { bool used; int32_t vMv[2], iMa[2]; bool out[2]; };
static SaveSlot s_slot[4];

// ── Per-line parse context (single-threaded; reused each segment) ───────────────
static char *s_tok[SCPI_MAX_TOK];
static int   s_ntok;
static int   s_ti;
static bool  s_isQuery;
static char *s_argv[SCPI_MAX_ARGV];
static int   s_argc;
static int   s_chset[2];
static int   s_nch;

// ── Response builder ────────────────────────────────────────────────────────────
static char s_resp[SCPI_RESP_MAX];
static int  s_respLen;
static bool s_respHas;   // at least one query segment emitted this line

static void respReset() { s_respLen = 0; s_resp[0] = '\0'; s_respHas = false; }

static void respRaw(const char *s) {
    int n = (int)strlen(s);
    if (s_respLen + n >= SCPI_RESP_MAX) n = SCPI_RESP_MAX - 1 - s_respLen;
    if (n <= 0) return;
    memcpy(s_resp + s_respLen, s, (size_t)n);
    s_respLen += n;
    s_resp[s_respLen] = '\0';
}

// Emit one query's response segment, inserting the ';' separator between the
// responses of chained queries on the same line.
static void emit(const char *s) {
    if (s_respHas) respRaw(";");
    respRaw(s);
    s_respHas = true;
}

static void emitf(const char *fmt, ...) {
    char buf[SCPI_RESP_MAX];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
}

// ── Error queue ─────────────────────────────────────────────────────────────────
static const char *errStdMsg(int code) {
    switch (code) {
    case 0:    return "No error";
    case -100: return "Command error";
    case -108: return "Parameter not allowed";
    case -109: return "Missing parameter";
    case -113: return "Undefined header";
    case -120: return "Numeric data error";
    case -221: return "Settings conflict";
    case -222: return "Data out of range";
    case -224: return "Illegal parameter value";
    case -241: return "Hardware missing";
    case -315: return "Configuration memory lost";
    case -363: return "Input buffer overrun";
    case 101:  return "Channel self-test failed";
    case 102:  return "Over-temperature";
    case 103:  return "ADC fault";
    case 104:  return "DAC fault";
    case 105:  return "Calibration invalid";
    case 106:  return "Output enable refused";
    case 107:  return "UART CRC / framing error";
    default:   return "Device error";
    }
}

static void errPush(int code, const char *msg) {
    if (s_errCount >= SCPI_ERR_DEPTH) {
        // Queue full: overwrite the newest with the standard overflow marker.
        int idx = (s_errHead + s_errCount - 1) % SCPI_ERR_DEPTH;
        s_err[idx].code = -350;
        strncpy(s_err[idx].msg, "Queue overflow", SCPI_ERR_MSGLEN - 1);
        s_err[idx].msg[SCPI_ERR_MSGLEN - 1] = '\0';
        return;
    }
    int idx = (s_errHead + s_errCount) % SCPI_ERR_DEPTH;
    s_err[idx].code = code;
    strncpy(s_err[idx].msg, msg ? msg : errStdMsg(code), SCPI_ERR_MSGLEN - 1);
    s_err[idx].msg[SCPI_ERR_MSGLEN - 1] = '\0';
    s_errCount++;
    s_esr |= 0x04;   // Standard Event: Query/Command error summary bit (bit2)
}

static void errStd(int code) { errPush(code, errStdMsg(code)); }

static bool errPop(int *code, char *msg, size_t msglen) {
    if (s_errCount == 0) { *code = 0; strncpy(msg, "No error", msglen); return false; }
    ErrEntry &e = s_err[s_errHead];
    *code = e.code;
    strncpy(msg, e.msg, msglen);
    msg[msglen - 1] = '\0';
    s_errHead = (s_errHead + 1) % SCPI_ERR_DEPTH;
    s_errCount--;
    return true;
}

// ── Keyword matching (SCPI short/long form, case-insensitive) ───────────────────
// Uppercase letters in `pat` are the accepted short form; the whole spelling is
// the long form. `tok` must equal exactly one of the two.
static bool kwMatch(const char *tok, const char *pat) {
    size_t patLen = strlen(pat), shortLen = 0;
    while (shortLen < patLen && isupper((unsigned char)pat[shortLen])) shortLen++;
    size_t tl = strlen(tok);
    if (tl == patLen  && strncasecmp(tok, pat, patLen)  == 0) return true;
    if (tl == shortLen && strncasecmp(tok, pat, shortLen) == 0) return true;
    return false;
}

// Same, for a standalone parameter word (MIN/MAX/DEF/ON/OFF...).
static bool kwWord(const char *w, const char *pat) { return kwMatch(w, pat); }

// Token cursor over the parsed header.
static bool at(const char *pat)  { return s_ti < s_ntok && kwMatch(s_tok[s_ti], pat); }
static bool eat(const char *pat) { if (at(pat)) { s_ti++; return true; } return false; }
static bool tdone()              { return s_ti >= s_ntok; }

// ── Small string helpers ────────────────────────────────────────────────────────
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

// Parse a boolean parameter (ON|OFF|1|0|TRUE|FALSE). Returns false if unparseable.
static bool parseBool(const char *s, bool *out) {
    if (kwWord(s, "ON") || !strcmp(s, "1") || kwWord(s, "TRUE"))  { *out = true;  return true; }
    if (kwWord(s, "OFF") || !strcmp(s, "0") || kwWord(s, "FALSE")) { *out = false; return true; }
    return false;
}

// Parse an NRf with MIN|MAX|DEF and optional k/m/u multiplier + ignored unit.
// Returns false when the field is empty/non-numeric; sets *clamped if a numeric
// value fell outside [lo,hi] (and was clamped).
static bool parseNum(const char *s, double lo, double hi, double def,
                     double *out, bool *clamped) {
    *clamped = false;
    while (*s == ' ') s++;
    if (!*s) return false;
    if (kwWord(s, "MINimum")) { *out = lo;  return true; }
    if (kwWord(s, "MAXimum")) { *out = hi;  return true; }
    if (kwWord(s, "DEFault")) { *out = def; return true; }

    char *end;
    double v = strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ') end++;
    if (*end) {
        char c = *end;
        if (c == 'k' || c == 'K')      v *= 1e3;   // kilo
        else if (c == 'm' || c == 'M') v *= 1e-3;  // milli
        else if (c == 'u' || c == 'U') v *= 1e-6;  // micro
        // any trailing unit letters (V/A/W/OHM/S) are ignored
    }
    if (v < lo)      { v = lo; *clamped = true; }
    else if (v > hi) { v = hi; *clamped = true; }
    *out = v;
    return true;
}

// Parse a channel name/number token → 0-based index. Accepts CH1|CH2|1|2.
static bool parseChannel(const char *s, int *ch) {
    if (kwWord(s, "CH1") || !strcmp(s, "1")) { *ch = 0; return true; }
    if (kwWord(s, "CH2") || !strcmp(s, "2")) { *ch = 1; return true; }
    return false;
}

// Parse a dotted IPv4 into 4 bytes.
static bool parseIp(const char *s, uint8_t out[4]) {
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if ((a | b | c | d) < 0 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

// ── Effective channel set (from a (@list) suffix or the selected channel) ───────
static void chsetSelected() { s_chset[0] = s_sel; s_nch = 1; }

// ── Live status conditions ──────────────────────────────────────────────────────
static uint16_t quesCondition() {
    uint16_t q = 0;
    for (int ch = 0; ch < LINK_COUNT; ch++) {
        const ChannelStatus &s = channel_status((uint8_t)ch);
        if (!s.linkUp) continue;
        if (s.flags & FLAG_OVERTEMP)    q |= QUES_OVERTEMP;
        if (s.flags & FLAG_CAL_INVALID) q |= QUES_CALINVALID;
        if (s.flags & FLAG_ADC_FAULT)   q |= QUES_ADCFAULT;
        if (s.flags & FLAG_DAC_FAULT)   q |= QUES_DACFAULT;
        if (s.state == ST_FAULT)        q |= QUES_FAULT;
    }
    return q;
}

static uint16_t operCondition() {
    uint16_t o = 0;
    if (channel_status(0).flags & FLAG_OUTPUT_ON) o |= OPER_CH1_ON;
    if (channel_status(1).flags & FLAG_OUTPUT_ON) o |= OPER_CH2_ON;
    return o;
}

// Status Byte (*STB?) assembled from the summaries.
static uint8_t statusByte() {
    uint8_t stb = 0;
    if (s_errCount > 0)                       stb |= 0x10;  // bit4 MAV (proxy)
    if (s_esr & s_ese)                        stb |= 0x20;  // bit5 ESB
    if (s_quesEvent & s_quesEnable)           stb |= 0x08;  // bit3 QUES summary
    if (s_operEvent & s_operEnable)           stb |= 0x80;  // bit7 OPER summary
    if (stb & s_sre)                          stb |= 0x40;  // bit6 MSS
    return stb;
}

// ── Fresh-measurement force (MEASure vs FETCh) ──────────────────────────────────
static void measForce(int ch) {
    uint32_t f0 = channel_status((uint8_t)ch).framesRx;
    channel_get_status((uint8_t)ch);
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 50) {
        channel_link_task();
        if (channel_status((uint8_t)ch).framesRx != f0) break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  IEEE-488.2 common commands (headers starting with '*')
// ════════════════════════════════════════════════════════════════════════════
static void doReset() {
    for (int ch = 0; ch < LINK_COUNT; ch++) {
        brain::setOutput((uint8_t)ch, false);
        brain::setVoltageMv((uint8_t)ch, 0);
        brain::setCurrentMa((uint8_t)ch, brain::currentMaxMa());
        brain::resetFault((uint8_t)ch);
    }
    s_operEvent = s_quesEvent = 0;
    s_prevQuesCond = s_prevOperCond = 0;
}

static void doCls() {
    s_errHead = s_errCount = 0;
    s_esr = 0;
    s_quesEvent = s_operEvent = 0;
}

static int selfTestMask() {
    int m = 0;
    for (int ch = 0; ch < LINK_COUNT; ch++) {
        const ChannelStatus &s = channel_status((uint8_t)ch);
        int commsBit = (ch == 0) ? 0x01 : 0x04;
        int stBit    = (ch == 0) ? 0x02 : 0x08;
        if (!s.linkUp) m |= commsBit;
        if (s.linkUp && (s.state == ST_FAULT ||
                         (s.flags & (FLAG_ADC_FAULT | FLAG_DAC_FAULT)))) m |= stBit;
        if (s.linkUp && (s.flags & FLAG_OVERTEMP)) m |= 0x10;
    }
    return m;
}

static void handleCommon(const char *hdr) {
    // hdr has no trailing '?'; s_isQuery flags whether one was present.
    if (kwWord(hdr, "*IDN")) {
        if (s_isQuery) emit("PreReg,PSU-2CH-36V2A,SN00001,0.2.0");
        else errStd(-100);
        return;
    }
    if (kwWord(hdr, "*RST")) { doReset(); return; }
    if (kwWord(hdr, "*CLS")) { doCls();   return; }
    if (kwWord(hdr, "*OPC")) { if (s_isQuery) emit("1"); else s_esr |= 0x01; return; }
    if (kwWord(hdr, "*WAI")) { return; }   // sequential execution: no-op
    if (kwWord(hdr, "*ESR")) { if (s_isQuery) { emitf("%u", s_esr); s_esr = 0; } return; }
    if (kwWord(hdr, "*ESE")) {
        if (s_isQuery) emitf("%u", s_ese);
        else if (s_argc >= 1) s_ese = (uint8_t)strtoul(s_argv[0], nullptr, 0);
        else errStd(-109);
        return;
    }
    if (kwWord(hdr, "*SRE")) {
        if (s_isQuery) emitf("%u", s_sre);
        else if (s_argc >= 1) s_sre = (uint8_t)strtoul(s_argv[0], nullptr, 0);
        else errStd(-109);
        return;
    }
    if (kwWord(hdr, "*STB")) { if (s_isQuery) emitf("%u", statusByte()); return; }
    if (kwWord(hdr, "*TST")) { if (s_isQuery) emitf("%d", selfTestMask()); return; }
    if (kwWord(hdr, "*SAV") || kwWord(hdr, "*RCL")) {
        bool save = kwWord(hdr, "*SAV");
        if (s_argc < 1) { errStd(-109); return; }
        int n = atoi(s_argv[0]);
        if (n < 0 || n > 3) { errStd(-222); return; }
        if (save) {
            s_slot[n].used = true;
            for (int ch = 0; ch < 2; ch++) {
                s_slot[n].vMv[ch] = brain::getVoltageSetMv((uint8_t)ch);
                s_slot[n].iMa[ch] = brain::getCurrentSetMa((uint8_t)ch);
                s_slot[n].out[ch] = brain::getOutputDesired((uint8_t)ch);
            }
        } else {
            if (!s_slot[n].used) { errStd(-221); return; }
            for (int ch = 0; ch < 2; ch++) {
                brain::setVoltageMv((uint8_t)ch, s_slot[n].vMv[ch]);
                brain::setCurrentMa((uint8_t)ch, s_slot[n].iMa[ch]);
                brain::setOutput((uint8_t)ch, s_slot[n].out[ch]);
            }
        }
        return;
    }
    errStd(-113);
}

// ════════════════════════════════════════════════════════════════════════════
//  SOURce: VOLTage / CURRent
// ════════════════════════════════════════════════════════════════════════════
static void cmdVoltCurr(bool isCurr) {
    const double maxU = isCurr ? brain::currentMaxMa() / 1000.0 : brain::voltageMaxMv() / 1000.0;
    const double defU = isCurr ? maxU : 0.0;   // *RST: I=MAX, V=0

    if (s_isQuery) {
        char out[128]; out[0] = '\0';
        for (int k = 0; k < s_nch; k++) {
            int ch = s_chset[k];
            double val;
            if (s_argc >= 1) {                       // VOLT? MIN|MAX|DEF → a limit
                bool cl; double lo = 0, hi = maxU;
                if (!parseNum(s_argv[0], lo, hi, defU, &val, &cl)) { errStd(-120); return; }
            } else {
                val = isCurr ? brain::getCurrentSetMa((uint8_t)ch) / 1000.0
                             : brain::getVoltageSetMv((uint8_t)ch) / 1000.0;
            }
            char f[32]; snprintf(f, sizeof(f), "%.3f", val);
            if (k) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, f, sizeof(out) - strlen(out) - 1);
        }
        emit(out);
        return;
    }

    if (s_argc < 1) { errStd(-109); return; }
    double v; bool clamped;
    if (!parseNum(s_argv[0], 0.0, maxU, defU, &v, &clamped)) { errStd(-120); return; }
    if (clamped) errStd(-222);
    for (int k = 0; k < s_nch; k++) {
        int ch = s_chset[k];
        if (isCurr) brain::setCurrentMa((uint8_t)ch, (int32_t)lround(v * 1000.0));
        else        brain::setVoltageMv((uint8_t)ch, (int32_t)lround(v * 1000.0));
    }
}

// APPLy {CH|list},<volt>,<curr>  /  APPLy? [{CH}]
static void cmdApply() {
    if (s_isQuery) {
        int ch = s_sel;
        if (s_argc >= 1 && !parseChannel(s_argv[0], &ch)) { errStd(-224); return; }
        emitf("%.3f,%.3f", brain::getVoltageSetMv((uint8_t)ch) / 1000.0,
                           brain::getCurrentSetMa((uint8_t)ch) / 1000.0);
        return;
    }
    if (s_argc < 3) { errStd(-109); return; }
    int ch;
    if (!parseChannel(s_argv[0], &ch)) { errStd(-224); return; }
    double vmax = brain::voltageMaxMv() / 1000.0, imax = brain::currentMaxMa() / 1000.0;
    double v, i; bool c1, c2;
    if (!parseNum(s_argv[1], 0, vmax, 0, &v, &c1) ||
        !parseNum(s_argv[2], 0, imax, imax, &i, &c2)) { errStd(-120); return; }
    if (c1 || c2) errStd(-222);
    brain::setVoltageMv((uint8_t)ch, (int32_t)lround(v * 1000.0));
    brain::setCurrentMa((uint8_t)ch, (int32_t)lround(i * 1000.0));
}

// ════════════════════════════════════════════════════════════════════════════
//  OUTPut
// ════════════════════════════════════════════════════════════════════════════
static void cmdOutput() {
    if (eat("PROTection")) {
        if (!eat("CLEar")) { errStd(-113); return; }
        if (!tdone())      { errStd(-113); return; }
        for (int k = 0; k < s_nch; k++) brain::resetFault((uint8_t)s_chset[k]);
        return;
    }
    eat("STATe");                       // optional [:STATe]
    if (!tdone()) { errStd(-113); return; }

    if (s_isQuery) {
        char out[32]; out[0] = '\0';
        for (int k = 0; k < s_nch; k++) {
            int on = (channel_status((uint8_t)s_chset[k]).flags & FLAG_OUTPUT_ON) ? 1 : 0;
            char f[4]; snprintf(f, sizeof(f), "%d", on);
            if (k) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, f, sizeof(out) - strlen(out) - 1);
        }
        emit(out);
        return;
    }

    if (s_argc < 1) { errStd(-109); return; }
    bool on;
    if (!parseBool(s_argv[0], &on)) { errStd(-224); return; }
    for (int k = 0; k < s_nch; k++) {
        int ch = s_chset[k];
        if (on) {
            const ChannelStatus &s = channel_status((uint8_t)ch);
            if (s.linkUp && (s.state == ST_FAULT || (s.flags & FLAG_CAL_INVALID))) {
                errStd(106); continue;             // refuse: fault / cal invalid
            }
        }
        if (!brain::setOutput((uint8_t)ch, on) && on) errStd(106);  // OTP latched
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  INSTrument
// ════════════════════════════════════════════════════════════════════════════
static void cmdInstrument() {
    if (eat("CATalog")) { if (s_isQuery) emit("CH1,CH2"); return; }
    if (eat("NSELect")) {
        if (s_isQuery) emitf("%d", s_sel + 1);
        else if (s_argc >= 1) {
            int n = atoi(s_argv[0]);
            if (n == 1 || n == 2) s_sel = n - 1; else errStd(-222);
        } else errStd(-109);
        return;
    }
    eat("SELect");                      // optional [:SELect]
    if (!tdone()) { errStd(-113); return; }
    if (s_isQuery) emit(s_sel == 0 ? "CH1" : "CH2");
    else if (s_argc >= 1) {
        int ch; if (parseChannel(s_argv[0], &ch)) s_sel = ch; else errStd(-224);
    } else errStd(-109);
}

// ════════════════════════════════════════════════════════════════════════════
//  MEASure / FETCh
// ════════════════════════════════════════════════════════════════════════════
static void cmdMeasure(bool fetch) {
    eat("SCALar");                      // optional [:SCALar]

    // TEMPerature is a single instrument value (no channel list).
    if (eat("TEMPerature")) {
        if (!tdone() || !s_isQuery) { errStd(-113); return; }
        float t = brain::systemTempC();
        if (isnan(t)) emit("9.91E37"); else emitf("%.1f", t);
        return;
    }

    enum { M_V, M_I, M_P, M_ALL } m;
    if      (eat("VOLTage")) { eat("DC"); m = M_V; }
    else if (eat("CURRent")) { eat("DC"); m = M_I; }
    else if (eat("POWer"))   { eat("DC"); m = M_P; }
    else if (eat("ALL"))     { m = M_ALL; }
    else { errStd(-113); return; }
    if (!tdone() || !s_isQuery) { errStd(-113); return; }

    char out[256]; out[0] = '\0';
    for (int k = 0; k < s_nch; k++) {
        int ch = s_chset[k];
        if (!fetch) measForce(ch);
        const ChannelStatus &s = channel_status((uint8_t)ch);
        double v = s.v_mV / 1000.0, i = s.i_dmA / 10000.0, p = s.p_mW / 1000.0;
        char f[64];
        switch (m) {
        case M_V:   snprintf(f, sizeof(f), "%.3f", v); break;
        case M_I:   snprintf(f, sizeof(f), "%.4f", i); break;
        case M_P:   snprintf(f, sizeof(f), "%.3f", p); break;
        default:    snprintf(f, sizeof(f), "%.3f,%.4f,%.3f", v, i, p); break;
        }
        if (k) strncat(out, ",", sizeof(out) - strlen(out) - 1);
        strncat(out, f, sizeof(out) - strlen(out) - 1);
    }
    emit(out);
}

// ════════════════════════════════════════════════════════════════════════════
//  STATus
// ════════════════════════════════════════════════════════════════════════════
static void cmdStatus() {
    if (eat("PRESet")) { s_quesEnable = 0; s_operEnable = 0; return; }

    bool ques;
    if      (eat("QUEStionable")) ques = true;
    else if (eat("OPERation"))    ques = false;
    else { errStd(-113); return; }

    uint16_t &evt = ques ? s_quesEvent : s_operEvent;
    uint16_t &ena = ques ? s_quesEnable : s_operEnable;

    if (eat("CONDition")) {
        if (s_isQuery) emitf("%u", ques ? quesCondition() : operCondition());
        return;
    }
    if (eat("ENABle")) {
        if (s_isQuery) emitf("%u", ena);
        else if (s_argc >= 1) ena = (uint16_t)strtoul(s_argv[0], nullptr, 0);
        else errStd(-109);
        return;
    }
    eat("EVENt");                       // optional [:EVENt]
    if (!tdone()) { errStd(-113); return; }
    if (s_isQuery) { emitf("%u", evt); evt = 0; }   // read & clear
}

// ════════════════════════════════════════════════════════════════════════════
//  SYSTem
// ════════════════════════════════════════════════════════════════════════════
static void cmdSystemLan() {
    if (!eat("LAN")) { errStd(-113); return; }

    if (eat("MAC")) {
        if (s_isQuery) {
            uint8_t m[6]; netcfg_mac(m);
            emitf("%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        return;
    }
    if (eat("DHCP")) {
        if (s_isQuery) emitf("%d", netcfg_dhcp() ? 1 : 0);
        else if (s_argc >= 1) {
            bool on; if (!parseBool(s_argv[0], &on)) { errStd(-224); return; }
            netcfg_set_dhcp(on); netcfg_request_apply();
        } else errStd(-109);
        return;
    }
    if (eat("HOSTname")) {
        if (s_isQuery) emitf("\"%s\"", netcfg_hostname());
        else if (s_argc >= 1) { netcfg_set_hostname(s_argv[0]); netcfg_save(); }
        else errStd(-109);
        return;
    }

    // IPADdress / NETMask / GATeway / DNS — dotted-quad set/query.
    enum { A_IP, A_MASK, A_GW, A_DNS } which;
    if      (eat("IPADdress")) which = A_IP;
    else if (eat("NETMask"))   which = A_MASK;
    else if (eat("GATeway"))   which = A_GW;
    else if (eat("DNS"))       which = A_DNS;
    else { errStd(-113); return; }

    if (s_isQuery) {
        uint8_t v[4];
        switch (which) {
        case A_IP:   netcfg_live_ip(v);   break;
        case A_MASK: netcfg_live_mask(v);  break;
        case A_GW:   netcfg_live_gw(v);    break;
        default:     netcfg_get_dns(v);    break;
        }
        emitf("%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
        return;
    }
    if (s_argc < 1) { errStd(-109); return; }
    uint8_t v[4];
    if (!parseIp(s_argv[0], v)) { errStd(-224); return; }
    switch (which) {
    case A_IP:   netcfg_set_ip(v);   break;
    case A_MASK: netcfg_set_mask(v); break;
    case A_GW:   netcfg_set_gw(v);   break;
    default:     netcfg_set_dns(v);  break;
    }
    netcfg_request_apply();
}

static void cmdSystem() {
    if (eat("ERRor")) {
        if (eat("COUNt")) { if (s_isQuery) emitf("%d", s_errCount); return; }
        eat("NEXT");                    // optional [:NEXT]
        if (s_isQuery) {
            int code; char msg[SCPI_ERR_MSGLEN];
            errPop(&code, msg, sizeof(msg));
            emitf("%d,\"%s\"", code, msg);
        }
        return;
    }
    if (eat("VERSion"))  { if (s_isQuery) emit("1999.0"); return; }
    if (eat("UPTime"))   { if (s_isQuery) emitf("%lu", (unsigned long)brain::uptimeS()); return; }
    if (eat("CHANnel"))  { if (eat("COUNt") && s_isQuery) emit("2"); return; }
    if (eat("BEEPer")) {
        if (eat("STATe")) {
            if (s_isQuery) emitf("%d", brain::getBeeper() ? 1 : 0);
            else if (s_argc >= 1) { bool on; if (parseBool(s_argv[0], &on)) brain::setBeeper(on); else errStd(-224); }
            else errStd(-109);
        } else {
            eat("IMMediate");
            brain::beepOnce();
        }
        return;
    }
    if (eat("LOCal"))  { brain::setFrontPanelLock(false); return; }
    if (eat("REMote")) { return; }      // remote mode; panel stays usable
    if (eat("RWLock")) { brain::setFrontPanelLock(true);  return; }
    if (eat("COMMunicate")) { cmdSystemLan(); return; }
    errStd(-113);
}

// ════════════════════════════════════════════════════════════════════════════
//  CALibration
// ════════════════════════════════════════════════════════════════════════════
static bool parseCalPath(const char *s, uint8_t *target) {
    if (kwWord(s, "VSET"))  { *target = CAL_VSET;  return true; }
    if (kwWord(s, "ISET"))  { *target = CAL_ISET;  return true; }
    if (kwWord(s, "VMEAS")) { *target = CAL_VMEAS; return true; }
    if (kwWord(s, "IMEAS")) { *target = CAL_IMEAS; return true; }
    return false;
}

static void cmdCal() {
    if (eat("STATe")) {
        if (s_isQuery) { emitf("%d", s_calUnlocked ? 1 : 0); return; }
        if (s_argc < 1) { errStd(-109); return; }
        bool on; if (!parseBool(s_argv[0], &on)) { errStd(-224); return; }
        long code = (s_argc >= 2) ? strtol(s_argv[1], nullptr, 0) : 0;
        s_calUnlocked = on && (code == 0);   // default unlock code is 0
        if (on && code != 0) errStd(-224);
        return;
    }
    if (eat("VALid")) {
        if (!s_isQuery) { errStd(-113); return; }
        char out[16]; out[0] = '\0';
        for (int k = 0; k < s_nch; k++) {
            int ok = (channel_status((uint8_t)s_chset[k]).flags & FLAG_CAL_INVALID) ? 0 : 1;
            char f[4]; snprintf(f, sizeof(f), "%d", ok);
            if (k) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, f, sizeof(out) - strlen(out) - 1);
        }
        emit(out);
        return;
    }
    if (eat("DATA")) {                  // not retrievable over the link
        if (s_isQuery) emit("9.91E37,9.91E37");
        return;
    }

    // POINt / COMMit / RESet all require the path parameter and an unlock.
    bool isPoint  = eat("POINt");
    bool isCommit = !isPoint && eat("COMMit");
    bool isReset  = !isPoint && !isCommit && eat("RESet");
    if (!isPoint && !isCommit && !isReset) { errStd(-113); return; }

    if (!s_calUnlocked) { errPush(-221, "Calibration locked"); return; }
    if (s_argc < 1) { errStd(-109); return; }
    uint8_t target;
    if (!parseCalPath(s_argv[0], &target)) { errStd(-224); return; }
    bool isCurrPath = (target == CAL_ISET || target == CAL_IMEAS);

    if (isPoint) {
        if (s_argc < 3) { errStd(-109); return; }
        int index = atoi(s_argv[1]);
        if (index != 0 && index != 1) { errStd(-222); return; }
        double actual = strtod(s_argv[2], nullptr);
        // Units: mV for voltage paths, 0.1 mA for current paths.
        int32_t raw = isCurrPath ? (int32_t)lround(actual * 10000.0)
                                 : (int32_t)lround(actual * 1000.0);
        for (int k = 0; k < s_nch; k++)
            channel_cal_point((uint8_t)s_chset[k], target, (uint8_t)index, raw);
        return;
    }
    for (int k = 0; k < s_nch; k++) {
        if (isCommit) channel_cal_commit((uint8_t)s_chset[k], target);
        else          channel_cal_reset ((uint8_t)s_chset[k], target);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Segment dispatch
// ════════════════════════════════════════════════════════════════════════════
// Extract a trailing "(@ ... )" channel list from `args` (removing it in place).
// Fills s_chset/s_nch and returns true on success; sets s_nch from the selection
// when no list is present.
static bool extractChannelList(char *args) {
    char *p = strstr(args, "(@");
    if (!p) { chsetSelected(); return true; }
    char *q = strchr(p, ')');
    if (!q) { errStd(-109); chsetSelected(); return false; }

    int n = 0;
    char *r = p + 2;
    while (r < q && n < 2) {
        while (r < q && (*r == ',' || *r == ' ')) r++;
        if (r >= q) break;
        int v = (int)strtol(r, &r, 10);
        if (v == 1 || v == 2) s_chset[n++] = v - 1;
    }
    // Splice the "(@...)" span out of args.
    memmove(p, q + 1, strlen(q + 1) + 1);

    if (n == 0) { chsetSelected(); return true; }
    s_nch = n;
    return true;
}

// Split the residual parameter text on ',' into s_argv (trimmed, empties dropped).
static void splitArgs(char *args) {
    s_argc = 0;
    char *s = trim(args);
    if (!*s) return;
    char *tok = s;
    while (s_argc < SCPI_MAX_ARGV) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        char *f = trim(tok);
        if (*f) s_argv[s_argc++] = f;
        if (!comma) break;
        tok = comma + 1;
    }
}

static void processSegment(char *seg) {
    char *s = trim(seg);
    if (!*s) return;

    // Split header / parameters on the first whitespace.
    char *sp = s;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    char *args = (char *)"";
    if (*sp) { *sp = '\0'; args = trim(sp + 1); }
    char *header = s;

    if (!*header) return;
    if (header[0] == ':') header++;      // absolute path prefix

    // Query flag: trailing '?' on the header.
    size_t hl = strlen(header);
    s_isQuery = (hl > 0 && header[hl - 1] == '?');
    if (s_isQuery) header[hl - 1] = '\0';

    // Common '*' commands have no ':' tree; split args and dispatch directly.
    static char argbuf[SCPI_ARG_MAX];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    if (header[0] == '*') {
        if (!extractChannelList(argbuf)) return;   // (unlikely on common cmds)
        splitArgs(argbuf);
        handleCommon(header);
        return;
    }

    // Tokenize the header on ':'.
    s_ntok = 0;
    char *t = header;
    while (s_ntok < SCPI_MAX_TOK) {
        char *colon = strchr(t, ':');
        if (colon) *colon = '\0';
        if (*t == '\0') { errStd(-113); return; }   // empty token (e.g. "VOLT::LEV")
        s_tok[s_ntok++] = t;
        if (!colon) break;
        t = colon + 1;
    }
    s_ti = 0;

    if (!extractChannelList(argbuf)) return;
    splitArgs(argbuf);

    // Optional :SOURce root, then VOLTage/CURRent.
    bool hadSource = eat("SOURce");
    if (at("VOLTage") || at("CURRent")) {
        bool isCurr = at("CURRent"); s_ti++;
        eat("LEVel"); eat("IMMediate"); eat("AMPLitude");
        if (!tdone()) { errStd(-113); return; }
        cmdVoltCurr(isCurr);
        return;
    }
    if (hadSource) { errStd(-113); return; }

    if (eat("APPLy"))       { if (!tdone()) { errStd(-113); return; } cmdApply();      return; }
    if (eat("OUTPut"))      { cmdOutput();     return; }
    if (eat("MEASure"))     { cmdMeasure(false); return; }
    if (eat("FETCh"))       { cmdMeasure(true);  return; }
    if (eat("INSTrument"))  { cmdInstrument();  return; }
    if (eat("STATus"))      { cmdStatus();      return; }
    if (eat("SYSTem"))      { cmdSystem();      return; }
    if (eat("CALibration")) { cmdCal();         return; }

    errStd(-113);
}

// ════════════════════════════════════════════════════════════════════════════
//  Public API
// ════════════════════════════════════════════════════════════════════════════
void scpi_init() {
    s_sel = 0;
    s_quesEnable = s_quesEvent = s_operEnable = s_operEvent = 0;
    s_esr = s_ese = s_sre = 0;
    s_calUnlocked = false;
    s_errHead = s_errCount = 0;
    memset(s_slot, 0, sizeof(s_slot));
    for (int i = 0; i < LINK_COUNT; i++)
        s_prevChEvent[i] = channel_status((uint8_t)i).lastEvent;
    s_prevQuesCond = s_prevOperCond = 0;
}

void scpi_process_line(const char *line, ScpiWriteFn write, void *ctx) {
    if (!line) return;
    // Work on a mutable copy (we tokenize in place).
    static char work[SCPI_ARG_MAX + 64];
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    respReset();

    // Execute each ';'-separated command as an absolute command.
    char *seg = work;
    while (seg) {
        char *semi = strchr(seg, ';');
        if (semi) *semi = '\0';
        processSegment(seg);
        seg = semi ? semi + 1 : nullptr;
    }

    if (s_respHas && write) {
        // Terminate directly rather than via respRaw(): once the buffer is full
        // respRaw() drops what it is given, so a ';'-chained line whose combined
        // response exceeds SCPI_RESP_MAX used to go out with no trailing LF at
        // all, and the client waited forever for a line that never ended. The
        // response is still truncated in that case - it does not fit, and the
        // transport is line-oriented - but it is always a complete line.
        if (s_respLen > SCPI_RESP_MAX - 2) s_respLen = SCPI_RESP_MAX - 2;
        s_resp[s_respLen++] = '\n';
        s_resp[s_respLen]   = '\0';
        write(ctx, s_resp, (size_t)s_respLen);
    }
}

void scpi_report_line_overflow(ScpiWriteFn write, void *ctx) {
    (void)write; (void)ctx;
    errStd(-363);
}

void scpi_task() {
    // Fold asynchronous channel events into the error queue.
    for (int ch = 0; ch < LINK_COUNT; ch++) {
        uint8_t ev = channel_status((uint8_t)ch).lastEvent;
        if (ev == s_prevChEvent[ch]) continue;
        s_prevChEvent[ch] = ev;
        switch (ev) {
        case EVT_SELFTEST_FAIL: errStd(101); break;
        case EVT_OVERTEMP:      errStd(102); break;
        case EVT_ADC_FAULT:     errStd(103); break;
        case EVT_DAC_FAULT:     errStd(104); break;
        case EVT_CAL_INVALID:   errStd(105); break;
        case EVT_COMMS_TIMEOUT: errStd(107); break;
        default: break;         // BOOT / OVERTEMP_CLEAR / CAL_STORED: informational
        }
    }

    // Latch positive transitions of the live conditions into the event registers.
    uint16_t q = quesCondition();
    s_quesEvent |= (uint16_t)(q & ~s_prevQuesCond);
    s_prevQuesCond = q;
    uint16_t o = operCondition();
    s_operEvent |= (uint16_t)(o & ~s_prevOperCond);
    s_prevOperCond = o;
}
