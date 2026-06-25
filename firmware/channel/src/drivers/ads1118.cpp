#include <Arduino.h>

#include "ads1118.h"
#include "hal/spi_bus.h"

/* ---- Config register bit fields (datasheet Fig. 44 / Table 7) ----------- */
#define ADS_SS          (1u << 15)            /* start single conversion      */
#define ADS_MUX(x)      (((uint16_t)(x) & 7u) << 12)
#define ADS_PGA(x)      (((uint16_t)(x) & 7u) << 9)
#define ADS_MODE_SS     (1u << 8)             /* single-shot / power-down     */
#define ADS_DR(x)       (((uint16_t)(x) & 7u) << 5)
#define ADS_TS_ADC      (0u << 4)             /* ADC mode (not temp sensor)   */
#define ADS_PULLUP_EN   (1u << 3)
#define ADS_NOP_VALID   (0b01u << 1)          /* required to write config     */
#define ADS_RESERVED    (1u << 0)             /* reads 1                      */

#define ADS_DR_128SPS   0b100u                /* 128 SPS (~7.8 ms)            */

/* MUX codes for the two differential pairs. */
#define ADS_MUX_V       0b000u                /* AIN0(P) - AIN1(N)            */
#define ADS_MUX_I       0b011u                /* AIN2(P) - AIN3(N)            */

/* PGA index -> full-scale volts. */
const float kAdsFsVolts[8] = {
    6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f, 0.256f, 0.256f
};

static uint16_t build_config(AdsChannel ch, uint8_t pga)
{
    const uint16_t mux = (ch == ADS_CH_V) ? ADS_MUX_V : ADS_MUX_I;
    return (uint16_t)(ADS_SS
                    | ADS_MUX(mux)
                    | ADS_PGA(pga)
                    | ADS_MODE_SS
                    | ADS_DR(ADS_DR_128SPS)
                    | ADS_TS_ADC
                    | ADS_PULLUP_EN
                    | ADS_NOP_VALID
                    | ADS_RESERVED);
}

uint8_t ads1118_conv_time_ms()
{
    return 9;   /* 1/128 SPS = 7.8 ms, rounded up with margin */
}

void ads1118_start(AdsChannel ch, uint8_t pga)
{
    const uint16_t cfg = build_config(ch, pga);
    uint8_t tx[2] = { (uint8_t)(cfg >> 8), (uint8_t)cfg };
    spi_bus_xfer(SPI_DEV_ADC, tx, nullptr, 2);   /* loads config, starts conv */
}

int16_t ads1118_read(AdsChannel ch, uint8_t pga)
{
    const uint16_t cfg = build_config(ch, pga);  /* re-arm same channel */
    uint8_t tx[2] = { (uint8_t)(cfg >> 8), (uint8_t)cfg };
    uint8_t rx[2] = { 0, 0 };
    spi_bus_xfer(SPI_DEV_ADC, tx, rx, 2);
    return (int16_t)(((uint16_t)rx[0] << 8) | rx[1]);
}

float ads1118_code_to_volts(int16_t code, uint8_t pga)
{
    return (float)code * (kAdsFsVolts[pga & 7u] / 32768.0f);
}

bool ads1118_selftest()
{
    /* 32-bit cycle: write Config twice, read [data16][config16] back. The
     * config we wrote reappears in the last two bytes (Fig. 40). */
    const uint16_t cfg = build_config(ADS_CH_V, 2);
    uint8_t tx[4] = { (uint8_t)(cfg >> 8), (uint8_t)cfg,
                      (uint8_t)(cfg >> 8), (uint8_t)cfg };
    uint8_t rx[4] = { 0, 0, 0, 0 };
    spi_bus_xfer(SPI_DEV_ADC, tx, rx, 4);

    const uint16_t rb = (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
    /* Ignore SS (auto-clears, reads 0) and the reserved bit0. */
    return (uint16_t)(rb & 0x7FFEu) == (uint16_t)(cfg & 0x7FFEu);
}
