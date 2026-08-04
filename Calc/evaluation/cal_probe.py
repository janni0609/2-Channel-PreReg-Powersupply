#!/usr/bin/env python3
"""Recover the IMEAS gain/offset the channel actually stored, and compare it
with the 2-point fit it should have produced.

This exists because CAL:DATA? returns NaN: there is no way to read the stored
calibration constants over the link, which is what let a measure-path
calibration bug hide for as long as it did. Retire it once CAL:DATA? is
implemented.

Trick: with CAL:RES the coefficients are exactly (gain 833.3333 mA/V, offset 0),
so a default-cal readback tells us the raw ADC volts x at a given true current:
    readback_mA = 833.3333 * x   ->   x = readback_A * 1.2
Those x values are a property of the hardware, not of the calibration, so after
a calibration we can solve the stored line from two readbacks at the same
currents.
"""
import argparse
import sys
import time

from psu_accuracy import ScpiSocket  # noqa: E402

DEFAULT_GAIN = 1000.0 / 1.2      # mA per ADC volt, = kDefaultIMeasGain
LOW, HIGH = 0.2, 1.8


def drain(psu, tag=""):
    while True:
        e = psu.query("SYST:ERR?")
        if e.startswith("0,"):
            return
        print("   ERR", tag, e)


def dmm_mean(dmm, n=5):
    return sum(float(dmm.query("READ?")) for _ in range(n)) / n


def set_current(psu, ch, iset, tag):
    """Write a setpoint and confirm the instrument echoes it back."""
    psu.write(f"CURR {iset:.4f},(@{ch})")
    echo = float(psu.query(f"CURR? (@{ch})"))
    if abs(echo - iset) > 1e-3:
        print(f"    !! {tag}: wrote CURR {iset:.4f} but CURR? echoes {echo:.4f}")
    return echo


def settle_to(psu, dmm, ch, iset, settle, tag, tol=0.02, tries=4):
    """Set a current and wait until the DMM agrees the channel really got there.

    Setpoint frames to the channel are fire-and-forget and have been seen to go
    missing, which silently invalidates everything measured afterwards.
    """
    for attempt in range(tries):
        set_current(psu, ch, iset, tag)
        time.sleep(settle)
        got = dmm_mean(dmm, 3)
        if abs(got - iset) <= tol:
            return got
        print(f"    !! {tag}: setpoint {iset:.4f} A did not land "
              f"(dmm {got:.4f} A), retry {attempt+1}/{tries}")
    raise SystemExit(f"{tag}: channel never reached {iset} A")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--psu", default="192.168.2.128")
    ap.add_argument("--dmm", default="192.168.2.222")
    ap.add_argument("--channel", type=int, default=2)
    ap.add_argument("--settle", type=float, default=3.0,
                    help="cal-point settle, matching psu_accuracy --cal-settle")
    ap.add_argument("--verify-settle", type=float, default=8.0)
    args = ap.parse_args()
    ch = args.channel

    psu = ScpiSocket(args.psu, name="psu", timeout=8).open()
    dmm = ScpiSocket(args.dmm, name="dmm", timeout=15).open()
    try:
        psu.write(f"INST:NSEL {ch}")
        psu.write(f"VOLT 2.0,(@{ch})")
        psu.write("CAL:STAT ON,0")
        assert psu.query("CAL:STAT?").strip() == "1", "cal did not unlock"
        drain(psu)

        # ---- 1. defaults: learn x(I) -------------------------------------
        psu.write("CAL:RES IMEAS")
        drain(psu, "reset")
        time.sleep(1.0)
        set_current(psu, ch, 0.0, "pre")
        psu.write(f"OUTP ON,(@{ch})")
        print(f"\n[1] IMEAS at compile-time defaults (gain {DEFAULT_GAIN:.4f} mA/V, offset 0)")
        xs = {}
        for iset in (LOW, HIGH):
            settle_to(psu, dmm, ch, iset, args.verify_settle, f"default {iset}")
            psu_a = float(psu.query(f"MEAS:CURR? (@{ch})"))
            true_a = dmm_mean(dmm)
            x = psu_a * 1.2                      # volts at the ADC I input
            xs[iset] = (x, true_a, psu_a)
            print(f"    set {iset:.2f} A -> true {true_a*1000:9.4f} mA, "
                  f"psu {psu_a*1000:9.4f} mA ({(psu_a-true_a)*1000:+7.3f} mA, "
                  f"{(psu_a/true_a-1)*100:+6.3f} %), x = {x:.6f} V")

        kx = (xs[HIGH][0] - xs[LOW][0]) / (xs[HIGH][1] - xs[LOW][1])   # V per A
        x_of = lambda i: xs[LOW][0] + kx * (i - xs[LOW][1])            # noqa: E731
        print(f"    -> sense scale kx = {kx:.6f} V/A  (nominal 1.2)")

        # ---- 2. calibrate the way psu_accuracy.py does --------------------
        print(f"\n[2] calibrating IMEAS at {LOW}/{HIGH} A with settle {args.settle} s")
        set_current(psu, ch, 0.0, "cal pre")
        psu.write(f"OUTP ON,(@{ch})")
        pts = []
        for idx, iset in ((0, LOW), (1, HIGH)):
            settle_to(psu, dmm, ch, iset, args.settle, f"cal point {idx}")
            true_a = dmm_mean(dmm, 5)
            live = float(psu.query(f"MEAS:CURR? (@{ch})"))
            psu.write(f"CAL:POIN IMEAS,{idx},{true_a:.6f}")
            drain(psu, f"point{idx}")
            pts.append((idx, iset, true_a))
            print(f"    point {idx}: set {iset:.2f} A -> reported {true_a*1000:.4f} mA "
                  f"(psu live readback {live*1000:.4f} mA)")
        psu.write("CAL:COMM IMEAS")
        drain(psu, "commit")
        print("    committed")

        # ---- 3. recover the stored line ----------------------------------
        print("\n[3] post-cal readback")
        res = {}
        for iset in (LOW, HIGH):
            settle_to(psu, dmm, ch, iset, args.verify_settle, f"verify {iset}")
            psu_a = float(psu.query(f"MEAS:CURR? (@{ch})"))
            true_a = dmm_mean(dmm)
            res[iset] = (psu_a, true_a)
            print(f"    set {iset:.2f} A -> true {true_a*1000:9.4f} mA, "
                  f"psu {psu_a*1000:9.4f} mA ({(psu_a-true_a)*1000:+7.3f} mA, "
                  f"{(psu_a/true_a-1)*100:+6.3f} %)")

        xa, ya = x_of(res[LOW][1]),  res[LOW][0] * 1000.0
        xb, yb = x_of(res[HIGH][1]), res[HIGH][0] * 1000.0
        g_stored = (yb - ya) / (xb - xa)
        o_stored = ya - g_stored * xa

        ix0, iy0 = x_of(pts[0][2]), pts[0][2] * 1000.0
        ix1, iy1 = x_of(pts[1][2]), pts[1][2] * 1000.0
        g_ideal = (iy1 - iy0) / (ix1 - ix0)
        o_ideal = iy0 - g_ideal * ix0

        print("\n[4] coefficients")
        print(f"    ideal  fit : gain {g_ideal:10.4f} mA/V   offset {o_ideal:+9.4f} mA")
        print(f"    stored     : gain {g_stored:10.4f} mA/V   offset {o_stored:+9.4f} mA")
        print(f"    delta      : gain {g_stored-g_ideal:+10.4f} "
              f"({(g_stored/g_ideal-1)*100:+.3f} %)  offset {o_stored-o_ideal:+9.4f} mA")

        rx0 = (iy0 - o_stored) / g_stored
        rx1 = (iy1 - o_stored) / g_stored
        print("\n[5] implied recorded x (assuming y was recorded correctly)")
        print(f"    point 0: true x {ix0:.6f} V   implied {rx0:.6f} V   "
              f"({(rx0-ix0)*1000:+.3f} mV, {(rx0/ix0-1)*100:+.3f} %)")
        print(f"    point 1: true x {ix1:.6f} V   implied {rx1:.6f} V   "
              f"({(rx1-ix1)*1000:+.3f} mV, {(rx1/ix1-1)*100:+.3f} %)")
    finally:
        try:
            psu.write("CAL:STAT OFF,0")
            psu.write(f"CURR 0,(@{ch})")
            psu.write(f"OUTP OFF,(@{ch})")
        finally:
            psu.close()
            dmm.close()


if __name__ == "__main__":
    main()
