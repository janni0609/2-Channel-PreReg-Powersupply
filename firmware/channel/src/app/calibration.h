/**
 * calibration.h - 2-point linear calibration for the four signal paths.
 *
 * Every path is modelled as  y = gain * x + offset.  The meaning of x/y is
 * per-target but the math is identical:
 *
 *   CAL_VSET / CAL_ISET (set paths):   x = desired engineering value (mV/mA)
 *                                      y = DAC code to write
 *   CAL_VMEAS / CAL_IMEAS (meas paths):x = raw ADC voltage (volts)
 *                                      y = engineering value (mV/mA)
 *
 * Calibration captures two (x,y) points and solves the line. Coefficients live
 * in EEPROM (see storage.*) and survive power cycles.
 */
#ifndef CHANNEL_CALIBRATION_H
#define CHANNEL_CALIBRATION_H

#include <stdint.h>
#include "protocol.h"   /* CAL_* targets, CAL_COUNT */

struct CalCoeff {
    float gain;
    float offset;
};

/* Load coefficients from EEPROM, or install defaults if storage is invalid.
 * Sets/clears FLAG_CAL_INVALID accordingly. */
void  cal_init();

/* y = gain*x + offset for the given target. */
float cal_apply(uint8_t target, float x);

/* Record one of the two calibration points (index 0 or 1). */
void  cal_record_point(uint8_t target, uint8_t index, float x, float y);

/* Solve gain/offset from the two recorded points and persist. Returns false
 * if the points are missing or degenerate (equal x). */
bool  cal_commit(uint8_t target);

/* Restore one target to its compile-time default and persist. */
void  cal_reset(uint8_t target);

/* True if the coefficients loaded from EEPROM were valid at boot. */
bool  cal_is_valid();

#endif /* CHANNEL_CALIBRATION_H */
