#!/usr/bin/env python3
"""Output-voltage accuracy evaluation for the 2-Channel Pre-Reg Power Supply.

Ramps one PSU channel from 0 V to 36 V in 500 mV steps and, at every step,
compares three numbers:

    v_set   the SCPI setpoint written to the PSU          (what we asked for)
    v_psu   the PSU's own readback, MEAS:VOLT?            (what it thinks it did)
    v_dmm   a Siglent SDM3065X 6.5-digit DMM reading      (ground truth)

and reports the two deviations that matter:

    setpoint error  = v_set - v_dmm     (DAC + Vset scaling path, "CAL VSET")
    readback error  = v_psu - v_dmm     (ADC + Vsense path,      "CAL VMEAS")

Both instruments are driven over raw SCPI/TCP on port 5025, so no VISA runtime
or pyvisa is required.  Outputs a CSV, a summary report and light/dark PNG
plots into ``Calc/evaluation/results/``.

Wiring
------
PSU channel output --> SDM3065X DC-V input, sense leads at the PSU binding
posts (or, better, at the DUT end you care about).  The ramp is meant to be run
into an *open circuit*: the DMM draws <= ~4 uA, so the current reading stays at
zero and any current above ``--abort-current`` aborts the run as a wiring
mistake.

Examples
--------
    py psu_voltage_accuracy.py --psu 192.168.1.50 --dmm 192.168.1.51
    py psu_voltage_accuracy.py --psu psu.local --dmm 192.168.1.51 --channel 2
    py psu_voltage_accuracy.py --simulate            # no hardware, exercises plots
    py psu_voltage_accuracy.py --replot results/ramp_20260803_101500.csv
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import socket
import sys
import time
from dataclasses import dataclass, asdict
from datetime import datetime
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
RESULTS_DIR = HERE / "results"

SCPI_PORT = 5025

# ---------------------------------------------------------------------------
# Chart palette (dataviz reference instance, categorical slots 1 and 2).
# Two series only; slots 1/2 are an adjacent pair and pass the CVD gates in
# both modes as documented, so they are used verbatim.
# ---------------------------------------------------------------------------
THEMES = {
    "light": dict(
        surface="#fcfcfb", page="#f9f9f7",
        ink="#0b0b0b", ink2="#52514e", muted="#898781",
        grid="#e1e0d9", axis="#c3c2b7",
        s1="#2a78d6", s2="#eb6834",
    ),
    "dark": dict(
        surface="#1a1a19", page="#0d0d0d",
        ink="#ffffff", ink2="#c3c2b7", muted="#898781",
        grid="#2c2c2a", axis="#383835",
        s1="#3987e5", s2="#d95926",
    ),
}


@dataclass(frozen=True)
class Quantity:
    """Everything that differs between a voltage ramp and a current ramp."""
    name: str          # "voltage" / "current"
    unit: str          # "V" / "A"
    munit: str         # "mV" / "mA"
    set_cmd: str       # SCPI header that writes the setpoint
    meas_cmd: str      # SCPI query that reads it back
    aux_cmd: str       # the companion reading logged alongside
    aux_munit: str     # unit of that companion reading
    dmm_fn: str        # DMM function for CONFigure / NPLC / AZ
    cal_set: str       # calibration path for the setpoint chain
    cal_meas: str      # calibration path for the measure chain
    dec: int           # decimals used when writing the setpoint
    set_lsb_m: float   # DAC step, in munit (for interpreting the sawtooth)
    meas_lsb_m: float  # ADC step, in munit


QUANTITIES = {
    # DAC full scale = 2.44 V x DEFAULT_V_GAIN 15 = 36.6 V -> 8.94 mV/code.
    # ADC +-4.096 V FSR = 125 uV/LSB, x14.42 to the output -> 1.80 mV.
    "voltage": Quantity("voltage", "V", "mV", "VOLT", "MEAS:VOLT?", "MEAS:CURR?", "mA",
                        "VOLT:DC", "VSET", "VMEAS", 3, 36.6 / 4096 * 1000, 1.80),
    # DAC full scale = 2.44 V / DEFAULT_I_DIV 1.2 = 2.033 A -> 0.496 mA/code.
    # ADC 125 uV/LSB / 1.225 V/A -> 0.102 mA.
    "current": Quantity("current", "A", "mA", "CURR", "MEAS:CURR?", "MEAS:VOLT?", "mV",
                        "CURR:DC", "ISET", "IMEAS", 4, 2.4402 / 1.2 / 4096 * 1000, 0.102),
}

SERIES_SET = "Setpoint error (setpoint - DMM)"
SERIES_MEAS = "Readback error (PSU measurement - DMM)"


class ScpiError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class ScpiSocket:
    """Line-oriented SCPI over a raw TCP socket (port 5025)."""

    def __init__(self, host: str, port: int = SCPI_PORT, timeout: float = 10.0,
                 name: str = "instrument", verbose: bool = False,
                 retries: int = 0, retry_delay: float = 3.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.name = name
        self.verbose = verbose
        self.retries = retries
        self.retry_delay = retry_delay
        self._sock: socket.socket | None = None
        self._buf = b""

    def open(self) -> "ScpiSocket":
        try:
            self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as exc:
            raise ScpiError(f"{self.name}: cannot connect to {self.host}:{self.port} ({exc})") from exc
        self._sock.settimeout(self.timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return self

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def __enter__(self) -> "ScpiSocket":
        return self.open()

    def __exit__(self, *exc) -> None:
        self.close()

    def write(self, cmd: str, _retry: bool = True) -> None:
        if self.verbose:
            print(f"  -> [{self.name}] {cmd}")
        try:
            if self._sock is None:
                raise ScpiError(f"{self.name}: not connected")
            self._sock.sendall(cmd.encode("ascii") + b"\n")
        except (OSError, ScpiError) as exc:
            # A command that never lands would silently corrupt the run (a
            # setpoint not applied still gets measured), so retry it too.
            if not _retry or self.retries <= 0:
                raise ScpiError(f"{self.name}: send failed on '{cmd}' ({exc})") from exc
            for attempt in range(self.retries):
                print(f"  ~  [{self.name}] send failed on '{cmd}', reconnecting "
                      f"({attempt + 1}/{self.retries}) ...")
                try:
                    self.reconnect()
                    self.write(cmd, _retry=False)
                    return
                except ScpiError:
                    continue
            raise ScpiError(f"{self.name}: send failed on '{cmd}' ({exc})") from exc

    def read_line(self) -> str:
        if self._sock is None:
            raise ScpiError(f"{self.name}: not connected")
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self._buf:
            if time.monotonic() > deadline:
                raise ScpiError(f"{self.name}: timeout waiting for a response line")
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout as exc:
                raise ScpiError(f"{self.name}: timeout waiting for a response line") from exc
            if not chunk:
                raise ScpiError(f"{self.name}: connection closed by instrument")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        text = line.decode("ascii", errors="replace").strip()
        if self.verbose:
            print(f"  <- [{self.name}] {text}")
        return text

    def reconnect(self) -> None:
        """Drop and re-open the socket, discarding any partial line.

        SCPI state (selected channel, setpoints, calibration unlock) lives in the
        instrument and is shared across sessions, so a reconnect mid-run is safe.
        """
        try:
            self.close()
        except Exception:
            pass
        self._buf = b""
        time.sleep(self.retry_delay)
        self.open()

    def query(self, cmd: str, retries: int | None = None) -> str:
        """Send a query, retrying over a fresh socket if the instrument stalls.

        The brain can stall for several seconds at a time (a DHCP lease renewal
        blocks its main loop), which otherwise kills a multi-minute ramp.
        """
        attempts = self.retries if retries is None else retries
        for attempt in range(attempts + 1):
            try:
                self.write(cmd)
                return self.read_line()
            except ScpiError:
                if attempt >= attempts:
                    raise
                print(f"  ~  [{self.name}] stalled on '{cmd}', reconnecting "
                      f"({attempt + 1}/{attempts}) ...")
                try:
                    self.reconnect()
                except ScpiError:
                    pass          # retry the connect on the next pass
        raise ScpiError(f"{self.name}: unreachable")

    def query_float(self, cmd: str) -> float:
        raw = self.query(cmd)
        try:
            return float(raw.split(",")[0])
        except ValueError as exc:
            raise ScpiError(f"{self.name}: non-numeric answer to '{cmd}': {raw!r}") from exc


# ---------------------------------------------------------------------------
# Simulated instruments (--simulate): lets the whole pipeline run with no bench
# ---------------------------------------------------------------------------
class _SimState:
    """Shared truth for the fake PSU/DMM pair.

    Deliberately mirrors the real firmware's split between *hardware* and
    *calibration constants* (see firmware/channel/src/app/calibration.cpp), so
    the calibration routine below can be exercised end to end:

        firmware:  code   = cal_vset_gain * mV + cal_vset_offset
        hardware:  v_out  = code * v_per_code + v_floor
        hardware:  v_adc  = v_out * sense_gain + sense_off
        firmware:  v_meas = cal_vmeas_gain * v_adc + cal_vmeas_offset

    The cal constants start slightly wrong (an ~8 mV setpoint bias and a
    ~1.6 mV readback bias, matching what CH1/CH2 actually measured) so a
    calibration run has something real to remove.
    """

    def __init__(self, seed: int = 7):
        self.rng = random.Random(seed)
        self.v_set = 0.0
        self.output_on = False
        # --- hardware (unknown to the firmware) ---
        # Full scale is DAC 2.44 V x DEFAULT_V_GAIN 15 = 36.6 V (calibration.cpp),
        # not the 35.2 V nominal of the DAC_ADC analysis - so 36 V sits at about
        # code 4025 and there is roughly 0.6 V of headroom above it.
        self.v_per_code = 36.6 / 4096.0 * 1.0021
        self.v_floor = 0.0320            # output floor at code 0
        self.sense_gain = (1.0 / 14.42) * 0.998
        self.sense_off = 2.0e-4
        self.adc_lsb = 4.096 * 2 / 2 ** 16          # ADC volts per code
        # --- firmware calibration constants (what CAL:COMMit rewrites) ---
        # Current chain: DAC FS 2.44 V / DEFAULT_I_DIV 1.2 = 2.033 A -> 0.496 mA
        # per code; shunt 0.1 ohm x 12.25 -> 1.225 V/A into the ADC.
        self.i_per_code = 2.4402 / 1.2 / 4096.0 * 1.0015
        self.i_floor = 0.0
        self.isense_gain = 1.225 * 0.997
        self.isense_off = 1.0e-4
        self.i_set = 0.0
        self.cal = {
            "VSET": (1.0 / (self.v_per_code * 1000.0), -0.98),   # code per mV, code
            "VMEAS": (1.0 / self.sense_gain * 1000.0, -1.56),    # mV per Vadc, mV
            "ISET": (1.0 / (self.i_per_code * 1000.0), -0.4),    # code per mA, code
            "IMEAS": (1.0 / self.isense_gain * 1000.0, -0.3),    # mA per Vadc, mA
        }
        self._points: dict[str, dict[int, tuple[float, float]]] = {
            k: {} for k in self.cal}

    # -- hardware ---------------------------------------------------------
    def _code(self) -> int:
        g, o = self.cal["VSET"]
        code = int(round(g * self.v_set * 1000.0 + o))
        return max(0, min(4095, code))

    def v_true(self) -> float:
        if not self.output_on:
            return 0.0
        v = self._code() * self.v_per_code + self.v_floor
        v += 0.008 * math.sin(2 * math.pi * v / 12.0)          # crude DAC INL
        return max(0.0, v + self.rng.gauss(0, 2e-4))

    def _v_adc(self) -> float:
        return self.v_true() * self.sense_gain + self.sense_off

    def v_readback(self) -> float:
        vadc = round(self._v_adc() / self.adc_lsb) * self.adc_lsb
        g, o = self.cal["VMEAS"]
        return max(0.0, round((g * vadc + o)) / 1000.0)

    def v_dmm(self) -> float:
        return self.v_true() + self.rng.gauss(0, 15e-6)

    # -- current chain (the channel is assumed to be in CC) ----------------
    def _icode(self) -> int:
        g, o = self.cal["ISET"]
        return max(0, min(4095, int(round(g * self.i_set * 1000.0 + o))))

    def i_true(self) -> float:
        if not self.output_on:
            return 0.0
        i = self._icode() * self.i_per_code + self.i_floor
        i += 0.00012 * math.sin(2 * math.pi * i / 0.7)      # crude INL
        return max(0.0, i + self.rng.gauss(0, 3e-6))

    def _i_adc(self) -> float:
        return self.i_true() * self.isense_gain + self.isense_off

    def i_readback(self) -> float:
        iadc = round(self._i_adc() / self.adc_lsb) * self.adc_lsb
        g, o = self.cal["IMEAS"]
        return max(0.0, round(g * iadc + o, 1) / 1000.0)    # telemetry is 0.1 mA

    def i_dmm(self) -> float:
        return self.i_true() + self.rng.gauss(0, 2e-6)

    # -- calibration ------------------------------------------------------
    def cal_point(self, path: str, index: int, actual: float) -> None:
        if path not in self._points:
            return
        # x = true value, y = the code we drove (set paths); or
        # x = raw ADC volts, y = true value (measure paths).
        raw = {"VSET": lambda: (actual * 1000.0, float(self._code())),
               "ISET": lambda: (actual * 1000.0, float(self._icode())),
               "VMEAS": lambda: (self._v_adc(), actual * 1000.0),
               "IMEAS": lambda: (self._i_adc(), actual * 1000.0)}[path]
        self._points[path][index] = raw()

    def cal_commit(self, path: str) -> bool:
        pts = self._points.get(path, {})
        if 0 not in pts or 1 not in pts:
            return False
        (x0, y0), (x1, y1) = pts[0], pts[1]
        if abs(x1 - x0) < 1e-9:
            return False
        gain = (y1 - y0) / (x1 - x0)
        self.cal[path] = (gain, y0 - gain * x0)
        return True


class SimPsu:
    def __init__(self, state: _SimState, verbose: bool = False):
        self.state = state
        self.name = "PSU(sim)"
        self.verbose = verbose

    def open(self):
        return self

    def close(self):
        pass

    def write(self, cmd: str) -> None:
        head = cmd.strip().upper()
        if head.startswith("VOLT ") or head.startswith("VOLTAGE "):
            self.state.v_set = round(float(cmd.split()[1].split(",")[0]) * 100) / 100  # 10 mV grid
        elif head.startswith("CURR "):
            self.state.i_set = round(float(cmd.split()[1].split(",")[0]), 3)          # 1 mA grid
        elif head.startswith("OUTP"):
            self.state.output_on = head.split()[-1].split(",")[0] in ("ON", "1")
        elif head.startswith("CAL:POIN"):
            path, idx, actual = head.split(None, 1)[1].split(",")[:3]
            self.state.cal_point(path.strip(), int(idx), float(actual))
        elif head.startswith("CAL:COMM"):
            self.state.cal_commit(head.split(None, 1)[1].strip())
        if self.verbose:
            print(f"  -> [{self.name}] {cmd}")

    def query(self, cmd: str) -> str:
        head = cmd.strip().upper()
        if head.startswith("*IDN"):
            return "PreReg,PSU-2CH-36V2A,SN00001,0.2.0-sim"
        if head.startswith("SYST:ERR"):
            return '0,"No error"'
        if head.startswith("CAL:STAT"):
            return "1"
        if head.startswith("CAL:VAL"):
            return "1"
        if head.startswith("MEAS:TEMP"):
            return "38.5"
        if head.startswith("MEAS") and "CURR" in head:
            # In a voltage ramp the output is open-circuit; in a current ramp
            # the DMM is the load and the channel is in CC.
            return f"{self.state.i_readback():.4f}" if self.state.i_set > 0.0005 else "0.0000"
        if head.startswith("MEAS") or head.startswith("FETC"):
            return f"{self.state.v_readback():.3f}"
        if head.startswith("VOLT?"):
            return f"{self.state.v_set:.3f}"
        return "0"

    def query_float(self, cmd: str) -> float:
        return float(self.query(cmd).split(",")[0])


class SimDmm:
    def __init__(self, state: _SimState, verbose: bool = False):
        self.state = state
        self.name = "DMM(sim)"
        self.verbose = verbose
        self.func = "VOLT"

    def open(self):
        return self

    def close(self):
        pass

    def write(self, cmd: str) -> None:
        head = cmd.strip().upper()
        if head.startswith("CONF:"):
            self.func = "CURR" if "CURR" in head else "VOLT"
        if self.verbose:
            print(f"  -> [{self.name}] {cmd}")

    def query(self, cmd: str) -> str:
        if cmd.strip().upper().startswith("*IDN"):
            return "Siglent Technologies,SDM3065X,SIM0001,1.01.01.15-sim"
        val = self.state.i_dmm() if self.func == "CURR" else self.state.v_dmm()
        return f"{val:.7E}"

    def query_float(self, cmd: str) -> float:
        return float(self.query(cmd).split(",")[0])


# ---------------------------------------------------------------------------
# Instrument setup / teardown
# ---------------------------------------------------------------------------
def drain_psu_errors(psu, label: str, quiet: bool = False) -> list[str]:
    """Pop the PSU error queue; returns any non-zero entries."""
    errors: list[str] = []
    for _ in range(32):
        entry = psu.query("SYST:ERR?")
        if not entry or entry.split(",")[0].strip().lstrip("+") in ("0", "0.0"):
            break
        errors.append(entry)
    if errors and not quiet:
        for e in errors:
            print(f"  !  PSU error after {label}: {e}")
    return errors


def setup_psu(psu, channel: int, q: Quantity, companion: float, verbose: bool) -> str:
    """Park the channel and set the *companion* setpoint.

    A voltage ramp needs a current limit above the (near-zero) load current; a
    current ramp needs a compliance voltage above the DMM's burden voltage, so
    the channel sits in CC for the whole run.
    """
    idn = psu.query("*IDN?")
    print(f"  PSU : {idn}")
    psu.write("*CLS")
    psu.write("SYST:REM")
    psu.write(f"INST:NSEL {channel}")
    psu.write(f"OUTP OFF,(@{channel})")
    psu.write(f"{q.set_cmd} 0,(@{channel})")
    other = "CURR" if q.set_cmd == "VOLT" else "VOLT"
    psu.write(f"{other} {companion:.3f},(@{channel})")
    print(f"  {'current limit' if other == 'CURR' else 'compliance voltage'}"
          f" set to {companion:g} {'A' if other == 'CURR' else 'V'}")
    drain_psu_errors(psu, "setup")
    return idn


def setup_dmm(dmm, q: Quantity, dmm_range: str, nplc: float, autozero: bool,
              verbose: bool) -> str:
    idn = dmm.query("*IDN?")
    print(f"  DMM : {idn}")
    if "SDM30" not in idn.upper() and "SIM" not in idn.upper():
        print("  !  Warning: DMM does not identify as an SDM30xx; commands may differ.")
    dmm.write("*CLS")
    dmm.write(f"CONF:{q.dmm_fn} {dmm_range}")
    dmm.write(f"{q.dmm_fn}:NPLC {nplc:g}")
    dmm.write(f"{q.dmm_fn}:AZ {'ON' if autozero else 'OFF'}")
    dmm.write("TRIG:SOUR IMM")
    dmm.write("SAMP:COUN 1")
    return idn


def shutdown_psu(psu, channel: int) -> None:
    try:
        psu.write(f"VOLT 0,(@{channel})")
        psu.write(f"CURR 0,(@{channel})")
        psu.write(f"OUTP OFF,(@{channel})")
        psu.write("CAL:STAT OFF,0")   # never leave calibration unlocked
        psu.write("SYST:LOC")
    except Exception as exc:  # best effort - we are already on the way out
        print(f"  !  Could not return the PSU to a safe state: {exc}")


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------
@dataclass
class Point:
    """One ramp step. Field names stay v_* for CSV compatibility with earlier
    voltage runs; they hold the *ramped* quantity, whichever that is."""
    index: int
    t_s: float
    v_set: float       # setpoint written
    v_dmm: float       # DMM reading (ground truth)
    v_dmm_sd: float    # sd of the DMM samples
    v_psu: float       # PSU readback
    aux_psu: float     # companion reading (current on a V ramp, voltage on an I ramp)
    temp_c: float = float("nan")


def read_dmm_mean(dmm, samples: int) -> tuple[float, float]:
    vals = [dmm.query_float("READ?") for _ in range(samples)]
    arr = np.asarray(vals, dtype=float)
    return float(arr.mean()), float(arr.std(ddof=1)) if arr.size > 1 else 0.0


def run_ramp(psu, dmm, args, q: Quantity) -> list[Point]:
    ch = args.channel
    steps_m = list(range(int(round(args.vmin * 1000)),
                         int(round(args.vmax * 1000)) + 1,
                         int(round(args.vstep * 1000))))
    n = len(steps_m)
    print(f"\nRamping CH{ch} {q.name}: {args.vmin:g} {q.unit} -> {args.vmax:g} {q.unit} "
          f"in {args.vstep*1000:g} {q.munit} steps "
          f"({n} points, {args.dmm_samples} DMM samples each)")
    print(f"{'#':>4} {'set/'+q.unit:>9} {'dmm/'+q.unit:>11} {'psu/'+q.unit:>9} "
          f"{'set-dmm/'+q.munit:>12} {'psu-dmm/'+q.munit:>12} "
          f"{'aux/'+q.aux_munit:>9} {'T/C':>6}")

    psu.write(f"{q.set_cmd} 0,(@{ch})")
    psu.write(f"OUTP ON,(@{ch})")
    time.sleep(max(args.settle, 0.5))
    if args.warmup > 0:
        print(f"  warm-up {args.warmup:g} s at 0 {q.unit} ...")
        time.sleep(args.warmup)

    points: list[Point] = []
    t0 = time.monotonic()
    for i, m in enumerate(steps_m):
        v_set = m / 1000.0
        psu.write(f"{q.set_cmd} {v_set:.{q.dec}f},(@{ch})")
        time.sleep(args.settle)

        v_dmm, v_sd = read_dmm_mean(dmm, args.dmm_samples)
        v_psu = float(np.mean([psu.query_float(f"{q.meas_cmd} (@{ch})")
                               for _ in range(args.psu_samples)]))
        aux = psu.query_float(f"{q.aux_cmd} (@{ch})")
        temp = psu.query_float("MEAS:TEMP?") if args.max_temp > 0 else float("nan")

        pt = Point(i, time.monotonic() - t0, v_set, v_dmm, v_sd, v_psu, aux, temp)
        points.append(pt)
        print(f"{i:>4} {v_set:>9.{q.dec}f} {v_dmm:>11.6f} {v_psu:>9.{q.dec}f} "
              f"{(v_set - v_dmm)*1000:>12.2f} {(v_psu - v_dmm)*1000:>12.2f} "
              f"{aux*1000:>9.2f} {temp:>6.1f}")

        # A voltage ramp must stay open-circuit; a current ramp must stay in CC,
        # which shows up as the companion voltage staying below compliance.
        if q.name == "voltage" and abs(aux) > args.abort_current:
            raise ScpiError(
                f"output current {aux:.3f} A exceeds --abort-current {args.abort_current:.3f} A "
                f"at {v_set:.3f} V - the output should be open-circuit apart from the DMM. Aborting.")
        if args.max_deviation > 0 and abs(v_dmm - v_set) > args.max_deviation:
            extra = (" - the channel has fallen out of CC (load open, or compliance too low)"
                     if q.name == "current" else " - check the leads/range")
            raise ScpiError(
                f"DMM reads {v_dmm:.4f} {q.unit} at a {v_set:.{q.dec}f} {q.unit} setpoint, off by "
                f"more than --max-deviation {args.max_deviation:g} {q.unit}{extra}. Aborting.")
        if args.max_temp > 0 and temp == temp and temp > args.max_temp:
            raise ScpiError(
                f"system temperature {temp:.1f} C exceeds --max-temp {args.max_temp:g} C "
                f"(OTP trips at 60 C) - aborting and shutting the output down.")

    errs = drain_psu_errors(psu, "ramp")
    if errs:
        print("  !  The PSU queued errors during the ramp (listed above).")
    return points


def run_calibration(psu, dmm, args) -> None:
    """Drive two points and record a live 2-point calibration.

    The channel firmware captures the *hardware* side of each cal point at the
    instant CAL:POIN arrives - the DAC code being driven (VSET) or an average of
    the raw ADC volts (VMEAS/IMEAS). Only the `<actual>` value comes from us, so
    every point must be measured while the channel is actually sitting at that
    operating point. Stored values from an earlier run cannot be replayed.

    `--cal-settle` therefore has to outlast that capture average, not just the
    hardware. It is the whole ballgame for the measure paths: the average is
    taken right after stepping the channel to the point, so any lag left in it
    is recorded as the point's x and shows up directly as gain/offset error.
    Firmware before the alpha=1/4 + snap fix in measure.cpp needed >=20 s here;
    the 3 s that used to be the default stored a 2.2 % gain error on IMEAS while
    reporting success. The default is now 12 s, and the residual at each cal
    point is checked below so a bad fit can never pass silently again.

    The fit is absolute (x = true value, y = raw hardware quantity), so
    re-calibrating an already-calibrated channel converges instead of
    compounding.
    """
    ch = args.channel
    q = QUANTITIES[args.quantity]
    paths = ([p.strip().upper() for p in args.cal_paths.split(",") if p.strip()]
             if args.cal_paths else [q.cal_set, q.cal_meas])
    print(f"\nCalibrating CH{ch}: {', '.join(paths)} "
          f"at {args.cal_low:g} {q.unit} and {args.cal_high:g} {q.unit}")
    print("  ! This overwrites the stored gain/offset in EEPROM and cannot be undone")
    print("  ! (CAL:DATA? cannot read the present constants back; CAL:RESet only")
    print("  !  restores compile-time defaults).")

    psu.write(f"INST:NSEL {ch}")
    psu.write("CAL:STAT ON,0")
    if psu.query("CAL:STAT?").strip() != "1":
        raise ScpiError("calibration did not unlock (CAL:STAT? != 1)")
    drain_psu_errors(psu, "cal unlock")

    psu.write(f"{q.set_cmd} 0,(@{ch})")
    psu.write(f"OUTP ON,(@{ch})")

    for index, v_point in ((0, args.cal_low), (1, args.cal_high)):
        psu.write(f"{q.set_cmd} {v_point:.{q.dec}f},(@{ch})")
        time.sleep(args.cal_settle)          # must outlast the capture average
        actual, sd = read_dmm_mean(dmm, args.cal_samples)
        aux = psu.query_float(f"{q.aux_cmd} (@{ch})")
        if q.name == "voltage" and abs(aux) > args.abort_current:
            raise ScpiError(f"output current {aux:.3f} A at the cal point - aborting "
                            f"before writing anything")
        if abs(actual - v_point) > args.max_deviation > 0:
            raise ScpiError(f"DMM reads {actual:.4f} {q.unit} at a {v_point:.{q.dec}f} {q.unit} "
                            f"cal point - refusing to calibrate against a suspect reading")
        print(f"  point {index}: set {v_point:8.{q.dec}f} {q.unit} -> DMM {actual:.6f} {q.unit} "
              f"(sd {sd*1e6:.1f} u{q.unit}, {args.cal_samples} samples)")
        for path in paths:
            psu.write(f"CAL:POIN {path},{index},{actual:.6f}")
        drain_psu_errors(psu, f"cal point {index}")

    for path in paths:
        psu.write(f"CAL:COMM {path}")
        errs = drain_psu_errors(psu, f"commit {path}")
        print(f"  commit {path:<6} {'FAILED - see errors above' if errs else 'stored'}")

    psu.write("CAL:STAT OFF,0")
    valid = psu.query(f"CAL:VAL? (@{ch})")
    print(f"  CAL:VALid? -> {valid}")

    # A 2-point fit must pass through its own two points, so the readback error
    # back at each cal point is the one check that cannot be fooled: CAL:VALid?
    # and an empty error queue only say the commit was not rejected, and
    # CAL:DATA? cannot read the constants back. Anything much above the ADC
    # quantum here means the stored line is not the line those points define.
    if q.cal_meas in paths:
        print("  residual at the cal points (must be ~0 for a landed fit):")
        tol_m = 5.0 * q.meas_lsb_m
        worst = 0.0
        for v_point in (args.cal_low, args.cal_high):
            psu.write(f"{q.set_cmd} {v_point:.{q.dec}f},(@{ch})")
            time.sleep(args.cal_settle)
            actual, _ = read_dmm_mean(dmm, args.cal_samples)
            back = psu.query_float(f"{q.meas_cmd} (@{ch})")
            resid_m = (back - actual) * 1000.0
            worst = max(worst, abs(resid_m))
            print(f"    {v_point:8.{q.dec}f} {q.unit}: readback - DMM = "
                  f"{resid_m:+8.3f} {q.munit}"
                  f"{'   <-- TOO LARGE' if abs(resid_m) > tol_m else ''}")
        if worst > tol_m:
            print(f"  !  {q.cal_meas} did not take: worst residual {worst:.3f} {q.munit} "
                  f"exceeds {tol_m:.3f} {q.munit} ({q.meas_lsb_m:.3f} {q.munit}/LSB).")
            print(f"  !  The usual cause is too short a --cal-settle ({args.cal_settle:g} s): "
                  f"the firmware's capture average was still lagging the step.")

    psu.write(f"{q.set_cmd} 0,(@{ch})")


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------
@dataclass
class FitResult:
    label: str
    gain: float          # v_dut = gain * v_dmm + offset
    offset_mv: float
    gain_error_pct: float
    max_abs_mv: float
    rms_mv: float
    max_resid_mv: float  # worst deviation from the best-fit line = linearity
    max_rel_pct: float   # worst % of reading above rel_floor
    cal_low: tuple[float, float]
    cal_high: tuple[float, float]


def fit_series(v_dmm: np.ndarray, v_dut: np.ndarray, err_mv: np.ndarray,
               label: str, rel_floor: float) -> FitResult:
    gain, offset = np.polyfit(v_dmm, v_dut, 1)
    resid_mv = (v_dut - (gain * v_dmm + offset)) * 1000.0
    mask = v_dmm > rel_floor
    rel = np.zeros_like(err_mv)
    rel[mask] = err_mv[mask] / (v_dmm[mask] * 1000.0) * 100.0

    lo_i = int(np.argmin(np.abs(v_dmm - 0.1 * v_dmm.max())))
    hi_i = int(np.argmin(np.abs(v_dmm - 0.9 * v_dmm.max())))
    return FitResult(
        label=label,
        gain=float(gain),
        offset_mv=float(offset * 1000.0),
        gain_error_pct=float((gain - 1.0) * 100.0),
        max_abs_mv=float(np.max(np.abs(err_mv))),
        rms_mv=float(np.sqrt(np.mean(err_mv ** 2))),
        max_resid_mv=float(np.max(np.abs(resid_mv))),
        max_rel_pct=float(np.max(np.abs(rel[mask]))) if mask.any() else 0.0,
        cal_low=(float(v_dut[lo_i]), float(v_dmm[lo_i])),
        cal_high=(float(v_dut[hi_i]), float(v_dmm[hi_i])),
    )


def drop_unreachable(points: list[Point], q: Quantity, clip_tol: float | None,
                     keep: bool = False) -> tuple[list[Point], list[Point], list[Point]]:
    """Split off the ramp endpoints the channel physically cannot reach.

    Both ends of a ramp contain points that measure a limit rather than an error,
    and each one dominates every statistic it is included in:

    * **Top** - neither channel reaches its rated 2.000 A (README Known issues).
      Above the DAC ceiling every higher setpoint returns the same current. One
      clipped point took CH2's worst setpoint error from ~1 mA to 49 mA.
    * **Bottom** - the 0 V / 0 A setpoint sits on the output floor, not at zero
      (CH1 measured 31.6 mV for a 0 V request), so it reads as a large setpoint
      error that no calibration could remove.

    Returns (kept, dropped_low, dropped_high). Dropped points stay in the CSV,
    which is the raw measurement record; only the report and plots exclude them.
    This is the basis the README's accuracy table has always quoted -- it used to
    have to be recomputed by hand.

    Clipping is detected only as a *contiguous tail*, so a genuinely poor setpoint
    calibration -- which would exceed the same tolerance mid-ramp -- is never
    quietly discarded as "clipping".
    """
    if keep:
        return points, [], []
    lo = 0
    while lo < len(points) and points[lo].v_set == 0.0:
        lo += 1
    tol = clip_tol if clip_tol is not None else 5.0 * q.set_lsb_m / 1000.0
    hi = len(points)
    if tol > 0:
        while hi > lo and (points[hi - 1].v_set - points[hi - 1].v_dmm) > tol:
            hi -= 1
    return points[lo:hi], points[:lo], points[hi:]


def analyse(points: list[Point], rel_floor: float):
    v_set = np.array([p.v_set for p in points])
    v_dmm = np.array([p.v_dmm for p in points])
    v_psu = np.array([p.v_psu for p in points])
    err_set_mv = (v_set - v_dmm) * 1000.0
    err_psu_mv = (v_psu - v_dmm) * 1000.0
    fit_set = fit_series(v_dmm, v_set, err_set_mv, SERIES_SET, rel_floor)
    fit_psu = fit_series(v_dmm, v_psu, err_psu_mv, SERIES_MEAS, rel_floor)
    return v_set, v_dmm, v_psu, err_set_mv, err_psu_mv, fit_set, fit_psu


def format_report(points: list[Point], fits, channel: int, psu_idn: str, dmm_idn: str,
                  rel_floor: float, q: Quantity) -> str:
    fit_set, fit_psu = fits
    v_max = max(p.v_dmm for p in points)
    dmm_sd = np.array([p.v_dmm_sd for p in points])
    u, mu = q.unit, q.munit
    temps = [p.temp_c for p in points if p.temp_c == p.temp_c]
    lines = [
        "=" * 78,
        f"{q.name.capitalize()} accuracy report - CH{channel}",
        "=" * 78,
        f"PSU        : {psu_idn}",
        f"DMM        : {dmm_idn}",
        f"Points     : {len(points)}   span 0 .. {v_max:.{q.dec}f} {u}",
        f"DMM noise  : mean sample sd {dmm_sd.mean()*1e6:.1f} u{u}, "
        f"worst {dmm_sd.max()*1e6:.1f} u{u}",
    ]
    if temps:
        lines.append(f"System temp: {min(temps):.1f} .. {max(temps):.1f} C (OTP trips at 60 C)")
    lines += [
        "",
        f"{'':<34}{'setpoint path':>16}{'readback path':>17}",
        f"{'':<34}{'(DAC / '+q.cal_set+')':>16}{'(ADC / '+q.cal_meas+')':>17}",
        "-" * 78,
        f"{'gain error':<34}{fit_set.gain_error_pct:>15.3f}%{fit_psu.gain_error_pct:>16.3f}%",
        f"{'offset':<34}{fit_set.offset_mv:>13.2f} {mu}{fit_psu.offset_mv:>14.2f} {mu}",
        f"{'worst absolute error':<34}{fit_set.max_abs_mv:>13.2f} {mu}"
        f"{fit_psu.max_abs_mv:>14.2f} {mu}",
        f"{'rms error':<34}{fit_set.rms_mv:>13.2f} {mu}{fit_psu.rms_mv:>14.2f} {mu}",
        f"{f'worst relative error (>{rel_floor:g} {u})':<34}"
        f"{fit_set.max_rel_pct:>15.3f}%{fit_psu.max_rel_pct:>16.3f}%",
        f"{'worst non-linearity (vs best fit)':<34}"
        f"{fit_set.max_resid_mv:>13.2f} {mu}{fit_psu.max_resid_mv:>14.2f} {mu}",
        "-" * 78,
        f"{'quantisation floor (1 LSB)':<34}"
        f"{q.set_lsb_m:>13.2f} {mu}{q.meas_lsb_m:>14.2f} {mu}",
        "",
        "Gain/offset above are of the PSU relative to the DMM: a positive gain",
        "error means the PSU number is larger than the truth.  The non-linearity",
        "row is what a 2-point calibration cannot remove; compare it against the",
        "quantisation floor - a residual near or below 1 LSB leaves calibration",
        "nothing to grip, and a *bow* between the cal points is INL, which a",
        "2-point linear fit cannot remove at all.",
        "",
        "Suggested 2-point calibration (values taken from this run):",
        f"  INST:NSEL {channel}",
        "  CAL:STAT ON,0",
        f"  {q.set_cmd} {fit_set.cal_low[0]:.{q.dec}f}",
        f"  CAL:POIN {q.cal_set},0,{fit_set.cal_low[1]:.4f}",
        f"  {q.set_cmd} {fit_set.cal_high[0]:.{q.dec}f}",
        f"  CAL:POIN {q.cal_set},1,{fit_set.cal_high[1]:.4f}",
        f"  CAL:COMM {q.cal_set}",
        f"  ; {q.cal_meas}: at those points the PSU read {fit_psu.cal_low[0]:.{q.dec}f} {u} and "
        f"{fit_psu.cal_high[0]:.{q.dec}f} {u}",
        f"  CAL:POIN {q.cal_meas},0,{fit_psu.cal_low[1]:.4f}   ; while sitting at the low point",
        f"  CAL:POIN {q.cal_meas},1,{fit_psu.cal_high[1]:.4f}   ; while sitting at the high point",
        f"  CAL:COMM {q.cal_meas}",
        "  CAL:STAT OFF,0",
        "",
        "  (or just: --calibrate --verify, which drives and captures these live)",
        "=" * 78,
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CSV
# ---------------------------------------------------------------------------
CSV_FIELDS = ["index", "t_s", "v_set", "v_dmm", "v_dmm_sd", "v_psu", "aux_psu", "temp_c"]


def write_csv(path: Path, points: list[Point], meta: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        for k, v in meta.items():
            fh.write(f"# {k}: {v}\n")
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for p in points:
            writer.writerow(asdict(p))


def read_csv(path: Path) -> tuple[list[Point], dict]:
    meta: dict[str, str] = {}
    rows: list[Point] = []
    with path.open("r", encoding="utf-8") as fh:
        data_lines = []
        for line in fh:
            if line.startswith("#"):
                key, _, val = line[1:].partition(":")
                meta[key.strip()] = val.strip()
            else:
                data_lines.append(line)
    for r in csv.DictReader(data_lines):
        # "i_psu" is the pre-rename name of the companion column, kept readable
        # so earlier voltage runs still --replot.
        aux = r.get("aux_psu", r.get("i_psu", "nan"))
        rows.append(Point(int(r["index"]), float(r["t_s"]), float(r["v_set"]),
                          float(r["v_dmm"]), float(r["v_dmm_sd"]), float(r["v_psu"]),
                          float(aux), float(r.get("temp_c") or "nan")))
    return rows, meta


# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------
def plot(points: list[Point], fits, channel: int, out_png: Path, theme: str,
         rel_floor: float, q: Quantity) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    c = THEMES[theme]
    fit_set, fit_psu = fits
    _, v_dmm, _, err_set, err_psu, *_ = analyse(points, rel_floor)

    plt.rcParams.update({
        "font.family": ["Segoe UI", "DejaVu Sans", "sans-serif"],
        "font.size": 9,
        "figure.facecolor": c["page"],
        "axes.facecolor": c["surface"],
        "savefig.facecolor": c["page"],
        "text.color": c["ink"],
        "axes.labelcolor": c["ink2"],
        "xtick.color": c["muted"],
        "ytick.color": c["muted"],
        "axes.edgecolor": c["axis"],
    })

    # Two panels: absolute and relative deviation. The old third panel (residual
    # after a gain/offset fit, i.e. what a 2-point calibration cannot remove) is
    # deliberately gone -- it is analysis of the calibration scheme rather than of
    # the instrument, and the report still carries the number as
    # "worst non-linearity" for anyone who wants it.
    fig, axes = plt.subplots(2, 1, figsize=(9.0, 7.2), sharex=True,
                             gridspec_kw=dict(hspace=0.16, left=0.10, right=0.965,
                                              top=0.875, bottom=0.090))
    ax_abs, ax_rel = axes

    for ax in axes:
        ax.grid(True, color=c["grid"], linewidth=0.8, alpha=1.0)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(c["axis"])
            ax.spines[side].set_linewidth(1.0)
        ax.axhline(0, color=c["axis"], linewidth=1.2, zorder=1)

    # Dense ramps read as dotted lines if every point wears a ringed marker,
    # so markers only appear when the run is coarse enough to benefit.
    style = "-o" if len(points) <= 40 else "-"
    common = dict(linewidth=2.0, markersize=5.0, markeredgewidth=1.4,
                  markeredgecolor=c["surface"], solid_capstyle="round", zorder=3)

    # --- panel 1: absolute deviation --------------------------------------
    ax_abs.plot(v_dmm, err_set, style, color=c["s1"], label=SERIES_SET, **common)
    ax_abs.plot(v_dmm, err_psu, style, color=c["s2"], label=SERIES_MEAS, **common)
    ax_abs.set_ylabel(f"deviation from DMM  /  {q.munit}")
    ax_abs.set_title("Absolute deviation", loc="left", color=c["ink"],
                     fontsize=11, fontweight="bold", pad=8)
    leg = ax_abs.legend(frameon=False, loc="best", fontsize=9)
    for text in leg.get_texts():
        text.set_color(c["ink2"])
    # Direct labels beside the last point, so identity is never colour-alone.
    ax_abs.margins(y=0.12)
    for arr, name in ((err_set, "setpoint"), (err_psu, "readback")):
        ax_abs.annotate(name, xy=(v_dmm[-1], arr[-1]), xytext=(6, 0),
                        textcoords="offset points", ha="left", va="center",
                        fontsize=8.5, color=c["ink2"], annotation_clip=False)

    # --- panel 2: relative deviation --------------------------------------
    mask = v_dmm > rel_floor
    rel_set = err_set[mask] / (v_dmm[mask] * 1000.0) * 100.0
    rel_psu = err_psu[mask] / (v_dmm[mask] * 1000.0) * 100.0
    ax_rel.plot(v_dmm[mask], rel_set, style, color=c["s1"], **common)
    ax_rel.plot(v_dmm[mask], rel_psu, style, color=c["s2"], **common)
    ax_rel.set_ylabel("deviation  /  % of reading")
    ax_rel.set_xlabel(f"DMM {q.name}  /  {q.unit}")
    ax_rel.set_title(f"Relative deviation  (points above {rel_floor:g} {q.unit})", loc="left",
                     color=c["ink"], fontsize=11, fontweight="bold", pad=8)

    span = float(v_dmm.max() - v_dmm.min()) or 1.0
    ax_rel.set_xlim(v_dmm.min() - 0.02 * span, v_dmm.max() + 0.09 * span)

    fig.suptitle(f"PSU CH{channel} output-{q.name} accuracy vs SDM3065X",
                 x=0.10, ha="left", color=c["ink"], fontsize=14, fontweight="bold", y=0.972)
    fig.text(0.10, 0.925,
             f"worst: setpoint {fit_set.max_abs_mv:.1f} {q.munit} / {fit_set.max_rel_pct:.2f} %"
             f"  ·  readback {fit_psu.max_abs_mv:.1f} {q.munit} / {fit_psu.max_rel_pct:.2f} %"
             f"  ·  1 LSB = {q.set_lsb_m:.2f} / {q.meas_lsb_m:.2f} {q.munit}",
             ha="left", color=c["ink2"], fontsize=9.5)

    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, dpi=160)
    plt.close(fig)
    print(f"  plot  -> {out_png}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Ramp the PSU 0..36 V and compare setpoint / readback against an SDM3065X.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--psu", metavar="HOST", help="PSU hostname or IP (SCPI raw TCP)")
    p.add_argument("--psu-port", type=int, default=SCPI_PORT)
    p.add_argument("--dmm", metavar="HOST", help="SDM3065X hostname or IP")
    p.add_argument("--dmm-port", type=int, default=SCPI_PORT)
    p.add_argument("--channel", type=int, choices=(1, 2), default=1, help="PSU channel under test")
    p.add_argument("--quantity", choices=("voltage", "current"), default="voltage",
                   help="which chain to characterise")

    p.add_argument("--vmin", type=float, help="ramp start (V or A) [V:0, I:0]")
    p.add_argument("--vmax", type=float, help="ramp end (V or A) [V:36, I:2]")
    p.add_argument("--vstep", type=float, help="step size (V or A) [V:0.5, I:0.025]")
    p.add_argument("--ilim", type=float, default=0.050,
                   help="voltage ramp: current limit, A (open circuit expected)")
    p.add_argument("--compliance", type=float, default=5.0,
                   help="current ramp: compliance voltage, V (must exceed the DMM burden)")
    p.add_argument("--settle", type=float, default=0.6, help="settling time after each step, s")
    p.add_argument("--warmup", type=float, default=0.0, help="dwell at 0 V before the ramp, s")

    p.add_argument("--dmm-samples", type=int, default=3, help="DMM readings averaged per step")
    p.add_argument("--psu-samples", type=int, default=3, help="MEAS:VOLT? readings averaged per step")
    p.add_argument("--dmm-range", default="AUTO",
                   help="DMM range: AUTO, or e.g. 2/20/200 (V) or 0.2/2/10 (A)")
    p.add_argument("--nplc", type=float, default=10.0, help="DMM integration time in NPLC")
    p.add_argument("--no-autozero", action="store_true", help="disable DMM auto-zero (faster)")

    p.add_argument("--abort-current", type=float, default=0.020,
                   help="voltage ramp: abort if the PSU sources more than this, A")
    p.add_argument("--max-deviation", type=float,
                   help="abort if |DMM - setpoint| exceeds this [V:3.0, I:0.2] (0 disables)")
    p.add_argument("--max-temp", type=float, default=55.0,
                   help="abort above this system temperature, C (OTP trips at 60; 0 disables)")
    p.add_argument("--rel-floor", type=float,
                   help="ignore points below this in the %%-of-reading plot [V:1.0, I:0.05]")
    p.add_argument("--clip-tol", type=float,
                   help="setpoint-vs-DMM gap above which a trailing point counts as "
                        "clipped and is left out of the report and plots "
                        "[default 5 setpoint LSB]; 0 disables the clip check")
    p.add_argument("--keep-endpoints", action="store_true",
                   help="report every point, including the 0 V/0 A output floor and "
                        "the clipped top of the range")

    p.add_argument("--calibrate", action="store_true",
                   help="run a live 2-point calibration (WRITES EEPROM, not undoable)")
    p.add_argument("--cal-paths", default="",
                   help="comma-separated cal paths; default = both paths of --quantity")
    p.add_argument("--cal-low", type=float, help="low cal point [V:3.5, I:0.2]")
    p.add_argument("--cal-high", type=float, help="high cal point [V:32.5, I:1.8]")
    p.add_argument("--cal-settle", type=float, default=12.0,
                   help="dwell at each cal point, s (the measure-path capture "
                        "average must fully settle - see run_calibration)")
    p.add_argument("--cal-samples", type=int, default=8, help="DMM readings per cal point")
    p.add_argument("--verify", action="store_true",
                   help="after --calibrate, run the ramp and report the result")

    p.add_argument("--tag", default="", help="suffix appended to the output file names")
    p.add_argument("--theme", choices=("light", "dark", "both"), default="both")
    p.add_argument("--simulate", action="store_true",
                   help="run against a modelled PSU/DMM - no hardware needed")
    p.add_argument("--replot", metavar="CSV", help="re-render plots/report from an existing CSV")
    p.add_argument("--timeout", type=float, default=15.0,
                   help="per-response socket timeout, s")
    p.add_argument("--retries", type=int, default=4,
                   help="reconnect-and-retry attempts when an instrument stalls "
                        "(the brain's DHCP renewal can block its loop for seconds)")
    p.add_argument("--verbose", action="store_true", help="echo every SCPI exchange")
    return p


# Per-quantity defaults for the options left as None above. Current steps of
# 25 mA give 81 points (comparable density to the 73-point voltage ramp) and
# 0.2/1.8 A sits at 10 %/90 % of the 2 A full scale.
DEFAULTS = {
    "voltage": dict(vmin=0.0, vmax=36.0, vstep=0.5, max_deviation=3.0, rel_floor=1.0,
                    cal_low=3.5, cal_high=32.5),
    "current": dict(vmin=0.0, vmax=2.0, vstep=0.025, max_deviation=0.2, rel_floor=0.05,
                    cal_low=0.2, cal_high=1.8),
}


def apply_defaults(args) -> None:
    for key, val in DEFAULTS[args.quantity].items():
        if getattr(args, key) is None:
            setattr(args, key, val)


def render_outputs(points, channel, psu_idn, dmm_idn, args, stem: Path) -> None:
    q = QUANTITIES[args.quantity]
    points, dropped_lo, dropped_hi = drop_unreachable(points, q, args.clip_tol,
                                                      args.keep_endpoints)
    if dropped_lo:
        print(f"\n  excluded {len(dropped_lo)} point(s) at a 0 {q.unit} setpoint "
              f"(output floor, not an accuracy error)")
    if dropped_hi:
        edge = min(p.v_set for p in dropped_hi)
        print(f"  excluded {len(dropped_hi)} clipped point(s) from {edge:.{q.dec}f} {q.unit} up: "
              f"the setpoint no longer tracks there (DAC ceiling, see Known issues)")
    if dropped_lo or dropped_hi:
        print("  (they remain in the CSV; --keep-endpoints reports every point)")
    if not points:
        raise ScpiError("no points left after endpoint exclusion - "
                        "check the wiring, or pass --keep-endpoints")
    fits_full = analyse(points, args.rel_floor)
    fits = (fits_full[5], fits_full[6])
    report = format_report(points, fits, channel, psu_idn, dmm_idn, args.rel_floor, q)
    print("\n" + report)
    stem.parent.mkdir(parents=True, exist_ok=True)
    report_path = stem.with_name(stem.name + "_report.txt")
    report_path.write_text(report + "\n", encoding="utf-8")
    print(f"\n  report-> {report_path}")
    themes = ("light", "dark") if args.theme == "both" else (args.theme,)
    for th in themes:
        plot(points, fits, channel, stem.with_name(f"{stem.name}_{th}.png"), th,
             args.rel_floor, q)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.replot:
        csv_path = Path(args.replot)
        if not csv_path.is_absolute():
            csv_path = (HERE / csv_path).resolve()
        points, meta = read_csv(csv_path)
        # Runs written before --quantity existed are voltage runs.
        args.quantity = meta.get("quantity", args.quantity)
        apply_defaults(args)
        channel = int(meta.get("channel", args.channel))
        render_outputs(points, channel, meta.get("psu_idn", "?"), meta.get("dmm_idn", "?"),
                       args, csv_path.with_suffix(""))
        return 0

    apply_defaults(args)
    q = QUANTITIES[args.quantity]

    if args.simulate:
        state = _SimState()
        psu, dmm = SimPsu(state, args.verbose), SimDmm(state, args.verbose)
    else:
        if not args.psu or not args.dmm:
            print("error: --psu and --dmm are required (or use --simulate).", file=sys.stderr)
            return 2
        psu = ScpiSocket(args.psu, args.psu_port, name="PSU", verbose=args.verbose,
                         timeout=args.timeout, retries=args.retries)
        dmm = ScpiSocket(args.dmm, args.dmm_port, name="DMM", verbose=args.verbose,
                         timeout=args.timeout, retries=args.retries)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    tag = f"_{args.tag}" if args.tag else ""
    kind = "v" if args.quantity == "voltage" else "i"
    stem = RESULTS_DIR / f"ramp_{kind}_ch{args.channel}_{stamp}{tag}"

    rc = 0
    points: list[Point] = []
    psu_idn = dmm_idn = "?"
    try:
        print("Connecting ...")
        psu.open()
        dmm.open()
        companion = args.ilim if args.quantity == "voltage" else args.compliance
        psu_idn = setup_psu(psu, args.channel, q, companion, args.verbose)
        dmm_idn = setup_dmm(dmm, q, args.dmm_range, args.nplc,
                            not args.no_autozero, args.verbose)
        if args.calibrate:
            run_calibration(psu, dmm, args)
        if not args.calibrate or args.verify:
            points = run_ramp(psu, dmm, args, q)
    except KeyboardInterrupt:
        print("\n  !  Interrupted - shutting the output down.")
        rc = 130
    except ScpiError as exc:
        print(f"\n  !  {exc}", file=sys.stderr)
        rc = 1
    finally:
        try:
            shutdown_psu(psu, args.channel)
        finally:
            psu.close()
            dmm.close()

    if len(points) >= 3:
        meta = {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "quantity": args.quantity,
            "channel": args.channel,
            "psu_idn": psu_idn,
            "dmm_idn": dmm_idn,
            f"min_{q.unit}": args.vmin, f"max_{q.unit}": args.vmax,
            f"step_{q.unit}": args.vstep,
            "companion": companion, "settle_s": args.settle,
            "dmm_range": args.dmm_range, "nplc": args.nplc,
            "dmm_samples": args.dmm_samples, "psu_samples": args.psu_samples,
            "simulated": args.simulate,
        }
        csv_path = stem.with_suffix(".csv")
        write_csv(csv_path, points, meta)
        print(f"\n  data  -> {csv_path}")
        render_outputs(points, args.channel, psu_idn, dmm_idn, args, stem)
    elif rc == 0 and not (args.calibrate and not args.verify):
        print("  !  Too few points to analyse.")
        rc = 1

    return rc


if __name__ == "__main__":
    sys.exit(main())
