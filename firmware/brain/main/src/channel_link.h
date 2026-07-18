/**
 * channel_link.h - Brain-side UART link to the two per-channel ATtiny1614
 *                  boards, using the shared framed protocol (protocol.h).
 *
 * Wiring (RP2350, see platformio.ini / Control.kicad_sch):
 *   CH1 = UART0 (Serial1):  TX GPIO0 -> Rx_Ch1,  RX GPIO1 <- Tx_Ch1
 *   CH2 = UART1 (Serial2):  TX GPIO4 -> Rx_Ch2,  RX GPIO5 <- Tx_Ch2
 * Both run at 1 Mbps to match the channel's BRAIN_BAUD.
 *
 * The channel pushes a CMD_TELEMETRY frame every ~100 ms unsolicited, so this
 * module mostly listens: channel_link_task() drains both UARTs, decodes frames
 * and keeps a per-channel status snapshot. A link is flagged "down" when no
 * valid telemetry has arrived within LINK_TIMEOUT_MS. Commands (set voltage /
 * current / output, calibration, fault reset) are one-shot sends.
 */
#ifndef BRAIN_CHANNEL_LINK_H
#define BRAIN_CHANNEL_LINK_H

#include <stdint.h>

/* Channel indices (also the array index into the status table). */
enum { LINK_CH1 = 0, LINK_CH2 = 1, LINK_COUNT = 2 };

/* A link is considered down if no telemetry arrives within this window
 * (5 missed 100 ms telemetry pushes). */
#define LINK_TIMEOUT_MS   500u

/* How often the Brain pings each channel. This is the channel's comms-loss
 * watchdog heartbeat: the channel trips its output off if it hears nothing
 * for COMMS_TIMEOUT_MS (1000 ms), so pinging at 250 ms keeps a healthy but
 * idle link fed with ~4 missed-ping margin. Must stay well below the
 * channel's COMMS_TIMEOUT_MS. */
#define LINK_HEARTBEAT_MS 250u

/* Latest snapshot for one channel: decoded telemetry plus link health. */
struct ChannelStatus {
    /* --- decoded CMD_TELEMETRY --- */
    int32_t  v_mV;        /* measured output voltage (mV)                 */
    int32_t  i_dmA;       /* measured output current (0.1 mA units)       */
    int32_t  p_mW;        /* measured output power   (mW)                 */
    int16_t  temp_cC;     /* temperature (centi-Celsius, 2531 = 25.31 C)  */
    uint8_t  state;       /* ST_* run-state                               */
    uint8_t  flags;       /* FLAG_* status bits                           */

    /* --- per-channel settings (from CMD_SETTINGS) --- */
    uint8_t  avgV;        /* voltage measurement averaging window (1..32)  */
    uint8_t  avgI;        /* current measurement averaging window (1..32)  */
    bool     avgValid;    /* true once CMD_SETTINGS has been received       */

    /* --- link health / bookkeeping --- */
    bool     linkUp;      /* telemetry seen within LINK_TIMEOUT_MS        */
    uint32_t lastRxMs;    /* millis() of the last valid frame             */
    uint32_t framesRx;    /* total valid frames received                 */
    uint32_t crcErrors;   /* frames dropped on a bad CRC                  */
    uint32_t rxOverflows; /* times the UART RX buffer overflowed (drops)  */

    /* --- last async notifications --- */
    uint8_t  lastEvent;       /* last EVT_* code (0 = none yet)           */
    uint8_t  lastAckCmd;      /* CMD id of the last CMD_ACK               */
    uint8_t  lastNackCmd;     /* CMD id of the last CMD_NACK              */
    uint8_t  lastNackReason;  /* NACK_* reason of the last CMD_NACK       */
};

/* Configure the two UARTs (pins + 1 Mbps) and reset the parsers/state. */
void channel_link_init();

/* Drain both UART RX buffers, decode frames and update link-timeout flags.
 * Call every loop iteration. */
void channel_link_task();

/* --- Brain -> Channel commands (fire-and-forget, ACK tracked in status) --- */
void channel_set_voltage(uint8_t ch, int32_t mV);
void channel_set_current(uint8_t ch, int32_t mA);
void channel_set_output (uint8_t ch, bool on);
/* Set a measurement averaging window (which = AVG_V / AVG_I, count 1..32).
 * Updates the cached ChannelStatus optimistically so the UI reflects it at once;
 * the channel confirms with a CMD_SETTINGS reply. */
void channel_set_avg    (uint8_t ch, uint8_t which, uint8_t count);
void channel_ping       (uint8_t ch);
void channel_get_status (uint8_t ch);
void channel_get_settings(uint8_t ch);
void channel_reset_fault(uint8_t ch);
void channel_cal_point  (uint8_t ch, uint8_t target, uint8_t index, int32_t actual);
void channel_cal_commit (uint8_t ch, uint8_t target);
void channel_cal_reset  (uint8_t ch, uint8_t target);

/* Read-only access to the latest snapshot for channel `ch` (0/1). */
const ChannelStatus &channel_status(uint8_t ch);

#endif /* BRAIN_CHANNEL_LINK_H */
