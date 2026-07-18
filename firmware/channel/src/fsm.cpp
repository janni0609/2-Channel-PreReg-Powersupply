#include <Arduino.h>

#include "fsm.h"
#include "config.h"
#include "state.h"
#include "protocol.h"
#include "app/selftest.h"
#include "app/output.h"
#include "app/setpoint.h"
#include "app/thermal.h"
#include "app/comms.h"

static void enter_fault(uint8_t event)
{
    output_disable();
    setpoint_zero();
    g_state.run_state = ST_FAULT;
    comms_send_event(event);
}

void fsm_init()
{
    g_state.run_state = ST_SELFTEST;
}

void fsm_task()
{
    switch (g_state.run_state) {
    case ST_SELFTEST:
        if (selftest_run()) {
            g_state.run_state = ST_IDLE;
            comms_send_event(EVT_BOOT);
        } else {
            g_state.run_state = ST_FAULT;
            comms_send_event(EVT_SELFTEST_FAIL);
        }
        break;

    case ST_IDLE:
    case ST_RUN:
        /* Over-temperature is the unconditional safety trip. */
        if (thermal_overtemp()) {
            enter_fault(EVT_OVERTEMP);
            break;
        }
        /* Comms-loss watchdog: if the Brain falls silent while the output
         * is live, drop to a safe state. Not a latched fault -- we return
         * to IDLE with the output off; the Brain must explicitly command
         * the output back on (it clears its desired-on state on link loss,
         * so a reconnect will not silently re-energize the output). */
        if (g_state.run_state == ST_RUN &&
            comms_since_rx_ms() > COMMS_TIMEOUT_MS) {
            output_disable();
            g_state.run_state = ST_IDLE;
            comms_send_event(EVT_COMMS_TIMEOUT);
        }
        break;

    case ST_FAULT:
        /* Stay until explicitly reset; keep the output disabled. */
        if (output_is_on()) output_disable();
        break;

    default:
        break;
    }
}

bool fsm_request_output(bool on)
{
    if (on) {
        if (g_state.run_state == ST_IDLE) {
            output_enable();
            g_state.run_state = ST_RUN;
            return true;
        }
        if (g_state.run_state == ST_RUN) {
            return true;          /* already on */
        }
        return false;             /* cannot enable while SELFTEST/FAULT */
    } else {
        if (g_state.run_state == ST_RUN) {
            output_disable();
            g_state.run_state = ST_IDLE;
        }
        return true;              /* off is always honoured */
    }
}

void fsm_reset_fault()
{
    if (g_state.run_state != ST_FAULT) return;

    /* Only allow recovery once the over-temp condition has cleared. */
    if (thermal_overtemp()) return;

    comms_send_event(EVT_OVERTEMP_CLEAR);
    g_state.run_state = ST_SELFTEST;   /* re-validate before returning to IDLE */
}
