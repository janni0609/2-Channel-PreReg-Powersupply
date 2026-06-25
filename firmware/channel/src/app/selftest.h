/**
 * selftest.h - Power-on self test: verify the ADC and DAC respond over SPI and
 *              that the temperature is within the allowed startup range.
 */
#ifndef CHANNEL_SELFTEST_H
#define CHANNEL_SELFTEST_H

/* Returns true if all checks pass. On failure, the relevant FLAG_* bits are
 * set in g_state. */
bool selftest_run();

#endif /* CHANNEL_SELFTEST_H */
