/**
 * comms.h - UART link to the Brain: frame parser, command dispatch and
 *           outbound telemetry/ACK/EVENT using the shared protocol.
 */
#ifndef CHANNEL_COMMS_H
#define CHANNEL_COMMS_H

#include <stdint.h>

void comms_init();

/* Drain the UART RX buffer and dispatch any complete frames. */
void comms_task();

/* Send one telemetry frame built from g_state. */
void comms_send_telemetry();

/* Send an asynchronous event/error notification. */
void comms_send_event(uint8_t code);

/* Milliseconds since the last valid frame from the Brain was received.
 * The FSM uses this as a link-loss watchdog (see COMMS_TIMEOUT_MS). */
uint32_t comms_since_rx_ms();

#endif /* CHANNEL_COMMS_H */
