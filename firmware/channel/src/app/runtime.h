/**
 * runtime.h - Channel operating-hours meter.
 *
 * Counts elapsed time whenever the channel is powered and accumulates it into a
 * persistent seconds counter (EEPROM RuntimeStore, see storage.*). The total is
 * reported to the Brain in the CMD_SETTINGS reply and shown on the "Runtime" row
 * of the channel menu. To bound EEPROM wear the counter is flushed only every
 * RUNTIME_SAVE_INTERVAL_S of accumulated time (config.h), so an unexpected
 * power-off loses at most that much of the current session.
 */
#ifndef CHANNEL_RUNTIME_H
#define CHANNEL_RUNTIME_H

#include <stdint.h>

/* Load the persisted total from EEPROM (seeds 0 if the blob is absent/corrupt)
 * and start the elapsed-time clock. Call once at boot. */
void runtime_init();

/* Advance the meter from elapsed millis() and flush to EEPROM when a full
 * RUNTIME_SAVE_INTERVAL_S has accumulated since the last save. Call every loop;
 * it is cheap (integer math) between the occasional save. */
void runtime_task();

/* Total accumulated powered-on time, in seconds (persisted + unsaved). */
uint32_t runtime_seconds();

#endif /* CHANNEL_RUNTIME_H */
