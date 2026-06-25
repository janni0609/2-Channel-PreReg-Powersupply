#include <Arduino.h>
#include <SPI.h>

#include "spi_bus.h"
#include "config.h"

/* Per-device SPI settings. The ADS1118 tolerates up to 4 MHz SCLK; the DAC up
 * to 20 MHz (write). 2/4 MHz are comfortably within both and keep edges clean
 * on a shared bus. */
static const SPISettings kAdcSettings(2000000, MSBFIRST, SPI_MODE1);
static const SPISettings kDacSettings(4000000, MSBFIRST, SPI_MODE0);

static uint8_t cs_pin(SpiDevice dev)
{
    return (dev == SPI_DEV_ADC) ? PIN_ADC_CS : PIN_DAC_CS;
}

void spi_bus_init()
{
    /* Chip selects idle high (deselected). */
    pinMode(PIN_ADC_CS, OUTPUT);
    digitalWrite(PIN_ADC_CS, HIGH);
    pinMode(PIN_DAC_CS, OUTPUT);
    digitalWrite(PIN_DAC_CS, HIGH);

    /* DAC latch held low = transparent: a volatile-register write updates the
     * output immediately. (Pulse-latching is not needed for this design.) */
    pinMode(PIN_DAC_LAT, OUTPUT);
    digitalWrite(PIN_DAC_LAT, LOW);

    SPI.begin();
}

void spi_bus_xfer(SpiDevice dev, const uint8_t *tx, uint8_t *rx, uint8_t len)
{
    const uint8_t cs = cs_pin(dev);

    SPI.beginTransaction(dev == SPI_DEV_ADC ? kAdcSettings : kDacSettings);
    digitalWrite(cs, LOW);

    for (uint8_t i = 0; i < len; i++) {
        const uint8_t in = SPI.transfer(tx ? tx[i] : 0x00);
        if (rx) rx[i] = in;
    }

    digitalWrite(cs, HIGH);
    SPI.endTransaction();
}
