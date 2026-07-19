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

/* ---- Compile-time defaults (uncalibrated behaviour) --------------------- */
/* Nominal channel transfer functions used until a 2-point calibration overrides
 * them. Vout/Iout are the supply OUTPUT; Vset/Iset are the DAC output voltage:
 *
 *     Vout = 15 * Vset        ->  Vset = Vout / 15
 *     Iout = Iset / 1.2       ->  Iset = Iout * 1.2
 *
 * The ADC is assumed to sense the same feedback nodes the CV/CC loops regulate
 * to, so the measure paths use the inverse of the same ratios. At full output
 * (36.6 V / 2 A) both sense nodes stay inside the ADC's +-4.096 V window. */
#define DEFAULT_V_GAIN   15.0f   /* Vout per volt of DAC Vset                 */
#define DEFAULT_I_DIV    1.2f    /* Iset(V) per amp of Iout; Iout = Iset/1.2  */

/* Set paths: OUTPUT engineering value (mV / mA) -> DAC code. */
static const float kBaseCodePerVolt  = (float)DAC_MAX_CODE / DAC_FULLSCALE_V;
static const float kDefaultVSetGain  = kBaseCodePerVolt / (DEFAULT_V_GAIN * 1000.0f);
static const float kDefaultISetGain  = (kBaseCodePerVolt * DEFAULT_I_DIV) / 1000.0f;
/* Meas paths: ADC volts -> OUTPUT engineering value (mV / mA). */
static const float kDefaultVMeasGain = DEFAULT_V_GAIN * 1000.0f;          /* mV per ADC volt */
static const float kDefaultIMeasGain = (1.0f / DEFAULT_I_DIV) * 1000.0f;  /* mA per ADC volt */

static void load_defaults()
{
    s_store.coeff[CAL_VSET]  = { kDefaultVSetGain,  0.0f };
    s_store.coeff[CAL_ISET]  = { kDefaultISetGain,  0.0f };
    s_store.coeff[CAL_VMEAS] = { kDefaultVMeasGain, 0.0f };
    s_store.coeff[CAL_IMEAS] = { kDefaultIMeasGain, 0.0f };
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

    /* The points are deliberately KEPT: the Brain re-sends a commit when its
     * ACK got lost, and a re-commit of the same points must succeed (it just
     * recomputes the identical line). A fresh cal run overwrites the points
     * before it commits, so nothing stale is ever committed. */

    /* A successful commit on every target clears the "invalid" flag once all
     * paths have at least been touched; for simplicity we clear it here since
     * the operator is actively calibrating. */
    state_clear_flag(FLAG_CAL_INVALID);
    return true;
}

void cal_reset(uint8_t target)
{
    if (target >= CAL_COUNT) return;
    switch (target) {
    case CAL_VSET:  s_store.coeff[target] = { kDefaultVSetGain,  0.0f }; break;
    case CAL_ISET:  s_store.coeff[target] = { kDefaultISetGain,  0.0f }; break;
    case CAL_VMEAS: s_store.coeff[target] = { kDefaultVMeasGain, 0.0f }; break;
    case CAL_IMEAS: s_store.coeff[target] = { kDefaultIMeasGain, 0.0f }; break;
    default: return;
    }
    storage_save(&s_store);
}

bool cal_is_valid()
{
    return !state_flag(FLAG_CAL_INVALID);
}
