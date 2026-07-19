#include <Arduino.h>

#include "settings.h"
#include "config.h"
#include "hal/storage.h"

/* Working copy (mirrors the EEPROM SettingsStore). */
static SettingsStore s_store;

static uint8_t clamp_avg(uint8_t n)
{
    if (n < AVG_MIN) return (uint8_t)AVG_MIN;
    if (n > AVG_MAX) return (uint8_t)AVG_MAX;
    return n;
}

static uint8_t clamp_otp(uint8_t n)
{
    if (n < OTP_MIN_C) return (uint8_t)OTP_MIN_C;
    if (n > OTP_MAX_C) return (uint8_t)OTP_MAX_C;
    return n;
}

static void load_defaults()
{
    for (uint8_t i = 0; i < AVG_COUNT; i++)
        s_store.avg[i] = (uint8_t)MEAS_AVG_DEFAULT;
    s_store.otp_c = (uint8_t)TEMP_OTP_DEFAULT_C;
}

void settings_init()
{
    if (!settings_store_load(&s_store)) {
        load_defaults();
        settings_store_save(&s_store);   /* seed EEPROM with defaults */
    }
    /* Guard against a valid-CRC blob written with an out-of-range value. */
    for (uint8_t i = 0; i < AVG_COUNT; i++)
        s_store.avg[i] = clamp_avg(s_store.avg[i]);
    s_store.otp_c = clamp_otp(s_store.otp_c);
}

uint8_t settings_avg(uint8_t which)
{
    if (which >= AVG_COUNT) return (uint8_t)AVG_MIN;
    return s_store.avg[which];
}

bool settings_set_avg(uint8_t which, uint8_t count)
{
    if (which >= AVG_COUNT) return false;
    const uint8_t v = clamp_avg(count);
    if (s_store.avg[which] != v) {
        s_store.avg[which] = v;
        settings_store_save(&s_store);
    }
    return true;
}

uint8_t settings_otp_c()
{
    return s_store.otp_c;
}

void settings_set_otp_c(uint8_t trip_c)
{
    const uint8_t v = clamp_otp(trip_c);
    if (s_store.otp_c != v) {
        s_store.otp_c = v;
        settings_store_save(&s_store);
    }
}
