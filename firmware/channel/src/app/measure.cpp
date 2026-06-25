#include <Arduino.h>

#include "measure.h"
#include "calibration.h"
#include "config.h"
#include "state.h"
#include "drivers/ads1118.h"

/* Acquisition phases for the non-blocking single-shot cycle. */
enum MeasPhase { M_START, M_WAIT, M_READ };

static MeasPhase s_phase;
static AdsChannel s_ch;
static uint8_t    s_pga[2];        /* current PGA index per channel */
static uint32_t   s_t0;
static float      s_vadc[2];       /* last input voltage per channel */

static const int32_t kFsCode = 32767;
static const int32_t kUpThresh = (kFsCode * ADC_AUTOSCALE_UP_PCT) / 100;
static const int32_t kDnThresh = (kFsCode * ADC_AUTOSCALE_DN_PCT) / 100;

void measure_init()
{
    s_pga[ADS_CH_V] = ADC_PGA_START_INDEX;
    s_pga[ADS_CH_I] = ADC_PGA_START_INDEX;
    s_vadc[ADS_CH_V] = 0.0f;
    s_vadc[ADS_CH_I] = 0.0f;
    s_ch = ADS_CH_V;
    s_phase = M_START;
}

static void autoscale(AdsChannel ch, int16_t code)
{
    int32_t mag = code;
    if (mag < 0) mag = -mag;

    if (mag >= kUpThresh && s_pga[ch] > ADC_PGA_MIN_INDEX) {
        s_pga[ch]--;            /* near full scale -> larger FS (lower gain) */
    } else if (mag < kDnThresh && s_pga[ch] < ADC_PGA_MAX_INDEX) {
        s_pga[ch]++;            /* small signal -> smaller FS (higher gain)  */
    }
}

static void process(AdsChannel ch, int16_t code)
{
    const float volts = ads1118_code_to_volts(code, s_pga[ch]);
    s_vadc[ch] = volts;

    if (ch == ADS_CH_V) {
        int32_t v = (int32_t)lroundf(cal_apply(CAL_VMEAS, volts));
        if (v < 0) v = 0;
        g_state.meas_v_mV = v;
    } else {
        int32_t i = (int32_t)lroundf(cal_apply(CAL_IMEAS, volts));
        if (i < 0) i = 0;
        g_state.meas_i_mA = i;
    }

    /* Power (mW) from the latest V and I. */
    g_state.meas_p_mW =
        (int32_t)(((int64_t)g_state.meas_v_mV * g_state.meas_i_mA) / 1000);

    autoscale(ch, code);   /* adjust gain for the NEXT conversion */
}

void measure_task()
{
    switch (s_phase) {
    case M_START:
        ads1118_start(s_ch, s_pga[s_ch]);
        s_t0 = millis();
        s_phase = M_WAIT;
        break;

    case M_WAIT:
        if ((uint32_t)(millis() - s_t0) >= ads1118_conv_time_ms())
            s_phase = M_READ;
        break;

    case M_READ: {
        const int16_t code = ads1118_read(s_ch, s_pga[s_ch]);
        process(s_ch, code);
        s_ch = (s_ch == ADS_CH_V) ? ADS_CH_I : ADS_CH_V;
        s_phase = M_START;
        break;
    }
    }
}

float measure_last_vadc(uint8_t target)
{
    if (target == CAL_VMEAS) return s_vadc[ADS_CH_V];
    if (target == CAL_IMEAS) return s_vadc[ADS_CH_I];
    return 0.0f;
}
