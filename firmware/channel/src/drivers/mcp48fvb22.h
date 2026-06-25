/**
 * mcp48fvb22.h - Driver for the Microchip MCP48FVB22 dual 12-bit DAC.
 *
 * Reference: internal band gap (1.22 V) buffered x2 -> VRL = 2.44 V, output
 * gain 1x, so VOUT spans 0..2.44 V full scale (the "0..2.5 V" requirement).
 *
 *   DAC_CH_VSET : VOUT0 -> set-voltage control
 *   DAC_CH_ISET : VOUT1 -> set-current control
 */
#ifndef CHANNEL_MCP48FVB22_H
#define CHANNEL_MCP48FVB22_H

#include <stdint.h>

enum DacChannel {
    DAC_CH_VSET = 0,   /* VOUT0 */
    DAC_CH_ISET = 1    /* VOUT1 */
};

/* Configure reference (band gap), gain (1x), power-down (normal) and zero the
 * outputs. Returns true if the VREF register reads back as written. */
bool mcp48_init();

/* Write a 12-bit code (0..4095) to a DAC channel. Code is clamped. */
void mcp48_set_code(DacChannel ch, uint16_t code);

/* Self-test: read the VREF register back and confirm it matches config. */
bool mcp48_selftest();

#endif /* CHANNEL_MCP48FVB22_H */
