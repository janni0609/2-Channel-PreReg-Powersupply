#include <Arduino.h>

#include "output.h"
#include "hal/board.h"
#include "state.h"

static bool s_on = false;

void output_init()
{
    board_set_dcdc(false);
    board_set_lin(false);
    s_on = false;
    g_state.output_on = false;
    state_clear_flag(FLAG_OUTPUT_ON);
}

void output_enable()
{
    board_set_dcdc(true);     /* bring up the pre-regulator first */
    delay(2);                 /* let the buck rail settle         */
    board_set_lin(true);      /* then enable the linear stage     */

    s_on = true;
    g_state.output_on = true;
    state_set_flag(FLAG_OUTPUT_ON);
}

void output_disable()
{
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
