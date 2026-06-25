#include <Arduino.h>

#include "setpoint.h"
#include "calibration.h"
#include "config.h"
#include "state.h"
#include "drivers/mcp48fvb22.h"

static uint16_t s_last_code[2];   /* indexed by CAL_VSET(0)/CAL_ISET(1) */

static uint16_t to_code(uint8_t target, float eng)
{
    float code_f = cal_apply(target, eng);
    if (code_f < 0.0f) code_f = 0.0f;
    if (code_f > (float)DAC_MAX_CODE) code_f = (float)DAC_MAX_CODE;
    return (uint16_t)lroundf(code_f);
}

void setpoint_init()
{
    s_last_code[CAL_VSET] = 0;
    s_last_code[CAL_ISET] = 0;
    setpoint_zero();
}

void setpoint_apply_voltage(int32_t mV)
{
    if (mV < 0) mV = 0;
    if (mV > SETPOINT_V_MAX_MV) mV = SETPOINT_V_MAX_MV;
    g_state.set_v_mV = mV;

    const uint16_t code = to_code(CAL_VSET, (float)mV);
    s_last_code[CAL_VSET] = code;
    mcp48_set_code(DAC_CH_VSET, code);
}

void setpoint_apply_current(int32_t mA)
{
    if (mA < 0) mA = 0;
    if (mA > SETPOINT_I_MAX_MA) mA = SETPOINT_I_MAX_MA;
    g_state.set_i_mA = mA;

    const uint16_t code = to_code(CAL_ISET, (float)mA);
    s_last_code[CAL_ISET] = code;
    mcp48_set_code(DAC_CH_ISET, code);
}

void setpoint_zero()
{
    mcp48_set_code(DAC_CH_VSET, 0);
    mcp48_set_code(DAC_CH_ISET, 0);
    s_last_code[CAL_VSET] = 0;
    s_last_code[CAL_ISET] = 0;
}

uint16_t setpoint_last_code(uint8_t target)
{
    if (target == CAL_VSET) return s_last_code[CAL_VSET];
    if (target == CAL_ISET) return s_last_code[CAL_ISET];
    return 0;
}
