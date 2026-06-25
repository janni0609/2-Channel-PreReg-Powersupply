/**
 * output.h - Output enable/disable with a safe power sequence.
 *
 * Enable order : pre-regulator (DCDC) first, settle, then linear stage.
 * Disable order: linear stage first, then pre-regulator.
 * (If the hardware needs the opposite order, swap the two calls here.)
 */
#ifndef CHANNEL_OUTPUT_H
#define CHANNEL_OUTPUT_H

void output_init();
void output_enable();
void output_disable();
bool output_is_on();

#endif /* CHANNEL_OUTPUT_H */
