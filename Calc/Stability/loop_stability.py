#!/usr/bin/env py
# -*- coding: utf-8 -*-
"""
Small-signal loop-stability model of the page-10 linear post-regulator
(sheet Linear_PostReg.kicad_sch) of the 2-channel pre-reg power supply.

Topology (reverse-engineered from the KiCad schematic + datasheets):

  Pass element  Q303 = TIP125 PNP Darlington, COMMON-EMITTER
                emitter -> PreReg (= Vout + 1.7 V), collector -> OUT+
                => high-impedance output node  => LDO-type loop.
                beta >= 1000 (typ 2500); small-signal h_fe = 4 @ 1 MHz, Ic=3A
                => f_T ~ 4 MHz ; Cob ~ 300 pF.

  Driver        Q301 = BCP56 NPN, common-emitter LEVEL SHIFTER,
                emitter degeneration R315 = 150 ohm.
                Forward transconductance  v_err -> i_pass  =  beta(s)/R315.

  CV error amp  U301A TLV4387 (GBW 5.7 MHz, zero-drift), inverting integrator:
                input  R301 = 10 k ; feedback  R303 = 1 k  in series with  C303 = 1 nF
                => integrator unity-gain 1/(2*pi*R301*C303) = 15.9 kHz,
                   comp zero          1/(2*pi*R303*C303) = 159 kHz.

  CC error amp  U301B TLV4387, integrator: R302 = 10 k, feedback R304 = 0 ohm + C304 = 100 pF
                => PURE integrator 1/(2*pi*R302*C304) = 159 kHz, NO comp zero.

  V feedback    U301D difference amp, H = 4.3k/62k = 0.0694 ; C306 = 10 pF (=> ~3.7 MHz pole)
  I feedback    U301C non-inv amp, gain 1 + 18k/1.6k = 12.25 ; C308 = 10 pF (=> ~0.88 MHz pole)
                sense resistor R318 = 100 mohm in the OUT- return (senses full pass current).

  Output cap    C309 = 10 uF (1206 ceramic) IN SERIES WITH R319 = 200 mohm damping
                => output-cap "ESR" zero 1/(2*pi*R319*C309) = 79.6 kHz.

All component values are taken directly from Linear_PostReg.kicad_sch.
"""

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ----------------------------------------------------------------------------
# Component values (from schematic)
# ----------------------------------------------------------------------------
R301 = 10e3      # CV integrator input resistor
R303 = 1e3       # CV comp zero resistor
C303 = 1e-9      # CV integrator cap
R302 = 10e3      # CC integrator input resistor
R304 = 0.0       # CC comp zero resistor (ZERO -> no zero)
C304 = 100e-12   # CC integrator cap
R315 = 150.0     # driver emitter degeneration -> Gm_fwd = beta/R315
C309 = 10e-6     # output capacitor
R319 = 0.200     # series damping R with C309
R318 = 0.100     # current-sense shunt (OUT- leg)
H_V  = 4.3e3/62e3        # voltage-feedback divider/diff-amp gain = 0.0694
A_CS = 1.0 + 18e3/1.6e3  # current-sense amp gain = 12.25

Vout = 35.0
fT   = 4.0e6     # Darlington f_T: this is the GUARANTEED MIN (h_fe=4 @1MHz spec) => beta-pole ~1.6kHz.
                 # NOTE: datasheet Fig.7 is TYPICAL and shows f_T ~30 MHz (beta-pole ~20kHz). This file
                 # therefore models the WORST-CASE device. See loop_tip125_recomp.py for the typ-vs-worst sweep.
VA   = 75.0      # assumed Early voltage of the pass device (for r_o = VA/Ic)
f_H  = 1/(2*np.pi*4.3e3*10e-12)   # Vsense amp pole (R306*C306)  ~ 3.7 MHz
f_CS = 1/(2*np.pi*18e3*10e-12)    # Isense amp pole (R316*C308)  ~ 0.88 MHz

# ----------------------------------------------------------------------------
# Frequency axis
# ----------------------------------------------------------------------------
f = np.logspace(0, 7, 4000)      # 1 Hz .. 10 MHz
w = 2*np.pi*f
s = 1j*w

def metrics(L):
    """Return (f_c, PM_deg, GM_dB, f_180) from a complex loop-gain array L(f)."""
    mag = np.abs(L)
    ph  = np.unwrap(np.angle(L))*180/np.pi
    # gain crossover: |L| crosses 1
    fc = pm = None
    idx = np.where(np.diff(np.sign(mag-1.0)) != 0)[0]
    if len(idx):
        i = idx[0]
        # log-interp the crossover frequency
        fc = np.exp(np.interp(0.0, [np.log(mag[i+1]), np.log(mag[i])],
                                    [np.log(f[i+1]),   np.log(f[i])]))
        phc = np.interp(np.log(fc), np.log(f), ph)
        pm  = 180.0 + phc
    # phase crossover: phase crosses -180  -> gain margin
    gm = f180 = None
    jdx = np.where(np.diff(np.sign(ph+180.0)) != 0)[0]
    if len(jdx):
        j = jdx[0]
        f180 = np.exp(np.interp(-180.0, [ph[j+1], ph[j]], [np.log(f[j+1]), np.log(f[j])]))
        magd = np.interp(np.log(f180), np.log(f), np.log(mag))
        gm   = -20*np.log10(np.exp(magd))
    return fc, pm, gm, f180

def beta_of(beta0):
    return beta0/(1 + s/(2*np.pi*fT/beta0))   # beta(s), pole at f_T/beta0

def Zout(Iload):
    RL  = Vout/max(Iload, 1e-6)
    ro  = VA/max(Iload, 1e-6)
    Rp  = RL*ro/(RL+ro)
    Zc  = R319 + 1/(s*C309)
    return Rp*Zc/(Rp+Zc), Rp

def L_cv(Iload, beta0):
    A_ea = (1 + s*R303*C303)/(s*R301*C303)      # inverting integrator + zero (magnitude/phase)
    Gm   = beta_of(beta0)/R315                  # A/V
    Z,_  = Zout(Iload)
    Hf   = H_V/(1 + s/(2*np.pi*f_H))
    return A_ea * Gm * Z * Hf

def L_cc(Iload, beta0):
    A_ea = 1/(s*R302*C304)                      # pure integrator (R304 = 0)
    Gm   = beta_of(beta0)/R315
    Acs  = A_CS/(1 + s/(2*np.pi*f_CS))
    return A_ea * Gm * R318 * Acs

# ----------------------------------------------------------------------------
# CV loop: sweep load, beta = 2500 (typ) and 1000 (min)
# ----------------------------------------------------------------------------
loads = [0.012, 0.1, 0.5, 2.0]
print("="*78)
print("CV LOOP  (constant-voltage)   f_T = %.1f MHz (constant, optimistic)" % (fT/1e6))
print("-"*78)
print("%-8s %-7s %12s %10s %10s" % ("Iload", "beta", "f_cross", "PhaseMrgn", "GainMrgn"))
for b0 in (2500, 1000):
    for Il in loads:
        fc, pm, gm, f180 = metrics(L_cv(Il, b0))
        print("%-8s %-7d %12s %10s %10s" % (
            f"{Il*1000:.0f}mA", b0,
            f"{fc/1e3:.2f} kHz" if fc else "-- (no XO)",
            f"{pm:+.1f} deg" if pm is not None else "--",
            f"{gm:+.1f} dB" if gm is not None else "--"))
    print()

print("="*78)
print("CC LOOP  (constant-current)   R304 = 0  ->  NO compensation zero")
print("-"*78)
print("%-8s %-7s %12s %10s %10s" % ("Iload", "beta", "f_cross", "PhaseMrgn", "GainMrgn"))
for b0 in (2500, 1000):
    for Il in (0.5, 2.0):
        fc, pm, gm, f180 = metrics(L_cc(Il, b0))
        print("%-8s %-7d %12s %10s %10s" % (
            f"{Il*1000:.0f}mA", b0,
            f"{fc/1e3:.2f} kHz" if fc else "-- (no XO)",
            f"{pm:+.1f} deg" if pm is not None else "--",
            f"{gm:+.1f} dB" if gm is not None else "--"))
    print()

# Key pole/zero summary
print("="*78)
print("KEY POLE / ZERO LOCATIONS")
print("-"*78)
print("  CV integrator unity gain : %7.1f kHz" % (1/(2*np.pi*R301*C303)/1e3))
print("  CV comp zero (R303*C303) : %7.1f kHz" % (1/(2*np.pi*R303*C303)/1e3))
print("  CC integrator unity gain : %7.1f kHz" % (1/(2*np.pi*R302*C304)/1e3))
print("  Output-cap damping zero  : %7.1f kHz" % (1/(2*np.pi*R319*C309)/1e3))
print("  Darlington beta-pole     : %7.2f kHz (beta=2500) ... %7.2f kHz (beta=1000)"
      % (fT/2500/1e3, fT/1000/1e3))
for Il in loads:
    _, Rp = Zout(Il)
    fp = 1/(2*np.pi*Rp*C309)
    print("  Output pole @ %5.0f mA   : %7.2f Hz   (R_out = %.1f ohm)" % (Il*1000, fp, Rp))

# ----------------------------------------------------------------------------
# Plots
# ----------------------------------------------------------------------------
def bode(ax_m, ax_p, L, label):
    ax_m.semilogx(f, 20*np.log10(np.abs(L)), label=label)
    ax_p.semilogx(f, np.unwrap(np.angle(L))*180/np.pi, label=label)

# --- CV ---
fig, (axm, axp) = plt.subplots(2, 1, figsize=(9, 8), sharex=True)
for Il in loads:
    bode(axm, axp, L_cv(Il, 2500), f"{Il*1000:.0f} mA")
axm.axhline(0, color='k', lw=0.8); axp.axhline(-180, color='r', lw=0.8, ls='--')
axm.set_ylabel("|T|  [dB]"); axm.set_title("CV loop gain  (beta=2500 typ, f_T=4 MHz)  vs load")
axm.grid(True, which='both', alpha=.3); axm.legend(title="load"); axm.set_ylim(-60, 120)
axp.set_ylabel("phase [deg]"); axp.set_xlabel("frequency [Hz]")
axp.grid(True, which='both', alpha=.3); axp.set_ylim(-300, -60)
axp.text(2, -185, "-180 deg", color='r', fontsize=8)
fig.tight_layout(); fig.savefig("cv_loop_bode.png", dpi=130); plt.close(fig)

# --- CC ---
fig, (axm, axp) = plt.subplots(2, 1, figsize=(9, 8), sharex=True)
for Il in (0.5, 2.0):
    bode(axm, axp, L_cc(Il, 2500), f"{Il*1000:.0f} mA")
    bode(axm, axp, L_cc(Il, 1000), f"{Il*1000:.0f} mA (beta=1000)")
axm.axhline(0, color='k', lw=0.8); axp.axhline(-180, color='r', lw=0.8, ls='--')
axm.set_ylabel("|T|  [dB]"); axm.set_title("CC loop gain  (R304=0 -> no zero)")
axm.grid(True, which='both', alpha=.3); axm.legend(); axm.set_ylim(-60, 120)
axp.set_ylabel("phase [deg]"); axp.set_xlabel("frequency [Hz]")
axp.grid(True, which='both', alpha=.3); axp.set_ylim(-300, -60)
fig.tight_layout(); fig.savefig("cc_loop_bode.png", dpi=130); plt.close(fig)

print("\nWrote cv_loop_bode.png and cc_loop_bode.png")
