#!/usr/bin/env py
# -*- coding: utf-8 -*-
"""Test concrete compensation fixes against the same small-signal model.
Reports phase margin so the recommendations are verified, not hand-waved."""
import numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt

# baseline params (see loop_stability.py)
R301, R303_0, C303_0 = 10e3, 1e3, 1e-9
R302, R304_0, C304_0 = 10e3, 0.0, 100e-12
R315, R318 = 150.0, 0.100
H_V, A_CS  = 4.3e3/62e3, 1.0 + 18e3/1.6e3
Vout, VA   = 35.0, 75.0
f_H, f_CS  = 1/(2*np.pi*4.3e3*10e-12), 1/(2*np.pi*18e3*10e-12)

f = np.logspace(0, 7, 6000); s = 1j*2*np.pi*f

def pm_of(L):
    mag, ph = np.abs(L), np.unwrap(np.angle(L))*180/np.pi
    idx = np.where(np.diff(np.sign(mag-1))!=0)[0]
    if not len(idx): return None, None
    i = idx[0]
    fc = np.exp(np.interp(0,[np.log(mag[i+1]),np.log(mag[i])],[np.log(f[i+1]),np.log(f[i])]))
    return fc, 180+np.interp(np.log(fc), np.log(f), ph)

def L_cv(Il, beta0=2500, fT=4e6, R303=R303_0, C303=C303_0, Cout=10e-6, R319=0.2):
    A   = (1+s*R303*C303)/(s*R301*C303)
    Gm  = (beta0/(1+s/(2*np.pi*fT/beta0)))/R315
    RL, ro = Vout/Il, VA/Il; Rp = RL*ro/(RL+ro)
    Zc  = R319 + 1/(s*Cout); Z = Rp*Zc/(Rp+Zc)
    return A*Gm*Z*(H_V/(1+s/(2*np.pi*f_H)))

def L_cc(Il, beta0=2500, fT=4e6, R304=R304_0, C304=C304_0):
    A  = (1+s*R304*C304)/(s*R302*C304)
    Gm = (beta0/(1+s/(2*np.pi*fT/beta0)))/R315
    return A*Gm*R318*(A_CS/(1+s/(2*np.pi*f_CS)))

print("CV LOOP fixes (worst case shown; beta=2500, f_T as noted)")
print("-"*72)
cases = [
 ("baseline (TIP125, as-drawn)",        dict()),
 ("A) fast PNP/PMOS pass  f_T=80 MHz",  dict(fT=80e6)),
 ("B) re-comp  C303=10n, R303=10k",     dict(R303=10e3, C303=10e-9)),
 ("C) B + Cout 100uF, R319=0.5",        dict(R303=10e3, C303=10e-9, Cout=100e-6, R319=0.5)),
 ("D) fast PNP f_T=80M + C303=4.7n",    dict(fT=80e6, C303=4.7e-9)),
]
for name, kw in cases:
    out=[]
    for Il in (0.012, 2.0):
        fc, pm = pm_of(L_cv(Il, **kw))
        out.append(f"{Il*1000:.0f}mA: fc={fc/1e3:5.1f}kHz PM={pm:+5.0f}d" if fc else f"{Il*1000:.0f}mA: no-XO")
    print(f"  {name:34s} | " + " | ".join(out))

print("\nCC LOOP fixes (beta=2500)")
print("-"*72)
cc_cases = [
 ("baseline (R304=0, no zero)",      dict()),
 ("add zero R304=100k (z~16kHz)",    dict(R304=100e3)),
 ("slow it: C304=1n + R304=100k",    dict(R304=100e3, C304=1e-9)),
 ("fast pass f_T=80MHz (R304=0)",    dict(fT=80e6)),
]
for name, kw in cc_cases:
    fc, pm = pm_of(L_cc(2.0, **kw))
    print(f"  {name:34s} | fc={fc/1e3:6.1f}kHz  PM={pm:+5.0f} deg" if fc else f"  {name:34s} | no crossover")

# overlay plot: CV baseline vs two best fixes, at full load + light load
fig,(am,ap)=plt.subplots(2,1,figsize=(9,8),sharex=True)
def add(L,lbl,ls='-'):
    am.semilogx(f,20*np.log10(np.abs(L)),ls,label=lbl)
    ap.semilogx(f,np.unwrap(np.angle(L))*180/np.pi,ls,label=lbl)
add(L_cv(2.0),                                   "baseline 2A")
add(L_cv(0.012),                                 "baseline 12mA")
add(L_cv(2.0, fT=80e6, C303=4.7e-9),             "fix D 2A (fast pass)")
add(L_cv(0.012, fT=80e6, C303=4.7e-9),           "fix D 12mA (fast pass)")
am.axhline(0,color='k',lw=.8); ap.axhline(-180,color='r',ls='--',lw=.8)
am.set_ylim(-60,120); ap.set_ylim(-300,-60); am.grid(1,'both',alpha=.3); ap.grid(1,'both',alpha=.3)
am.set_ylabel("|T| [dB]"); ap.set_ylabel("phase [deg]"); ap.set_xlabel("frequency [Hz]")
am.set_title("CV loop: as-drawn vs fast-pass-device fix"); am.legend(fontsize=8)
fig.tight_layout(); fig.savefig("cv_loop_fix.png",dpi=130); plt.close(fig)
print("\nWrote cv_loop_fix.png")
