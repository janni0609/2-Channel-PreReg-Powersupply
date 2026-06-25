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
#define CAL_STORE_VERSION  1

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

#endif /* CHANNEL_STORAGE_H */
