#include <Arduino.h>

#include "output.h"
#include "hal/board.h"
#include "setpoint.h"
#include "state.h"

static bool s_on = false;

void output_init()
{
    board_set_dcdc(false);
    board_set_lin(false);
    setpoint_park();          /* off: Vset 0 V, Iset full scale */
    s_on = false;
    g_state.output_on = false;
    state_clear_flag(FLAG_OUTPUT_ON);
}

void output_enable()
{
    setpoint_drive_current(); /* 1. establish the current limit first        */

    board_set_dcdc(true);     /* 2. bring up the pre-regulator ...           */
    delay(70);                /*    ... let the buck rail settle ...          */
    board_set_lin(true);      /*    ... then enable the linear stage          */

    s_on = true;
    g_state.output_on = true;
    state_set_flag(FLAG_OUTPUT_ON);

    setpoint_drive_voltage(); /* 3. enable lines up -> ramp Vset to setpoint  */
}

void output_disable()
{
    setpoint_park();          /* Vset -> 0 V (Iset -> full) before de-energizing */
    board_set_lin(false);     /* drop the linear stage first */
    delay(1);
    board_set_dcdc(false);    /* then the pre-regulator      */

    s_on = false;
    g_state.output_on = false;
    state_clear_flag(FLAG_OUTPUT_ON);
}

bool output_is_on()
{
    return s_on;
}
