#!/usr/bin/env py
# -*- coding: utf-8 -*-
"""
(1) CC-loop small-signal verification with the corrected comp (R304=2k2, C304=220n)
    across beta-pole corners.  R304=100k (proposed) is UNSTABLE at the typical
    beta-pole because crossover runs up into the current-sense-amp poles.
(2) Large-signal CV<->CC handover transient (Bode cannot capture this), showing the
    integrator-windup overshoot and that simple tracking anti-windup fixes it.
"""
import numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt

# ---------------- (1) CC loop Bode ----------------
R302=10e3; R315=150.0; R318=0.1; A_CS=1+18e3/1.6e3
f_cs1=5.7e6/A_CS; f_cs2=1/(2*np.pi*18e3*10e-12)
f=np.logspace(0,7.8,16000); s=1j*2*np.pi*f
def Lcc(R304,C304,fbeta,beta0=2500):
    A=(1+s*R304*C304)/(s*R302*C304)
    Gm=(beta0/(1+s/(2*np.pi*fbeta)))/R315
    Acs=A_CS/((1+s/(2*np.pi*f_cs1))*(1+s/(2*np.pi*f_cs2)))
    return A*Gm*R318*Acs
def fcpm(L):
    m,p=np.abs(L),np.unwrap(np.angle(L))*180/np.pi
    i=np.where(np.diff(np.sign(m-1))!=0)[0]
    if not len(i): return None,None
    i=i[0]; fc=np.exp(np.interp(0,[np.log(m[i+1]),np.log(m[i])],[np.log(f[i+1]),np.log(f[i])]))
    return fc,180+np.interp(np.log(fc),np.log(f),p)

print("CC loop, comp R304=2.2k / C304=220n (vs proposed 100k/100p):")
for tag,R304,C304 in [("CHOSEN 2k2/220n",2.2e3,220e-9),("proposed 100k/100p",100e3,100e-12)]:
    for fb in (1.6e3,20e3):
        fc,pm=fcpm(Lcc(R304,C304,fb))
        print(f"  {tag:18s} beta-pole {fb/1e3:4.1f}kHz: fc={fc:7.0f}Hz  PM={pm:+.0f} deg")

fig,(am,ap)=plt.subplots(2,1,figsize=(9,7),sharex=True)
for fb,ls in [(1.6e3,'-'),(20e3,'--')]:
    L=Lcc(2.2e3,220e-9,fb)
    am.semilogx(f,20*np.log10(np.abs(L)),ls,label=f'2k2/220n, beta-pole {fb/1e3:g}kHz')
    ap.semilogx(f,np.unwrap(np.angle(L))*180/np.pi,ls)
L=Lcc(100e3,100e-12,20e3)
am.semilogx(f,20*np.log10(np.abs(L)),'r:',label='100k/100p, beta-pole 20kHz (UNSTABLE)')
ap.semilogx(f,np.unwrap(np.angle(L))*180/np.pi,'r:')
am.axhline(0,color='k',lw=.8); ap.axhline(-180,color='r',ls='--',lw=.8)
am.set_ylim(-40,100); ap.set_ylim(-300,-60); am.grid(1,'both',alpha=.3); ap.grid(1,'both',alpha=.3)
am.set_ylabel("|T| [dB]"); ap.set_ylabel("phase [deg]"); ap.set_xlabel("frequency [Hz]")
am.set_title("CC loop: R304=2k2/C304=220n robust vs R304=100k/100p unstable@typ"); am.legend(fontsize=8)
fig.tight_layout(); fig.savefig("cc_recomp.png",dpi=130); plt.close(fig)

# ---------------- (2) Handover transient ----------------
# Behavioural dual-loop limiter: u = commanded pass current (A), higher u = more current.
# Pass current with beta-pole lag; output node = C309; CV PI and CC PI; auction = min(u_cv,u_cc).
Vref, Iref, C, umax = 35.0, 2.0, 10e-6, 4.0
taub = 1/(2*np.pi*20e3)          # typical beta-pole lag (~8 us)
H = 4.3e3/62e3
# PI gains from the re-comp values:  Kp=Rz/Rin,  Ki=1/(Rin*C)
Kp_cv,Ki_cv = 10e3/100e3, 1/(100e3*220e-9)     # CV: R301=100k,R303=10k,C303=220n
Kp_cc,Ki_cc = 2.2e3/10e3, 1/(10e3*220e-9)      # CC: R302=10k,R304=2k2,C304=220n

Vrail=36.7         # pass element cannot push output above the PreReg rail (Vout+1.7)
margin=0.05        # anti-windup keeps the idle integrator this far above the control point
def run(anti_windup):
    dt=1e-6; T=18e-3; n=int(T/dt)
    t=np.arange(n)*dt
    i=np.zeros(n); v=np.zeros(n)
    icv=0.01; icc=0.06          # integrator states (Ki*integral); init near light-load steady state
    ip=0.01; vo=35.0
    for k in range(n):
        Rload = 3500.0 if (t[k]<5e-3 or t[k]>11e-3) else 9.0   # light -> overload -> light
        eV = Vref - vo
        eI = Iref - ip
        u_cv = Kp_cv*eV + icv
        u_cc = Kp_cc*eI + icc
        u = max(0.0, min(umax, min(u_cv,u_cc)))   # auction: lowest wins, clipped to rail
        sel_cv = (u_cv <= u_cc)
        # integrate the controlling loop normally
        if sel_cv: icv += Ki_cv*eV*dt
        else:      icc += Ki_cc*eI*dt
        if not sel_cv:                              # CV is idle
            if anti_windup: icv = min(icv, (u+margin)-Kp_cv*eV)   # clamp idle integrator (tracking AW)
            else:           icv += Ki_cv*eV*dt                    # idle loop keeps winding -> rails
        if sel_cv:                                  # CC is idle
            if anti_windup: icc = min(icc, (u+margin)-Kp_cc*eI)
            else:           icc += Ki_cc*eI*dt
        # rail clamp on integrator outputs (op-amp saturation)
        icv=min(icv,umax-Kp_cv*eV); icv=max(icv,-Kp_cv*eV)
        icc=min(icc,umax-Kp_cc*eI); icc=max(icc,-Kp_cc*eI)
        # plant: pass current lags command (beta pole); C309 integrates; output clamped to rail
        ip += (u-ip)/taub*dt;  ip=max(0.0,ip)
        vo += (ip - vo/Rload)/C*dt;  vo=min(Vrail,max(0.0,vo))
        i[k]=ip; v[k]=vo
    return t,i,v

fig,(a1,a2)=plt.subplots(2,1,figsize=(9,7),sharex=True)
for aw,c,lab in [(False,'C0','no anti-windup'),(True,'C1','with tracking anti-windup')]:
    t,i,v=run(aw)
    a1.plot(t*1e3,i,c,label=lab); a2.plot(t*1e3,v,c,label=lab)
a1.axhline(Iref,color='k',ls=':',lw=.8); a1.set_ylabel("pass current [A]")
a1.set_title("CV->CC->CV handover (load step 10mA<->~2A limit at 5 & 11 ms)")
a1.legend(fontsize=8); a1.grid(alpha=.3); a1.set_ylim(0,4.2)
a2.axhline(Vref,color='k',ls=':',lw=.8); a2.set_ylabel("V_out [V]"); a2.set_xlabel("time [ms]")
a2.legend(fontsize=8); a2.grid(alpha=.3)
fig.tight_layout(); fig.savefig("handover.png",dpi=130); plt.close(fig)
print("\nWrote cc_recomp.png and handover.png")
