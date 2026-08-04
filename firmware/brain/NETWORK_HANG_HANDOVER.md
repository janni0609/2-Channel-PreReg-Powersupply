# Handover — brain freezes during sustained SCPI sessions

**Date:** 2026-08-04
**Symptom:** the brain (RP2350) stops running `loop()` after a few minutes of
continuous SCPI-over-Ethernet traffic. Only a power cycle recovers it reliably.
**Status:** **ROOT CAUSE FOUND, FIXED AND VERIFIED** (2026-08-04, later the same
day). See §0 for the cause, §0c for the fixes and the evidence they work.

---

## 0. Root cause — `socketSend()` blocks forever on a stalled TCP peer

`EthernetClass::socketSend()`, `lib/Ethernet/src/socket.cpp:435-445`:

```c
do {
    freesize = getSnTX_FSR(s);
    status   = W5100.readSnSR(s);
    if ((status != SnSR::ESTABLISHED) && (status != SnSR::CLOSE_WAIT)) { ret = 0; break; }
    yield();
} while (freesize < ret);
```

This waits for space in the W5500's 2 KB per-socket TX buffer and leaves the loop
**only** if the socket drops out of ESTABLISHED/CLOSE_WAIT. A peer that stops
reading while keeping the connection open advertises a zero window: the W5500
never frees TX space, the socket stays perfectly ESTABLISHED, and the loop never
terminates. There is no timeout and no iteration cap.

`netWrite()` → `EthernetClient::write()` → `socketSend()` is on the SCPI response
path, so this runs from inside `netcfg_task()`, i.e. from `loop()`. When it
spins, `loop()` stops — which is exactly the reported symptom set, including the
dead front panel and the silent-but-enumerated USB CDC port.

### Reproduction (deterministic, seconds)

`Calc/evaluation/net_stress.py --phase noread` — connect with a small
`SO_RCVBUF`, pipeline queries, never read the answers. Two runs, both identical:

| t | observation |
|---|---|
| 0 s | healthy, uptime 1126 s |
| ~14 s | **neither LAN nor USB answers, but the TCP handshake still completes** |
| ~26 s | handshake stops too (nothing re-arms the listener while `loop()` is blocked) |
| ~51 s | client drains its socket, reopening the window |
| ~55 s | fully healthy again, **uptime 1177 s** |

Two things that settle it:

- **Uptime ran straight through the outage** (1126 → 1177 ≈ wall time). The MCU
  never reset: no crash, no brown-out, no watchdog. It was *blocked in a call*
  and resumed where it left off.
- **Recovery is caused by the client, not by the instrument.** It came back
  ~4 s after the host reopened its receive window, and in a second run where the
  window stayed shut it was still dead 10 minutes later and needed a power cycle.
  The spin really is unbounded.

This also explains why the earlier fixes (§3) extended time-to-freeze without
curing it — they removed two *other* blocking paths, not this one.

### The fix

Bound the wait. Sample `millis()` before the loop and give up after a budget
comparable to `NET_CLIENT_CLOSE_MS` (tens of ms — a SCPI reply is not worth
stalling the instrument for), returning 0 so `EthernetClient::write()` sets the
write error and the caller drops the session. The same treatment is needed for
the three other unbounded spins on the same path:

| location | spin |
|---|---|
| `socket.cpp:453-462` | wait for `SEND_OK`; checks only `CLOSED`, ignores the `Sn_IR` TIMEOUT bit — `socketSendUDP()` at line 518 does check it |
| `w5100.cpp:464-470` | `execCmdSn()`: `while (readSnCR(s));` with no cap |
| `socket.cpp:263-280`, `382-395` | `getSnRX_RSR()` / `getSnTX_FSR()`: `while(1)` until two reads agree |

Add the RP2350 watchdog as defence in depth regardless (`rp2040.wdt_begin()` /
`wdt_reset()` in `loop()`): it converts any residual wedge into a ~2 s blip.

## 0b. Second, independent bug — SCPI session pool starves permanently

Also reproduced on the bench (`net_stress.py --phase abandon`): park four TCP
sessions and simply walk away — no FIN, no RST. Every subsequent connection
attempt was refused or timed out for as long as the ghosts were held (0 served,
11-13 refused), while `loop()` stayed perfectly healthy.

`netcfg.cpp` frees a pool slot only when `EthernetClient::connected()` goes
false, the W5500's TCP keepalive is off by default (`Sn_KPALVTR` = 0), and
nothing ages a session out. So a peer that vanishes without closing (killed
script, host sleep, cable pull) holds its slot **forever**; four of those and the
instrument is unreachable over LAN with nothing wrong with the firmware's main
loop. This is a good match for the "flapping" in symptom 4, and it is *not*
fixed by fixing §0.

Fix: enable keepalive (`Sn_KPALVTR`) and/or track a per-slot last-activity
timestamp in `netcfg.cpp` and `stop()` a session idle beyond a few minutes.

## 0c. What was changed, and the evidence it works

All of the below is applied and flashed. The vendored Ethernet library is no
longer stock 2.0.2 — every local change is commented as such at the site.

| file | change |
|---|---|
| `socket.cpp` `socketSend()` | both waits bounded by `W5100_SEND_TIMEOUT_MS` (100 ms); SEND_OK wait now also honours the `Sn_IR` TIMEOUT bit; stale SEND_OK cleared before each send |
| `socket.cpp` `getSnRX_RSR()` / `getSnTX_FSR()` | `while(1)` capped at `W5100_SIZE_POLL_MAX` |
| `w5100.cpp` `execCmdSn()` | `while (readSnCR(s));` capped at `W5100_CMD_POLL_MAX` |
| `EthernetClient.cpp` `flush()` | bounded, and now yields |
| `w5100.h` / `Ethernet.h` / `EthernetClient.cpp` | expose `Sn_KPALVTR` as `EthernetClient::setKeepAlive()` |
| `netcfg.cpp` | keep-alive on accepted sockets; `NET_CLIENT_IDLE_MS` backstop; shared `NET_RX_BUDGET_PER_PASS` RX budget; block reads instead of one SPI transaction per byte; drop a session as soon as a response fails to send |
| `main.cpp` | RP2350 hardware watchdog, `BRAIN_WDT_TIMEOUT_MS` = 4 s, armed after `setup()`, fed at the end of `loop()` |
| `scpi.cpp` | always terminate the response line (see below) |

Verified with `Calc/evaluation/net_stress.py`, full 11-phase sweep:

- **`noread` no longer wedges anything.** Before: CPU_WEDGED at t=14 s, DEAD at
  t=26 s, power cycle required. After: stays HEALTHY, drops the misbehaving
  client after ~1.2 s, and the run exits 0.
- ~72 000 queries per sweep, 930+ queries/s sustained, **zero stalls over 1 s**,
  every phase recovered on its own.
- **Zero watchdog resets** across three full sweeps (uptime strictly monotonic,
  59→491 s, 38→455 s, 489→616 s). That was the thing to check when adding a
  watchdog: 4 s is comfortably above every legitimate `loop()` pass under load.
- `POOL_FULL` still appears in `slowloris` / `pool` / `abandon` — that is those
  phases deliberately occupying all four slots, and it clears the moment they
  let go. Not a fault.

**Bonus bug found by the `malformed` phase** (pre-existing, unrelated to the
hang): a `;`-chained response longer than `SCPI_RESP_MAX` (480 B) lost its
trailing LF, because `respRaw()` drops what it cannot fit and the terminator was
appended through it. On a line-oriented transport the client then waited forever
for a line that never ended. `scpi_process_line()` now writes the terminator
directly, so an over-long response is truncated but always a complete line.

Note: a line longer than `NET_LINE_MAX` (256 B) is still answered with silence
by design — it is a framing error, logged as `-363` in the error queue. That is
normal instrument behaviour, not a bug.

---

## 1. What the failure looks like

Observed repeatedly, in this order as it degrades:

1. SCPI queries start timing out mid-run (TCP session still open).
2. New TCP connections are accepted but the instrument never replies.
3. TCP `connect()` itself times out.
4. It **flaps** — connect succeeds, then fails, then succeeds, without intervention.
5. **The front panel buttons and encoder stop responding** (user-confirmed).
6. The **USB CDC port enumerates but does not answer SCPI**.

Points 5 and 6 are the important ones: this is **not a network-stack problem, it
is `loop()` not running**. USB enumeration is handled outside `loop()` on this
core, which is why the port appears while SCPI stays silent. Any fix aimed only
at Ethernet is aimed at the wrong layer.

There appears to be **no watchdog** — the unit never self-recovers fully.

## 2. Reproduction

Sustained SCPI traffic. The reliable trigger is a ramp from the evaluation
script (~1 command every 0.7 s for 2–3 minutes, one long-lived TCP session):

```powershell
cd Calc/evaluation
py psu_accuracy.py --psu 192.168.2.128 --dmm 192.168.2.222 --channel 2 `
   --quantity current --dmm-range 2 --compliance 2.0 --tag baseline
```

The script has reconnect-and-retry built in (`--retries`, `--timeout`), so it
will report `stalled ... reconnecting` lines before giving up — useful signal for
how long the stall lasted.

Time-to-freeze is **inconsistent**, which is itself a clue:

| Attempt | Traffic before freeze |
|---|---|
| 1 | ~45 min of mixed use, incl. a full 81-point ramp; died at ramp point 2 |
| 2 (after power cycle) | ~1 min; died at ramp point 0 |
| 3 (after firmware fix) | one full 81-point ramp + a probe (~4 min), then froze on the next ramp |

Note attempt 3: the fix clearly **extended** the time-to-freeze but did not
eliminate it.

## 3. Fixes already applied (uncommitted, flashed to the device)

Both are in `firmware/brain/main/src/netcfg.cpp`. **Note this file already had
uncommitted changes before this work started** — `git diff` shows the two mixed
together (48 insertions / 8 deletions vs HEAD).

### 3.1 `EthernetClient::stop()` blocking

`stop()` sends a FIN then spins on `socketStatus()` until CLOSED or its
`_timeout` expires — library default **1000 ms** — before forcing the close. A
peer that vanished (killed script, pulled cable) never completes the handshake,
so each such socket costs the full second, and `netcfg_task()` can call `stop()`
once per pool slot (`NET_MAX_CLIENTS` = 4) per pass. Fixed by setting
`setConnectionTimeout(NET_CLIENT_CLOSE_MS = 20)` on owned clients.

### 3.2 DHCP renewal blocking

`DhcpClass::checkLease()` (`lib/Ethernet/src/Dhcp.cpp:382-394`) can call the
blocking `request_DHCP_lease()` **twice in one call** — the renew arm, then
falling straight into the rebind arm. Each is bounded only by
`DhcpClass::_timeout`, which is armed by whatever the last `Ethernet.begin()`
passed. `bringUp()` passed `NET_DHCP_INIT_TIMEOUT_MS` = 6000, so a unit that
leased at boot silently carried a ~12 s worst-case freeze into every future
renewal. Fixed by making the DHCP budget uniform at 800/250 ms and pinging the
channels either side of `maintain()` (as the re-acquisition path already did).

**Neither fix cured it.** They were real bugs, but at least one more blocking
path remains.

## 4. Where to look next

Ranked by my estimate of likelihood. Nothing here has been tested.

1. **Instrument `loop()` first — don't guess again.** Track per-pass duration and
   the worst offender per section (`netcfg_task`, `channel_link_task`,
   `scpi_process_line`, display update), expose via a SCPI query or the OLED.
   Two rounds of static reading found two real bugs and still missed the cause;
   measurement will be faster than a third round.
2. **Add the RP2350 hardware watchdog.** Independent of root cause, this turns a
   dead instrument into a blip. Given the outputs are live during bench runs,
   arguably this should exist regardless.
3. **SPI bus contention.** The W5500 (CS GPIO17, SPI0) and the SSD1322 OLED share
   the SPI peripheral. Check for a missing `beginTransaction`/`endTransaction`
   pairing or a re-entrancy window — a wedged SPI transaction would freeze the
   panel and the network together, which matches the symptom exactly.
4. **`serviceClient()` read loop** — `while (avail-- > 0) c.read();` performs one
   SPI transaction per byte. Bounded, but worth timing under load.
5. **`scpi_process_line()` calling `channel_link_task()`** (`scpi.cpp:290`) —
   re-entrancy into the channel layer from inside the network path.
6. **W5500 socket exhaustion.** Ruled *unlikely*: `EthernetServer::accept()`
   re-listens correctly via `begin()`. But the flapping in symptom 4 would also
   be explained by sockets slowly timing out, so not fully excluded.
7. **Eliminate DHCP as a variable** — set a static IP (front panel →
   Settings → Network). If it still freezes, DHCP is definitively not involved
   and `netcfg` drops down the suspect list.

## 5. Facts worth not re-deriving

- **−7.77 mA resting current** through a DMM in series with an *off* output is
  **normal** for this design (output pre-load), confirmed by the user. It is not
  a stuck output. Identical on the 20 mA and 200 mA ranges.
- The **USB CDC port (COM9) disappeared after the last flash** and had not
  returned. Unclear whether related. The CDC console is the natural fallback
  when Ethernet misbehaves, so it is worth restoring.
- Flashing works over USB with `pio run -t upload` **even while frozen** — the
  1200-baud touch reset is handled in the USB callback, not `loop()`. No need to
  hold BOOT.
  `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload`
- After a reboot the unit takes **~12 s** to reappear on the network (shortened
  DHCP budget means a first-attempt miss defers to the 20 s retry).
- Instruments: PSU `192.168.2.128`, Siglent SDM3065X `192.168.2.222`, both raw
  SCPI on TCP 5025. No VISA/pyvisa needed or installed.

## 6. Measurement work this interrupted

Tooling is `Calc/evaluation/psu_accuracy.py` (+ `README.md`), results in
`Calc/evaluation/results/`. All of `Calc/evaluation/` is **untracked** — not yet
committed.

| Task | State |
|---|---|
| Voltage CH1 + CH2 | **Done.** Both already optimal; 2-point recal is a no-op. Residual = DAC INL bow + ~8.9 mV quantisation, not calibration. |
| Current CH1 | **Done.** ISET cal a clear win (gain +0.144 % → −0.020 %, rms 0.89 → 0.30 mA). IMEAS unchanged (offset −1.30 mA both runs) — suspected physical, not firmware. |
| **Current CH2** | **Not started.** DMM current leads are on CH2 now, confirmed reading correctly. This is the only outstanding measurement. |

To resume once the brain is stable — baseline, then calibrate and verify:

```powershell
py psu_accuracy.py --psu 192.168.2.128 --dmm 192.168.2.222 --channel 2 `
   --quantity current --dmm-range 2 --compliance 2.0 --tag baseline
py psu_accuracy.py --psu 192.168.2.128 --dmm 192.168.2.222 --channel 2 `
   --quantity current --dmm-range 2 --compliance 2.0 --calibrate --verify --tag postcal
```

Pin `--dmm-range 2`: on AUTO the SDM3065X reads ~1.2 % high around 250 mA
(overranged on its 200 mA range before it steps up).

**Hardware finding to keep in mind:** a 2.000 A setpoint only delivers 1.989 A.
The ISET DAC saturates — full scale is 2.44 V ÷ 1.225 V/A = **1.99 A**, but
`DEFAULT_V_GAIN`'s sibling `DEFAULT_I_DIV` (1.2) assumes 2.033 A and
`SETPOINT_I_MAX_MA` is 2000. The top ~10 mA of the rated range is unreachable.
