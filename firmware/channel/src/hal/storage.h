/**
 * storage.h - Persist the calibration coefficients in EEPROM with a magic +
 *             version + CRC16 guard so corrupt/blank EEPROM is detected.
 */
#ifndef CHANNEL_STORAGE_H
#define CHANNEL_STORAGE_H

#include <stdint.h>
#include "app/calibration.h"   /* CalCoeff */
#include "protocol.h"      /* CAL_COUNT */

#define CAL_STORE_MAGIC    0x43414C31UL  /* 'C''A''L''1' */
/* v2: default (uncalibrated) coefficients changed to the real channel transfer
 * functions (Vout = 15*Vset, Iout = Iset/1.2). Bumping the version invalidates
 * any EEPROM seeded with the old placeholder defaults so it is re-seeded. */
#define CAL_STORE_VERSION  2

struct CalStore {
    uint32_t magic;
    uint16_t version;
    CalCoeff coeff[CAL_COUNT];
    uint16_t crc;            /* CRC16 over all preceding bytes */
};

/* Load and validate. Returns true and fills *out on success. */
bool storage_load(CalStore *out);

/* Stamp magic/version, compute CRC and write to EEPROM. */
void storage_save(CalStore *in);

/* ---- Channel settings blob (separate from calibration) ------------------ */
/* Kept in its own EEPROM region with its own magic/version/CRC so evolving the
 * settings never risks invalidating (and wiping) the calibration coefficients,
 * and vice-versa. Holds the per-measurement averaging window lengths. */
#define SETTINGS_STORE_MAGIC    0x53455431UL  /* 'S''E''T''1' */
#define SETTINGS_STORE_VERSION  1

struct SettingsStore {
    uint32_t magic;
    uint16_t version;
    uint8_t  avg[AVG_COUNT]; /* [AVG_V], [AVG_I] moving-average window length */
    uint16_t crc;            /* CRC16 over all preceding bytes */
};

/* Load and validate. Returns true and fills *out on success. */
bool settings_store_load(SettingsStore *out);

/* Stamp magic/version, compute CRC and write to EEPROM. */
void settings_store_save(SettingsStore *in);

#endif /* CHANNEL_STORAGE_H */
