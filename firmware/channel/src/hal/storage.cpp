#include <Arduino.h>
#include <EEPROM.h>

#include "storage.h"

#define CAL_STORE_ADDR       0    /* calibration blob base address (~40 B) */
#define SETTINGS_STORE_ADDR  64   /* settings blob base address (clears CalStore) */

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) over a byte span. */
static uint16_t crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool storage_load(CalStore *out)
{
    EEPROM.get(CAL_STORE_ADDR, *out);

    if (out->magic != CAL_STORE_MAGIC)   return false;
    if (out->version != CAL_STORE_VERSION) return false;

    const uint16_t want = crc16((const uint8_t *)out,
                                sizeof(CalStore) - sizeof(out->crc));
    return out->crc == want;
}

void storage_save(CalStore *in)
{
    in->magic   = CAL_STORE_MAGIC;
    in->version = CAL_STORE_VERSION;
    in->crc     = crc16((const uint8_t *)in,
                        sizeof(CalStore) - sizeof(in->crc));
    EEPROM.put(CAL_STORE_ADDR, *in);   /* EEPROM.put only writes changed bytes */
}

bool settings_store_load(SettingsStore *out)
{
    EEPROM.get(SETTINGS_STORE_ADDR, *out);

    if (out->magic != SETTINGS_STORE_MAGIC)     return false;
    if (out->version != SETTINGS_STORE_VERSION) return false;

    const uint16_t want = crc16((const uint8_t *)out,
                                sizeof(SettingsStore) - sizeof(out->crc));
    return out->crc == want;
}

void settings_store_save(SettingsStore *in)
{
    in->magic   = SETTINGS_STORE_MAGIC;
    in->version = SETTINGS_STORE_VERSION;
    in->crc     = crc16((const uint8_t *)in,
                        sizeof(SettingsStore) - sizeof(in->crc));
    EEPROM.put(SETTINGS_STORE_ADDR, *in);   /* EEPROM.put only writes changed bytes */
}
