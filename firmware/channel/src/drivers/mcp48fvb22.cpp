#include <Arduino.h>

#include "mcp48fvb22.h"
#include "config.h"
#include "hal/spi_bus.h"

/* ---- Register addresses (5-bit) ----------------------------------------- */
#define MCP_REG_DAC0    0x00u
#define MCP_REG_DAC1    0x01u
#define MCP_REG_VREF    0x08u
#define MCP_REG_PD      0x09u
#define MCP_REG_GAIN    0x0Au

/* ---- Command bits (C1:C0) ----------------------------------------------- */
#define MCP_CMD_WRITE   0b00u
#define MCP_CMD_READ    0b11u

/* Command byte = [AD4:AD0][C1:C0][x] */
#define MCP_CMDBYTE(addr, cmd)  (uint8_t)(((addr) << 3) | ((cmd) << 1))

/* ---- Register values we program ----------------------------------------- */
/* VREF: each DAC 2 bits, 01 = internal band gap. DAC1[3:2], DAC0[1:0]. */
#define MCP_VREF_BANDGAP_BOTH   0x0005u   /* 0b0101 */
/* GAIN: G1 (bit9), G0 (bit8) = 0 -> 1x for both. */
#define MCP_GAIN_1X_BOTH        0x0000u
/* PD: 00 = normal for both. */
#define MCP_PD_NORMAL_BOTH      0x0000u

static void mcp_write_reg(uint8_t addr, uint16_t value)
{
    uint8_t tx[3] = {
        MCP_CMDBYTE(addr, MCP_CMD_WRITE),
        (uint8_t)(value >> 8),
        (uint8_t)value
    };
    spi_bus_xfer(SPI_DEV_DAC, tx, nullptr, 3);
}

static uint16_t mcp_read_reg(uint8_t addr)
{
    uint8_t tx[3] = { MCP_CMDBYTE(addr, MCP_CMD_READ), 0x00, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };
    spi_bus_xfer(SPI_DEV_DAC, tx, rx, 3);
    /* rx[0] is the command-byte echo (incl. CMDERR); data is in rx[1..2]. */
    return (uint16_t)(((uint16_t)rx[1] << 8) | rx[2]);
}

bool mcp48_init()
{
    mcp_write_reg(MCP_REG_VREF, MCP_VREF_BANDGAP_BOTH);
    mcp_write_reg(MCP_REG_GAIN, MCP_GAIN_1X_BOTH);
    mcp_write_reg(MCP_REG_PD,   MCP_PD_NORMAL_BOTH);

    /* Start both outputs at zero. */
    mcp_write_reg(MCP_REG_DAC0, 0);
    mcp_write_reg(MCP_REG_DAC1, 0);

    return mcp48_selftest();
}

void mcp48_set_code(DacChannel ch, uint16_t code)
{
    if (code > DAC_MAX_CODE) code = DAC_MAX_CODE;
    const uint8_t addr = (ch == DAC_CH_VSET) ? MCP_REG_DAC0 : MCP_REG_DAC1;
    mcp_write_reg(addr, code);
}

bool mcp48_selftest()
{
    const uint16_t vref = mcp_read_reg(MCP_REG_VREF);
    /* Only the low 4 bits are implemented. */
    return (uint16_t)(vref & 0x000Fu) == MCP_VREF_BANDGAP_BOTH;
}
