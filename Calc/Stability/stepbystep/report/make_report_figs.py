# Generate SVG figures for the Typst loop-analysis report (final component values)
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from scipy import signal
import os

OUT = r"c:\Users\janni\GitSSD\2-Channel-PreReg-Powersupply\Calc\Stability\stepbystep\report"
os.makedirs(OUT, exist_ok=True)
plt.rcParams.update({'font.size': 8.5, 'svg.fonttype': 'path', 'axes.grid': True,
                     'grid.alpha': 0.3, 'lines.linewidth': 1.3, 'figure.dpi': 110})

f = np.logspace(0, 7, 6000); s = 1j*2*np.pi*f
GBW = 5.7e6
fb1, fb2 = 60e3, 300e3

# ---------------- shared helpers ----------------
def beta_tf(b0, f1=fb1, f2=fb2):
    return b0/((1 + s/(2*np.pi*f1))*(1 + s/(2*np.pi*f2)))

def margins(T):
    m = 20*np.log10(np.abs(T)); p = np.unwrap(np.angle(T))*180/np.pi
    idx = np.where(np.diff(np.sign(m)) != 0)[0]
    fc = 10**np.interp(0.0, [m[idx[0]+1], m[idx[0]]], [np.log10(f[idx[0]+1]), np.log10(f[idx[0]])])
    pm = 180 + np.interp(np.log10(fc), np.log10(f), p)
    return fc, pm, m, p

def pmul(a,b): return np.polymul(a,b)
def padd(a,b): return np.polyadd(a,b)
def par(n1,d1,n2,d2): return pmul(n1,n2), padd(pmul(n1,d2), pmul(n2,d1))
def stepresp(num, den, tauf=0.0, tmax=None, npt=20000):
    if tauf > 0: den = pmul(den, [tauf, 1.0])
    nd = len(den)-1; w0 = abs(den[-1]/den[0])**(1.0/nd)
    sc = lambda c: np.asarray(c, float)*(w0**np.arange(len(c)-1, -1, -1))
    nS, dS = sc(num), sc(den); ps = np.roots(dS)*w0
    if tmax is None: tmax = 10.0/np.min(np.abs(ps.real))
    t = np.linspace(0, tmax, npt)
    _, y = signal.step(signal.StateSpace(*signal.tf2ss(nS, dS)), T=w0*t)
    return t, y

# ================= 0) block diagram =================
fig, ax = plt.subplots(figsize=(6.6, 1.9)); ax.axis('off'); ax.grid(False)
boxes = [(0.06,'error amp +\ncompensator\nU1 / U3'), (0.31,'driver + pass\nQ1,Q2 / Q3,Q4\nTIP125'),
         (0.56,'output network\n$Z_{out}$ / shunt $R_{21}$'), (0.81,'sense amp\nU2 (1/15) /\nU4 (x12)')]
for x, txt in boxes:
    ax.add_patch(plt.Rectangle((x, 0.35), 0.16, 0.5, fc='#eef3fa', ec='k', lw=1))
    ax.text(x+0.08, 0.60, txt, ha='center', va='center', fontsize=7.5)
for x0, x1 in [(0.22,0.31),(0.47,0.56),(0.72,0.81)]:
    ax.annotate('', xy=(x1,0.60), xytext=(x0,0.60), arrowprops=dict(arrowstyle='->', lw=1.2))
ax.annotate('', xy=(0.06,0.60), xytext=(0.0,0.60), arrowprops=dict(arrowstyle='->', lw=1.2))
ax.text(0.028, 0.75, '$V_{set}$ (+)', fontsize=8, ha='center')
ax.plot([0.89,0.89,0.03,0.03],[0.35,0.10,0.10,0.35],'k',lw=1.2)
ax.annotate('', xy=(0.03,0.35), xytext=(0.03,0.20), arrowprops=dict(arrowstyle='->', lw=1.2))
ax.text(0.46, 0.03, 'feedback  $V_{fb}$  (into $-$ input via $R_1$ / $R_8$)', ha='center', fontsize=8)
ax.text(0.235, 0.68, '$H_1$', fontsize=9, color='C0'); ax.text(0.485, 0.68, '$H_2$', fontsize=9, color='C1')
ax.text(0.735, 0.68, '$H_3$', fontsize=9, color='C2'); ax.text(0.975, 0.68, '$H_4$', fontsize=9, color='C3')
ax.set_xlim(0,1.02); ax.set_ylim(0,1)
fig.savefig(OUT+r'\fig_blockdiag.svg', bbox_inches='tight'); plt.close(fig)

# ================= 1) TIP125 beta fit =================
fig, ax = plt.subplots(figsize=(4.6, 2.6))
ax.loglog(f, np.abs(beta_tf(2000)), 'C0', label=r'2-pole fit: $\beta_0$=2000, 60 kHz, 300 kHz')
pts_f = [1e3, 10e3, 30e3, 100e3, 200e3, 500e3, 1e6]
pts_b = [2000, 2000, 1800, 1000, 520, 120, 33]
ax.loglog(pts_f, pts_b, 'ks', ms=4, mfc='w', label='datasheet Fig. 7 (PNP, 3 A) read-off')
ax.set_xlim(1e3, 3e6); ax.set_ylim(10, 4000)
ax.set_xlabel('frequency [Hz]'); ax.set_ylabel(r'$|h_{fe}|$')
ax.legend(fontsize=7.5, loc='lower left')
fig.tight_layout(); fig.savefig(OUT+r'\fig_tip125.svg', bbox_inches='tight'); plt.close(fig)

# ================= V loop model (final values) =================
R1, R2, C1, C3 = 10e3, 18e3, 47e-9, 22e-12
R3, R4, rd = 1e3, 150.0, 26.0
R5, C2 = 0.2, 10e-6
R7, R9, R10, R11 = 2.2e3, 2.2e3, 33e3, 33e3
H4dc = (1+R9/R11)*(R7/(R7+R10))
A_op = 2*np.pi*GBW/s
Zf_v = 1.0/(1.0/(R2 + 1.0/(s*C1)) + s*C3)
H1_v = (Zf_v/R1)/(1.0 + (1.0+Zf_v/R1)/A_op)
H4_v = H4dc/(1.0 + s/(2*np.pi*GBW/(1+R9/R11)))
gm1 = 1.0/R4; dio = R3/(R3+rd)

def Zout_v(Rl, Cl, El, R5c=R5):
    Y = 1.0/Rl
    if Cl > 0: Y = Y + 1.0/(El + 1.0/(s*Cl))
    return 1.0/(1.0/(R5c + 1.0/(s*C2)) + Y)

loads = {
    'no load (100k)': (1e5, 0.0, 0.0), 'R 1k': (1e3, 0.0, 0.0),
    'R 100R (nom.)': (100.0, 0.0, 0.0), 'R 10R': (10.0, 0.0, 0.0),
    '100R+1uF/1R': (100.0, 1e-6, 1.0), '1k+4u7/5m': (1e3, 4.7e-6, 0.005),
    '10k+10uF/10m': (1e4, 10e-6, 0.01), '10R+100uF/50m': (10.0, 100e-6, 0.05),
    '10R+330uF/30m': (10.0, 330e-6, 0.03), '100R+470uF/0.5R': (100.0, 470e-6, 0.5),
    '10R+1000uF/20m': (10.0, 1000e-6, 0.02)}

# --- V loop shaping fig (nominal load, beta=2000) ---
H2_v = gm1*dio*beta_tf(2000)
H3_v = Zout_v(100.0, 1e-6, 1.0)
T_v = H1_v*H2_v*H3_v*H4_v
Hrest = H2_v*H3_v*H4_v
fc, pm, mT, pT = margins(T_v)
fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 4.6), sharex=True)
a1.semilogx(f, 20*np.log10(np.abs(H1_v)), 'C0', label='compensator $H_1$')
a1.semilogx(f, 20*np.log10(np.abs(Hrest)), 'C1', label='rest $H_2 H_3 H_4$')
a1.semilogx(f, mT, 'C3', lw=1.9, label='loop $T$')
a1.axhline(0, color='k', lw=0.7); a1.set_ylabel('magnitude [dB]'); a1.set_ylim(-50, 100)
a1.legend(fontsize=8); a1.axvline(fc, color='0.4', ls=':')
a1.annotate(f'$f_c$ = {fc/1e3:.1f} kHz', (fc, 2), xytext=(5, 5), textcoords='offset points', fontsize=8)
a2.semilogx(f, np.angle(H1_v, deg=True), 'C0')
a2.semilogx(f, np.unwrap(np.angle(Hrest))*180/np.pi, 'C1')
a2.semilogx(f, pT, 'C3', lw=1.9)
a2.axhline(-180, color='k', lw=0.7, ls='--'); a2.axvline(fc, color='0.4', ls=':')
a2.annotate(f'PM = {pm:.0f}°', (fc, pm-180), xytext=(6, 8), textcoords='offset points', fontsize=8, color='C3')
a2.set_ylabel('phase [deg]'); a2.set_xlabel('frequency [Hz]'); a2.set_ylim(-220, 20)
fig.tight_layout(); fig.savefig(OUT+r'\fig_v_loopshape.svg', bbox_inches='tight'); plt.close(fig)

# --- V loop load sweep fig ---
fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 4.8), sharex=True)
for name, (Rl, Cl, El) in loads.items():
    T = H1_v*H2_v*Zout_v(Rl, Cl, El)*H4_v
    ls = '-' if Cl > 0 else '--'
    a1.semilogx(f, 20*np.log10(np.abs(T)), ls, lw=1.1, label=name)
    a2.semilogx(f, np.unwrap(np.angle(T))*180/np.pi, ls, lw=1.1)
a1.axhline(0, color='k', lw=0.7); a1.set_ylabel('|T| [dB]'); a1.set_ylim(-40, 80)
a1.legend(fontsize=6.5, ncol=2)
a2.axhline(-180, color='k', lw=0.7, ls='--'); a2.set_ylabel('phase [deg]')
a2.set_xlabel('frequency [Hz]'); a2.set_ylim(-270, 0)
fig.tight_layout(); fig.savefig(OUT+r'\fig_v_loadsweep.svg', bbox_inches='tight'); plt.close(fig)

# --- V loop step (with / without prefilter) ---
def vloop_poly(b0=2000.0, Rl=100.0, Cl=1e-6, El=1.0):
    Gm = b0/R4*dio
    nZf, dZf = par([R2*C1, 1.0], [C1, 0.0], [1.0], [C3, 0.0])
    nA, dA = nZf, pmul(dZf, [R1])
    if Cl > 0: n_load, d_load = par([Rl], [1.0], [El*Cl, 1.0], [Cl, 0.0])
    else: n_load, d_load = [Rl], [1.0]
    n_Zo, d_Zo = par([R5*C2, 1.0], [C2, 0.0], n_load, d_load)
    d_b = pmul([1/(2*np.pi*fb1), 1], [1/(2*np.pi*fb2), 1])
    nP, dP = pmul([Gm], n_Zo), pmul(d_Zo, d_b)
    num = pmul(nP, padd(dA, nA)); den = padd(pmul(dA, dP), pmul([H4dc], pmul(nA, nP)))
    return num, den
num, den = vloop_poly()
t0, y0 = stepresp(num, den, 0.0, tmax=6e-3)
t1, y1 = stepresp(num, den, (R1+R2)*C1, tmax=6e-3)
fig, ax = plt.subplots(figsize=(6.2, 2.7))
ax.plot(t0*1e3, y0, 'C3', label='no prefilter:  54 % overshoot')
ax.plot(t1*1e3, y1, 'C0', lw=1.8, label=r'RC prefilter $\tau=(R_1{+}R_2)C_1$ = 1.32 ms:  ~0 %')
ax.axhline(15, color='0.5', ls='--', lw=0.8)
ax.set_xlabel('time [ms]'); ax.set_ylabel('$V_{out}$ [V]  (1 V setpoint step)')
ax.legend(fontsize=8); ax.set_ylim(0, 25)
fig.tight_layout(); fig.savefig(OUT+r'\fig_v_step.svg', bbox_inches='tight'); plt.close(fig)

# ================= I loop model (final values) =================
R8, C4 = 10e3, 27e-9
R13, R15 = 1e3, 150.0
R19, C6, R21 = 0.2, 10e-6, 0.2
R16, R17 = 11e3, 1e3
H4g = 1 + R16/R17
Zf_i = 1.0/(s*C4)
H1_i = (Zf_i/R8)/(1.0 + (1.0+Zf_i/R8)/A_op)
H4_i = H4g/(1.0 + s/(2*np.pi*GBW/H4g))
gm3 = 1.0/R15; dio2 = R13/(R13+rd)

# --- I loop shaping fig ---
T_i = H1_i*(gm3*dio2*beta_tf(2000))*R21*H4_i
Hrest_i = (gm3*dio2*beta_tf(2000))*R21*H4_i
fc_i, pm_i, mTi, pTi = margins(T_i)
fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 4.6), sharex=True)
a1.semilogx(f, 20*np.log10(np.abs(H1_i)), 'C0', label='compensator $H_1$ (pure integrator)')
a1.semilogx(f, 20*np.log10(np.abs(Hrest_i)), 'C1', label='rest $H_2 R_{21} H_4$  (flat + poles)')
a1.semilogx(f, mTi, 'C3', lw=1.9, label='loop $T$')
a1.axhline(0, color='k', lw=0.7); a1.set_ylabel('magnitude [dB]'); a1.set_ylim(-50, 100)
a1.legend(fontsize=8); a1.axvline(fc_i, color='0.4', ls=':')
a1.annotate(f'$f_c$ = {fc_i/1e3:.1f} kHz', (fc_i, 2), xytext=(5, 5), textcoords='offset points', fontsize=8)
a2.semilogx(f, np.angle(H1_i, deg=True), 'C0')
a2.semilogx(f, np.unwrap(np.angle(Hrest_i))*180/np.pi, 'C1')
a2.semilogx(f, pTi, 'C3', lw=1.9)
a2.axhline(-180, color='k', lw=0.7, ls='--'); a2.axvline(fc_i, color='0.4', ls=':')
a2.annotate(f'PM = {pm_i:.0f}°', (fc_i, pm_i-180), xytext=(6, 8), textcoords='offset points', fontsize=8, color='C3')
a2.set_ylabel('phase [deg]'); a2.set_xlabel('frequency [Hz]'); a2.set_ylim(-280, 20)
fig.tight_layout(); fig.savefig(OUT+r'\fig_i_loopshape.svg', bbox_inches='tight'); plt.close(fig)

# --- I loop beta corners fig ---
corners = [('β=500', 500, fb1, fb2), ('β=1000', 1000, fb1, fb2), ('β=2000 (typ)', 2000, fb1, fb2),
           ('β=4000', 4000, fb1, fb2), ('β=1000 low-bias', 1000, 20e3, 100e3),
           ('β=2000 low-bias', 2000, 20e3, 100e3)]
fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.4, 4.8), sharex=True)
for lbl, b0, f1c, f2c in corners:
    T = H1_i*(gm3*dio2*beta_tf(b0, f1c, f2c))*R21*H4_i
    ls = '--' if 'low' in lbl else '-'
    a1.semilogx(f, 20*np.log10(np.abs(T)), ls, lw=1.2, label=lbl)
    a2.semilogx(f, np.unwrap(np.angle(T))*180/np.pi, ls, lw=1.2)
a1.axhline(0, color='k', lw=0.7); a1.set_ylabel('|T| [dB]'); a1.set_ylim(-40, 100)
a1.legend(fontsize=7.5, ncol=2)
a2.axhline(-180, color='k', lw=0.7, ls='--'); a2.set_ylabel('phase [deg]')
a2.set_xlabel('frequency [Hz]'); a2.set_ylim(-280, -60)
fig.tight_layout(); fig.savefig(OUT+r'\fig_i_corners.svg', bbox_inches='tight'); plt.close(fig)

# --- I loop step: spike vs matched prefilter ---
def iloop_poly(b0=2000.0):
    Gm = b0/R15*dio2
    nA, dA = [1.0], [R8*C4, 0.0]
    d_b = pmul([1/(2*np.pi*fb1), 1], [1/(2*np.pi*fb2), 1])
    d_u4 = [H4g/(2*np.pi*GBW), 1.0]
    num = pmul([Gm], pmul(padd(dA, nA), d_u4))
    den = padd(pmul(dA, pmul(d_b, d_u4)), pmul([Gm*R21*H4g], nA))
    return num, den
num_i, den_i = iloop_poly()
ta, ya = stepresp(num_i, den_i, 0.0, tmax=120e-6)
tb, yb = stepresp(num_i, den_i, R8*C4, tmax=120e-6)
fig, (a1, a2) = plt.subplots(1, 2, figsize=(6.6, 2.6))
a1.plot(ta*1e6, ya, 'C3'); a1.axhline(0.4167, color='0.5', ls='--', lw=0.8)
a1.set_title('no prefilter — feed-forward spike', fontsize=8.5)
a1.set_xlabel('time [µs]'); a1.set_ylabel('$I_{out}$ [A]  (1 V step)')
a2.plot(tb*1e6, yb, 'C0'); a2.axhline(0.4167, color='0.5', ls='--', lw=0.8)
a2.set_title(r'RC prefilter $\tau = R_8 C_4$ = 270 µs', fontsize=8.5)
a2.set_xlabel('time [µs]'); a2.set_ylim(0, 0.5)
fig.tight_layout(); fig.savefig(OUT+r'\fig_i_step.svg', bbox_inches='tight'); plt.close(fig)

print('figures written to', OUT)
for fn in sorted(os.listdir(OUT)):
    print('  ', fn)
