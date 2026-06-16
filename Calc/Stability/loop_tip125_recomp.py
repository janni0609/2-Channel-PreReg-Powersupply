#!/usr/bin/env py
# -*- coding: utf-8 -*-
"""
TIP125 CV-loop re-compensation, with the CORRECTED beta-pole understanding.

Figure 7 of the onsemi TIP120/125 datasheet (small-signal current gain) is a
TYPICAL curve: gain is flat at ~2000-3000 out to ~15-25 kHz, then rolls off.
So the TYPICAL beta-pole is ~15-25 kHz (f_T ~ 30-60 MHz), NOT 1.5 kHz.

The datasheet only GUARANTEES h_fe >= 4 @ 1 MHz  => f_T >= 4 MHz (worst case).
With a high small-signal beta0 (~2500) that worst-case unit has its beta-pole
as low as f_beta = f_T/beta0 = 4 MHz / 2500 = 1.6 kHz.

=> beta-pole spans ~1.6 kHz (datasheet floor) ... ~20 kHz (typical).

(a) This file sweeps that typ-vs-worst beta-pole range.
(b) It verifies a re-compensation that keeps PM >= 45 deg even at the 1.6 kHz floor.

Forward path (reverse-engineered earlier): v_err -> Q301 (Gm = beta/R315) -> Q303
common-emitter PNP -> high-Z output node (C309 10uF + R319 0.2ohm damping).
CV error amp = inverting integrator: input R301, feedback R303 in series with C303.
"""
import numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt

# --- fixed plant / feedback (from schematic) ---
R315=150.0; H_V=4.3e3/62e3; Vout,VA=35.0,75.0
C309=10e-6; R319=0.2
f_H=1/(2*np.pi*4.3e3*10e-12)         # Vsense-amp pole ~3.7 MHz
beta0=2500                            # small-signal DC current gain

# --- compensation options (R301 input, R303 zero-R, C303 integrator cap) ---
AS_DRAWN  = dict(R301=10e3,  R303=1e3,  C303=1e-9)    # present
RECOMP_C  = dict(R301=100e3, R303=10e3, C303=220e-9)  # robust: >=45 deg at f_T floor
RECOMP_B  = dict(R301=47e3,  R303=22e3, C303=100e-9)  # faster alt: >=45 deg at f_beta>=5kHz

f=np.logspace(-1,7.5,16000); s=1j*2*np.pi*f

def L(comp, Il, fbeta):
    R301,R303,C303 = comp['R301'],comp['R303'],comp['C303']
    A  = (1+s*R303*C303)/(s*R301*C303)
    Gm = (beta0/(1+s/(2*np.pi*fbeta)))/R315
    RL,ro = Vout/Il, VA/Il; Rp = RL*ro/(RL+ro)
    Zc = R319 + 1/(s*C309); Z = Rp*Zc/(Rp+Zc)
    return A*Gm*Z*(H_V/(1+s/(2*np.pi*f_H)))

def fc_pm(Lv):
    m,p=np.abs(Lv),np.unwrap(np.angle(Lv))*180/np.pi
    i=np.where(np.diff(np.sign(m-1))!=0)[0]
    if not len(i): return None,None
    i=i[0]; fc=np.exp(np.interp(0,[np.log(m[i+1]),np.log(m[i])],[np.log(f[i+1]),np.log(f[i])]))
    return fc,180+np.interp(np.log(fc),np.log(f),p)

def worst(comp,fbeta):
    out=[fc_pm(L(comp,Il,fbeta)) for Il in (0.012,0.1,0.5,2.0)]
    pms=[pm for fc,pm in out if pm is not None]
    return (min(pms) if pms else None)

print("="*74)
print("CV loop phase margin (worst over load 12mA..2A), beta0=2500")
print("="*74)
print("%-32s %12s %12s %12s"%("compensation (R301/R303/C303)","beta=20k(typ)","beta=5k","beta=1.6k(floor)"))
print("-"*74)
for name,comp in [("as-drawn  10k / 1k / 1n",AS_DRAWN),
                  ("RECOMP-B  47k / 22k / 100n",RECOMP_B),
                  ("RECOMP-C  100k / 10k / 220n",RECOMP_C)]:
    row=[worst(comp,fb) for fb in (20e3,5e3,1.6e3)]
    print("%-32s %10s   %10s   %10s"%(name,
        *[f"{r:+.0f} deg" if r is not None else "unstbl" for r in row]))
for name,comp in [("RECOMP-C",RECOMP_C),("RECOMP-B",RECOMP_B)]:
    fz=1/(2*np.pi*comp['R303']*comp['C303']); fi=1/(2*np.pi*comp['R301']*comp['C303'])
    fc,_=fc_pm(L(comp,0.012,1.6e3))
    print(f"   {name}: comp zero {fz:.0f} Hz, integrator unity {fi:.1f} Hz, crossover ~{fc:.0f} Hz")

# ---------- (a) as-drawn: typ vs worst beta-pole ----------
fig,(am,ap)=plt.subplots(2,1,figsize=(9,8),sharex=True)
for fb,lab in [(1.6e3,'beta-pole 1.6kHz (f_T=4MHz floor)'),(5e3,'5kHz'),(20e3,'20kHz (typical)')]:
    Lv=L(AS_DRAWN,2.0,fb)
    am.semilogx(f,20*np.log10(np.abs(Lv)),label=lab)
    ap.semilogx(f,np.unwrap(np.angle(Lv))*180/np.pi,label=lab)
am.axhline(0,color='k',lw=.8); ap.axhline(-180,color='r',ls='--',lw=.8)
am.set_ylim(-60,120); ap.set_ylim(-300,-60); am.grid(1,'both',alpha=.3); ap.grid(1,'both',alpha=.3)
am.set_ylabel("|T| [dB]"); ap.set_ylabel("phase [deg]"); ap.set_xlabel("frequency [Hz]")
am.set_title("(a) AS-DRAWN CV loop @2A: typical vs worst-case beta-pole"); am.legend(fontsize=8)
fig.tight_layout(); fig.savefig("cv_betapole_sweep.png",dpi=130); plt.close(fig)

# ---------- (b) RECOMP-C: stable across beta-pole and load ----------
fig,(am,ap)=plt.subplots(2,1,figsize=(9,8),sharex=True)
for Il in (0.012,2.0):
    for fb,ls in [(1.6e3,'-'),(20e3,'--')]:
        Lv=L(RECOMP_C,Il,fb)
        lab=f"{Il*1000:.0f}mA, beta-pole {fb/1e3:g}kHz"
        am.semilogx(f,20*np.log10(np.abs(Lv)),ls,label=lab)
        ap.semilogx(f,np.unwrap(np.angle(Lv))*180/np.pi,ls,label=lab)
am.axhline(0,color='k',lw=.8); ap.axhline(-180,color='r',ls='--',lw=.8)
am.set_ylim(-60,120); ap.set_ylim(-300,-60); am.grid(1,'both',alpha=.3); ap.grid(1,'both',alpha=.3)
am.set_ylabel("|T| [dB]"); ap.set_ylabel("phase [deg]"); ap.set_xlabel("frequency [Hz]")
am.set_title("(b) RE-COMP C (100k/10k/220n): stable across beta-pole & load"); am.legend(fontsize=8)
fig.tight_layout(); fig.savefig("cv_recomp_tip125.png",dpi=130); plt.close(fig)
print("\nWrote cv_betapole_sweep.png and cv_recomp_tip125.png")
