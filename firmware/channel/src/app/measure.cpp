#include <Arduino.h>

#include "measure.h"
#include "calibration.h"
#include "settings.h"
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

/* Averaged raw ADC input voltage per channel: the x-sample recorded when a
 * measure path is calibrated (CMD_CAL_POINT). An exponential moving average
 * stands in for a second ring pair, which would cost another 260 bytes of the
 * tiny's RAM for a value only read during calibration.
 *
 * The averaging is deliberately fast. An EWMA's residual after a step is
 * (1-alpha)^n, so alpha sets the settling time and not just the noise floor --
 * and a cal point is *always* taken just after stepping the channel to it, so
 * whatever lag is left goes straight into the fitted line. At the original
 * alpha = 1/16 the average was still ~2 % away from the new operating point
 * several seconds later: measured on CH2, the x recorded at 0.2 A came out
 * 20 mV high and the one at 1.8 A 22 mV low (each pulled back toward the other
 * point), storing a 2.2 % gain error while the identical calibration run with a
 * 25 s settle landed within 0.003 %.
 *
 * alpha = 1/4 decays a step to <1e-4 in 32 samples (~1 s) and still averages a
 * noise-equivalent ~7 samples, well below the ~0.1 mA ADC quantum this
 * calibration resolves. CAL_AVG_SNAP_V additionally slams the average onto any
 * sample that is nowhere near it, so a setpoint step costs no settling at all;
 * at ~400 LSB it is far above sample noise and output ripple, and alpha then
 * clears the sub-threshold remainder within the same ~1 s. */
#define CAL_AVG_ALPHA     (1.0f / 4.0f)
#define CAL_AVG_SNAP_V    0.050f
static float      s_vadc_avg[2];
static bool       s_vadc_avg_seeded[2];

/* Moving-average ring per channel. We keep the last AVG_MAX samples and, on
 * each update, average the most recent `n` of them (n = the Brain-set window,
 * 1..AVG_MAX). Re-summing up to 32 int32s per read is trivial at the ADC rate
 * and stays correct across window changes and warm-up without incremental-sum
 * bookkeeping. Indexed by AdsChannel (ADS_CH_V / ADS_CH_I). */
struct AvgRing {
    int32_t buf[AVG_MAX];
    uint8_t head;     /* next write slot; (head-1) is the most recent sample */
    uint8_t filled;   /* valid samples so far (<= AVG_MAX)                   */
};
static AvgRing s_avg[2];

/* Push one sample and return the mean of the most recent `n` samples. */
static int32_t avg_push(AvgRing &a, int32_t sample, uint8_t n)
{
    if (n < AVG_MIN) n = (uint8_t)AVG_MIN;
    if (n > AVG_MAX) n = (uint8_t)AVG_MAX;

    a.buf[a.head] = sample;
    a.head = (uint8_t)((a.head + 1u) % AVG_MAX);
    if (a.filled < AVG_MAX) a.filled++;

    uint8_t win = (n < a.filled) ? n : a.filled;   /* don't average empty slots */
    int64_t sum = 0;
    uint8_t i = a.head;                            /* walk back from most recent */
    for (uint8_t k = 0; k < win; k++) {
        i = (i == 0) ? (uint8_t)(AVG_MAX - 1) : (uint8_t)(i - 1);
        sum += a.buf[i];
    }
    return (int32_t)(sum / win);
}

static const int32_t kFsCode = 32767;
static const int32_t kUpThresh = (kFsCode * ADC_AUTOSCALE_UP_PCT) / 100;
static const int32_t kDnThresh = (kFsCode * ADC_AUTOSCALE_DN_PCT) / 100;

void measure_init()
{
    s_pga[ADS_CH_V] = ADC_PGA_START_INDEX;
    s_pga[ADS_CH_I] = ADC_PGA_START_INDEX;
    s_vadc[ADS_CH_V] = 0.0f;
    s_vadc[ADS_CH_I] = 0.0f;
    s_vadc_avg[ADS_CH_V] = 0.0f;
    s_vadc_avg[ADS_CH_I] = 0.0f;
    s_vadc_avg_seeded[ADS_CH_V] = false;
    s_vadc_avg_seeded[ADS_CH_I] = false;
    s_avg[ADS_CH_V].head = s_avg[ADS_CH_V].filled = 0;
    s_avg[ADS_CH_I].head = s_avg[ADS_CH_I].filled = 0;
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

    /* Keep the calibration-capture average warm (seed on the first sample so
     * boot doesn't start the average from an artificial zero, and re-seed on a
     * step so it never lags a setpoint change into a cal point). */
    const float davg = volts - s_vadc_avg[ch];
    if (!s_vadc_avg_seeded[ch] || fabsf(davg) > CAL_AVG_SNAP_V) {
        s_vadc_avg[ch] = volts;
        s_vadc_avg_seeded[ch] = true;
    } else {
        s_vadc_avg[ch] += davg * CAL_AVG_ALPHA;
    }

    if (ch == ADS_CH_V) {
        int32_t v = (int32_t)lroundf(cal_apply(CAL_VMEAS, volts));
        if (v < 0) v = 0;
        g_state.meas_v_mV = avg_push(s_avg[ADS_CH_V], v, settings_avg(AVG_V));
    } else {
        /* cal_apply returns mA; store in 0.1 mA units so the Brain can show a
         * 4th decimal (1 LSB at the pinned +-4.096 V range is ~0.1 mA). */
        int32_t i = (int32_t)lroundf(cal_apply(CAL_IMEAS, volts) * 10.0f);
        if (i < 0) i = 0;
        g_state.meas_i_dmA = avg_push(s_avg[ADS_CH_I], i, settings_avg(AVG_I));
    }

    /* Power (mW) from the latest V (mV) and I (0.1 mA): mV * dmA / 10000. */
    g_state.meas_p_mW =
        (int32_t)(((int64_t)g_state.meas_v_mV * g_state.meas_i_dmA) / 10000);

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
        const int16_t code = ads1118_read();
        process(s_ch, code);
        s_ch = (s_ch == ADS_CH_V) ? ADS_CH_I : ADS_CH_V;
        s_phase = M_START;
        break;
    }
    }
}

float measure_last_vadc(uint8_t target)
{
    if (target == CAL_VMEAS) return s_vadc_avg[ADS_CH_V];
    if (target == CAL_IMEAS) return s_vadc_avg[ADS_CH_I];
    return 0.0f;
}
