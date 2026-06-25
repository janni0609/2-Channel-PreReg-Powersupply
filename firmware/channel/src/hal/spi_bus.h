/**
 * spi_bus.h - Shared SPI bus arbitration for the ADS1118 and MCP48FVB22.
 *
 * Both devices sit on one bus (MOSI/MISO/SCK = PA1/PA2/PA3) but use DIFFERENT
 * SPI modes: the ADS1118 is mode 1 (CPOL=0, CPHA=1) and the MCP48FVB22 is
 * mode 0. Every transfer therefore re-applies the correct SPISettings and
 * frames its own chip-select, so callers can never mix the two up.
 */
#ifndef CHANNEL_SPI_BUS_H
#define CHANNEL_SPI_BUS_H

#include <stdint.h>

enum SpiDevice {
    SPI_DEV_ADC = 0,   /* ADS1118    - mode 1 */
    SPI_DEV_DAC = 1    /* MCP48FVB22 - mode 0 */
};

/* Initialise the SPI peripheral and chip-select / latch pins. */
void spi_bus_init();

/*
 * Full-duplex transfer of `len` bytes to the selected device. `rx` may be the
 * same buffer as `tx` (in-place) or nullptr if the read data is not needed.
 * CS is asserted for the whole transfer and the correct SPI mode is applied.
 */
void spi_bus_xfer(SpiDevice dev, const uint8_t *tx, uint8_t *rx, uint8_t len);

#endif /* CHANNEL_SPI_BUS_H */
