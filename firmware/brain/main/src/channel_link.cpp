#include <Arduino.h>
#include <string.h>

#include "channel_link.h"
#include "protocol.h"

// ── UART assignment ─────────────────────────────────────────────────────────────
// Serial1 = RP2350 UART0 (default TX GPIO0 / RX GPIO1) -> CH1
// Serial2 = RP2350 UART1 (default TX GPIO4 / RX GPIO5) -> CH2
// Pins match the board (Control.kicad_sch), but we set them explicitly so a core
// default change can't silently move the link.
#define LINK_BAUD   1000000UL   // 1 Mbps, matches channel config.h BRAIN_BAUD

// RP2350 UART RX ring-buffer size (bytes). Precautionary: the default core
// buffer is only 32 bytes, and the blocking per-byte SSD1322 flush in loop()
// stalls UART draining for milliseconds at a time; 256 bytes buffers ~12
// telemetry frames of stall so a long block can never overrun the ring. (In
// practice overflow was never observed at 32 either -- the "Comm FAIL" blips
// were a timeout-underflow bug in channel_link_task(), fixed there -- but the
// headroom is cheap insurance for this blocking-flush design.)
#define LINK_RX_FIFO_BYTES  256

#define CH1_TX_PIN  0
#define CH1_RX_PIN  1
#define CH2_TX_PIN  4
#define CH2_RX_PIN  5

static HardwareSerial *s_uart[LINK_COUNT];
static ChannelStatus   s_status[LINK_COUNT];

// ── Inbound frame parser (one byte-at-a-time state machine per link) ─────────────
enum RxState { RX_SOF, RX_LEN, RX_DATA, RX_CRC };

struct RxCtx {
    RxState st;
    uint8_t len;                       // CMD + payload length
    uint8_t idx;
    uint8_t buf[PROTO_MAX_PAYLOAD + 1];
};
static RxCtx s_rx[LINK_COUNT];

// ── Outbound framing ─────────────────────────────────────────────────────────────
static void link_send(uint8_t ch, uint8_t cmd, const uint8_t *payload, uint8_t plen)
{
    if (ch >= LINK_COUNT || plen > PROTO_MAX_PAYLOAD) return;

    uint8_t frame[PROTO_MAX_FRAME];
    const uint8_t len = (uint8_t)(plen + 1);        // CMD + payload

    frame[0] = PROTO_SOF;
    frame[1] = len;
    frame[2] = cmd;
    if (plen) memcpy(&frame[3], payload, plen);

    // CRC over [LEN, CMD, payload...]
    frame[3 + plen] = proto_crc8(&frame[1], (uint8_t)(len + 1));

    s_uart[ch]->write(frame, (size_t)(plen + 4));
}

// ── Command API ──────────────────────────────────────────────────────────────────
void channel_set_voltage(uint8_t ch, int32_t mV)
{
    uint8_t p[4];
    proto_put_i32(p, mV);
    link_send(ch, CMD_SET_VOLTAGE, p, 4);
}

void channel_set_current(uint8_t ch, int32_t mA)
{
    uint8_t p[4];
    proto_put_i32(p, mA);
    link_send(ch, CMD_SET_CURRENT, p, 4);
}

void channel_set_output(uint8_t ch, bool on)
{
    uint8_t p[1] = { (uint8_t)(on ? 1 : 0) };
    link_send(ch, CMD_SET_OUTPUT, p, 1);
}

void channel_set_avg(uint8_t ch, uint8_t which, uint8_t count)
{
    if (ch >= LINK_COUNT || which >= AVG_COUNT) return;
    if (count < AVG_MIN) count = AVG_MIN;
    if (count > AVG_MAX) count = AVG_MAX;

    // Optimistic local update so the menu shows the new value immediately; the
    // channel's CMD_SETTINGS reply will reconcile if it clamped differently.
    if (which == AVG_V) s_status[ch].avgV = count;
    else                s_status[ch].avgI = count;
    s_status[ch].avgValid = true;

    uint8_t p[2] = { which, count };
    link_send(ch, CMD_SET_AVG, p, 2);
}

void channel_ping(uint8_t ch)          { link_send(ch, CMD_PING,         nullptr, 0); }
void channel_get_status(uint8_t ch)    { link_send(ch, CMD_GET_STATUS,   nullptr, 0); }
void channel_get_settings(uint8_t ch)  { link_send(ch, CMD_GET_SETTINGS, nullptr, 0); }
void channel_reset_fault(uint8_t ch)   { link_send(ch, CMD_RESET_FAULT,  nullptr, 0); }

void channel_cal_point(uint8_t ch, uint8_t target, uint8_t index, int32_t actual)
{
    uint8_t p[6];
    p[0] = target;
    p[1] = index;
    proto_put_i32(&p[2], actual);
    link_send(ch, CMD_CAL_POINT, p, 6);
}

void channel_cal_commit(uint8_t ch, uint8_t target)
{
    uint8_t p[1] = { target };
    link_send(ch, CMD_CAL_COMMIT, p, 1);
}

void channel_cal_reset(uint8_t ch, uint8_t target)
{
    uint8_t p[1] = { target };
    link_send(ch, CMD_CAL_RESET, p, 1);
}

// ── Inbound frame handling ───────────────────────────────────────────────────────
static void handle_frame(uint8_t ch, uint8_t cmd, const uint8_t *pl, uint8_t plen)
{
    ChannelStatus &st = s_status[ch];

    switch (cmd) {
    case CMD_TELEMETRY: {
        if (plen < TELEMETRY_PAYLOAD_LEN) return;    // malformed, ignore
        const bool wasUp = st.linkUp;
        st.v_mV    = proto_get_i32(&pl[0]);
        st.i_dmA   = proto_get_i32(&pl[4]);
        st.p_mW    = proto_get_i32(&pl[8]);
        st.temp_cC = proto_get_i16(&pl[12]);
        st.state   = proto_get_u8 (&pl[14]);
        st.flags   = proto_get_u8 (&pl[15]);
        st.linkUp   = true;
        st.lastRxMs = millis();
        st.framesRx++;
        // On a fresh (or recovered) link, pull the channel's stored settings so
        // the menu shows the value the channel actually persisted, not a guess.
        if (!wasUp || !st.avgValid) channel_get_settings(ch);
        break;
    }

    case CMD_SETTINGS:
        if (plen < SETTINGS_PAYLOAD_LEN) return;      // malformed, ignore
        st.avgV     = proto_get_u8(&pl[0]);
        st.avgI     = proto_get_u8(&pl[1]);
        st.avgValid = true;
        break;

    case CMD_ACK:
        if (plen >= 1) st.lastAckCmd = pl[0];
        break;

    case CMD_NACK:
        if (plen >= 2) { st.lastNackCmd = pl[0]; st.lastNackReason = pl[1]; }
        break;

    case CMD_EVENT:
        if (plen >= 1) st.lastEvent = pl[0];
        break;

    default:
        break;   // unknown reply id -> ignore
    }
}

// Feed one received byte through channel `ch`'s parser.
static void rx_byte(uint8_t ch, uint8_t b)
{
    RxCtx &rx = s_rx[ch];

    switch (rx.st) {
    case RX_SOF:
        if (b == PROTO_SOF) rx.st = RX_LEN;
        break;

    case RX_LEN:
        if (b < 1 || b > (PROTO_MAX_PAYLOAD + 1)) {
            rx.st = RX_SOF;                 // implausible length, resync
        } else {
            rx.len = b;
            rx.idx = 0;
            rx.st  = RX_DATA;
        }
        break;

    case RX_DATA:
        rx.buf[rx.idx++] = b;
        if (rx.idx >= rx.len) rx.st = RX_CRC;
        break;

    case RX_CRC: {
        // Recompute CRC over [LEN, CMD, payload...]
        uint8_t tmp[PROTO_MAX_PAYLOAD + 2];
        tmp[0] = rx.len;
        memcpy(&tmp[1], rx.buf, rx.len);
        const uint8_t crc = proto_crc8(tmp, (uint8_t)(rx.len + 1));

        if (crc == b) {
            handle_frame(ch, rx.buf[0], &rx.buf[1], (uint8_t)(rx.len - 1));
        } else {
            s_status[ch].crcErrors++;
        }
        rx.st = RX_SOF;
        break;
    }
    }
}

// ── Public lifecycle ─────────────────────────────────────────────────────────────
void channel_link_init()
{
    s_uart[LINK_CH1] = &Serial1;
    s_uart[LINK_CH2] = &Serial2;

    memset(s_status, 0, sizeof(s_status));
    memset(s_rx,     0, sizeof(s_rx));
    for (int i = 0; i < LINK_COUNT; i++) s_rx[i].st = RX_SOF;

    // Enlarge the RX ring *before* begin() (the core allocates it there).
    Serial1.setFIFOSize(LINK_RX_FIFO_BYTES);
    Serial1.setTX(CH1_TX_PIN);
    Serial1.setRX(CH1_RX_PIN);
    Serial1.begin(LINK_BAUD);

    Serial2.setFIFOSize(LINK_RX_FIFO_BYTES);
    Serial2.setTX(CH2_TX_PIN);
    Serial2.setRX(CH2_RX_PIN);
    Serial2.begin(LINK_BAUD);
}

void channel_link_task()
{
    for (uint8_t ch = 0; ch < LINK_COUNT; ch++) {
        // Poll (and clear) the core's RX-overflow flag every pass; it latches
        // whenever the ring filled and bytes were dropped since the last call.
        // overflow() lives on the concrete SerialUART, not the HardwareSerial
        // base that s_uart[] is typed as; the pointers are always SerialUARTs.
        if (static_cast<SerialUART *>(s_uart[ch])->overflow())
            s_status[ch].rxOverflows++;

        // Bounded drain so a flood on one link can't starve the UI loop.
        int budget = 64;
        while (budget-- > 0 && s_uart[ch]->available())
            rx_byte(ch, (uint8_t)s_uart[ch]->read());

        // Link-timeout: no valid telemetry within the window -> mark down.
        // Sample the clock *after* draining: a frame parsed just above set
        // lastRxMs to a millis() that can be a tick later than any value read
        // at the top of this call. With unsigned math a lastRxMs in that tiny
        // "future" underflows to ~4e9 and spuriously trips the timeout (brief
        // Comm FAIL on a healthy link). Signed elapsed reads ~0 in that case
        // and only goes positive on a genuine gap.
        const int32_t sinceRx = (int32_t)(millis() - s_status[ch].lastRxMs);
        if (s_status[ch].linkUp && sinceRx > (int32_t)LINK_TIMEOUT_MS) {
            s_status[ch].linkUp = false;
        }
    }
}

const ChannelStatus &channel_status(uint8_t ch)
{
    return s_status[ch < LINK_COUNT ? ch : 0];
}
