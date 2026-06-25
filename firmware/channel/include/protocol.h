/**
 * protocol.h - Binary UART protocol shared by the BRAIN (RP2350) and the
 *              CHANNEL (ATtiny1614).
 *
 * This header is intentionally dependency-free (only <stdint.h>) and pure C
 * so the exact same file can be dropped into the Brain firmware. Keep the two
 * copies in sync.
 *
 * Frame layout (little-endian payload):
 *
 *     +------+------+------+===========+------+
 *     | SOF  | LEN  | CMD  |  payload  | CRC8 |
 *     +------+------+------+===========+------+
 *       0xA5   1B     1B      LEN-1 B     1B
 *
 *   SOF : start-of-frame marker (0xA5)
 *   LEN : number of bytes in [CMD .. last payload byte]  (i.e. 1 + payload)
 *   CMD : command id (see enum below)
 *   CRC : CRC-8 (poly 0x07, init 0x00) computed over LEN, CMD and payload
 *
 * Max payload keeps frames small enough for the ATtiny's RAM.
 */
#ifndef CHANNEL_PROTOCOL_H
#define CHANNEL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SOF            0xA5u
#define PROTO_MAX_PAYLOAD    16u      /* bytes after CMD                     */
#define PROTO_MAX_FRAME      (PROTO_MAX_PAYLOAD + 4u) /* SOF+LEN+CMD+pl+CRC  */

/* ---- Command IDs -------------------------------------------------------- */
/* Brain -> Channel */
enum {
    CMD_PING          = 0x01,  /* no payload                                 */
    CMD_SET_VOLTAGE   = 0x10,  /* int32 mV                                   */
    CMD_SET_CURRENT   = 0x11,  /* int32 mA                                   */
    CMD_SET_OUTPUT    = 0x12,  /* uint8 (0=off, 1=on)                        */
    CMD_GET_STATUS    = 0x20,  /* no payload -> replies CMD_TELEMETRY        */
    CMD_CAL_POINT     = 0x30,  /* uint8 target, uint8 index, int32 actual    */
    CMD_CAL_COMMIT    = 0x31,  /* uint8 target                               */
    CMD_CAL_RESET     = 0x32,  /* uint8 target (restore defaults)            */
    CMD_RESET_FAULT   = 0x40,  /* no payload                                 */

    /* Channel -> Brain */
    CMD_TELEMETRY     = 0x80,  /* see telemetry layout below                 */
    CMD_ACK           = 0x81,  /* uint8 acked_cmd                            */
    CMD_NACK          = 0x82,  /* uint8 acked_cmd, uint8 reason              */
    CMD_EVENT         = 0x83   /* uint8 event_code                           */
};

/* ---- Calibration targets ------------------------------------------------ */
enum {
    CAL_VSET  = 0,   /* set-voltage path  (DAC VOUT0) */
    CAL_ISET  = 1,   /* set-current path  (DAC VOUT1) */
    CAL_VMEAS = 2,   /* measure-voltage path (ADS1118 AIN0-AIN1) */
    CAL_IMEAS = 3,   /* measure-current path (ADS1118 AIN2-AIN3) */
    CAL_COUNT = 4
};

/* ---- NACK reasons ------------------------------------------------------- */
enum {
    NACK_BAD_CRC      = 1,
    NACK_BAD_LEN      = 2,
    NACK_UNKNOWN_CMD  = 3,
    NACK_BAD_PARAM    = 4,
    NACK_WRONG_STATE  = 5
};

/* ---- Event / error codes (CMD_EVENT) ------------------------------------ */
enum {
    EVT_BOOT            = 1,
    EVT_SELFTEST_FAIL   = 2,
    EVT_OVERTEMP        = 3,
    EVT_OVERTEMP_CLEAR  = 4,
    EVT_ADC_FAULT       = 5,
    EVT_DAC_FAULT       = 6,
    EVT_CAL_STORED      = 7,
    EVT_CAL_INVALID     = 8
};

/* ---- Channel run-state (telemetry 'state' byte) ------------------------- */
enum {
    ST_INIT      = 0,
    ST_SELFTEST  = 1,
    ST_IDLE      = 2,   /* output off, healthy */
    ST_RUN       = 3,   /* output on           */
    ST_FAULT     = 4    /* latched fault       */
};

/* ---- Telemetry status-flag bits (telemetry 'flags' byte) ---------------- */
#define FLAG_OUTPUT_ON     0x01u
#define FLAG_OVERTEMP      0x02u
#define FLAG_CAL_INVALID   0x04u
#define FLAG_ADC_FAULT     0x08u
#define FLAG_DAC_FAULT     0x10u
#define FLAG_CC_MODE       0x20u  /* reserved: in constant-current limiting */

/*
 * Telemetry payload (CMD_TELEMETRY), little-endian, 14 bytes:
 *   int32  v_mV       output voltage (millivolts)
 *   int32  i_mA       output current (milliamps)
 *   int32  p_mW       output power   (milliwatts)   -- redundant convenience
 *   int16  temp_cC    temperature    (centi-Celsius, e.g. 2531 = 25.31 C)
 *   uint8  state      ST_*
 *   uint8  flags      FLAG_*
 * (the p_mW field is included to save the Brain a multiply; 16 bytes total)
 */
#define TELEMETRY_PAYLOAD_LEN  16u

/* ---- CRC-8 (poly 0x07, init 0x00) --------------------------------------- */
static inline uint8_t proto_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00u;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ---- Little-endian (de)serialization helpers ---------------------------- */
static inline void proto_put_u8 (uint8_t *p, uint8_t v)  { p[0] = v; }
static inline void proto_put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void proto_put_i16(uint8_t *p, int16_t v)  { proto_put_u16(p,(uint16_t)v); }
static inline void proto_put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline void proto_put_i32(uint8_t *p, int32_t v)  { proto_put_u32(p,(uint32_t)v); }

static inline uint8_t  proto_get_u8 (const uint8_t *p) { return p[0]; }
static inline uint16_t proto_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static inline int16_t  proto_get_i16(const uint8_t *p) { return (int16_t)proto_get_u16(p); }
static inline uint32_t proto_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline int32_t  proto_get_i32(const uint8_t *p) { return (int32_t)proto_get_u32(p); }

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_PROTOCOL_H */
