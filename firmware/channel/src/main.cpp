/**
 * main.cpp - Channel firmware entry point and cooperative scheduler.
 *
 * Boot order: init HAL -> drivers -> app modules -> FSM (which runs the
 * power-on self test on its first task call). The loop is a non-blocking
 * super-loop: measurement paces itself off the ADC, while thermal/telemetry
 * run on fixed periods. Over-temperature protection lives in the thermal task
 * and FSM, independent of the comms link.
 */
#include <Arduino.h>

#include "config.h"
#include "state.h"
#include "protocol.h"
#include "fsm.h"

#include "hal/spi_bus.h"
#include "hal/board.h"
#include "drivers/mcp48fvb22.h"

#include "app/calibration.h"
#include "app/setpoint.h"
#include "app/measure.h"
#include "app/thermal.h"
#include "app/output.h"
#include "app/comms.h"

/* The single shared run-state (declared extern in state.h). */
ChannelState g_state;

void setup()
{
    g_state.run_state = ST_INIT;

    BRAIN_UART.begin(BRAIN_BAUD);

    /* HAL */
    spi_bus_init();
    board_init();

    /* Calibration must load before drivers that use it. */
    cal_init();

    /* Drivers */
    mcp48_init();          /* program DAC reference/gain and zero outputs */

    /* App modules */
    setpoint_init();
    measure_init();
    thermal_init();
    output_init();
    comms_init();

    /* Hand over to the state machine (begins in SELFTEST). */
    fsm_init();
}

void loop()
{
    static uint32_t t_thermal = 0;
    static uint32_t t_tele    = 0;

    const uint32_t now = millis();

    /* Always-responsive tasks. */
    comms_task();
    measure_task();

    /* Periodic safety task. */
    if ((uint32_t)(now - t_thermal) >= PERIOD_THERMAL_MS) {
        t_thermal = now;
        thermal_task();
    }

    /* Periodic telemetry push. */
    if ((uint32_t)(now - t_tele) >= PERIOD_TELEMETRY_MS) {
        t_tele = now;
        comms_send_telemetry();
    }

    /* State machine (self-test, fault monitoring). */
    fsm_task();
}
