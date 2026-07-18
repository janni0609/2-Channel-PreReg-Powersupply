# SCPI Command Reference — 2-Channel Bench Power Supply

Remote-control command set for the power supply. Commands are sent from a PC to
the PSU over **USB (CDC-ACM / USB-TMC)** or **Ethernet (raw TCP, port 5025 —
the SCPI "instrument" port)**. Both transports address the same command parser
on the **Brain** (RP2350), which translates every command into the binary UART
protocol spoken to each **Channel** (ATtiny1614). See
`firmware/channel/include/protocol.h` for that lower layer.

The instrument aims to be a well-behaved **SCPI-1999 / IEEE-488.2** subset so it
works with VISA, PyVISA, `lxi`, NI-MAX, and generic terminal tools.

---

## 1. Instrument model

| Property                | Value                                                   |
|-------------------------|---------------------------------------------------------|
| Channels                | 2 (CH1, CH2) — independent, isolated                    |
| Output voltage          | 0 … 35.000 V per channel                                |
| Output current limit    | 0 … 2.000 A per channel                                 |
| Setpoint resolution     | 1 mV / 1 mA (protocol carries int32 mV / mA)            |
| Measurement             | V, I, P (V·I) per channel; one system temperature        |
| Temperature             | Highest of the Brain sensor and both Channel sensors    |
| Regulation              | CV / current-limited output; the firmware does **not** report which mode is active |
| Protection              | Over-temperature only (hardware OTP, ≥60 °C per channel). No OVP/OCP. |
| Calibration             | 2-point linear, 4 paths per channel, stored in EEPROM   |

> **Firmware note:** the setpoint clamps in `firmware/channel/include/config.h`
> (`SETPOINT_V_MAX_MV`, `SETPOINT_I_MAX_MA`) must match these limits — set them
> to `35000` and `2000` respectively.

---

## 2. Syntax conventions

- Commands are **case-insensitive**. Upper-case letters show the accepted short
  form; lower-case letters are optional (`VOLTage` ⇒ `VOLT` or `VOLTAGE`).
- A command **line** is terminated by `\n` (LF). `\r` is ignored. Max line
  length 256 bytes.
- Multiple commands may be **chained** with `;` on one line. A leading `:` after
  `;` restarts from the root (`:VOLT 5;:CURR 1`).
- **Queries** end in `?` and return a single line terminated by `\n`.
- Numeric parameters accept decimals and SI suffixes/multipliers
  (`2.5`, `2500m`, `2.5V`). Booleans accept `ON|OFF|1|0`.
- `MIN | MAX | DEF` may be used anywhere a numeric value is expected, and as a
  query argument (`VOLT? MAX`).
- **Channel selection** — two equivalent styles are supported:
  - *Stateful:* select with `INSTrument:NSELect <n>`; subsequent SOURce/MEASure
    commands act on that channel.
  - *Per-command channel list:* append `(@<list>)`, e.g. `(@1)`, `(@2)`,
    `(@1,2)`. A channel list overrides the selected channel for that command
    only. Queries with `(@1,2)` return comma-separated values, CH1 first.

Notation below: `<n>` = channel 1|2, `<NRf>` = flexible numeric, `<bool>` =
ON|OFF|1|0, `[]` = optional node, `{a|b}` = choice.

---

## 3. IEEE-488.2 common commands

| Command | Action |
|---------|--------|
| `*IDN?` | Identify. Returns `Manufacturer,Model,Serial,FW-version` e.g. `PreReg,PSU-2CH-30V5A,SN00001,1.0.0` |
| `*RST`  | Reset to default state: both outputs **OFF**, V=0, I limit=MAX, protections cleared. Maps to `SET_OUTPUT 0` on both channels. |
| `*CLS`  | Clear status: error queue, Standard Event (ESR) and Questionable/Operation event registers. |
| `*ESE <NRf>` / `*ESE?` | Set/query Standard Event Status **Enable** mask. |
| `*ESR?` | Read & clear Standard Event Status register. |
| `*SRE <NRf>` / `*SRE?` | Set/query Service Request Enable mask. |
| `*STB?` | Read Status Byte (does not clear). |
| `*OPC` / `*OPC?` | Operation complete. `*OPC?` returns `1` when all pending overlapped commands finish. All commands here are sequential, so `*OPC?` returns `1` immediately. |
| `*WAI` | Wait for pending operations (no-op; sequential execution). |
| `*TST?` | Run/report power-on **self-test**. Returns `0` = pass, non-zero = fault bitmask (see §10). Re-runs the Channel `SELFTEST` state. |
| `*SAV {0..3}` | Save current setpoints/output state to instrument memory slot. |
| `*RCL {0..3}` | Recall a saved slot. |

---

## 4. INSTrument — channel selection

| Command | Description |
|---------|-------------|
| `INSTrument[:SELect] {CH1|CH2}` | Select the active channel by name. |
| `INSTrument:NSELect <n>` / `?` | Select/query active channel by number (1\|2). |
| `INSTrument[:SELect]?` | Query active channel name. |
| `INSTrument:CATalog?` | List available channels → `CH1,CH2`. |

---

## 5. SOURce subsystem (setpoints & protection)

The `SOURce:` root node is optional and usually omitted. Commands act on the
selected channel unless a `(@<list>)` suffix is given.

### 5.1 Voltage

| Command | Description | UART mapping |
|---------|-------------|--------------|
| `[SOURce:]VOLTage[:LEVel][:IMMediate][:AMPLitude] <NRf>[,(@list)]` | Set output voltage (V). | `CMD_SET_VOLTAGE` (int32 mV) |
| `[SOURce:]VOLTage? [{MIN\|MAX\|DEF}][,(@list)]` | Query voltage **setpoint**. | — |

### 5.2 Current (current limit)

The current setpoint **is** the current limit — the channel holds this current
when the load demands it. The firmware does not report a CV/CC mode flag, so
there is no query for the active regulation mode.

| Command | Description | UART mapping |
|---------|-------------|--------------|
| `[SOURce:]CURRent[:LEVel][:IMMediate][:AMPLitude] <NRf>[,(@list)]` | Set current limit (A). | `CMD_SET_CURRENT` (int32 mA) |
| `[SOURce:]CURRent? [{MIN\|MAX\|DEF}][,(@list)]` | Query current setpoint. |

> **Protection:** the only protection is the Channel's autonomous
> over-**temperature** shutdown (≥60 °C, `EVT_OVERTEMP`). It cannot be enabled,
> disabled, or configured remotely. There is **no** OVP or OCP — the current
> setpoint is a simple current limit, nothing more.

### 5.3 Combined apply (convenience)

| Command | Description |
|---------|-------------|
| `APPLy {CH1\|CH2},<volt>,<curr>` | Set voltage and current in one call. |
| `APPLy? [{CH1\|CH2}]` | Query as `<volt>,<curr>`. |

---

## 6. OUTPut subsystem

| Command | Description | UART mapping |
|---------|-------------|--------------|
| `OUTPut[:STATe] <bool>[,(@list)]` | Enable/disable output. On enable the Channel runs its DCDC→linear sequence (`IDLE→RUN`). | `CMD_SET_OUTPUT` (u8 0/1) |
| `OUTPut[:STATe]? [(@list)]` | Query output state → `0`/`1`. | telemetry `FLAG_OUTPUT_ON` |
| `OUTPut:PROTection:CLEar[,(@list)]` | Clear a latched over-temperature `FAULT` on the channel (once cooled). | `CMD_RESET_FAULT` |

> There is no `OUTPut:MODE?` query — the firmware does not distinguish CV from
> current-limited operation, so the active regulation mode is not observable.

`OUTPut:STATe` refuses to turn on a channel that is in `FAULT` or reports
`FLAG_CAL_INVALID` as a hard error (see §10) unless overridden — bring the
channel out of fault first with `OUTP:PROT:CLE`.

---

## 7. MEASure / FETCh (readback)

`MEASure:...?` returns the most recent telemetry (the Channel pushes telemetry
every 100 ms; the Brain caches it and may force a fresh `CMD_GET_STATUS`).
`FETCh:...?` returns the cached value without forcing a poll.

| Query | Returns | Telemetry field |
|-------|---------|-----------------|
| `MEASure[:SCALar]:VOLTage[:DC]? [(@list)]` | Measured output voltage (V) | `v_mV` |
| `MEASure[:SCALar]:CURRent[:DC]? [(@list)]` | Measured output current (A) | `i_dmA` |
| `MEASure[:SCALar]:POWer[:DC]? [(@list)]`   | Measured output power (W)   | `p_mW` |
| `MEASure:TEMPerature?`                      | **System** temperature (°C) — the highest of the Brain sensor and both Channel `temp_cC` readings | max of all |
| `MEASure:ALL? [(@list)]`                   | `V,I,P` comma-separated (per channel) | frame |
| `FETCh:VOLTage? / :CURRent? / :POWer? / :ALL?` | Cached equivalents      | — |

`MEASure:TEMPerature?` takes **no** channel list — it is a single instrument
value (the hottest of Brain + CH1 + CH2), which is also what drives OTP
reporting. For `(@1,2)` a scalar V/I/P query returns `ch1,ch2`; `MEAS:ALL?
(@1,2)` returns `v1,i1,p1,v2,i2,p2`.

**Numeric format:** measured **voltage** is returned with **3** decimal places
(1 mV resolution, e.g. `12.001`) and measured **current** with **4** decimal
places (0.1 mA resolution, e.g. `0.2531`), matching the front-panel readout.

---

## 8. STATus subsystem (SCPI status model)

The instrument maps Channel telemetry flags/state into the SCPI Questionable
and Operation status registers.

| Command | Description |
|---------|-------------|
| `STATus:QUEStionable[:EVENt]?` | Read & clear Questionable event register. |
| `STATus:QUEStionable:CONDition?` | Read live Questionable condition. |
| `STATus:QUEStionable:ENABle <NRf>` / `?` | Enable mask. |
| `STATus:OPERation[:EVENt]?` / `:CONDition?` / `:ENABle` | Operation register set. |
| `STATus:PRESet` | Set enable masks to default. |

**Questionable-status bit assignment (per channel, OR'd into the summary):**

| Bit | Weight | Meaning | Source |
|-----|--------|---------|--------|
| 4 | 16  | Over-temperature | `FLAG_OVERTEMP` / `EVT_OVERTEMP` |
| 8 | 256 | Calibration invalid | `FLAG_CAL_INVALID` |
| 9 | 512 | ADC fault | `FLAG_ADC_FAULT` |
| 10| 1024| DAC fault | `FLAG_DAC_FAULT` |
| 11| 2048| Channel in FAULT state | `ST_FAULT` |

**Operation-status bits:** bit 8 (256) = CH1 output ON, bit 9 (512) = CH2 output
ON (`FLAG_OUTPUT_ON`). The firmware exposes no CV/CC mode flag, so there is no
constant-current status bit.

---

## 9. SYSTem subsystem

| Command | Description |
|---------|-------------|
| `SYSTem:ERRor[:NEXT]?` | Pop oldest entry from the error queue → `<code>,"<message>"`. `0,"No error"` when empty. |
| `SYSTem:ERRor:COUNt?` | Number of queued errors. |
| `SYSTem:VERSion?` | SCPI version → `1999.0`. |
| `SYSTem:CHANnel:COUNt?` | Number of channels → `2`. |
| `SYSTem:BEEPer[:IMMediate]` | Sound the panel buzzer once. |
| `SYSTem:BEEPer:STATe <bool>` / `?` | Enable/disable UI beeps. |
| `SYSTem:LOCal` / `SYSTem:REMote` / `SYSTem:RWLock` | Local / remote / remote-with-lockout front-panel control. |
| `SYSTem:COMMunicate:LAN:IPADdress <ip>` / `?` | Static IP (Ethernet). |
| `SYSTem:COMMunicate:LAN:DHCP <bool>` / `?` | Enable DHCP. |
| `SYSTem:COMMunicate:LAN:MAC?` | MAC address. |
| `SYSTem:UPTime?` | Seconds since boot. |

---

## 10. CALibration subsystem

Exposes the Channel's 2-point linear calibration (`y = gain·x + offset`) for all
four signal paths. Requires unlocking with a code to prevent accidental changes.
Maps to `CMD_CAL_POINT`, `CMD_CAL_COMMIT`, `CMD_CAL_RESET`.

Path selector `<path>` = `VSET | ISET | VMEAS | IMEAS`
(→ `CAL_VSET/ISET/VMEAS/IMEAS`).

| Command | Description | UART mapping |
|---------|-------------|--------------|
| `CALibration:STATe <bool>,<code>` | Unlock/lock calibration (default code `0`). | — |
| `CALibration:STATe?` | `1` if unlocked. | — |
| `CALibration:POINt <path>,{0\|1},<actual>` | Record a cal point: index 0 = low, 1 = high; `<actual>` = externally measured true value (V or A). | `CMD_CAL_POINT{target,index,actual}` |
| `CALibration:COMMit <path>` | Solve the line from the two points and persist to EEPROM. | `CMD_CAL_COMMIT{target}` → `EVT_CAL_STORED`/`EVT_CAL_INVALID` |
| `CALibration:RESet <path>` | Restore compile-time default gain/offset. | `CMD_CAL_RESET{target}` |
| `CALibration:DATA? <path>` | Query stored `gain,offset` for a path. | — |
| `CALibration:VALid? [(@list)]` | `1` if all paths calibrated (not `FLAG_CAL_INVALID`). | telemetry flag |

**Typical procedure (per channel, VSET example):**
```
INST:NSEL 1
CAL:STAT ON,0
VOLT 5.0            ; drive a low point
CAL:POIN VSET,0,4.987   ; report DMM reading
VOLT 25.0           ; drive a high point
CAL:POIN VSET,1,24.912
CAL:COMM VSET       ; solve & store
CAL:STAT OFF,0
```

---

## 11. Error / status codes

Standard SCPI error queue (negative = SCPI-defined, positive = device-specific):

| Code | Message | Cause |
|------|---------|-------|
| `0`    | No error | queue empty |
| `-100` | Command error | unrecognized command header |
| `-108` | Parameter not allowed | too many parameters |
| `-109` | Missing parameter | too few parameters |
| `-113` | Undefined header | unknown command |
| `-120` | Numeric data error | malformed number |
| `-222` | Data out of range | value clamped to MIN/MAX (e.g. V>35, I>2) |
| `-241` | Hardware missing | channel not responding on UART |
| `-315` | Configuration memory lost | EEPROM cal CRC bad |
| `+101` | Channel self-test failed | `EVT_SELFTEST_FAIL` |
| `+102` | Over-temperature | `EVT_OVERTEMP` (output forced off) |
| `+103` | ADC fault | `EVT_ADC_FAULT` |
| `+104` | DAC fault | `EVT_DAC_FAULT` |
| `+105` | Calibration invalid | `EVT_CAL_INVALID` / `FLAG_CAL_INVALID` |
| `+106` | Output enable refused | channel in FAULT / cal invalid |
| `+107` | UART CRC / framing error | link error to a channel |

`*TST?` fault bitmask (bit set = failed): b0 CH1 comms, b1 CH1 selftest,
b2 CH2 comms, b3 CH2 selftest, b4 any over-temp at boot.

---

## 12. Brain ↔ Channel mapping summary

| SCPI action | UART command (per channel) |
|-------------|----------------------------|
| `VOLT <v>` | `CMD_SET_VOLTAGE` (int32 mV) |
| `CURR <i>` | `CMD_SET_CURRENT` (int32 mA) |
| `OUTP ON/OFF` | `CMD_SET_OUTPUT` (u8) |
| `MEAS:*?`, `FETC:*?` | cached `CMD_TELEMETRY`, or `CMD_GET_STATUS` poll |
| `OUTP:PROT:CLE`, `*RST` fault clear | `CMD_RESET_FAULT` |
| `CAL:POIN` | `CMD_CAL_POINT` |
| `CAL:COMM` | `CMD_CAL_COMMIT` |
| `CAL:RES` | `CMD_CAL_RESET` |
| connectivity check | `CMD_PING` → `CMD_ACK` |

Async Channel `CMD_EVENT` frames (BOOT, OVERTEMP, faults, cal-stored) are
converted by the Brain into error-queue entries and Questionable-status bits,
so the host sees them via `SYST:ERR?` / `STAT:QUES?` and, if enabled, an SRQ.

---

## 13. Worked examples

**Basic setup — 12 V, 1 A limit on CH1, read back:**
```
*RST
INST:NSEL 1
APPL CH1,12.0,1.0
OUTP ON
MEAS:VOLT?      ; -> 12.001
MEAS:CURR?      ; -> 0.2531
MEAS:POW?       ; -> 3.036
```

**Both channels at once via channel list:**
```
VOLT 5.0,(@1,2)
CURR 2.0,(@1,2)
OUTP ON,(@1,2)
MEAS:ALL? (@1,2)   ; -> v1,i1,p1,v2,i2,p2
MEAS:TEMP?         ; -> 41.2   (hottest of brain + both channels)
```

**Recover a channel after an over-temperature shutdown:**
```
STAT:QUES:COND?    ; bit4 (16) set -> over-temperature
OUTP:PROT:CLE (@1) ; clears the FAULT once cooled below re-arm temp
OUTP ON (@1)
```

**Poll for errors (PyVISA idiom):**
```
while True:
    code, msg = psu.query("SYST:ERR?").split(",", 1)
    if int(code) == 0: break
    log(code, msg)
```

---

*Implementation note:* this document is the **specification** for the SCPI layer
that runs on the Brain (RP2350). The parser should be table-driven (header tree +
handler), keep a per-session error queue, and translate every accepted command
into the binary UART frames defined in `firmware/channel/include/protocol.h`.
Values are clamped to the Channel limits (`SETPOINT_V_MAX_MV`,
`SETPOINT_I_MAX_MA`) with a `-222` warning rather than being rejected.
