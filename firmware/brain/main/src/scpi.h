/**
 * scpi.h - SCPI-1999 / IEEE-488.2 subset command parser for the RP2350 brain.
 *
 * Transport-agnostic: a transport (the W5500 TCP server in netcfg.cpp, the USB
 * CDC console in main.cpp) assembles one command line and hands it to
 * scpi_process_line() along with a writer callback that routes the response back
 * to that session. The parser translates accepted commands into the binary UART
 * protocol (channel_link.*) and the brain application state (brain_api.h).
 *
 * The full command reference this implements lives in firmware/SCPI/commands.md.
 */
#ifndef BRAIN_SCPI_H
#define BRAIN_SCPI_H

#include <stddef.h>

/* Sink for response bytes. `ctx` is the transport's opaque per-session handle
 * (e.g. the EthernetClient*). The parser calls it with the complete response
 * line, including the trailing '\n'. */
typedef void (*ScpiWriteFn)(void *ctx, const char *data, size_t len);

/* One-time init: clears the error queue, status registers and selection. */
void scpi_init();

/* Parse and execute one command line (LF/CR already stripped, NUL-terminated).
 * Any query output is written via `write`; command-only lines produce nothing. */
void scpi_process_line(const char *line, ScpiWriteFn write, void *ctx);

/* Report that a transport dropped an over-length input line (queues a command
 * error). `write`/`ctx` are accepted for symmetry but no response is emitted. */
void scpi_report_line_overflow(ScpiWriteFn write, void *ctx);

/* Periodic housekeeping: fold asynchronous channel events (boot, over-temp,
 * faults, cal-stored) into the error queue and latch the status-register event
 * bits from live conditions. Call once per main loop. */
void scpi_task();

#endif /* BRAIN_SCPI_H */
