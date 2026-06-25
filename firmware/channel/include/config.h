/**
 * config.h - Board pin map, build-time constants and tunables for the
 *            power-supply CHANNEL firmware (ATtiny1614 @ 3.3 V).
 *
 * All hardware-specific numbers live here so the rest of the code stays
 * portable and readable. Pins use megaTinyCore's PIN_Pxn macros.
 *
 * NOTE ON ADC_CS: the requirements doc lists ADC_CS on PA3, which collides
 * with SCK. The bus is shared (MOSI/MISO/SCK on PA1/PA2/PA3) so ADC_CS must
 * be its own line; it is wired to PA4 (the ATtiny1614 default SPI SS).
 */
#ifndef CHANNEL_CONFIG_H
#define CHANNEL_CONFIG_H

#include <Arduino.h>

/* ------------------------------------------------------------------ */
/* Pin map (ATtiny1614, 14-pin SOIC)                                  */
/* ------------------------------------------------------------------ */

/* Shared SPI bus (handled by the SPI library on the default pins):
 *   MOSI = PA1, MISO = PA2, SCK = PA3                                 */
#define PIN_ADC_CS      PIN_PA4   /* ADS1118    chip select (active low) */
#define PIN_DAC_CS      PIN_PA7   /* MCP48FVB22 chip select (active low) */
#define PIN_DAC_LAT     PIN_PA6   /* MCP48FVB22 latch (active low)       */

/* Output enable lines */
#define PIN_EN_DCDC     PIN_PB1   /* Enable_DCDC : pre-regulator (buck)  */
#define PIN_EN_LIN      PIN_PA5   /* Enable_lin  : linear post-regulator */

/* Thermistor divider (on-chip ADC input) */
#define PIN_NTC         PIN_PB0   /* 10k NTC, Rtop=10k to VDD, Rbot=NTC  */

/* UART to the Brain is USART0 on PB2(TX)/PB3(RX) -> the 'Serial' object */
#define BRAIN_UART      Serial
#define BRAIN_BAUD      1000000UL   /* 1 Mbps = F_CPU(16M)/16, USART0 Normal-mode max */

/* ------------------------------------------------------------------ */
/* NTC thermistor (10k, B=3380K, beta model)                          */
/* ------------------------------------------------------------------ */
#define NTC_R_TOP_OHMS      10000.0f   /* fixed top resistor to VDD       */
#define NTC_R0_OHMS         10000.0f   /* NTC nominal resistance @ T0     */
#define NTC_T0_KELVIN       298.15f    /* 25 C                            */
#define NTC_BETA            3380.0f    /* B-constant (K)                  */
#define NTC_ADC_BITS        10         /* analogRead resolution we use    */

/* ------------------------------------------------------------------ */
/* Thermal protection                                                 */
/* ------------------------------------------------------------------ */
#define TEMP_OTP_TRIP_C     60.0f      /* shut output off at/above this   */
#define TEMP_OTP_REARM_C    55.0f      /* allow recovery below this       */
#define TEMP_SELFTEST_MIN_C (-10.0f)   /* startup must be within this ... */
#define TEMP_SELFTEST_MAX_C 60.0f      /* ... range                       */

/* ------------------------------------------------------------------ */
/* DAC (MCP48FVB22) - internal band gap, gain 1x -> 0..2.44 V FS       */
/* ------------------------------------------------------------------ */
#define DAC_FULLSCALE_V     2.44f      /* VRL = 2 x 1.22V band gap, G=1x  */
#define DAC_MAX_CODE        4095u      /* 12-bit                          */

/* ------------------------------------------------------------------ */
/* ADC (ADS1118)                                                      */
/* ------------------------------------------------------------------ */
/* Front-end full scale sits near +-4.096 V; start there and let the
 * autoscaler raise gain for small signals. Indices map to ADS1118 PGA. */
#define ADC_PGA_START_INDEX     1u     /* 001 = +-4.096 V                 */
#define ADC_PGA_MIN_INDEX       1u     /* never below this (>= no benefit)*/
#define ADC_PGA_MAX_INDEX       5u     /* 101 = +-0.256 V (max gain)      */
#define ADC_AUTOSCALE_UP_PCT    88     /* |code| above % FS -> lower gain */
#define ADC_AUTOSCALE_DN_PCT    42     /* |code| below % FS -> higher gain*/

/* ------------------------------------------------------------------ */
/* Task periods (ms) for the cooperative scheduler                    */
/* ------------------------------------------------------------------ */
#define PERIOD_THERMAL_MS       100u
#define PERIOD_TELEMETRY_MS     100u
#define PERIOD_HOUSEKEEP_MS     1000u
/* measurement paces itself off the ADC conversion time, see measure.cpp */

/* ------------------------------------------------------------------ */
/* Setpoint limits (engineering units, see README for the convention) */
/* ------------------------------------------------------------------ */
#define SETPOINT_V_MAX_MV       30000   /* clamp for incoming set voltage  */
#define SETPOINT_I_MAX_MA       5000    /* clamp for incoming set current  */

#endif /* CHANNEL_CONFIG_H */
