/**
 * ads1118.h - Driver for the TI ADS1118 16-bit delta-sigma ADC.
 *
 * Single-shot operation: start writes a Config word (SS set) which begins one
 * conversion; read clocks the result out with an all-zero DIN (invalid NOP)
 * so the Config register stays untouched and the device remains powered down.
 * The driver is non-blocking from the caller's perspective: start a
 * conversion, wait the conversion time, then read the result.
 *
 * Inputs (per requirements / board). Both signals are single-ended to GND:
 *   ADS_CH_V : AIN0(P)-AIN1(N)  (MUX=000)  -> voltage sense (AIN0=sig, AIN1=GND)
 *   ADS_CH_I : AIN2(P)-AIN3(N)  (MUX=011)  -> current sense (AIN2=sig, AIN3=GND)
 *
 * The PGA index selects the full-scale range and is managed by the autoscaler
 * in measure.cpp.
 */
#ifndef CHANNEL_ADS1118_H
#define CHANNEL_ADS1118_H

#include <stdint.h>

enum AdsChannel {
    ADS_CH_V = 0,   /* AIN0-AIN1 (AIN1=GND) */
    ADS_CH_I = 1    /* AIN2-AIN3 (AIN3=GND) */
};

/* PGA index -> full-scale volts. Index matches the ADS1118 PGA[2:0] field. */
extern const float kAdsFsVolts[8];

/* Data rate -> conversion time in ms (worst case, rounded up). */
uint8_t ads1118_conv_time_ms();

/* Begin a single-shot conversion on `ch` at PGA index `pga` (0..7). */
void ads1118_start(AdsChannel ch, uint8_t pga);

/* Read the most recent conversion result (raw signed 16-bit code). Pure
 * read: the Config register is not written, so the device stays powered
 * down until the next ads1118_start(). */
int16_t ads1118_read();

/* Convert a raw code at the given PGA index into volts at the ADC input. */
float ads1118_code_to_volts(int16_t code, uint8_t pga);

/* Self-test: write a known Config and read it back (32-bit cycle).
 * Returns true if the readback matches. */
bool ads1118_selftest();

#endif /* CHANNEL_ADS1118_H */
