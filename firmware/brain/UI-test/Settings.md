# Settings — Submenu Structure

## Channel 1 Settings

| Item          | Description                                                                    |
| ------------- | ------------------------------------------------------------------------------ |
| Temp          | Measured temperature |
| Volt          | Measured voltage |
| Curr          | Measured current |
| OTP           | Set OTP (0–100 °C); reads the OTP from the channel and updates it if changed|
| DAC state     | OK / not OK |
| ADC state     | OK / not OK |
| Communication | OK / not OK (comm of channel with brain) |
| Runtime       | Read from channel: the operating time |
| Cal values V  | Read from channel and display them |
| Cal values I  | Read from channel and display them |
| Manual Cal V  | Enter manual cal procedure for voltage (add confirmation so it is not pressed axidantely) |
| Manual Cal I  | Enter manual cal procedure for current (add confirmation so it is not pressed axidantely) |

## Channel 2 Settings

Same as Channel 1.

## Temp and Fan Settings

| Item           | Description                                                     |
| -------------- | --------------------------------------------------------------- |
| Temp Ch1       | Measured temperature CH1                                        |
| Temp Ch2       | Measured temperature CH2                                        |
| Temp heatsink  | Measured temperature brain (heatsink)                           |
| Temp max       | Max of all 3                                                    |
| SYS OTP        | OTP setpoint managed by the brain; compared against max temp    |
| Fan min speed  | Setpoint of fan minimum speed (%)                               |
| Fan start temp | Setpoint of fan start temperature (0–100 °C)                    |
| Fan max temp   | Setpoint of fan max temperature (0–100 °C)                      |

## Network

| Item        | Description                                                                           |
| ----------- | ------------------------------------------------------------------------------------- |
| Status      | Link up / down, current IP (read only, actual values in use)                          |
| MAC address | e.g. `A8:61:0A:12:34:56` (read only)                                                  |
| DHCP        | ON / OFF — when ON: IP/Netmask/Gateway below are read only and show the leased values |
| IP address  | e.g. `192.168.1.50` (set with encoder, octet by octet)                                |
| Netmask     | e.g. `/24` (set with encoder as CIDR prefix, shown as `255.255.255.0`)                |
| Gateway     | e.g. `192.168.1.1` (set with encoder, needed for access from other subnets)           |
| Hostname    | e.g. `psu-2ch`                                                                        |
| Apply       | Applies the settings above and restarts the network interface                         |

Edits do nothing until **Apply**, so a half-edited IP config never goes live.

## System

| Item        | Description                                                                           |
| ----------- | ------------------------------------------------------------------------------------- |
| firmware version      | fw version |
| remember set values on restart| Yes/No |
| Beeper | ON / OFF |
| Brain runtime | Brain runtime|
