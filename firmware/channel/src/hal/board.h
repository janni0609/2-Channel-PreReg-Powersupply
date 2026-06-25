/**
 * board.h - Misc on-chip I/O: output-enable lines and the NTC analog input.
 */
#ifndef CHANNEL_BOARD_H
#define CHANNEL_BOARD_H

#include <stdint.h>

/* Configure GPIO directions and safe default levels (both enables LOW/off). */
void board_init();

/* Drive the two output-enable lines (true = asserted/on). */
void board_set_dcdc(bool on);
void board_set_lin(bool on);

/* Raw NTC ADC reading (0 .. 2^NTC_ADC_BITS-1). */
uint16_t board_read_ntc_raw();

#endif /* CHANNEL_BOARD_H */
