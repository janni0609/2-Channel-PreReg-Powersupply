/**
 * fsm.h - Channel state machine.
 *
 *   INIT -> SELFTEST -> IDLE(output off) <-> RUN(output on)
 *                          \                    /
 *                           \--> FAULT <-------/   (over-temp / device error)
 *
 * FAULT forces the output off and is only left via fsm_reset_fault() once the
 * fault condition has cleared.
 */
#ifndef CHANNEL_FSM_H
#define CHANNEL_FSM_H

#include <stdint.h>

/* Begin in SELFTEST (call after all modules are initialised). */
void fsm_init();

/* Periodic: handle self-test, monitor faults, maintain state. */
void fsm_task();

/* Request output on/off. Returns true if accepted, false if the current state
 * forbids it (e.g. trying to enable while faulted). */
bool fsm_request_output(bool on);

/* Attempt to leave FAULT: re-runs self-test if the fault has cleared. */
void fsm_reset_fault();

#endif /* CHANNEL_FSM_H */
