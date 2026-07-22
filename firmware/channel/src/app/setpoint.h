/**
 * setpoint.h - Applies voltage/current setpoints to the DAC through the
 *              calibration map, and remembers the last code written (needed
 *              when calibrating the set paths).
 */
#ifndef CHANNEL_SETPOINT_H
#define CHANNEL_SETPOINT_H

#include <stdint.h>

void setpoint_init();

/* Apply setpoints (engineering units). Values are clamped to config limits and
 * always remembered in g_state, but the Vset DAC is only written while the
 * output is on; while off Vset is held at 0 (see setpoint_park()). */
void setpoint_apply_voltage(int32_t mV);
void setpoint_apply_current(int32_t mA);

/* Drive one DAC channel to its currently remembered setpoint. output_enable()
 * uses these to sequence the turn-on: Iset first, then (after the enable lines
 * are up) Vset. */
void setpoint_drive_voltage();
void setpoint_drive_current();

/* Off/safe state: Vset -> 0 V, Iset -> full-scale current (open CC limit).
 * Used when the output is disabled/faulted and at init. */
void setpoint_park();

/* Last DAC code written for a set target (CAL_VSET / CAL_ISET), for use as the
 * y-value when calibrating that path. */
uint16_t setpoint_last_code(uint8_t target);

#endif /* CHANNEL_SETPOINT_H */
