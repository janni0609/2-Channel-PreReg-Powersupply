/**
 * setpoint.h - Applies voltage/current setpoints to the DAC through the
 *              calibration map, and remembers the last code written (needed
 *              when calibrating the set paths).
 */
#ifndef CHANNEL_SETPOINT_H
#define CHANNEL_SETPOINT_H

#include <stdint.h>

void setpoint_init();

/* Apply setpoints (engineering units). Values are clamped to config limits. */
void setpoint_apply_voltage(int32_t mV);
void setpoint_apply_current(int32_t mA);

/* Force both DAC channels to zero (used when the output is disabled/faulted). */
void setpoint_zero();

/* Last DAC code written for a set target (CAL_VSET / CAL_ISET), for use as the
 * y-value when calibrating that path. */
uint16_t setpoint_last_code(uint8_t target);

#endif /* CHANNEL_SETPOINT_H */
