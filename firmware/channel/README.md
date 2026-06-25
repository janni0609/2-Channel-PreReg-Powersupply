# Channel firmware (ATtiny1614)

Firmware for one power-supply **channel** MCU. Each channel drives its own
ADC + DAC and talks to the **Brain** (RP2350) over UART. Built with
**megaTinyCore** (Arduino) under PlatformIO.

## Build / flash

```sh
pio run                 # build
pio run -t upload       # flash via UPDI (SerialUPDI by default)
pio device monitor      # 1000000 baud (1 Mbps), the Brain link
```

Adjust `upload_protocol` in `platformio.ini` for your UPDI programmer.

## Architecture

Layered, cooperative super-loop (no RTOS). One shared `g_state` is the single
source of truth; modules update it, `comms` serializes it into telemetry.

```
main.cpp            super-loop scheduler + g_state
fsm.cpp             INIT -> SELFTEST -> IDLE <-> RUN -> FAULT
include/
  config.h          pin map, limits, tunables
  protocol.h        shared binary UART protocol (COPY into the Brain project)
src/hal/
  spi_bus.*         shared SPI, per-device mode switch + CS framing
  board.*           enable lines + NTC analog input
  storage.*         calibration EEPROM (magic + version + CRC16)
src/drivers/
  ads1118.*         16-bit ADC: single-shot, config readback self-test
  mcp48fvb22.*      dual 12-bit DAC: band-gap ref, gain 1x
src/app/
  measure.*         non-blocking V/I acquisition, PGA autoscale, power
  setpoint.*        engineering setpoint -> DAC code (via calibration)
  calibration.*     2-point linear cal for the 4 paths
  thermal.*         NTC -> degC, over-temp protection (independent of comms)
  output.*          enable sequencing (DCDC first, then linear)
  selftest.*        power-on ADC/DAC/temperature checks
  comms.*           frame parser, dispatch, telemetry/ACK/EVENT
```

### State machine

`SELFTEST` runs on boot; on pass -> `IDLE` (output off). `SET_OUTPUT 1` from
`IDLE` -> `RUN`. Over-temperature (>= 60 °C) from any state -> `FAULT`, which
forces both enable lines low. `FAULT` clears only via `RESET_FAULT` once the
temperature has dropped below 55 °C (hysteresis), then re-runs `SELFTEST`.

Over-temp protection runs in the thermal task + FSM, so a hung UART can never
keep the output enabled.

## UART protocol

Frame: `[SOF 0xA5][LEN][CMD][payload...][CRC8]`, little-endian, CRC-8 poly
0x07. See `include/protocol.h` for command IDs and payloads. The Channel pushes
`TELEMETRY` every 100 ms and ACK/NACKs every command; the Brain can also poll
with `GET_STATUS`.

Telemetry payload (16 B): `v_mV(i32) i_mA(i32) p_mW(i32) temp_cC(i16)
state(u8) flags(u8)`.

## Calibration

Every signal path is `y = gain*x + offset`, calibrated from two points and
stored in EEPROM. Procedure, per target (`CAL_VSET/ISET/VMEAS/IMEAS`):

1. Drive a low point (set a setpoint, or just read at low signal), measure the
   true value externally, send `CAL_POINT{target, index=0, actual}`.
2. Repeat for a high point with `index=1`.
3. `CAL_COMMIT{target}` solves and persists the line.

`CAL_RESET{target}` restores compile-time defaults.

## Pin map (corrected from requirements)

| Signal       | Pin  | Notes                                            |
|--------------|------|--------------------------------------------------|
| SPI MOSI     | PA1  | shared bus (ADC + DAC)                           |
| SPI MISO     | PA2  | shared bus                                       |
| SPI SCK      | PA3  | shared bus                                       |
| ADC_CS       | PA4  | **was PA3 in requirements (collided with SCK)**  |
| DAC_CS       | PA7  |                                                  |
| DAC_LAT      | PA6  | held low (transparent latch)                     |
| Enable_DCDC  | PB1  | pre-regulator                                    |
| Enable_lin   | PA5  | linear post-regulator                            |
| NTC          | PB0  | 10k NTC, Rtop=10k to VDD                          |
| UART TX/RX   | PB2/PB3 | USART0 -> the `Serial` object                 |

## Assumptions to confirm

These were chosen to make the firmware complete and buildable; please verify
against the hardware:

1. **ADC_CS = PA4** (requirements listed PA3, which is SCK).
2. **Enable sequence**: pre-regulator (DCDC) is enabled before the linear
   stage, and disabled after it. Swap in `output.cpp` if the hardware wants the
   opposite order.
3. **Setpoint units**: `SET_VOLTAGE`/`SET_CURRENT` carry mV/mA. The default
   (uncalibrated) map treats them as desired DAC output mV (0..2440). The real
   mapping to the supply's output range is established by 2-point calibration;
   `FLAG_CAL_INVALID` is reported until then.
4. **DAC reference**: internal band gap, gain 1x -> 0..2.44 V full scale (the
   "0..2.5 V" spec). The band gap is ±3.3 %, so calibration is required.
5. **ADS1118 PGA** starts at ±4.096 V and autoscales up for small signals; the
   front end is assumed to keep inputs within ±4.096 V at full output.
