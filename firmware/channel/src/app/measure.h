/**
 * measure.h - Non-blocking acquisition of output voltage and current from the
 *             ADS1118, with per-channel PGA autoscaling, calibration apply and
 *             power computation. Results land in g_state.
 */
#ifndef CHANNEL_MEASURE_H
#define CHANNEL_MEASURE_H

#include <stdint.h>

void measure_init();

/* Call frequently from the main loop. Internally paced by the ADC conversion
 * time; alternates between the V and I channels. */
void measure_task();

/* Latest raw ADC input voltage (volts) for a measure target (CAL_VMEAS /
 * CAL_IMEAS). Used as the x-value when calibrating a measure path. */
float measure_last_vadc(uint8_t target);

#endif /* CHANNEL_MEASURE_H */
