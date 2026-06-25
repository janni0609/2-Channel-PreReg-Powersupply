#include <Arduino.h>

#include "fsm.h"
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
