#!/usr/bin/env py
# -*- coding: utf-8 -*-
r"""
tf_plot.py - type a transfer function, get a Bode plot.

The Laplace variable is  s = j*w  (w in rad/s); the x-axis is frequency in Hz.
Enter H(s) as a normal Python expression in `s`, or use the Hz-based helpers:

    P(f)        pole at f Hz            = 1/(1+s/(2*pi*f))
    Z(f)        zero at f Hz            = 1+s/(2*pi*f)
    I(f0)       integrator, 0 dB @ f0   = 2*pi*f0/s
    Diff(f0)    differentiator          = s/(2*pi*f0)
    P2(f0,Q)    complex pole pair @ f0
    Z2(f0,Q)    complex zero pair  @ f0
    delay(td)   time delay td seconds   = exp(-s*td)
  also available: s, w, j, pi, sqrt, exp, abs, log10, ...

EXAMPLES
    100 * I(16e3) * Z(159e3) * P(909) * P(1.6e3)        # an LDO-type loop gain
    1000 / (1 + s/6.28e3) / (1 + s/6.28e5)             # two real poles (rad/s form)
    50 * P2(1e3, 0.5)                                   # resonant pole pair

USAGE
    py tf_plot.py                       # interactive prompt (plot many, 'q' to quit)
    py tf_plot.py "100*P(1e3)*P(1e5)"   # plot once
    py tf_plot.py "..." 1 1e8           # plot once, fmin=1 Hz, fmax=100 MHz

In the prompt:  type an expression to plot it,  `freq FMIN FMAX` to set the range,
`help` for this text,  `q` to quit.
"""
import sys
import numpy as np
import matplotlib.pyplot as plt


def make_ns(f):
    """Build the evaluation namespace for a given frequency array f [Hz]."""
    w = 2 * np.pi * f
    s = 1j * w
    return {
        "__builtins__": {},
        "s": s, "w": w, "j": 1j, "pi": np.pi, "e": np.e,
        "sqrt": np.sqrt, "exp": np.exp, "log": np.log, "log10": np.log10,
        "sin": np.sin, "cos": np.cos, "tan": np.tan, "abs": np.abs,
        "real": np.real, "imag": np.imag, "conj": np.conj,
        # Hz-based building blocks
        "P":    lambda f0: 1.0 / (1 + s / (2 * np.pi * f0)),
        "Z":    lambda f0: (1 + s / (2 * np.pi * f0)),
        "I":    lambda f0: (2 * np.pi * f0) / s,
        "Diff": lambda f0: s / (2 * np.pi * f0),
        "P2":   lambda f0, Q: 1.0 / (1 + s / (2 * np.pi * f0 * Q) + (s / (2 * np.pi * f0)) ** 2),
        "Z2":   lambda f0, Q: (1 + s / (2 * np.pi * f0 * Q) + (s / (2 * np.pi * f0)) ** 2),
        "delay": lambda td: np.exp(-s * td),
        # longer aliases
        "pole": lambda f0: 1.0 / (1 + s / (2 * np.pi * f0)),
        "zero": lambda f0: (1 + s / (2 * np.pi * f0)),
        "integ": lambda f0: (2 * np.pi * f0) / s,
    }


def margins(f, H):
    """Return (f_xover, PM_deg, f_180, GM_dB) where defined, else None."""
    mag = np.abs(H)
    ph = np.unwrap(np.angle(H)) * 180 / np.pi
    fc = pm = f180 = gm = None
    k = np.where(np.diff(np.sign(mag - 1.0)) != 0)[0]
    if len(k):
        i = k[0]
        fc = np.exp(np.interp(0.0, [np.log(mag[i + 1]), np.log(mag[i])],
                                    [np.log(f[i + 1]), np.log(f[i])]))
        pm = 180.0 + np.interp(np.log(fc), np.log(f), ph)
    k = np.where(np.diff(np.sign(ph + 180.0)) != 0)[0]
    if len(k):
        i = k[0]
        f180 = np.exp(np.interp(-180.0, [ph[i + 1], ph[i]], [np.log(f[i + 1]), np.log(f[i])]))
        gm = -20 * np.log10(np.exp(np.interp(np.log(f180), np.log(f), np.log(mag))))
    return fc, pm, f180, gm


def plot_tf(expr, fmin, fmax, save=None):
    f = np.logspace(np.log10(fmin), np.log10(fmax), 4000)
    try:
        H = eval(expr, make_ns(f))                      # noqa: S307 (local tool, restricted ns)
    except Exception as e:                              # noqa: BLE001
        print(f"  ! could not evaluate: {e}")
        return
    H = np.asarray(H, dtype=complex) * np.ones_like(f)  # broadcast constants
    magdb = 20 * np.log10(np.abs(H))
    phdeg = np.unwrap(np.angle(H)) * 180 / np.pi
    fc, pm, f180, gm = margins(f, H)

    print(f"  H(s) = {expr}")
    if fc is not None:
        print(f"  gain crossover : {fc:10.3g} Hz   phase margin : {pm:+.1f} deg")
    else:
        print("  gain crossover : none (|H| does not cross 0 dB in range)")
    if gm is not None:
        print(f"  phase xover    : {f180:10.3g} Hz   gain margin  : {gm:+.1f} dB")

    fig, (am, ap) = plt.subplots(2, 1, figsize=(9, 7), sharex=True)
    am.semilogx(f, magdb)
    am.axhline(0, color="k", lw=0.8)
    am.set_ylabel("magnitude [dB]"); am.grid(True, which="both", alpha=0.3)
    am.set_title(f"H(s) = {expr}", fontsize=9)
    ap.semilogx(f, phdeg)
    ap.axhline(-180, color="r", ls="--", lw=0.8)
    ap.set_ylabel("phase [deg]"); ap.set_xlabel("frequency [Hz]")
    ap.grid(True, which="both", alpha=0.3)
    if fc is not None:
        am.axvline(fc, color="g", ls=":", lw=0.9)
        ap.axvline(fc, color="g", ls=":", lw=0.9)
        am.plot([fc], [0], "go")
        ap.annotate(f"PM = {pm:+.0f} deg", xy=(fc, np.interp(np.log(fc), np.log(f), phdeg)),
                    fontsize=8, color="g")
    fig.tight_layout()
    if save:
        fig.savefig(save, dpi=130)
        print(f"  saved {save}")
    plt.show()
    plt.close(fig)


def repl():
    fmin, fmax = 0.1, 1e7
    print(__doc__)
    print(f"[frequency range {fmin:g} .. {fmax:g} Hz]\n")
    while True:
        try:
            line = input("TF> ").strip()
        except (EOFError, KeyboardInterrupt):
            print(); break
        if not line:
            continue
        if line in ("q", "quit", "exit"):
            break
        if line in ("help", "?", "h"):
            print(__doc__); continue
        if line.startswith("freq"):
            try:
                _, a, b = line.split()
                fmin, fmax = float(a), float(b)
                print(f"  range set to {fmin:g} .. {fmax:g} Hz")
            except ValueError:
                print("  usage: freq FMIN FMAX   (Hz)")
            continue
        plot_tf(line, fmin, fmax)


if __name__ == "__main__":
    if len(sys.argv) >= 2:
        expr = sys.argv[1]
        fmin = float(sys.argv[2]) if len(sys.argv) >= 3 else 0.1
        fmax = float(sys.argv[3]) if len(sys.argv) >= 4 else 1e7
        plot_tf(expr, fmin, fmax, save="tf_plot.png")
    else:
        repl()
