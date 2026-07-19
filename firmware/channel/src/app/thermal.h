/**
 * thermal.h - NTC temperature measurement and over-temperature protection.
 *
 * Runs independently of the comms link so a hung UART can never keep the
 * output enabled while the channel overheats. The FSM consults
 * thermal_overtemp() to decide when to fault.
 */
#ifndef CHANNEL_THERMAL_H
#define CHANNEL_THERMAL_H

#include <stdint.h>

void  thermal_init();

/* Periodic: read NTC, update g_state.temp_cC and the OTP condition. */
void  thermal_task();

/* Current temperature in degrees Celsius. */
float thermal_temp_c();

/* True while over-temperature (trips at the runtime-settable per-channel OTP
 * point, clears TEMP_OTP_HYST_C below it - hysteresis prevents chatter). */
bool  thermal_overtemp();

#endif /* CHANNEL_THERMAL_H */
