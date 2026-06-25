/**
 * state.h - The single shared run-state structure for the channel.
 *
 * Modules read/write this struct; comms serializes it into telemetry. Keeping
 * one source of truth avoids passing values through long call chains.
 */
#ifndef CHANNEL_STATE_H
#define CHANNEL_STATE_H

#include <stdint.h>
#include "protocol.h"

struct ChannelState {
    /* Setpoints (engineering units, as received from the Brain) */
    int32_t set_v_mV;
    int32_t set_i_mA;
    bool     output_on;

    /* Latest measurements */
    int32_t meas_v_mV;
    int32_t meas_i_mA;
    int32_t meas_p_mW;
    int16_t temp_cC;     /* centi-Celsius */

    /* Status */
    uint8_t run_state;   /* ST_* from protocol.h */
    uint8_t flags;       /* FLAG_* from protocol.h */
};

extern ChannelState g_state;

static inline void state_set_flag(uint8_t bit)   { g_state.flags |= bit; }
static inline void state_clear_flag(uint8_t bit) { g_state.flags &= (uint8_t)~bit; }
static inline bool state_flag(uint8_t bit)       { return (g_state.flags & bit) != 0; }

#endif /* CHANNEL_STATE_H */
