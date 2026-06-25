#include <Arduino.h>

#include "selftest.h"
#include "thermal.h"
#include "config.h"
#include "state.h"
#include "drivers/ads1118.h"
#include "drivers/mcp48fvb22.h"

bool selftest_run()
{
    bool ok = true;

    /* ADC SPI communication (config readback). */
    if (ads1118_selftest()) {
        state_clear_flag(FLAG_ADC_FAULT);
    } else {
        state_set_flag(FLAG_ADC_FAULT);
        ok = false;
    }

    /* DAC SPI communication (VREF register readback). */
    if (mcp48_selftest()) {
        state_clear_flag(FLAG_DAC_FAULT);
    } else {
        state_set_flag(FLAG_DAC_FAULT);
        ok = false;
    }

    /* Temperature must be within the safe startup window. */
    thermal_task();   /* refresh the reading */
    const float t = thermal_temp_c();
    if (t < TEMP_SELFTEST_MIN_C || t > TEMP_SELFTEST_MAX_C) {
        ok = false;
    }

    return ok;
}
