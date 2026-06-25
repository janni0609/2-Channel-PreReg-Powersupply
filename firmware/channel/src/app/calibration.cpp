#include <Arduino.h>

#include "calibration.h"
#include "config.h"
#include "state.h"
#include "hal/storage.h"

/* Working copy of the coefficients (mirrors what is in EEPROM). */
static CalStore s_store;

/* Two captured points per target, gathered during a calibration run. */
struct CalPoints {
    float   x[2];
    float   y[2];
    bool    have[2];
};
static CalPoints s_points[CAL_COUNT];

/* ---- Compile-time defaults ---------------------------------------------- */
/* Set paths: map engineering value directly to DAC code assuming the value is
 * the desired DAC output in mV (0..2440 -> 0..4095). This is a placeholder
 * until a real 2-point calibration ties it to the supply's output units. */
static const float kDefaultSetGain = (float)DAC_MAX_CODE / (DAC_FULLSCALE_V * 1000.0f);

static void load_defaults()
{
    s_store.coeff[CAL_VSET]  = { kDefaultSetGain, 0.0f };
    s_store.coeff[CAL_ISET]  = { kDefaultSetGain, 0.0f };
    /* Meas paths: volts-at-ADC -> mV/mA. 1000 is a neutral placeholder. */
    s_store.coeff[CAL_VMEAS] = { 1000.0f, 0.0f };
    s_store.coeff[CAL_IMEAS] = { 1000.0f, 0.0f };
}

void cal_init()
{
    for (uint8_t t = 0; t < CAL_COUNT; t++) {
        s_points[t].have[0] = s_points[t].have[1] = false;
    }

    if (storage_load(&s_store)) {
        state_clear_flag(FLAG_CAL_INVALID);
    } else {
        load_defaults();
        storage_save(&s_store);          /* seed EEPROM with defaults */
        state_set_flag(FLAG_CAL_INVALID);/* tell the Brain it's uncalibrated */
    }
}

float cal_apply(uint8_t target, float x)
{
    if (target >= CAL_COUNT) return x;
    return s_store.coeff[target].gain * x + s_store.coeff[target].offset;
}

void cal_record_point(uint8_t target, uint8_t index, float x, float y)
{
    if (target >= CAL_COUNT || index > 1) return;
    s_points[target].x[index] = x;
    s_points[target].y[index] = y;
    s_points[target].have[index] = true;
}

bool cal_commit(uint8_t target)
{
    if (target >= CAL_COUNT) return false;
    CalPoints &p = s_points[target];
    if (!p.have[0] || !p.have[1]) return false;

    const float dx = p.x[1] - p.x[0];
    if (fabsf(dx) < 1e-6f) return false;   /* degenerate */

    const float gain   = (p.y[1] - p.y[0]) / dx;
    const float offset = p.y[0] - gain * p.x[0];

    s_store.coeff[target].gain   = gain;
    s_store.coeff[target].offset = offset;
    storage_save(&s_store);

    p.have[0] = p.have[1] = false;

    /* A successful commit on every target clears the "invalid" flag once all
     * paths have at least been touched; for simplicity we clear it here since
     * the operator is actively calibrating. */
    state_clear_flag(FLAG_CAL_INVALID);
    return true;
}

void cal_reset(uint8_t target)
{
    if (target >= CAL_COUNT) return;
    if (target == CAL_VSET || target == CAL_ISET)
        s_store.coeff[target] = { kDefaultSetGain, 0.0f };
    else
        s_store.coeff[target] = { 1000.0f, 0.0f };
    storage_save(&s_store);
}

bool cal_is_valid()
{
    return !state_flag(FLAG_CAL_INVALID);
}
