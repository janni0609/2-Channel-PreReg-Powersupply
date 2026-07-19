#include <Arduino.h>

#include "runtime.h"
#include "config.h"
#include "hal/storage.h"

/* Total accumulated powered-on seconds (persisted base + this session). */
static uint32_t s_total_s;
/* Value last written to EEPROM; s_total_s - s_saved_s is the unsaved backlog. */
static uint32_t s_saved_s;
/* millis() at the previous runtime_task(), plus the sub-second carry so partial
 * seconds are never dropped between calls. */
static uint32_t s_last_ms;
static uint32_t s_frac_ms;

static void persist()
{
    RuntimeStore rs;
    rs.seconds = s_total_s;
    runtime_store_save(&rs);
    s_saved_s = s_total_s;
}

void runtime_init()
{
    RuntimeStore rs;
    if (runtime_store_load(&rs)) {
        s_total_s = rs.seconds;
    } else {
        s_total_s = 0;
        persist();                 /* seed EEPROM with a zeroed, valid blob */
    }
    s_saved_s = s_total_s;
    s_last_ms = millis();
    s_frac_ms = 0;
}

void runtime_task()
{
    const uint32_t now = millis();
    s_frac_ms += (uint32_t)(now - s_last_ms);   /* unsigned: wrap-safe */
    s_last_ms  = now;

    if (s_frac_ms >= 1000u) {
        s_total_s += s_frac_ms / 1000u;
        s_frac_ms %= 1000u;
    }

    if (s_total_s - s_saved_s >= RUNTIME_SAVE_INTERVAL_S)
        persist();
}

uint32_t runtime_seconds()
{
    return s_total_s;
}
