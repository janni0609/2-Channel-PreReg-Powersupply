#!/usr/bin/env python3
"""
net_stress.py - SCPI network stress + liveness harness for the PSU brain (RP2350).

Purpose: reproduce the "brain freezes during sustained SCPI sessions" fault
described in firmware/brain/NETWORK_HANG_HANDOVER.md, and - crucially - tell
apart the two failure classes it can belong to, which the handover says is the
single most important observation:

  * POOL_FULL   An already-open session still answers, but a *new* connection
                cannot get served. The 4-slot client pool in netcfg.cpp is held
                by sessions that will never be reclaimed.
  * NET_STALL   Ethernet went quiet but the USB CDC console still answers SCPI.
                loop() is running; the fault is in the network layer (socket
                pool starvation, half-open sessions, listener lost).
  * CPU_WEDGED  Neither transport answers, yet the W5500 still completes the
                TCP handshake in hardware. That is the signature of loop() not
                running at all: the chip accepts the connection on its own,
                nobody ever services it. Fits the reported "connect succeeds,
                instrument never replies" plus a dead front panel.
  * DEAD        Not even the TCP handshake completes: the W5500 ran out of
                listening sockets (or was reset), on top of whatever else.

A background monitor samples both transports on a fixed cadence and timestamps
every state transition, so whichever phase provokes the fault, the log says what
died, in which order, and whether it recovered.

The workload phases each target one suspected firmware path (see --list). All of
them are read-only SCPI: no OUTPut, no *RST, no CALibration. The script refuses
to start if either output is on, unless --force is given.

Typical use
-----------
    # full sequence, both transports, ~12 min
    py net_stress.py --psu 192.168.2.128 --usb auto

    # just the phase most likely to wedge it (unbounded socketSend spin)
    py net_stress.py --psu 192.168.2.128 --usb auto --phase noread

    # long unattended soak, JSON written on exit (incl. Ctrl-C)
    py net_stress.py --psu 192.168.2.128 --usb auto --phase soak --duration 3600

Results land in results/netstress-<timestamp>.json next to this script.

USB CDC note: the arduino-pico CDC port only sources data once the host asserts
DTR. A terminal that leaves DTR low sees a port that enumerates and stays silent
- which is exactly what "COM9 enumerates but does not answer SCPI" looks like
from the outside. This script asserts DTR, so a silent USB probe here is a real
firmware symptom rather than a host-side artefact.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import socket
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

SCPI_PORT = 5025

# firmware/brain/main/src/netcfg.cpp: NET_MAX_CLIENTS / NET_LINE_MAX.
FW_MAX_CLIENTS = 4
FW_LINE_MAX = 256

T0 = time.monotonic()


def ts() -> float:
    """Seconds since the run started (all logging is relative to this)."""
    return time.monotonic() - T0


# ═══════════════════════════════════════════════════════════════════════════
#  Event log
# ═══════════════════════════════════════════════════════════════════════════
class EventLog:
    """Timestamped, thread-safe event log; printed live and serialised at exit."""

    def __init__(self, verbose: bool = False):
        self.events: list[dict] = []
        self.verbose = verbose
        self._lock = threading.Lock()

    def add(self, kind: str, msg: str, quiet: bool = False, **fields) -> None:
        ev = {"t": round(ts(), 3), "kind": kind, "msg": msg, **fields}
        with self._lock:
            self.events.append(ev)
            if not quiet or self.verbose:
                print(f"[{ev['t']:8.2f}] {kind:<10} {msg}", flush=True)


# ═══════════════════════════════════════════════════════════════════════════
#  Transports
# ═══════════════════════════════════════════════════════════════════════════
class ScpiError(RuntimeError):
    pass


class ScpiSocket:
    """Line-oriented SCPI over raw TCP, deliberately without any auto-retry.

    psu_accuracy.py's transport reconnects on a stall so a measurement run can
    survive one; here a stall *is* the observable, so failures propagate.
    """

    def __init__(self, host: str, port: int = SCPI_PORT, timeout: float = 5.0,
                 rcvbuf: int | None = None, name: str = "lan"):
        self.host, self.port, self.timeout, self.name = host, port, timeout, name
        self.rcvbuf = rcvbuf
        self._sock: socket.socket | None = None
        self._buf = b""

    def open(self) -> "ScpiSocket":
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # SO_RCVBUF must be set before connect() to shrink the advertised window.
        if self.rcvbuf is not None:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf)
        s.settimeout(self.timeout)
        try:
            s.connect((self.host, self.port))
        except OSError as exc:
            s.close()
            raise ScpiError(f"connect to {self.host}:{self.port} failed ({exc})") from exc
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock, self._buf = s, b""
        return self

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def abort(self) -> None:
        """Close with SO_LINGER 0: the peer gets an RST instead of a FIN.

        Models a killed script / crashed host, the case netcfg.cpp's shortened
        NET_CLIENT_CLOSE_MS was meant to bound.
        """
        if self._sock is None:
            return
        try:
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                  b"\x01\x00\x00\x00\x00\x00\x00\x00")
        except OSError:
            pass
        self.close()

    def __enter__(self) -> "ScpiSocket":
        return self.open()

    def __exit__(self, *exc) -> None:
        self.close()

    def send_raw(self, data: bytes) -> None:
        if self._sock is None:
            raise ScpiError("not connected")
        try:
            self._sock.sendall(data)
        except OSError as exc:
            raise ScpiError(f"send failed ({exc})") from exc

    def write(self, cmd: str) -> None:
        self.send_raw(cmd.encode("ascii") + b"\n")

    def read_line(self, timeout: float | None = None) -> str:
        if self._sock is None:
            raise ScpiError("not connected")
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while b"\n" not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ScpiError("timeout waiting for a response line")
            self._sock.settimeout(remaining)
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout as exc:
                raise ScpiError("timeout waiting for a response line") from exc
            except OSError as exc:
                raise ScpiError(f"recv failed ({exc})") from exc
            if not chunk:
                raise ScpiError("connection closed by instrument")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return line.decode("ascii", errors="replace").strip()

    def query(self, cmd: str, timeout: float | None = None) -> str:
        self.write(cmd)
        return self.read_line(timeout)


# ── USB CDC ─────────────────────────────────────────────────────────────────
# pyserial is not installed on this bench, and the USB path is the whole point
# of the harness (it is the only way to prove loop() is alive when Ethernet is
# not), so there is a self-contained Win32 fallback. pyserial is used when it is
# available because it also works off-Windows.
try:
    import serial as _pyserial                                   # type: ignore
except ImportError:
    _pyserial = None


class _Win32Serial:
    """Minimal blocking serial port over the Win32 API (ctypes, no deps).

    Only what the probe needs: open at 115200 8N1 with DTR+RTS asserted, write,
    and read with a total timeout.
    """

    def __init__(self, port: str, baud: int = 115200, timeout: float = 3.0):
        import ctypes
        import ctypes.wintypes as wt

        self._ct = ctypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._k32 = k32

        class DCB(ctypes.Structure):
            _fields_ = [("DCBlength", wt.DWORD), ("BaudRate", wt.DWORD),
                        ("bitfield", wt.DWORD), ("wReserved", wt.WORD),
                        ("XonLim", wt.WORD), ("XoffLim", wt.WORD),
                        ("ByteSize", ctypes.c_ubyte), ("Parity", ctypes.c_ubyte),
                        ("StopBits", ctypes.c_ubyte), ("XonChar", ctypes.c_char),
                        ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                        ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                        ("wReserved1", wt.WORD)]

        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [("ReadIntervalTimeout", wt.DWORD),
                        ("ReadTotalTimeoutMultiplier", wt.DWORD),
                        ("ReadTotalTimeoutConstant", wt.DWORD),
                        ("WriteTotalTimeoutMultiplier", wt.DWORD),
                        ("WriteTotalTimeoutConstant", wt.DWORD)]

        self._COMMTIMEOUTS = COMMTIMEOUTS
        # "\\.\COMn" is required for COM10 and above; harmless below it.
        handle = k32.CreateFileW(f"\\\\.\\{port}", 0xC0000000, 0, None, 3, 0, None)
        if handle == -1 or handle == 0xFFFFFFFFFFFFFFFF:
            raise OSError(f"cannot open {port} (error {ctypes.get_last_error()})")
        self._h = handle

        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not k32.GetCommState(handle, ctypes.byref(dcb)):
            self.close()
            raise OSError(f"GetCommState failed on {port}")
        dcb.BaudRate = baud
        dcb.ByteSize, dcb.Parity, dcb.StopBits = 8, 0, 0
        # fBinary=1 | fDtrControl=ENABLE (bits 4:5) | fRtsControl=ENABLE (bits 12:13).
        # DTR matters: arduino-pico's CDC only streams once the host raises it.
        dcb.bitfield = 0x1 | (1 << 4) | (1 << 12)
        if not k32.SetCommState(handle, ctypes.byref(dcb)):
            self.close()
            raise OSError(f"SetCommState failed on {port}")
        k32.EscapeCommFunction(handle, 5)          # SETDTR
        k32.EscapeCommFunction(handle, 3)          # SETRTS
        self.timeout = timeout
        self._apply_timeout(timeout)
        self.reset_input_buffer()

    def _apply_timeout(self, seconds: float) -> None:
        t = self._COMMTIMEOUTS(0, 0, max(1, int(seconds * 1000)), 0, 2000)
        self._k32.SetCommTimeouts(self._h, self._ct.byref(t))

    def reset_input_buffer(self) -> None:
        self._k32.PurgeComm(self._h, 0x0008 | 0x0004)   # PURGE_RXCLEAR|TXCLEAR

    def write(self, data: bytes) -> None:
        written = self._ct.wintypes.DWORD(0)
        if not self._k32.WriteFile(self._h, data, len(data),
                                   self._ct.byref(written), None):
            raise OSError("WriteFile failed")

    def read(self, n: int) -> bytes:
        buf = self._ct.create_string_buffer(n)
        got = self._ct.wintypes.DWORD(0)
        if not self._k32.ReadFile(self._h, buf, n, self._ct.byref(got), None):
            raise OSError("ReadFile failed")
        return buf.raw[:got.value]

    def close(self) -> None:
        if getattr(self, "_h", None):
            self._k32.CloseHandle(self._h)
            self._h = None


class UsbScpi:
    """SCPI over the brain's USB CDC console (the independent liveness channel)."""

    def __init__(self, port: str, timeout: float = 3.0):
        self.port, self.timeout = port, timeout
        self._buf = b""
        if _pyserial is not None:
            self._p = _pyserial.Serial(port, 115200, timeout=timeout,
                                       dsrdtr=False, rtscts=False)
            self._p.dtr = True
            self._p.rts = True
            self._p.reset_input_buffer()
            self._backend = "pyserial"
        elif os.name == "nt":
            self._p = _Win32Serial(port, 115200, timeout)
            self._backend = "win32"
        else:
            raise ScpiError("USB probe needs pyserial on this platform "
                            "(py -m pip install pyserial)")

    def close(self) -> None:
        try:
            self._p.close()
        except Exception:
            pass

    def query(self, cmd: str, timeout: float | None = None) -> str:
        tmo = self.timeout if timeout is None else timeout
        self._buf = b""
        try:
            if self._backend == "pyserial":
                self._p.reset_input_buffer()
                self._p.write(cmd.encode("ascii") + b"\n")
            else:
                self._p.reset_input_buffer()
                self._p.write(cmd.encode("ascii") + b"\n")
        except Exception as exc:
            raise ScpiError(f"usb write failed ({exc})") from exc

        deadline = time.monotonic() + tmo
        while b"\n" not in self._buf:
            if time.monotonic() > deadline:
                raise ScpiError("usb timeout waiting for a response line")
            try:
                if self._backend == "pyserial":
                    self._p.timeout = max(0.05, deadline - time.monotonic())
                    chunk = self._p.read(256)
                else:
                    self._p._apply_timeout(max(0.05, deadline - time.monotonic()))
                    chunk = self._p.read(256)
            except Exception as exc:
                raise ScpiError(f"usb read failed ({exc})") from exc
            if chunk:
                self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return line.decode("ascii", errors="replace").strip()


def list_com_ports() -> list[str]:
    """COM ports from HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM (Windows only)."""
    if os.name != "nt":
        return []
    try:
        import winreg
        ports = []
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                ports.append(winreg.EnumValue(key, i)[1])
        return sorted(ports)
    except OSError:
        return []


def autodetect_usb(log: EventLog) -> UsbScpi | None:
    """Find the CDC port whose *IDN? identifies as this instrument."""
    for port in list_com_ports():
        try:
            probe = UsbScpi(port, timeout=2.0)
            idn = probe.query("*IDN?")
        except Exception:
            continue
        if "PreReg" in idn:
            log.add("usb", f"found instrument on {port}: {idn}")
            return probe
        probe.close()
    log.add("usb", "no instrument found on any COM port (USB probe disabled)")
    return None


# ═══════════════════════════════════════════════════════════════════════════
#  Liveness monitor
# ═══════════════════════════════════════════════════════════════════════════
# States, worst last: the summary reports the worst state reached per phase.
STATES = ["HEALTHY", "SLOW", "POOL_FULL", "NET_STALL", "CPU_WEDGED", "DEAD"]


@dataclass
class Sample:
    t: float
    state: str
    lan_ok: bool
    usb_ok: bool
    tcp_ok: bool
    fresh_ok: bool | None
    lan_ms: float | None
    usb_ms: float | None
    uptime: int | None
    phase: str


class Monitor(threading.Thread):
    """Samples both transports on a fixed cadence and classifies the instrument.

    Uses one persistent LAN session so it costs a single slot of the 4-slot pool,
    and opens a fresh connection when that session has failed - which is also the
    test that separates CPU_WEDGED from DEAD: a wedged brain still lets the W5500
    complete the handshake by itself.

    That persistent session would otherwise mask pool exhaustion (its slot was
    claimed before the pool filled, so it keeps answering while every new client
    is locked out), so every `fresh_every`-th pass also runs a full connect +
    query on a throwaway socket. That is the POOL_FULL detector.
    """

    def __init__(self, host: str, usb: UsbScpi | None, log: EventLog,
                 interval: float = 2.0, slow_ms: float = 1000.0,
                 fresh_every: int = 3):
        super().__init__(daemon=True)
        self.host, self.usb, self.log = host, usb, log
        self.interval, self.slow_ms = interval, slow_ms
        self.fresh_every = max(1, fresh_every)
        self._pass = 0
        self.samples: list[Sample] = []
        self.phase = "startup"
        # Phases that intentionally hog or abandon sockets: a LAN failure there
        # is the expected result, not a finding, so it is annotated not alarmed.
        self.phase_disrupts_lan = False
        self.state = "HEALTHY"
        self.first_boot_uptime: int | None = None
        self.reboots = 0
        self._stop = threading.Event()
        self._sess: ScpiSocket | None = None
        self._lock = threading.Lock()

    def stop(self) -> None:
        self._stop.set()

    # -- probes ------------------------------------------------------------
    def _probe_lan(self) -> tuple[bool, float | None, int | None]:
        try:
            if self._sess is None:
                self._sess = ScpiSocket(self.host, timeout=4.0, name="monitor").open()
            t = time.monotonic()
            reply = self._sess.query("SYST:UPT?", timeout=4.0)
            ms = (time.monotonic() - t) * 1000.0
            return True, ms, int(float(reply))
        except (ScpiError, ValueError):
            if self._sess is not None:
                self._sess.close()
                self._sess = None
            return False, None, None

    def _probe_usb(self) -> tuple[bool, float | None, int | None]:
        if self.usb is None:
            return False, None, None
        try:
            t = time.monotonic()
            reply = self.usb.query("SYST:UPT?", timeout=3.0)
            ms = (time.monotonic() - t) * 1000.0
            return True, ms, int(float(reply))
        except (ScpiError, ValueError, OSError):
            return False, None, None

    def _probe_tcp(self) -> bool:
        """Bare handshake test - does the W5500 still have a listening socket?"""
        try:
            s = socket.create_connection((self.host, SCPI_PORT), timeout=3.0)
            s.close()
            return True
        except OSError:
            return False

    def _probe_fresh(self) -> bool:
        """Full connect + query on a throwaway socket: can a NEW client be served?"""
        try:
            with ScpiSocket(self.host, timeout=4.0, name="fresh") as s:
                s.query("*IDN?", timeout=4.0)
            return True
        except ScpiError:
            return False

    # -- classification ----------------------------------------------------
    def _classify(self, lan_ok, usb_ok, tcp_ok, fresh_ok, lan_ms) -> str:
        have_usb = self.usb is not None
        if lan_ok and (usb_ok or not have_usb):
            if fresh_ok is False:
                return "POOL_FULL"
            if lan_ms is not None and lan_ms > self.slow_ms:
                return "SLOW"
            return "HEALTHY"
        if lan_ok and have_usb and not usb_ok:
            # Odd but real: the USB CDC host endpoint can be lost on its own.
            return "NET_STALL"
        if not lan_ok and usb_ok:
            return "NET_STALL"
        if not lan_ok and not usb_ok:
            return "CPU_WEDGED" if tcp_ok else "DEAD"
        return "HEALTHY"

    def run(self) -> None:
        while not self._stop.is_set():
            self._pass += 1
            lan_ok, lan_ms, up_lan = self._probe_lan()
            usb_ok, usb_ms, up_usb = self._probe_usb()
            tcp_ok = True if lan_ok else self._probe_tcp()
            fresh_ok: bool | None = None
            if lan_ok and self._pass % self.fresh_every == 0:
                fresh_ok = self._probe_fresh()
            uptime = up_lan if up_lan is not None else up_usb

            state = self._classify(lan_ok, usb_ok, tcp_ok, fresh_ok, lan_ms)
            with self._lock:
                phase, disrupts = self.phase, self.phase_disrupts_lan
                sample = Sample(round(ts(), 3), state, lan_ok, usb_ok, tcp_ok,
                                fresh_ok, lan_ms, usb_ms, uptime, phase)
                self.samples.append(sample)

                # Reboot detection: a watchdog or brown-out resets the uptime.
                if uptime is not None:
                    if self.first_boot_uptime is None:
                        self.first_boot_uptime = uptime
                    elif uptime + 5 < self._last_uptime:
                        self.reboots += 1
                        self.log.add("REBOOT",
                                     f"uptime went {self._last_uptime}s -> {uptime}s "
                                     f"(instrument restarted during '{phase}')")
                    self._last_uptime = uptime

                if state != self.state:
                    note = " (expected in this phase)" if disrupts and state in (
                        "POOL_FULL", "NET_STALL", "DEAD") else ""
                    self.log.add("STATE", f"{self.state} -> {state}  "
                                          f"[phase {phase}]{note}",
                                 prev=self.state, new=state, phase=phase)
                    self.state = state
            self._stop.wait(self.interval)

    _last_uptime: int = 0

    def set_phase(self, name: str, disrupts_lan: bool = False) -> None:
        with self._lock:
            self.phase, self.phase_disrupts_lan = name, disrupts_lan

    def worst_state_during(self, phase: str) -> str:
        seen = [s.state for s in self.samples if s.phase == phase]
        if not seen:
            return "HEALTHY"
        return max(seen, key=STATES.index)

    def states_during(self, phase: str) -> list[str]:
        """Every distinct state observed, worst first (not just the worst one).

        A phase that goes HEALTHY -> CPU_WEDGED -> DEAD must still report the
        CPU_WEDGED it passed through: that is the diagnostic, and reporting only
        the worst state would hide it behind DEAD.
        """
        seen = {s.state for s in self.samples if s.phase == phase}
        return sorted(seen, key=STATES.index, reverse=True)

    def uptime_continuous_across(self, phase: str) -> bool:
        """Did the uptime counter run straight through this phase's outage?

        If it did, the instrument never reset - so an outage during the phase was
        a blocking stall inside loop(), not a crash, watchdog or brown-out. This
        is the single most useful fact the harness can establish.
        """
        idx = [i for i, s in enumerate(self.samples) if s.phase == phase]
        if not idx:
            return False
        lo, hi = min(idx), max(idx)
        # Widen by one sample either side so the before/after readings are included.
        window = self.samples[max(0, lo - 1):min(len(self.samples), hi + 2)]
        ups = [(s.t, s.uptime) for s in window if s.uptime is not None]
        if len(ups) < 2:
            return False
        (t0, u0), (t1, u1) = ups[0], ups[-1]
        # Uptime must have advanced by roughly the wall time that elapsed.
        return u1 >= u0 and abs((u1 - u0) - (t1 - t0)) < max(10.0, 0.25 * (t1 - t0))

    def wait_recovered(self, timeout: float = 60.0) -> bool:
        """Block until a fresh LAN session answers again (post-phase recovery)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                with ScpiSocket(self.host, timeout=3.0) as s:
                    s.query("*IDN?", timeout=3.0)
                return True
            except ScpiError:
                time.sleep(1.0)
        return False


# ═══════════════════════════════════════════════════════════════════════════
#  Phase results
# ═══════════════════════════════════════════════════════════════════════════
@dataclass
class PhaseResult:
    name: str
    started: float
    duration: float = 0.0
    queries: int = 0
    errors: int = 0
    connects: int = 0
    latencies: list[float] = field(default_factory=list)   # ms
    stalls: list[float] = field(default_factory=list)      # ms, over --stall-ms
    notes: list[str] = field(default_factory=list)
    worst_state: str = "HEALTHY"
    states_seen: list[str] = field(default_factory=list)
    recovered: bool = True
    # Set when the instrument went unreachable but its uptime counter came back
    # continuous: it never reset, so it was blocked, not crashed.
    stalled_without_reset: bool = False

    def summary(self) -> dict:
        lat = sorted(self.latencies)
        def pct(p):
            return round(lat[min(len(lat) - 1, int(len(lat) * p))], 1) if lat else None
        return {
            "phase": self.name,
            "duration_s": round(self.duration, 1),
            "queries": self.queries,
            "errors": self.errors,
            "connects": self.connects,
            "lat_p50_ms": pct(0.50),
            "lat_p95_ms": pct(0.95),
            "lat_max_ms": round(lat[-1], 1) if lat else None,
            "stalls": len(self.stalls),
            "worst_stall_ms": round(max(self.stalls), 1) if self.stalls else None,
            "worst_state": self.worst_state,
            "states_seen": self.states_seen,
            "stalled_without_reset": self.stalled_without_reset,
            "recovered": self.recovered,
            "notes": self.notes,
        }


# ═══════════════════════════════════════════════════════════════════════════
#  Workload phases
# ═══════════════════════════════════════════════════════════════════════════
# Read-only command mix. MEAS:* is deliberately included: scpi.cpp measForce()
# busy-waits up to 50 ms per channel for a fresh telemetry frame, so a stream of
# MEAS:ALL? (@1,2) is the cheapest way to starve loop() without any bug at all.
READ_CMDS = [
    "*IDN?", "*OPC?", "*STB?", "*ESR?", "*TST?",
    "MEAS:VOLT? (@1)", "MEAS:CURR? (@2)", "MEAS:ALL? (@1,2)",
    "FETC:VOLT? (@1,2)", "FETC:ALL? (@1,2)",
    "VOLT? (@1)", "CURR? (@2)", "OUTP? (@1,2)",
    "STAT:QUES:COND?", "STAT:OPER:COND?",
    "SYST:UPT?", "SYST:ERR:COUNT?", "SYST:VERS?",
    "SYST:COMM:LAN:IPAD?", "SYST:COMM:LAN:MAC?",
    "INST:CAT?", "MEAS:TEMP?",
]

HEAVY_CMDS = ["MEAS:ALL? (@1,2)", "MEAS:VOLT? (@1,2)", "MEAS:CURR? (@1,2)"]


class Ctx:
    """Everything a phase needs: config, log, monitor, and the stop flag."""

    def __init__(self, args, log: EventLog, monitor: Monitor):
        self.args, self.log, self.mon = args, log, monitor
        self.stop = threading.Event()


def _timed_query(sock: ScpiSocket, cmd: str, res: PhaseResult,
                 stall_ms: float, log: EventLog) -> str | None:
    t = time.monotonic()
    try:
        reply = sock.query(cmd)
    except ScpiError as exc:
        res.errors += 1
        log.add("err", f"{res.name}: '{cmd}' -> {exc}", quiet=True)
        raise
    ms = (time.monotonic() - t) * 1000.0
    res.queries += 1
    res.latencies.append(ms)
    if ms > stall_ms:
        res.stalls.append(ms)
        log.add("stall", f"{res.name}: '{cmd}' took {ms:.0f} ms")
    return reply


# ── phase: soak ─────────────────────────────────────────────────────────────
def phase_soak(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """One long-lived session at a steady rate - the handover's known trigger."""
    period = 1.0 / ctx.args.rate
    end = time.monotonic() + duration
    sock = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open()
    res.connects += 1
    try:
        i = 0
        while time.monotonic() < end and not ctx.stop.is_set():
            cmd = READ_CMDS[i % len(READ_CMDS)]
            i += 1
            try:
                _timed_query(sock, cmd, res, ctx.args.stall_ms, ctx.log)
            except ScpiError:
                # A dropped session is itself a data point; re-establish so the
                # phase keeps measuring how long the instrument stays away.
                sock.close()
                time.sleep(1.0)
                try:
                    sock = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open()
                    res.connects += 1
                    res.notes.append(f"reconnected at t={ts():.0f}s")
                except ScpiError:
                    time.sleep(2.0)
                    continue
            time.sleep(max(0.0, period))
    finally:
        sock.close()


# ── phase: rapid ────────────────────────────────────────────────────────────
def phase_rapid(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Back-to-back queries on one session: maximum request rate, always read.

    Exercises netcfg.cpp serviceClient(), which does one SPI transaction per
    received byte and issues a Sock_RECV command every 250 bytes.
    """
    end = time.monotonic() + duration
    with ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout) as sock:
        res.connects += 1
        i = 0
        while time.monotonic() < end and not ctx.stop.is_set():
            try:
                _timed_query(sock, "*OPC?" if i % 4 else "MEAS:ALL? (@1,2)",
                             res, ctx.args.stall_ms, ctx.log)
            except ScpiError:
                break
            i += 1
    res.notes.append(f"{res.queries} queries in {duration:.0f}s "
                     f"({res.queries / max(duration, 1e-9):.1f}/s)")


# ── phase: churn ────────────────────────────────────────────────────────────
def phase_churn(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """connect -> query -> graceful close, as fast as possible.

    Targets the accept()/stop() path: every accept consumes the listening socket
    and EthernetServer::begin() has to re-open a new one from the free pool.
    """
    end = time.monotonic() + duration
    while time.monotonic() < end and not ctx.stop.is_set():
        try:
            with ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout) as sock:
                res.connects += 1
                _timed_query(sock, "*IDN?", res, ctx.args.stall_ms, ctx.log)
        except ScpiError as exc:
            res.errors += 1
            ctx.log.add("err", f"churn: {exc}", quiet=True)
            time.sleep(0.2)


# ── phase: rst ──────────────────────────────────────────────────────────────
def phase_rst(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Connect, send a query, then RST the connection without reading the reply.

    The brain is mid-write when the RST lands. Models the killed-script case.
    """
    end = time.monotonic() + duration
    while time.monotonic() < end and not ctx.stop.is_set():
        try:
            sock = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open()
            res.connects += 1
            sock.write("MEAS:ALL? (@1,2)")
            time.sleep(random.uniform(0.0, 0.05))   # land the RST at varied offsets
            sock.abort()
        except ScpiError as exc:
            res.errors += 1
            ctx.log.add("err", f"rst: {exc}", quiet=True)
            time.sleep(0.2)
        time.sleep(0.1)


# ── phase: abandon ──────────────────────────────────────────────────────────
def phase_abandon(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Open sessions and silently walk away - no FIN, no RST, ever.

    This is the pool-starvation test. The W5500 has no TCP keepalive enabled and
    netcfg.cpp never ages a session out, so an abandoned socket stays ESTABLISHED
    and EthernetClient::connected() keeps returning true. Four of those and the
    4-slot pool is full forever, with loop() perfectly healthy - which is what
    "connect succeeds but the instrument never replies" looks like.

    The socket objects are kept referenced (not GC'd) for the hold time, so the
    host OS sends nothing at all.
    """
    hold = min(duration, ctx.args.abandon_hold)
    ghosts: list[ScpiSocket] = []
    for i in range(FW_MAX_CLIENTS):
        try:
            s = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open()
            res.connects += 1
            s.write("*IDN?")                # make it a real, active session
            ghosts.append(s)
        except ScpiError as exc:
            res.errors += 1
            ctx.log.add("err", f"abandon: session {i} {exc}", quiet=True)
    ctx.log.add("phase", f"abandon: {len(ghosts)} sessions parked, holding {hold:.0f}s")

    # While they are parked, can a fresh client still get served?
    t_end = time.monotonic() + hold
    served, refused = 0, 0
    while time.monotonic() < t_end and not ctx.stop.is_set():
        try:
            with ScpiSocket(ctx.args.psu, timeout=4.0) as probe:
                probe.query("*IDN?", timeout=4.0)
                served += 1
                res.queries += 1
        except ScpiError:
            refused += 1
            res.errors += 1
        time.sleep(2.0)
    res.notes.append(f"while {len(ghosts)} sessions abandoned: "
                     f"{served} fresh sessions served, {refused} refused/timed out")

    for s in ghosts:
        s.abort()
    ctx.log.add("phase", "abandon: parked sessions released (RST)")

    # Being locked out *while* the pool is deliberately full is correct
    # behaviour, not a finding - the pool is NET_MAX_CLIENTS deep and we filled
    # it. The bug is a pool that never comes back, so that is what is judged.
    #
    # Note what this phase does and does not cover. The host OS here is alive and
    # answers the W5500's keep-alive probes, so keep-alive cannot reap these
    # sessions; only netcfg.cpp's NET_CLIENT_IDLE_MS backstop can, and that is
    # deliberately ~10 min - far longer than any sane --abandon-hold. So a
    # refusal during the hold is expected, and the real check is recovery.
    # Keep-alive covers the other case (host gone entirely), which needs a cable
    # pull to test and is out of scope here.
    freed = ctx.mon.wait_recovered(timeout=ctx.args.recover_timeout)
    res.recovered = freed
    if not freed:
        res.notes.append("POOL STARVATION: slots were NOT reclaimed after the "
                         "abandoned sessions were reset - netcfg.cpp is not "
                         "reaping dropped peers")
    elif refused:
        res.notes.append("pool saturated while held (expected), and fully "
                         "reclaimed once the peers went away")


# ── phase: noread ───────────────────────────────────────────────────────────
def phase_noread(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Pipeline queries and never read the answers, with a tiny receive window.

    The prime suspect. EthernetClass::socketSend() (lib/Ethernet/src/socket.cpp)
    spins `do {...} while (freesize < ret)` waiting for W5500 TX buffer space and
    only leaves that loop if the socket drops out of ESTABLISHED/CLOSE_WAIT. A
    peer that stops reading but keeps the connection open advertises a zero
    window, the W5500 never frees TX space, and loop() never returns. That wedges
    the front panel and the USB console too - exactly the reported symptom.

    Sequence: fill (no reads) -> hold -> start reading again -> check recovery.
    If the instrument wedges during the hold and does NOT come back once we drain,
    the spin is unbounded; if it comes back, the path is at least self-limiting.
    """
    fill = min(duration * 0.5, 45.0)
    hold = min(duration * 0.3, 30.0)

    # A small SO_RCVBUF closes the advertised window after a few hundred bytes,
    # so the brain hits the full-TX-buffer condition in seconds, not minutes.
    sock = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout, rcvbuf=512).open()
    res.connects += 1
    sent = 0
    try:
        sock._sock.setblocking(False)                       # type: ignore[union-attr]
        end = time.monotonic() + fill
        payload = b"MEAS:ALL? (@1,2)\n"
        while time.monotonic() < end and not ctx.stop.is_set():
            try:
                sock._sock.send(payload)                    # type: ignore[union-attr]
                sent += 1
            except BlockingIOError:
                # Our send buffer is full too: the brain has stopped draining its
                # RX side, which is itself evidence loop() is not running.
                time.sleep(0.05)
            except OSError as exc:
                res.notes.append(f"send died after {sent} queries ({exc})")
                break
            time.sleep(0.002)
        ctx.log.add("phase", f"noread: {sent} queries sent unread, "
                             f"holding {hold:.0f}s with the window shut")
        res.notes.append(f"{sent} queries sent without reading")

        t_end = time.monotonic() + hold
        while time.monotonic() < t_end and not ctx.stop.is_set():
            time.sleep(0.5)

        # Drain: if the brain was spinning in socketSend, opening the window is
        # the only thing that can release it.
        ctx.log.add("phase", "noread: draining - reopening the receive window")
        sock._sock.setblocking(True)                        # type: ignore[union-attr]
        sock._sock.settimeout(5.0)                          # type: ignore[union-attr]
        drained = 0
        try:
            while True:
                chunk = sock._sock.recv(4096)               # type: ignore[union-attr]
                if not chunk:
                    break
                drained += len(chunk)
                if drained > 256 * 1024:
                    break
        except (socket.timeout, OSError):
            pass
        res.notes.append(f"drained {drained} bytes of backed-up responses")
    finally:
        sock.close()

    recovered = ctx.mon.wait_recovered(timeout=ctx.args.recover_timeout)
    res.recovered = recovered
    if not recovered:
        res.notes.append("DID NOT RECOVER after the window reopened -> the "
                         "socketSend() free-space spin is unbounded (socket.cpp)")


# ── phase: pool ─────────────────────────────────────────────────────────────
def phase_pool(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Open more concurrent sessions than the pool holds, then release them.

    netcfg.cpp accepts up to NET_MAX_CLIENTS and stop()s the overflow. Checks the
    rejection is clean and, more importantly, that the pool frees up afterwards.
    """
    n = FW_MAX_CLIENTS + 2
    socks: list[ScpiSocket] = []
    answered = 0
    for i in range(n):
        try:
            s = ScpiSocket(ctx.args.psu, timeout=4.0).open()
            res.connects += 1
            socks.append(s)
            try:
                s.query("*IDN?", timeout=3.0)
                answered += 1
            except ScpiError:
                pass
        except ScpiError as exc:
            ctx.log.add("err", f"pool: session {i} connect failed ({exc})", quiet=True)
            res.errors += 1
    # The liveness monitor holds one slot of the pool for the whole run, so the
    # expected number of answering workload sessions is NET_MAX_CLIENTS - 1.
    expect = FW_MAX_CLIENTS - 1
    res.notes.append(f"{len(socks)}/{n} sessions connected, {answered} answered "
                     f"(firmware pool is {FW_MAX_CLIENTS}, monitor holds 1, "
                     f"so {expect} expected)")
    if answered > expect:
        res.notes.append("more sessions answered than the pool should allow - "
                         "pool accounting is off")
    if len(socks) == n:
        res.notes.append("all connections were accepted at the TCP level even "
                         "past the pool limit: the W5500 completes the handshake "
                         "in hardware, so an over-limit client sees a connected "
                         "socket that is silently dropped rather than a refusal")

    # Hold them, then close cleanly and confirm the slots come back.
    t_end = time.monotonic() + min(duration * 0.5, 15.0)
    while time.monotonic() < t_end and not ctx.stop.is_set():
        for s in socks:
            try:
                _timed_query(s, "*OPC?", res, ctx.args.stall_ms, ctx.log)
            except ScpiError:
                pass
        time.sleep(0.5)
    for s in socks:
        s.close()

    res.recovered = ctx.mon.wait_recovered(timeout=ctx.args.recover_timeout)
    if not res.recovered:
        res.notes.append("pool did not free up after all sessions closed cleanly")


# ── phase: slowloris ────────────────────────────────────────────────────────
def phase_slowloris(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Sessions that dribble one byte at a time and never terminate the line.

    Holds pool slots and per-slot line assemblers (s_line[]) indefinitely while
    the brain sees a trickle of legitimate traffic.
    """
    n = FW_MAX_CLIENTS - 1          # leave one slot so the monitor keeps working
    socks = []
    for _ in range(n):
        try:
            socks.append(ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open())
            res.connects += 1
        except ScpiError:
            res.errors += 1
    end = time.monotonic() + duration
    text = b"MEAS:ALL? (@1,2)"
    idx = 0
    while time.monotonic() < end and not ctx.stop.is_set():
        for s in socks:
            try:
                s.send_raw(text[idx % len(text):idx % len(text) + 1])
            except ScpiError:
                res.errors += 1
        idx += 1
        time.sleep(1.5)
    for s in socks:
        try:
            s.write("")             # terminate the dangling line
        except ScpiError:
            pass
        s.close()
    res.notes.append(f"{n} sessions dribbled {idx} bytes each over {duration:.0f}s")


# ── phase: malformed ────────────────────────────────────────────────────────
def phase_malformed(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Oversized lines, deep ';' chains, junk bytes, unterminated input.

    Checks the line assembler and the parser's fixed buffers: NET_LINE_MAX is
    256 while scpi_process_line() copies into a 264-byte static, and the shared
    response buffer is SCPI_RESP_MAX = 480 for a ';'-chained line.
    """
    cases: list[tuple[str, bytes, bool]] = [
        ("over-long line (-363 expected)", b"A" * (FW_LINE_MAX + 64) + b"\n", False),
        ("exactly at the cap", b"*IDN?" + b" " * (FW_LINE_MAX - 6) + b"\n", True),
        # Fits the line cap, but its combined response far exceeds
        # SCPI_RESP_MAX (480 B). Must still come back as one terminated line:
        # truncating without the trailing LF used to hang the client forever.
        ("chain overflowing the 480 B response", b";".join([b"*IDN?"] * 24) + b"\n", True),
        # Longer than NET_LINE_MAX, so it is a framing error by design: the
        # firmware logs -363 and deliberately sends nothing. No reply expected.
        ("chain past the line cap (-363, silent)",
         b";".join([b"MEAS:ALL? (@1,2)"] * 40) + b"\n", False),
        ("bare CR", b"*IDN?\r\n", True),
        ("empty lines", b"\n\n\n*IDN?\n", True),
        ("binary junk", bytes(random.randrange(1, 255) for _ in range(64)) + b"\n*IDN?\n", True),
        ("unknown header", b"NOSUCH:THING?\n*IDN?\n", True),
        ("bad channel list", b"MEAS:ALL? (@9,9\n*IDN?\n", True),
        ("no terminator then a good line", b"*IDN?", True),
    ]
    end = time.monotonic() + duration
    while time.monotonic() < end and not ctx.stop.is_set():
        for label, payload, expect_reply in cases:
            if ctx.stop.is_set():
                break
            try:
                with ScpiSocket(ctx.args.psu, timeout=4.0) as s:
                    res.connects += 1
                    s.send_raw(payload)
                    if not payload.endswith(b"\n"):
                        s.send_raw(b"\n")
                    try:
                        s.read_line(timeout=4.0)
                        res.queries += 1
                    except ScpiError:
                        if expect_reply:
                            res.errors += 1
                            res.notes.append(f"no reply to: {label}")
            except ScpiError as exc:
                res.errors += 1
                ctx.log.add("err", f"malformed [{label}]: {exc}", quiet=True)
            time.sleep(0.1)
    # Drain the error queue this phase deliberately filled.
    try:
        with ScpiSocket(ctx.args.psu, timeout=4.0) as s:
            count = s.query("SYST:ERR:COUNT?", timeout=4.0)
            res.notes.append(f"error queue held {count} entries afterwards")
            for _ in range(16):
                if s.query("SYST:ERR?", timeout=4.0).startswith("0,"):
                    break
    except ScpiError:
        pass


# ── phase: heavy ────────────────────────────────────────────────────────────
def phase_heavy(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Two concurrent sessions hammering MEAS:* (the 50 ms-per-channel path).

    scpi.cpp measForce() busy-waits for a fresh telemetry frame, so this measures
    how much of loop() the network path can legitimately consume before the UI
    suffers - no bug required.
    """
    end = time.monotonic() + duration
    lock = threading.Lock()

    def worker(wid: int) -> None:
        try:
            sock = ScpiSocket(ctx.args.psu, timeout=ctx.args.timeout).open()
        except ScpiError:
            with lock:
                res.errors += 1
            return
        with lock:
            res.connects += 1
        try:
            i = 0
            while time.monotonic() < end and not ctx.stop.is_set():
                cmd = HEAVY_CMDS[i % len(HEAVY_CMDS)]
                i += 1
                t = time.monotonic()
                try:
                    sock.query(cmd)
                except ScpiError:
                    with lock:
                        res.errors += 1
                    break
                ms = (time.monotonic() - t) * 1000.0
                with lock:
                    res.queries += 1
                    res.latencies.append(ms)
                    if ms > ctx.args.stall_ms:
                        res.stalls.append(ms)
        finally:
            sock.close()

    threads = [threading.Thread(target=worker, args=(i,), daemon=True)
               for i in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


# ── phase: mixed ────────────────────────────────────────────────────────────
def phase_mixed(ctx: Ctx, res: PhaseResult, duration: float) -> None:
    """Everything at once - the closest thing to a real, hostile bench session."""
    lock = threading.Lock()
    sub = [
        ("soak", phase_soak),
        ("churn", phase_churn),
        ("rst", phase_rst),
        ("heavy", phase_heavy),
    ]
    parts: list[PhaseResult] = []

    def runner(name, fn):
        r = PhaseResult(name=f"mixed/{name}", started=ts())
        try:
            fn(ctx, r, duration)
        except Exception as exc:                              # noqa: BLE001
            r.notes.append(f"aborted: {exc}")
        with lock:
            parts.append(r)

    threads = [threading.Thread(target=runner, args=s, daemon=True) for s in sub]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for r in parts:
        res.queries += r.queries
        res.errors += r.errors
        res.connects += r.connects
        res.latencies += r.latencies
        res.stalls += r.stalls
        res.notes += [f"{r.name}: {n}" for n in r.notes]


PHASES: dict[str, tuple[str, bool, float]] = {
    # name:        (description,                                     disrupts_lan, default_s)
    "soak":       ("steady single-session traffic (known trigger)",  False, 180.0),
    "rapid":      ("max-rate queries on one session",                False,  60.0),
    "churn":      ("connect/query/close as fast as possible",        False,  60.0),
    "rst":        ("abort mid-response with an RST",                 False,  45.0),
    "abandon":    ("park sessions with no FIN/RST (pool starvation)", True,  60.0),
    "noread":     ("stop reading, shut the window (socketSend spin)", True,  90.0),
    "pool":       ("oversubscribe the client pool, then release",     True,  45.0),
    "slowloris":  ("byte-at-a-time lines that never terminate",       True,  60.0),
    "malformed":  ("oversized/chained/junk SCPI lines",              False,  60.0),
    "heavy":      ("2x concurrent MEAS:* flood (measForce cost)",    False,  60.0),
    "mixed":      ("soak+churn+rst+heavy concurrently",              False, 180.0),
}

PHASE_FN = {
    "soak": phase_soak, "rapid": phase_rapid, "churn": phase_churn,
    "rst": phase_rst, "abandon": phase_abandon, "noread": phase_noread,
    "pool": phase_pool, "slowloris": phase_slowloris,
    "malformed": phase_malformed, "heavy": phase_heavy, "mixed": phase_mixed,
}

# Order matters: the benign phases run first so a fault they provoke is not
# confused with one the deliberately-hostile phases cause. 'noread' goes last
# because on this firmware it reliably wedges the instrument hard enough to need
# a power cycle - anything after it would be measuring a dead unit.
DEFAULT_ORDER = ["soak", "rapid", "churn", "heavy", "malformed", "rst",
                 "slowloris", "pool", "abandon", "mixed", "noread"]

# Phases known to leave the instrument needing a manual power cycle. Skipped
# unless asked for by name or with --destructive.
DESTRUCTIVE = {"noread"}


# ═══════════════════════════════════════════════════════════════════════════
#  Runner
# ═══════════════════════════════════════════════════════════════════════════
def preflight(args, log: EventLog) -> None:
    """Refuse to stress an instrument with a live output unless told otherwise."""
    with ScpiSocket(args.psu, timeout=args.timeout) as s:
        log.add("info", f"IDN: {s.query('*IDN?')}")
        log.add("info", f"uptime at start: {s.query('SYST:UPT?')} s")
        outs = s.query("OUTP? (@1,2)")
        log.add("info", f"outputs: {outs}")
        if any(v.strip() == "1" for v in outs.split(",")) and not args.force:
            raise SystemExit("Refusing to run: an output is ON. This test only "
                             "reads, but a wedge leaves it stuck on. Turn the "
                             "outputs off, or pass --force.")


def run_phase(name: str, ctx: Ctx) -> PhaseResult:
    desc, disrupts, default_s = PHASES[name]
    duration = ctx.args.duration if ctx.args.duration else default_s
    res = PhaseResult(name=name, started=ts())
    ctx.log.add("PHASE", f"=== {name}: {desc} ({duration:.0f}s) ===")
    ctx.mon.set_phase(name, disrupts_lan=disrupts)
    t0 = time.monotonic()
    try:
        PHASE_FN[name](ctx, res, duration)
    except KeyboardInterrupt:
        raise
    except Exception as exc:                                   # noqa: BLE001
        res.notes.append(f"phase raised: {type(exc).__name__}: {exc}")
        ctx.log.add("err", f"{name}: {type(exc).__name__}: {exc}")
    res.duration = time.monotonic() - t0
    res.worst_state = ctx.mon.worst_state_during(name)
    res.states_seen = ctx.mon.states_during(name)
    if res.worst_state in ("CPU_WEDGED", "DEAD"):
        res.stalled_without_reset = ctx.mon.uptime_continuous_across(name)

    # Every phase must leave the instrument reachable before the next one starts,
    # otherwise the results of the next phase mean nothing.
    ctx.mon.set_phase(f"{name}:recover", disrupts_lan=False)
    if res.recovered:                       # phases that check it themselves keep theirs
        res.recovered = ctx.mon.wait_recovered(timeout=ctx.args.recover_timeout)
    if not res.recovered:
        ctx.log.add("FAULT", f"{name}: instrument did not recover within "
                             f"{ctx.args.recover_timeout:.0f}s "
                             f"(state {ctx.mon.state}) - POWER CYCLE REQUIRED")
    s = res.summary()
    ctx.log.add("result", f"{name}: {s['queries']} queries, {s['errors']} errors, "
                          f"p50 {s['lat_p50_ms']} ms, max {s['lat_max_ms']} ms, "
                          f"{s['stalls']} stalls, worst state {s['worst_state']}, "
                          f"recovered={s['recovered']}")
    return res


def verdict(results: list[PhaseResult], mon: Monitor) -> list[str]:
    """Turn the run into statements about the firmware, not just numbers."""
    out: list[str] = []
    worst = max((r.worst_state for r in results), key=STATES.index, default="HEALTHY")
    # Match on every state a phase passed through, not just its worst: a phase
    # that ends DEAD usually went through CPU_WEDGED on the way, and that is the
    # observation that localises the bug.
    wedged = [r for r in results if "CPU_WEDGED" in r.states_seen]
    stalled = [r for r in results if "NET_STALL" in r.states_seen]
    poolfull = [r for r in results if "POOL_FULL" in r.states_seen]
    dead = [r for r in results if "DEAD" in r.states_seen]
    no_reset = [r for r in results if r.stalled_without_reset]
    unrecovered = [r for r in results if not r.recovered]

    out.append(f"worst state reached: {worst}")
    if mon.reboots:
        out.append(f"instrument restarted {mon.reboots}x during the run "
                   "(uptime went backwards) - something is resetting it")
    if wedged:
        out.append("CPU_WEDGED seen in: " + ", ".join(r.name for r in wedged))
        out.append("  -> neither transport answered while the W5500 still "
                   "completed the TCP handshake: loop() stopped running. Look "
                   "for an unbounded spin reached from the network path - "
                   "socket.cpp socketSend() free-space wait, "
                   "w5100.cpp execCmdSn(), socket.cpp getSnTX_FSR()/getSnRX_RSR().")
    if any(r.name == "noread" for r in wedged):
        out.append("  -> it wedged specifically while the client held its TCP "
                   "receive window shut, and came back when the window "
                   "reopened. That is EthernetClass::socketSend() "
                   "(lib/Ethernet/src/socket.cpp): it spins "
                   "`do {...} while (freesize < ret)` for W5500 TX space and "
                   "only breaks if the socket leaves ESTABLISHED/CLOSE_WAIT. A "
                   "peer advertising a zero window never triggers that, so "
                   "loop() blocks for as long as the peer stays quiet.")
    if dead:
        out.append("DEAD seen in: " + ", ".join(r.name for r in dead))
        out.append("  -> the TCP handshake stopped completing too: while loop() "
                   "is blocked nothing re-arms the listener "
                   "(EthernetServer::begin() runs from accept()), so the last "
                   "LISTEN socket is consumed and the port goes silent.")
    if no_reset:
        out.append("uptime ran continuously through the outage in: " +
                   ", ".join(r.name for r in no_reset))
        out.append("  -> the instrument never reset: no crash, no brown-out, no "
                   "watchdog. It was blocked in a call and resumed where it "
                   "left off. Confirms a blocking-call bug over a memory fault.")
    if stalled:
        out.append("NET_STALL seen in: " + ", ".join(r.name for r in stalled))
        out.append("  -> USB kept answering, so loop() was alive: the fault is "
                   "in the socket/session layer (pool slots not freed, listener "
                   "lost), not a blocking call.")
    if poolfull:
        transient = [r for r in poolfull if r.recovered]
        stuck = [r for r in poolfull if not r.recovered]
        out.append("POOL_FULL seen in: " + ", ".join(r.name for r in poolfull))
        if stuck:
            out.append("  -> and the pool did NOT come back in: " +
                       ", ".join(r.name for r in stuck) + ". That is the real "
                       "fault: netcfg.cpp frees a slot only when connected() "
                       "goes false, so check the keep-alive (Sn_KPALVTR) and "
                       "the NET_CLIENT_IDLE_MS backstop are actually applied.")
        if transient:
            out.append("  -> recovered on its own in: " +
                       ", ".join(r.name for r in transient) + ". Expected: those "
                       "phases deliberately hold every slot, and new clients are "
                       "meant to be locked out until they let go.")
    if unrecovered:
        out.append("did not self-recover after: " +
                   ", ".join(r.name for r in unrecovered))
    if worst == "HEALTHY":
        out.append("no fault provoked; if the bench hang is real, extend "
                   "--duration or run --phase soak for an hour")
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--psu", default="192.168.2.128", help="instrument IP")
    p.add_argument("--usb", default="auto",
                   help="USB CDC port for the liveness probe ('auto', 'none', or COMn)")
    p.add_argument("--phase", action="append", default=None,
                   help="phase to run (repeatable); default runs the full sequence")
    p.add_argument("--duration", type=float, default=0.0,
                   help="seconds per phase (0 = each phase's own default)")
    p.add_argument("--rate", type=float, default=1.4,
                   help="queries/s for the soak phase")
    p.add_argument("--timeout", type=float, default=5.0, help="per-query timeout, s")
    p.add_argument("--stall-ms", type=float, default=1000.0,
                   help="a reply slower than this is logged as a stall")
    p.add_argument("--probe-interval", type=float, default=2.0,
                   help="liveness monitor cadence, s")
    p.add_argument("--recover-timeout", type=float, default=60.0,
                   help="how long to wait for the instrument between phases, s")
    p.add_argument("--abandon-hold", type=float, default=45.0,
                   help="how long abandoned sessions are parked, s")
    p.add_argument("--force", action="store_true",
                   help="run even with an output on")
    p.add_argument("--destructive", action="store_true",
                   help=f"include phases that can wedge the unit until it is "
                        f"power-cycled ({', '.join(sorted(DESTRUCTIVE))})")
    p.add_argument("--keep-going", action="store_true",
                   help="carry on with later phases even if the instrument "
                        "never came back")
    p.add_argument("--verbose", action="store_true", help="log every event")
    p.add_argument("--list", action="store_true", help="list the phases and exit")
    p.add_argument("--out", default=None, help="JSON report path")
    args = p.parse_args(argv)

    if args.list:
        print("phases (default order):")
        for name in DEFAULT_ORDER:
            desc, disrupts, default_s = PHASES[name]
            flag = " [disrupts LAN by design]" if disrupts else ""
            if name in DESTRUCTIVE:
                flag += " [DESTRUCTIVE: needs --destructive; may need a power cycle]"
            print(f"  {name:<11} {default_s:>5.0f}s  {desc}{flag}")
        return 0

    log = EventLog(verbose=args.verbose)
    log.add("info", f"target {args.psu}:{SCPI_PORT}")

    usb: UsbScpi | None = None
    if args.usb.lower() == "auto":
        usb = autodetect_usb(log)
    elif args.usb.lower() != "none":
        try:
            usb = UsbScpi(args.usb)
            log.add("usb", f"probe on {args.usb}: {usb.query('*IDN?')}")
        except Exception as exc:                               # noqa: BLE001
            log.add("usb", f"cannot open {args.usb} ({exc}); USB probe disabled")
    if usb is None:
        log.add("warn", "no USB probe: NET_STALL and CPU_WEDGED cannot be told "
                        "apart (that distinction is the point of this test)")

    try:
        preflight(args, log)
    except ScpiError as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        return 2

    mon = Monitor(args.psu, usb, log, interval=args.probe_interval,
                  slow_ms=args.stall_ms)
    mon.start()

    ctx = Ctx(args, log, mon)
    phases = args.phase if args.phase else DEFAULT_ORDER
    unknown = [ph for ph in phases if ph not in PHASES]
    if unknown:
        print(f"unknown phase(s): {', '.join(unknown)} "
              f"(see --list)", file=sys.stderr)
        return 2

    if not args.phase and not args.destructive:
        skipped = [ph for ph in phases if ph in DESTRUCTIVE]
        phases = [ph for ph in phases if ph not in DESTRUCTIVE]
        if skipped:
            log.add("info", f"skipping destructive phase(s) {', '.join(skipped)} "
                            "- pass --destructive (or name the phase) to include "
                            "them; they can leave the unit needing a power cycle")

    results: list[PhaseResult] = []
    try:
        for name in phases:
            results.append(run_phase(name, ctx))
            if not results[-1].recovered and not args.keep_going:
                log.add("info", "instrument is still unreachable - stopping here. "
                                "Power-cycle it before the next run "
                                "(--keep-going overrides).")
                break
    except KeyboardInterrupt:
        log.add("info", "interrupted - writing what we have")
        ctx.stop.set()
    finally:
        mon.stop()
        mon.join(timeout=5.0)

    print("\n" + "=" * 78)
    print("SUMMARY")
    print("=" * 78)
    hdr = (f"{'phase':<11} {'qry':>7} {'err':>5} {'p50':>7} {'p95':>7} "
           f"{'max':>8} {'stall':>6} {'worst':<11} rec")
    print(hdr)
    print("-" * 78)
    for r in results:
        s = r.summary()
        print(f"{s['phase']:<11} {s['queries']:>7} {s['errors']:>5} "
              f"{str(s['lat_p50_ms']):>7} {str(s['lat_p95_ms']):>7} "
              f"{str(s['lat_max_ms']):>8} {s['stalls']:>6} "
              f"{s['worst_state']:<11} {'y' if s['recovered'] else 'N'}")
    print()
    for r in results:
        for n in r.summary()["notes"]:
            print(f"  {r.name}: {n}")

    print("\nVERDICT")
    print("-" * 78)
    lines = verdict(results, mon)
    for line in lines:
        print(f"  {line}")

    if any(not r.recovered for r in results):
        print("\n  *** The instrument is still unreachable. Power-cycle it "
              "before the next run. ***")

    out_path = Path(args.out) if args.out else (
        Path(__file__).parent / "results" /
        f"netstress-{time.strftime('%Y%m%d-%H%M%S')}.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "target": args.psu,
        "started": time.strftime("%Y-%m-%d %H:%M:%S"),
        "args": vars(args),
        "usb_probe": usb is not None,
        "phases": [r.summary() for r in results],
        "verdict": lines,
        "reboots": mon.reboots,
        "events": log.events,
        "samples": [vars(s) for s in mon.samples],
    }, indent=2))
    print(f"\nreport: {out_path}")

    if usb is not None:
        usb.close()

    worst = max((r.worst_state for r in results), key=STATES.index, default="HEALTHY")
    return 0 if worst in ("HEALTHY", "SLOW") else 1


if __name__ == "__main__":
    sys.exit(main())
