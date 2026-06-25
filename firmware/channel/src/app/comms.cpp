#include <Arduino.h>
#include <string.h>

#include "comms.h"
#include "config.h"
#include "protocol.h"
#include "state.h"
#include "setpoint.h"
#include "measure.h"
#include "calibration.h"
#include "fsm.h"

/* ---- Outbound framing ---------------------------------------------------- */
static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t plen)
{
    uint8_t frame[PROTO_MAX_FRAME];
    const uint8_t len = (uint8_t)(plen + 1);     /* CMD + payload */

    frame[0] = PROTO_SOF;
    frame[1] = len;
    frame[2] = cmd;
    if (plen) memcpy(&frame[3], payload, plen);

    /* CRC over [LEN, CMD, payload...] */
    frame[3 + plen] = proto_crc8(&frame[1], (uint8_t)(len + 1));

    BRAIN_UART.write(frame, (size_t)(plen + 4));
}

static void send_ack(uint8_t cmd)
{
    uint8_t p[1] = { cmd };
    send_frame(CMD_ACK, p, 1);
}

static void send_nack(uint8_t cmd, uint8_t reason)
{
    uint8_t p[2] = { cmd, reason };
    send_frame(CMD_NACK, p, 2);
}

void comms_send_event(uint8_t code)
{
    uint8_t p[1] = { code };
    send_frame(CMD_EVENT, p, 1);
}

void comms_send_telemetry()
{
    uint8_t p[TELEMETRY_PAYLOAD_LEN];
    proto_put_i32(&p[0],  g_state.meas_v_mV);
    proto_put_i32(&p[4],  g_state.meas_i_mA);
    proto_put_i32(&p[8],  g_state.meas_p_mW);
    proto_put_i16(&p[12], g_state.temp_cC);
    proto_put_u8 (&p[14], g_state.run_state);
    proto_put_u8 (&p[15], g_state.flags);
    send_frame(CMD_TELEMETRY, p, TELEMETRY_PAYLOAD_LEN);
}

/* ---- Command dispatch ---------------------------------------------------- */
static void handle(uint8_t cmd, const uint8_t *pl, uint8_t plen)
{
    switch (cmd) {
    case CMD_PING:
        send_ack(CMD_PING);
        break;

    case CMD_SET_VOLTAGE:
        if (plen < 4) { send_nack(cmd, NACK_BAD_LEN); break; }
        setpoint_apply_voltage(proto_get_i32(pl));
        send_ack(cmd);
        break;

    case CMD_SET_CURRENT:
        if (plen < 4) { send_nack(cmd, NACK_BAD_LEN); break; }
        setpoint_apply_current(proto_get_i32(pl));
        send_ack(cmd);
        break;

    case CMD_SET_OUTPUT:
        if (plen < 1) { send_nack(cmd, NACK_BAD_LEN); break; }
        if (!fsm_request_output(pl[0] != 0)) { send_nack(cmd, NACK_WRONG_STATE); break; }
        send_ack(cmd);
        break;

    case CMD_GET_STATUS:
        comms_send_telemetry();
        break;

    case CMD_CAL_POINT: {
        if (plen < 6) { send_nack(cmd, NACK_BAD_LEN); break; }
        const uint8_t target = pl[0];
        const uint8_t index  = pl[1];
        const int32_t actual = proto_get_i32(&pl[2]);
        if (target >= CAL_COUNT || index > 1) { send_nack(cmd, NACK_BAD_PARAM); break; }

        if (target == CAL_VSET || target == CAL_ISET) {
            /* x = externally measured value, y = the code we drove */
            cal_record_point(target, index, (float)actual,
                             (float)setpoint_last_code(target));
        } else {
            /* x = our raw ADC volts, y = externally measured value */
            cal_record_point(target, index, measure_last_vadc(target),
                             (float)actual);
        }
        send_ack(cmd);
        break;
    }

    case CMD_CAL_COMMIT:
        if (plen < 1) { send_nack(cmd, NACK_BAD_LEN); break; }
        if (cal_commit(pl[0])) {
            send_ack(cmd);
            comms_send_event(EVT_CAL_STORED);
        } else {
            send_nack(cmd, NACK_BAD_PARAM);
        }
        break;

    case CMD_CAL_RESET:
        if (plen < 1) { send_nack(cmd, NACK_BAD_LEN); break; }
        cal_reset(pl[0]);
        send_ack(cmd);
        break;

    case CMD_RESET_FAULT:
        fsm_reset_fault();
        send_ack(cmd);
        break;

    default:
        send_nack(cmd, NACK_UNKNOWN_CMD);
        break;
    }
}

/* ---- Inbound frame parser (byte-at-a-time state machine) ----------------- */
enum RxState { RX_SOF, RX_LEN, RX_DATA, RX_CRC };

static RxState s_st;
static uint8_t s_len;                      /* CMD + payload length */
static uint8_t s_idx;
static uint8_t s_buf[PROTO_MAX_PAYLOAD + 1];

void comms_init()
{
    s_st = RX_SOF;
    s_idx = 0;
    s_len = 0;
}

void comms_task()
{
    while (BRAIN_UART.available()) {
        const uint8_t b = (uint8_t)BRAIN_UART.read();

        switch (s_st) {
        case RX_SOF:
            if (b == PROTO_SOF) s_st = RX_LEN;
            break;

        case RX_LEN:
            if (b < 1 || b > (PROTO_MAX_PAYLOAD + 1)) {
                s_st = RX_SOF;             /* implausible length, resync */
            } else {
                s_len = b;
                s_idx = 0;
                s_st = RX_DATA;
            }
            break;

        case RX_DATA:
            s_buf[s_idx++] = b;
            if (s_idx >= s_len) s_st = RX_CRC;
            break;

        case RX_CRC: {
            /* Recompute CRC over [LEN, CMD, payload...] */
            uint8_t tmp[PROTO_MAX_PAYLOAD + 2];
            tmp[0] = s_len;
            memcpy(&tmp[1], s_buf, s_len);
            const uint8_t crc = proto_crc8(tmp, (uint8_t)(s_len + 1));

            if (crc == b) {
                handle(s_buf[0], &s_buf[1], (uint8_t)(s_len - 1));
            } else {
                send_nack(s_buf[0], NACK_BAD_CRC);
            }
            s_st = RX_SOF;
            break;
        }
        }
    }
}
