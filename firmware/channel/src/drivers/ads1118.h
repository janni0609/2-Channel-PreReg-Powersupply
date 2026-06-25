/**
 * ads1118.h - Driver for the TI ADS1118 16-bit delta-sigma ADC.
 *
 * Single-shot operation: each transaction simultaneously clocks out the
 * previous conversion result and clocks in a new Config word. The driver is
 * non-blocking from the caller's perspective: start a conversion, wait the
 * conversion time, then read the result.
 *
 * Inputs (per requirements):
 *   ADS_CH_V : AIN0-AIN1 differential  (MUX=000)  -> output voltage sense
 *   ADS_CH_I : AIN2-AIN3 differential  (MUX=011)  -> output current sense
 *
 * The PGA index selects the full-scale range and is managed by the autoscaler
 * in measure.cpp.
 */
#ifndef CHANNEL_ADS1118_H
#define CHANNEL_ADS1118_H

#include <stdint.h>

enum AdsChannel {
    ADS_CH_V = 0,   /* AIN0-AIN1 */
    ADS_CH_I = 1    /* AIN2-AIN3 */
};

/* PGA index -> full-scale volts. Index matches the ADS1118 PGA[2:0] field. */
extern const float kAdsFsVolts[8];

/* Data rate -> conversion time in ms (worst case, rounded up). */
uint8_t ads1118_conv_time_ms();

/* Begin a single-shot conversion on `ch` at PGA index `pga` (0..7). */
void ads1118_start(AdsChannel ch, uint8_t pga);

/* Read the most recent conversion result (raw signed 16-bit code). Also
 * re-arms the same channel/pga so the bus is never left mid-cycle. */
int16_t ads1118_read(AdsChannel ch, uint8_t pga);

/* Convert a raw code at the given PGA index into volts at the ADC input. */
float ads1118_code_to_volts(int16_t code, uint8_t pga);

/* Self-test: write a known Config and read it back (32-bit cycle).
 * Returns true if the readback matches. */
bool ads1118_selftest();

#endif /* CHANNEL_ADS1118_H */
