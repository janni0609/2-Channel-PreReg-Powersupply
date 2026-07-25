/**
 * brain_api.h - Bridge between the SCPI remote-control layer (scpi.cpp) and the
 *               front-panel application state (main.cpp).
 *
 * The SCPI parser must never keep its own copy of the setpoints or output state:
 * a remote "VOLT 5" and a knob turn have to end up in the *same* variables so the
 * OLED readout, the "Remember set" persistence and the channel link all stay
 * consistent no matter which interface drove the change. These hooks are defined
 * in main.cpp (where those variables live) and called from scpi.cpp.
 *
 * Channel index is 0 = CH1, 1 = CH2 (same convention as channel_link.h).
 */
#ifndef BRAIN_API_H
#define BRAIN_API_H

#include <stdint.h>

namespace brain {

/* --- Setpoints ---------------------------------------------------------------
 * Voltage is carried in millivolts, current in milliamps. The panel stores the
 * voltage setpoint at 10 mV (centivolt) resolution, so setVoltageMv() rounds to
 * the nearest 10 mV; current keeps full 1 mA resolution. Both clamp to the
 * channel limits and push the new value over the link + schedule persistence. */
void    setVoltageMv(uint8_t ch, int32_t mV);
void    setCurrentMa(uint8_t ch, int32_t mA);
int32_t getVoltageSetMv(uint8_t ch);
int32_t getCurrentSetMa(uint8_t ch);
int32_t voltageMaxMv();          /* upper setpoint limit (mV) */
int32_t currentMaxMa();          /* upper setpoint limit (mA) */

/* --- Output ------------------------------------------------------------------
 * setOutput() mirrors the panel toggle: it updates the desired-output state,
 * commands the channel and refreshes the header. It returns false without acting
 * if a system over-temperature trip is latched (turning on is refused). */
bool    setOutput(uint8_t ch, bool on);
bool    getOutputDesired(uint8_t ch);

/* Clear a latched fault on a channel (maps to CMD_RESET_FAULT). */
void    resetFault(uint8_t ch);

/* --- Instrument-wide readings ------------------------------------------------
 * systemTempC() is the hottest of the brain sensor and both channel sensors
 * (the value that drives OTP), NAN when no sensor is valid. */
float   systemTempC();

/* --- Buzzer ------------------------------------------------------------------ */
void    beepOnce();              /* short beep (honours the Beeper setting) */
void    setBeeper(bool on);
bool    getBeeper();

/* Seconds since boot (SYST:UPTime?). */
uint32_t uptimeS();

/* --- Front-panel remote lock (SYST:RWLock / :LOCal) --------------------------
 * When locked, the main-page knob/button edits are ignored so a remote session
 * can hold exclusive control. LOCal clears it. */
void    setFrontPanelLock(bool locked);
bool    getFrontPanelLock();

}  /* namespace brain */

#endif /* BRAIN_API_H */
