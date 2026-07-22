/**
 * output.h - Output enable/disable with a safe power sequence.
 *
 * Enable order : Iset -> set current, then pre-regulator (DCDC), settle, then
 *                linear stage, and finally Vset -> set voltage.
 * Disable order: Vset -> 0 (Iset parked at full scale), then linear stage,
 *                then pre-regulator.
 * While off: Vset held at 0 V, Iset at full-scale current. Setpoints received
 * in the meantime are remembered (g_state) and applied on the next enable.
 * (If the hardware needs the opposite enable order, swap the two board calls.)
 */
#ifndef CHANNEL_OUTPUT_H
#define CHANNEL_OUTPUT_H

void output_init();
void output_enable();
void output_disable();
bool output_is_on();

#endif /* CHANNEL_OUTPUT_H */
