#include <Arduino.h>

#include "board.h"
#include "config.h"

void board_init()
{
    pinMode(PIN_EN_DCDC, OUTPUT);
    pinMode(PIN_EN_LIN,  OUTPUT);
    digitalWrite(PIN_EN_DCDC, LOW);   /* pre-regulator off  */
    digitalWrite(PIN_EN_LIN,  LOW);   /* linear stage off   */

    pinMode(PIN_NTC, INPUT);
    analogReadResolution(NTC_ADC_BITS);
    /* Default analog reference is VDD, which matches the NTC divider (tied to
     * VDD), so the reading is ratiometric and supply-independent. */
}

void board_set_dcdc(bool on)
{
    digitalWrite(PIN_EN_DCDC, on ? HIGH : LOW);
}

void board_set_lin(bool on)
{
    digitalWrite(PIN_EN_LIN, on ? HIGH : LOW);
}

uint16_t board_read_ntc_raw()
{
    return (uint16_t)analogRead(PIN_NTC);
}
