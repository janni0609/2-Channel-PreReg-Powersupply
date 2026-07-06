// Control-loop analysis report — V loop & I loop of the study schematic
// (stepbystep/simple loops.pdf, pages 2 and 3). Figures generated from the
// same models as V_loop_transfer_functions.ipynb / I_loop_transfer_functions.ipynb.

#set page(paper: "a4", margin: (x: 2.2cm, y: 2.2cm), numbering: "1 / 1")
#set text(size: 10pt, lang: "en")
#set heading(numbering: "1.1")
#set math.equation(numbering: none)
#show link: set text(fill: blue.darken(30%))
#show figure.caption: set text(size: 8.5pt)
#show table: set text(size: 9pt)

#align(center)[
  #text(size: 17pt, weight: "bold")[Stability Analysis of the CV and CC Control Loops]\
  #v(2pt)
  #text(size: 11pt)[2-Channel Pre-Regulated Power Supply — study loops
  (#raw("simple loops.kicad_sch"), sheets V_loop and I_loop)]\
  #v(2pt)
  #text(size: 9pt, fill: gray.darken(40%))[Companion document to
  #raw("V_loop_transfer_functions.ipynb") and #raw("I_loop_transfer_functions.ipynb") — 2026-07-06]
]
#v(6pt)

= Overview and method

Both regulator loops share the same four-block structure (@blockdiag): an op-amp *error
amplifier* with a frequency-shaping feedback network (the compensator), a *driver + pass
stage* (small NPN driving a TIP125 PNP Darlington), an *output network*, and a *sense
amplifier* that scales the regulated quantity back to the error amp.

#figure(image("fig_blockdiag.svg", width: 92%),
  caption: [Common loop structure. The voltage loop senses $V_"out"$ (U2, gain 1/15); the
  current loop senses the shunt voltage $R_21 I$ (U4, gain 12).]) <blockdiag>

Each block is described by its small-signal transfer function $H_1 dots H_4$; the *loop gain* is

$ T(s) = H_1 (s) thin H_2 (s) thin H_3 (s) thin H_4 (s). $

The error amp is inverting for the fed-back signal, which supplies the 180° that makes the
feedback negative; stability is then read from the frequency shaping of $T$:

- *crossover* $f_c$: the frequency where $|T| = 1$ (0 dB);
- *phase margin* PM $= 180° + angle T(j 2 pi f_c)$ — distance from oscillation;
- *gain margin* GM $= -|T|_"dB"$ where $angle T = -180°$;
- *sensitivity peak* $S_"pk" = max |1 slash (1+T)|$ — a single robustness number
  ($S_"pk" < 1.5$: well damped; $S_"pk" > 2$: pronounced ringing).

Because several parameters are uncertain in reality, every design is evaluated over
*corners*, not just nominal values: the TIP125 gain spread ($beta_0 = 500 dots 4000$), its
frequency derating at low bias current, and (for the V loop) the load impedance and the
output-capacitor ESR.

= Device data used in the models

*Op-amps* (U1--U4): gain-bandwidth product GBW = 5.7 MHz, slew rate 2.8 V/µs. The open-loop
gain is modelled as a dominant-pole integrator $A(s) = 2 pi dot "GBW" slash s$ and enters the
compensator exactly through the noise-gain form (@sec_vstage1). Slew rate is a large-signal
limit and does not appear in the AC model.

*TIP125 PNP Darlington* (Q2/Q4): the datasheet's small-signal current gain (Fig. 7, PNP @ 3 A)
is fitted by a two-pole roll-off,

$ beta(s) = beta_0 / ( (1 + s slash omega_(beta 1)) (1 + s slash omega_(beta 2)) ),
  quad f_(beta 1) approx 60 "kHz", quad f_(beta 2) approx 300 "kHz", $

which reproduces the curve well (@tip125). *This roll-off — not the op-amp — is the dominant
high-frequency dynamic of both loops.* Two spreads are swept as corners:

- $beta_0$: datasheet minimum 1000, typical 2000--4000 (DC gain, Fig. 9); 500 is added as
  cold/low-current pessimism. Loop gain scales 1:1 with $beta_0$, so the crossover slides
  almost a decade across production spread.
- Low-bias derating: Fig. 7 is specified at 3 A. At small collector current $f_T$ drops, so a
  derated corner with $f_(beta 1,2) = 20 slash 100$ kHz is checked ("LB" in the tables).

*Minimum-load current sink (≈ 12 mA).* The production hardware places a constant-current
sink on each output (BCP56 #raw("Q304"), diode bias #raw("D1006"), 47 Ω degeneration
#raw("R320")), connected so that it *bypasses the current shunt*. The pass transistor
therefore never idles below ≈ 12 mA — without it, only the ~1 mA sense-divider current flows
at no load, and nothing bounds how far $f_beta$ collapses. Small-signal the sink is
high-impedance (≳ 100 kΩ), so it does not change any load case; its benefit is *bias*: the
LB corner becomes a *guaranteed pessimistic floor* instead of an open-ended assumption, and
if 12 mA is enough to hold the Fig.-7 poles, the LB corner does not apply at all. The tables
below report the worst case both ways. Because the sink bypasses the shunt, the current loop
sees no 12 mA offset — CC regulates the true load current and setpoints below 12 mA remain
usable; the pass device also never cuts off at no load, and the sink provides ~12 mA of
down-programming current.

#figure(image("fig_tip125.svg", width: 62%),
  caption: [Two-pole fit of the TIP125 small-signal current gain vs. datasheet Fig. 7
  read-off points (PNP, $I_C = 3$ A, $V_"CE" = 4$ V).]) <tip125>

Diodes D1/D2 (bias steering into the driver base) appear only as the divider
$R_3 slash (R_3 + r_d) approx 0.97$ with $r_d approx 26 space Omega$.

= Voltage loop (sheet V_loop, p. 2) <sec_vloop>

== Stage 1 — error amplifier / compensator U1 <sec_vstage1>

$V_"set"$ enters the (+) input; the feedback voltage $V_"fb"$ enters the (−) input through
$R_1$, with the feedback impedance $Z_f$ from output to (−):

$ Z_f (s) = (R_2 + 1/(s C_1)) parallel 1/(s C_3), quad quad
  H_1 (s) = - (Z_f slash R_1) / (1 + (1 + Z_f slash R_1) / A(s)). $

This is a *type-II compensator*: an integrator $1 slash (s R_1 (C_1 + C_3))$, a zero at
$f_z = 1 slash (2 pi R_2 C_1)$ and a high-frequency pole at
$f_p = (C_1 + C_3) slash (2 pi R_2 C_1 C_3)$. With the optimized values
*$R_1 = 10 "k"$, $R_2 = 18 "k"$, $C_1 = 47 "n"$, $C_3 = 22 "p"$*:

$ f_z approx 188 "Hz", quad "mid-band gain" R_2/R_1 = 1.8 "(5.1 dB)", quad f_p approx 400 "kHz". $

The finite GBW adds an effective pole near GBW$slash(1 + R_2 slash R_1) approx 2$ MHz —
a minor extra lag, but included exactly.

== Stage 2 — driver and pass transistor (Q1, R3, R4, D1, Q2)

U1's output drives Q1 (common emitter, degeneration $R_4$) through D1; Q1's collector current
is the base current of the TIP125, giving a transconductance

$ H_2 (s) = i_(c 2)/V_e
  = underbrace(R_3/(R_3 + r_d), approx 0.97) dot
    underbrace(1/(R_4 + r_(e 1)), g_(m 1)) dot beta_2 (s)
  quad ["A/V"], quad quad
  G_m = (0.97 dot beta_0) / (150 space Omega) = 13 "A/V" ("at" beta_0 = 2000). $

== Stage 3 — output network (R5, C2, load R6)

Q2's collector acts as a current source into the output capacitor branch and the load:

$ H_3 (s) = Z_"out" (s) =
  (R_5 + 1/(s C_2)) parallel [ R_"load" parallel (R_"esrL" + 1/(s C_"load")) ]. $

The load "R6" is *whatever the user connects*, so $Z_"out"$ — and with it the whole loop —
changes with the load: a resistive load leaves the $1 slash (omega C_2)$ slope up to the ESR
zero, while a large load capacitor drags the crossover down by decades. This is the defining
difficulty of the voltage loop.

== Stage 4 — sense amplifier U2

A resistive difference amplifier (plus the op-amp's GBW pole at
GBW$slash(1 + R_9 slash R_11) approx 5.3$ MHz):

$ H_4 = (1 + R_9/R_11) R_7/(R_7 + R_10) dot 1/(1 + s slash omega_(U 2)) = 1/15 dot 1/(1 + s slash omega_(U 2)), $

so the loop regulates $V_"out" = 15 thin V_"set"$.

== Loop gain and compensator design <sec_vdesign>

@vloopshape shows the assembled loop at the nominal load (100 Ω ∥ 1 µF): the integrator
crosses the flat mid-band on a clean single slope, the zero at 188 Hz has long since returned
the compensator phase, and the crossover lands at $f_c approx 22$ kHz with PM $approx 76°$.

#figure(image("fig_v_loopshape.svg", width: 88%),
  caption: [V loop at $beta_0 = 2000$, load 100 Ω ∥ 1 µF: compensator $H_1$, rest
  $H_2 H_3 H_4$, and loop gain $T$.]) <vloopshape>

The values were found by a grid search over E12 components with the corner requirements
PM ≥ 45° (≥ 35° for the extreme 1000 µF case), GM ≥ 10 dB and $S_"pk" lt.eq 1.7$ over *all*
corners (β spread × ESR × load set), maximising the worst-case crossover. Two rules of thumb
summarise why the result looks the way it does:

+ *The zero must sit below the biggest-cap crossover.* A large load capacitor drags $f_c$
  down; if $f_c$ falls below $f_z$ the loop reverts to integrator-on-top-of-cap-pole
  (−180° slope) and the margin collapses. $f_z = 188$ Hz keeps even the β = 500 + 1000 µF
  corner ($f_c approx 116$ Hz) inside the zero's phase lead.
+ *The mid-band gain sets the light-load crossover.* $R_2 slash R_1 = 1.8$ keeps the
  crossover at $beta_0 = 4000$ ($f_c approx 45$ kHz) clear of the β poles even when the
  design is denied the help of C2's ESR zero (ESR corner 50 mΩ).

== Robustness across loads and corners

#figure(image("fig_v_loadsweep.svg", width: 88%),
  caption: [V-loop gain across 11 load types at $beta_0 = 2000$ (dashed: purely resistive).
  Big capacitors move the crossover down by up to two decades — the compensator must be
  stable along the whole trajectory.]) <vloadsweep>

#figure(
  table(
    columns: (auto, auto, auto, auto, auto, auto, auto, auto),
    align: (left, right, right, right, right, right, right, right),
    stroke: 0.4pt + gray,
    table.header([*load case*], [*$f_c$ [Hz]*], [*PM [°]*], [*GM [dB]*], [*$S_"pk"$*],
                 [*worst PM [°]*], [*floor PM [°]*], [*floor $S_"pk"$*]),
    [no load (100 k)],        [23 930], [75.6], [28.1], [1.15], [47.8], [37.0], [1.99],
    [1 kΩ],                   [23 926], [75.7], [28.1], [1.15], [47.8], [37.0], [1.99],
    [100 Ω (nominal)],        [23 885], [76.1], [28.1], [1.15], [48.0], [37.5], [1.97],
    [10 Ω (heavy)],           [23 440], [79.7], [28.3], [1.14], [50.1], [42.2], [1.83],
    [100 Ω + 1 µF/1 Ω],       [21 801], [76.4], [28.9], [1.15], [50.9], [40.2], [1.87],
    [1 k + 4.7 µF/5 mΩ],      [16 534], [75.8], [24.3], [1.17], [55.7], [46.1], [1.69],
    [10 k + 10 µF/10 mΩ],     [12 240], [77.6], [26.3], [1.15], [62.4], [53.6], [1.51],
    [10 Ω + 100 µF/50 mΩ],    [2 252],  [89.7], [44.2], [1.01], [86.1], [84.5], [1.05],
    [10 Ω + 330 µF/30 mΩ],    [749],    [81.1], [48.7], [1.01], [62.7], [62.7], [1.03],
    [100 Ω + 470 µF/0.5 Ω],   [838],    [126.1],[32.2], [1.07], [61.0], [61.0], [1.26],
    [10 Ω + 1000 µF/20 mΩ],   [292],    [61.9], [52.2], [1.01], [40.0], [40.0], [1.48],
  ),
  caption: [V loop per load: left half at the typical corner ($beta_0 = 2000$,
  $f_beta = 60 slash 300$ kHz, ESR 0.2 Ω). *worst PM* = worst case over the β/ESR corners
  with datasheet β poles (valid if the 12 mA sink holds the Fig.-7 poles); *floor PM /
  floor $S_"pk"$* = additionally including the ÷3 low-bias pole corner — the guaranteed
  bound at ≥ 12 mA bias. The global worst with datasheet poles is the 1000 µF case (40.0°,
  β = 4000) — a load property, not a bias property.]) <vtable>

#figure(
  table(
    columns: (auto, auto, auto, auto, auto),
    align: (left, right, right, right, right),
    stroke: 0.4pt + gray,
    table.header([*β corner*], [*$f_c$ range [Hz]*], [*worst PM [°]*], [*min GM [dB]*], [*max $S_"pk"$*]),
    [β = 500],              [116 … 6 188],   [40.0], [35.0], [1.48],
    [β = 1000 (min spec)],  [178 … 12 279],  [49.5], [28.9], [1.21],
    [β = 2000 (typical)],   [292 … 23 930],  [61.9], [22.9], [1.33],
    [β = 4000 (typ. peak)], [522 … 44 766],  [47.8], [16.9], [1.64],
    [β = 1000, low-bias],   [178 … 10 920],  [49.1], [19.6], [1.49],
    [β = 2000, low-bias],   [291 … 18 403],  [37.0], [13.6], [1.99],
  ),
  caption: [V loop worst case per β corner, over all 11 loads and both ESR values. The
  binding corners are β = 4000 with low-ESR C2 (fast side) and the doubly derated low-bias
  case (slow side); both remain stable with sensible margin. The two low-bias rows are the
  floor guaranteed by the 12 mA minimum-load sink — they no longer bind if 12 mA holds the
  datasheet β poles.]) <vcorners>

== Setpoint step and prefilter

The closed-loop response from the setpoint is

$ V_"out"/V_"set" = (G_m (s) thin Z_"out" thin (1 + Z_f slash R_1)) / (1 + T), $

whose numerator carries the *noise-gain* term $(1 + Z_f slash R_1)$ — the compensator zero
appears in the reference path with a zero at

$ f_(z,"ref") = 1/(2 pi (R_1 + R_2) C_1) approx 121 "Hz". $

A setpoint edge therefore feeds forward and overshoots by ≈ 54 % — *not* ringing (the loop
is critically damped); it disappears if the setpoint never contains energy above that zero.
The clean fix is an RC low-pass in front of the (+) input with its pole *matched to the
zero*: $tau = (R_1 + R_2) C_1 approx 1.32$ ms, e.g. *27 k + 47 n*. The zero then cancels
the filter pole (@vstep). Note that $R_1 C_1$ alone (470 µs) is not the match — it leaves
about 13 %.

#figure(image("fig_v_step.svg", width: 80%),
  caption: [Closed-loop 1 V setpoint step ($beta_0 = 2000$, nominal load) with and without
  the matched RC prefilter.]) <vstep>

= Current loop (sheet I_loop, p. 3) <sec_iloop>

== Stages 1, 2 and 4

The error amplifier U3 and the driver Q3/Q4 are identical in topology to the voltage loop
($R_8 = 10$ k input resistor, $R_15 = 150 space Omega$ degeneration, same TIP125 model). The
optimized compensator is a *pure integrator* — $R_12$ and $C_5$ stay unpopulated:

$ H_1 (s) = - (1 slash (s R_8 C_4)) / (1 + (1 + 1 slash (s R_8 C_4)) slash A(s)), quad
  C_4 = 27 "n" quad arrow.r quad "unity gain at " 1/(2 pi R_8 C_4) approx 589 "Hz". $

The sense amplifier U4 is non-inverting with gain $1 + R_16 slash R_17 = 12$; its noise gain
of 12 pulls the op-amp pole down to GBW$slash 12 approx 475$ kHz — low enough to matter
alongside the β poles:

$ H_4 (s) = 12 / (1 + s slash (2 pi dot 475 "kHz")), quad quad
  V_"fb" = 12 dot R_21 dot I = 2.4 "V/A" quad (I approx 417 "mA per volt of" V_"set"). $

== Stage 3 — the flat, load-independent plant <sec_iplant>

Here the schematic differs decisively from the voltage loop: the output-capacitor branch
($R_19 + C_6$) *and* the load $R_20$ both return to the *top of the shunt* $R_21$, not to
ground. Every milliampere that Q4 sources must therefore pass through the shunt, and
Kirchhoff's current law gives, exactly and at every frequency,

$ i_(R_21) = i_(C_6) + i_"load" = i_(c 4)
  quad arrow.r.double quad
  H_3 (s) = V_"sh" / i_(c 4) = R_21 = 0.2 space Omega . $

The load impedance, $C_6$ and $R_19$ *drop out of the loop gain completely* — they only set
the voltage at the output node, which the regulated variable never sees. The loop is
identical for a short, a resistor, or 1000 µF. Including Q4's finite output resistance
($r_o approx 2$ k pessimistic) and output capacitance ($C_"ob" lt.eq 300$ pF) merely adds a
static gain error up to ≈ 5 % at light load (a current-divider effect, calibration
territory) and *no dynamics* below the MHz range.

== Loop gain — why a pure integrator <sec_idesign>

With a flat plant, the "rest" of the loop is a constant
$0.97 beta_0 R_21 dot 12 slash R_15 approx 31$ (30 dB at $beta_0 = 2000$) with the β/U4
poles hanging off the top (@iloopshape). A single integrator crosses it on a clean −20
dB/dec slope: crossover

$ f_c approx (0.97 thin beta_0 thin R_21 (1 + R_16 slash R_17)) / (2 pi R_8 C_4 R_15)
  = 17.6 "kHz at" beta_0 = 2000 . $

A voltage-loop-style zero ($R_12 > 0$) would be actively harmful here: it flattens the
compensator above $f_z$, and with the 8:1 β-gain spread the crossover would slide across
that flat region straight into the β poles. *Keep $R_12 = 0$.* $C_4 = 27$ n is the
speed/margin knee: 22 n is ≈ 20 % faster but sits at the acceptance edge
(low-bias $S_"pk" approx 2$), 47 n is the conservative fallback (PM ≥ 65°).

#figure(image("fig_i_loopshape.svg", width: 88%),
  caption: [I loop at $beta_0 = 2000$: integrator, flat rest, and loop gain. The phase
  budget at crossover is −90° (integrator) minus the β-pole lag.]) <iloopshape>

== Corner sweep

#figure(
  table(
    columns: (auto, auto, auto, auto, auto),
    align: (left, right, right, right, right),
    stroke: 0.4pt + gray,
    table.header([*β corner*], [*$f_c$ [Hz]*], [*PM [°]*], [*GM [dB]*], [*$S_"pk"$*]),
    [β = 500],              [4 582],  [84.2], [33.1], [1.08],
    [β = 1000 (min spec)],  [9 082],  [78.5], [27.1], [1.15],
    [β = 2000 (typical)],   [17 597], [68.0], [21.0], [1.30],
    [β = 4000 (typ. peak)], [32 148], [51.5], [15.0], [1.60],
    [β = 1000, low-bias],   [8 437],  [61.2], [20.3], [1.39],
    [β = 2000, low-bias],   [14 662], [43.5], [14.3], [1.78],
  ),
  caption: [I loop margins per β corner. The numbers are *identical for every load* — the
  load does not appear in $T(s)$ (@sec_iplant). Worst case with datasheet β poles: 51.5°
  (β = 4000); the low-bias rows are the 12 mA-sink-guaranteed floor (43.5° worst). The sink
  bypasses the shunt, so it adds *no offset* to the sensed current — CC regulates the true
  load current and setpoints below 12 mA remain usable.]) <itable>

#figure(image("fig_i_corners.svg", width: 88%),
  caption: [I-loop gain across the TIP125 β corners (dashed: low-bias derated poles). Only
  the gain scales; the pole structure — and therefore the phase curve — barely moves.]) <icorners>

== Setpoint step — the feed-forward spike

Because the plant has *no inertia* (a flat 0.2 Ω, no output pole in the current path), a
$V_"set"$ edge feeds through the noise-gain term $(1 + Z_f slash R_8) arrow.r 1$ directly to
the driver and commands $Delta I approx G_m Delta V_"set" approx 13$ A/V for the few
microseconds ($1 slash 2 pi f_c approx 9$ µs) the integrator needs to take over — the
small-signal model shows a spike of more than 2000 % (@istep, left). Hardware clips it
(op-amp swing and 2.8 V/µs slew, Q3's emitter resistor), but the lesson stands: *the current
setpoint must never step hard.* The matched RC prefilter (pole on the reference zero
at $1 slash 2 pi R_8 C_4 = 589$ Hz, i.e. $tau = R_8 C_4 = 270$ µs, *10 k + 27 n*) removes
the spike while keeping the setpoint response fast, because the compensator zero cancels the
filter pole (@istep, right).

#figure(image("fig_i_step.svg", width: 80%),
  caption: [I loop, 1 V setpoint step at $beta_0 = 2000$. Left: unfiltered — the feed-forward
  spike reaches amperes for a few µs (model; hardware clips it). Right: with the matched
  270 µs prefilter — ≈ 1.4 % residual, settled in ≈ 20 µs.]) <istep>

= Setpoint prefilters — general rule

Both overshoots have the same root cause: the compensator's feedback impedance appears in
the *reference path* as $(1 + Z_f slash R_"in")$, whose zero lets a setpoint edge feed
forward. The matched RC prefilter puts its pole exactly on that zero:

$ tau_"filt" = (R_"in" + R_"zero") dot C_"int" $

#figure(
  table(
    columns: (auto, auto, auto, auto, auto),
    align: (left, left, left, left, left),
    stroke: 0.4pt + gray,
    table.header([*loop*], [*reference zero*], [*matched $tau$*], [*suggested RC*], [*result (1 V step)*]),
    [V loop], [$1 slash 2 pi (R_1 + R_2) C_1 approx 121$ Hz], [1.32 ms],
      [27 k + 47 n (or 10 k + 120 n)], [54 % → ≈ 0 %],
    [I loop], [$1 slash 2 pi R_8 C_4 approx 589$ Hz], [270 µs],
      [10 k + 27 n], [2267 % → 1.4 %, settle ≈ 20 µs],
  ),
  caption: [Matched setpoint prefilters. The filter is a series R + C-to-ground directly at
  the error amp's (+) input (high impedance). A firmware DAC ramp of ≈ 1 ms is the
  part-free alternative. Note the values track the compensator: change $C_1$/$C_4$ and the
  filter caps follow.]) <preftable>

Because the reference zero contains only compensator parts, the cancellation is independent
of the TIP125 β and of the load.

= Summary

#figure(
  table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    stroke: 0.4pt + gray,
    table.header([*part*], [*schematic first guess*], [*final value*], [*role*]),
    [V: $R_1$], [10 k], [10 k (keep)], [input / gain reference],
    [V: $R_2$], [10 k], [*18 k*], [zero at 188 Hz, mid-band gain 1.8],
    [V: $C_1$], [1 n], [*47 n*], [integrator; zero with $R_2$],
    [V: $C_3$], [—], [*22 p*], [HF pole ≈ 400 kHz],
    [I: $R_8$], [10 k], [10 k (keep)], [input / gain reference],
    [I: $C_4$], [—], [*27 n*], [pure integrator, unity at 589 Hz],
    [I: $R_12$, $C_5$], [—], [*DNP*], [a zero would be harmful (flat plant)],
    [V prefilter], [—], [27 k + 47 n], [kills 54 % setpoint overshoot],
    [I prefilter], [—], [10 k + 27 n], [kills the feed-forward current spike],
  ),
  caption: [Final component values (all E12).]) <finaltable>

*Resulting worst-case margins* (with the ≈ 12 mA minimum-load sink keeping the pass device
biased) — V loop: PM ≥ 40° with datasheet β poles (the binding case is the extreme
10 Ω + 1000 µF load at β = 4000; every normal load stays ≥ 47.8°), GM ≥ 16.9 dB; the
÷3-derated low-bias floor bottoms at 37°. Typical-β margins 62–126°. I loop: PM ≥ 51.5°,
GM ≥ 15 dB over the full β spread (low-bias floor 43.5°), for *any* load. Before the sink,
the low-bias corner was an open-ended assumption — the pass device idled at the ~1 mA
divider current and nothing bounded the $f_beta$ collapse; now it is a guaranteed floor.

*Not covered by these AC models*, to be verified on hardware: the CV↔CC handover through the
D1/D2 diode-OR (the losing loop's integrator saturates and must slew back — an anti-windup
clamp shortens the transition), how far the TIP125 β poles actually derate at the 12 mA
minimum bias (bounded by the LB corner, but Fig. 7 is specified at 3 A), and thermal drift
of the TIP125 parameters.
