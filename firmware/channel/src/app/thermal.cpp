#include <Arduino.h>
#include <math.h>

#include "thermal.h"
#include "hal/board.h"
#include "config.h"
#include "state.h"

static float s_temp_c = 25.0f;
static bool  s_overtemp = false;

void thermal_init()
{
    s_overtemp = false;
    thermal_task();          /* prime with a real reading */
}

/* Convert a raw NTC ADC reading into degrees Celsius via the beta model. */
static float ntc_raw_to_c(uint16_t raw)
{
    const float adc_max = (float)((1u << NTC_ADC_BITS) - 1u);
    float ratio = (float)raw / adc_max;          /* Vnode / VDD */

    /* Clamp away from the rails to keep the math finite if the NTC is
     * open (ratio->1, very cold) or shorted (ratio->0, very hot). */
    if (ratio < 0.001f) ratio = 0.001f;
    if (ratio > 0.999f) ratio = 0.999f;

    /* Divider: Vnode = VDD * Rntc/(Rtop+Rntc)  ->  Rntc = Rtop*ratio/(1-ratio) */
    const float r_ntc = NTC_R_TOP_OHMS * (ratio / (1.0f - ratio));

    /* Beta equation: 1/T = 1/T0 + (1/B) * ln(Rntc/R0) */
    const float inv_t = (1.0f / NTC_T0_KELVIN)
                      + (1.0f / NTC_BETA) * logf(r_ntc / NTC_R0_OHMS);
    return (1.0f / inv_t) - 273.15f;
}

void thermal_task()
{
    const uint16_t raw = board_read_ntc_raw();
    s_temp_c = ntc_raw_to_c(raw);

    g_state.temp_cC = (int16_t)lroundf(s_temp_c * 100.0f);

    if (!s_overtemp && s_temp_c >= TEMP_OTP_TRIP_C) {
        s_overtemp = true;
        state_set_flag(FLAG_OVERTEMP);
    } else if (s_overtemp && s_temp_c <= TEMP_OTP_REARM_C) {
        s_overtemp = false;
        state_clear_flag(FLAG_OVERTEMP);
    }
}

float thermal_temp_c()  { return s_temp_c; }
bool  thermal_overtemp(){ return s_overtemp; }
