/**
 * settings.h - Persistent per-channel runtime settings (currently the moving-
 *              average window lengths for the V and I measurements).
 *
 * The values live in their own EEPROM blob (see storage.*), independent of the
 * calibration coefficients, and are pushed/queried by the Brain over the UART
 * protocol (CMD_SET_AVG / CMD_GET_SETTINGS).
 */
#ifndef CHANNEL_SETTINGS_H
#define CHANNEL_SETTINGS_H

#include <stdint.h>
#include "protocol.h"   /* AVG_* selectors, AVG_MIN/AVG_MAX */

/* Load from EEPROM, or seed defaults (and persist) if the blob is invalid. */
void settings_init();

/* Current averaging window length for AVG_V / AVG_I. Returns AVG_MIN for an
 * out-of-range selector. */
uint8_t settings_avg(uint8_t which);

/* Set the averaging window for AVG_V / AVG_I, clamped to AVG_MIN..AVG_MAX, and
 * persist if it changed. Returns false only if `which` is not a valid selector. */
bool settings_set_avg(uint8_t which, uint8_t count);

/* Current over-temperature trip point in whole degrees Celsius
 * (OTP_MIN_C..OTP_MAX_C). Read every thermal cycle by thermal_task(). */
uint8_t settings_otp_c();

/* Set the over-temperature trip point, clamped to OTP_MIN_C..OTP_MAX_C, and
 * persist if it changed. Always accepts (clamps out-of-range values). */
void settings_set_otp_c(uint8_t trip_c);

#endif /* CHANNEL_SETTINGS_H */
