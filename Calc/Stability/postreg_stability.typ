#set document(title: "Linear Post-Regulator Loop Stability Analysis", author: "J. Schreiber")
#set page(paper: "a4", margin: 2cm, numbering: "1 / 1")
#set par(justify: true)
#set heading(numbering: "1.")

#align(center)[
  #text(17pt, weight: "bold")[Linear Post-Regulator — Control-Loop Stability Analysis]
  #v(3pt)
  #text(11pt)[2-Channel Pre-Reg Power Supply · sheet `Linear_PostReg.kicad_sch` (schematic page 10)]
  #v(2pt)
  #text(9pt, style: "italic")[Current state — 2026-06-16]
]
#v(6pt)

#outline(depth: 2, indent: auto)
#v(6pt)

= Scope and summary

Stability assessment of the linear post-regulator output stage (35 V / 2 A) fed by a
tracking pre-regulator (PreReg = V#sub[out] + 1.7 V). Both control loops — constant-voltage
(CV) and constant-current (CC) — were reverse-engineered from the KiCad schematic, the
device dynamics taken from datasheets in the workspace, and the loops modelled in Python
(small-signal Bode plus a behavioural large-signal handover simulation).

*Headline result:* as drawn, the *CV loop is unstable* and the *CC loop is marginal*. The
dominant cause is the *compensation*, not the pass transistor. A re-compensation of both
error amplifiers (5 component changes, see @sec-fix) restores ≥ 45° phase margin across the
full load range and the worst-case transistor spread, at the cost of a slower loop. The
*CV/CC handover still needs an anti-windup clamp* (see @sec-handover).

= Topology (reverse-engineered) <sec-topo>

#table(
  columns: (auto, 1fr),
  align: (left, left),
  stroke: 0.5pt + luma(180),
  table.header([*Block*], [*Implementation*]),
  [Pass element], [Q303 = TIP125 PNP Darlington, *common-emitter* (emitter → PreReg, collector → OUT+). High-impedance output node ⇒ LDO-type loop. β ≥ 1000 (typ 2500).],
  [Driver / level shift], [Q301 = BCP56, common-emitter, emitter degeneration R315 = 150 Ω. Forward transconductance ≈ β / R315.],
  [CV error amp], [U301A TLV4387 (GBW 5.7 MHz, zero-drift), inverting integrator: input R301, feedback R303 in series with C303.],
  [CC error amp], [U301B TLV4387, inverting integrator: input R302, feedback R304 in series with C304 (feedback returns to the inverting input — confirmed from schematic).],
  [Auction], [CV and CC outputs diode-OR'd through D301 (BAT54A, common-anode) into the base-drive node — lowest wins.],
  [V sense], [U301D difference amp, H = 4.3k/62k = 0.0694 (36 V → 2.5 V). C306 = 10 p ⇒ pole ≈ 3.7 MHz (non-limiting).],
  [I sense], [R318 = 100 mΩ in the OUT− return (senses full pass current); U301C non-inverting, gain 1 + 18k/1.6k = 12.25.],
  [Output cap], [C309 = 10 µF (1206 ceramic) *in series with R319 = 200 mΩ* damping ⇒ ESR-zero ≈ 80 kHz.],
)

Forward path (CV loop):
$ T_"CV"(s) = A_"ea"(s) dot underbrace(beta(s) \/ R_315, "driver + pass") dot underbrace(Z_"out"(s), "C309 || R_L || r_o") dot underbrace(H, "V sense") $

with the error amplifier acting as an integrator with one zero:
$ A_"ea"(s) = (1 + s R_303 C_303) / (s R_301 C_303). $

= Key device dynamics

#table(
  columns: (auto, auto, 1fr),
  stroke: 0.5pt + luma(180),
  align: (left, center, left),
  table.header([*Parameter*], [*Value*], [*Source / note*]),
  [TLV4387 GBW], [5.7 MHz], [zero-drift quad op-amp; rails +3V3 / −1V2],
  [TIP125 DC β], [1000 min / 2500 typ], [onsemi TIP120-D / ST DS0854],
  [TIP125 small-signal h#sub[fe]], [4 @ 1 MHz (min)], [⇒ guaranteed f#sub[T] ≥ 4 MHz (*floor*)],
  [TIP125 typical f#sub[T]], [≈ 20–30 MHz], [datasheet Fig. 7 is *typical*: gain flat to ≈ 20 kHz],
  [TIP125 β-pole], [*1.6 kHz (floor) … 20 kHz (typ)*], [f#sub[β] = f#sub[T] / β#sub[0] — the critical plant pole],
  [TIP125 C#sub[ob]], [300 pF], [secondary],
)

The β-pole spread is the single most important fact: the datasheet only *guarantees*
f#sub[T] ≥ 4 MHz (β-pole as low as 1.6 kHz), while a typical unit is ≈ 10× faster
(β-pole ≈ 20 kHz). A robust design must cover the 1.6 kHz floor.

= CV loop — pole/zero map and as-drawn verdict

#table(
  columns: (1fr, auto, auto),
  stroke: 0.5pt + luma(180),
  align: (left, center, center),
  table.header([*Feature*], [*As-drawn*], [*Origin*]),
  [Integrator unity-gain], [15.9 kHz], [1/(2π R301 C303), 10k·1n],
  [Compensation zero], [159 kHz], [1/(2π R303 C303), 1k·1n],
  [Output pole (load-dep.)], [8 Hz … 1.33 kHz], [1/(2π(R#sub[L]‖r#sub[o]) C309), 12 mA … 2 A],
  [β-pole (load/part-dep.)], [1.6 … 20 kHz], [f#sub[T] / β#sub[0]],
  [Output-cap damping zero], [79.6 kHz], [1/(2π R319 C309)],
)

As drawn the loop crosses over at ≈ 7.7 kHz — right where the integrator plus the
load-dependent output pole already reach −180° — and the compensation zero (159 kHz) sits a
decade too high to help. Resulting phase margin (worst over 12 mA – 2 A):

#table(
  columns: (auto, auto, auto, auto),
  stroke: 0.5pt + luma(180),
  align: (left, center, center, center),
  table.header([*β-pole*], [1.6 kHz (floor)], [5 kHz], [20 kHz (typ)]),
  [PM, as-drawn], [−70°], [−54°], [−24°],
)

Even with a *typical* (fast) transistor the loop is −11°…−24° ⇒ *unstable*. The transistor
was taking the blame for a compensation defect.

#figure(image("cv_betapole_sweep.png", width: 78%),
  caption: [As-drawn CV loop @ 2 A — the phase dip deepens as the β-pole drops; unstable across the whole range.])

= Recommended fix — re-compensate, keep the TIP125 <sec-fix>

Five component changes, verified to give ≥ 45° phase margin *at the worst-case 4 MHz
β-pole floor*, across 12 mA – 2 A:

#table(
  columns: (auto, 1fr, auto, auto),
  stroke: 0.5pt + luma(180),
  align: (left, left, center, center),
  table.header([*Ref*], [*Net / role*], [*From*], [*To*]),
  [R301], [CV integrator input], [10 kΩ], [*100 kΩ*],
  [R303], [CV comp-zero resistor], [1 kΩ], [*10 kΩ*],
  [C303], [CV integrator cap], [1 nF], [*220 nF*],
  [R304], [CC comp-zero resistor], [0 Ω], [*2.2 kΩ*],
  [C304], [CC integrator cap], [100 pF], [*220 nF*],
)

Resulting phase margin:

#table(
  columns: (auto, auto, auto),
  stroke: 0.5pt + luma(180),
  align: (left, center, center),
  table.header([*Loop*], [*PM at β-pole floor (1.6 kHz)*], [*PM at typ (20 kHz)*]),
  [CV (R301/R303/C303)], [+47°  (crossover ≈ 1.4 kHz)], [+84°],
  [CC (R304/C304)], [+99°  (crossover ≈ 7 kHz)], [+87°  (crossover ≈ 86 kHz)],
)

These change loop *dynamics only* — not the output setpoint (the integrators force
V#sub[sense] = V#sub[set] regardless). The TLV4387's 300 pA bias current makes the 100 kΩ
input harmless (≈ 30 µV offset).

#grid(columns: (1fr, 1fr), gutter: 8pt,
  figure(image("cv_recomp_tip125.png", width: 100%),
    caption: [CV re-comp: stable for both β-pole corners at 12 mA and 2 A.]),
  figure(image("cc_recomp.png", width: 100%),
    caption: [CC re-comp (2k2 / 220n) robust; R304 = 100 k alone is unstable at the typical β-pole.]),
)

== Why these specific values

- *CV:* dropping the comp zero ≈ 100× (R303 1k→10k, C303 1n→220n) moves crossover from 7.7 kHz down to ≈ 1.4 kHz — below the β-pole, and managing the moving output pole.
- *CC:* the loop was *too fast* (C304 = 100 p, no zero). Adding R304 = 100 k makes it *worse* — stable at the floor (+40°) but *unstable at the typical β-pole* (−26°), because crossover runs to ≈ 1 MHz into the current-sense-amp poles (465 k / 884 kHz). The fix is to *slow it* (C304 = 220 n) with a *low* proportional gain (R304/R302 = 0.22), keeping crossover ≈ 7–86 kHz, safely below the sense-amp poles.
- A parallel "type-II" pole cap across the CC feedback branch was evaluated and does *not* help here: the wide β-pole spread (1.6 k–20 k) forces a low-gain / slow loop regardless.

== Cost and the pass-device alternative

The re-comp deliberately makes the loops slow (CV crossover ≈ 1.4 kHz). That is the
unavoidable price of guaranteeing stability for a *worst-case* (4 MHz) TIP125. A faster
pass device would lift this tax:

#table(
  columns: (auto, auto, auto, auto, 1fr),
  stroke: 0.5pt + luma(180),
  align: (left, center, center, center, left),
  table.header([*Device*], [*f#sub[T]*], [*Ratings*], [*Status*], [*Verdict*]),
  [TIP125 (in use)], [4 MHz floor], [60 V / 5 A / 65 W], [active], [keep + re-comp (this report)],
  [BDX54C], [≈ 4 MHz (same)], [100 V / 8 A / 65 W], [*discontinued*], [no benefit, avoid],
  [BDW94C], [≈ 20 MHz], [100 V / 12 A / 80 W], [active], [robust upgrade; would allow a faster loop],
)

= CV/CC handover — open item <sec-handover>

Small-signal stability of each loop is *necessary but not sufficient*. A behavioural
large-signal simulation (qualitative) shows the real issue is *integrator windup*: the
diode-OR clamps only the controlling amp, so the idle amp's integrator winds to the +3V3
rail. This is worsened by the new 220 nF caps (slow unwind):

- Entering limit (CV → CC): *≈ 40 % current overshoot* (≈ 2.85 A vs 2 A) before CC takes control.
- Leaving limit (CC → CV): the wound-up CV integrator drives V#sub[out] hard to the rail with slow recovery.

Simple *tracking anti-windup* (clamp each idle integrator ≈ 1 diode above the CC/CV node)
removes the overshoot in the simulation.

#figure(image("handover.png", width: 72%),
  caption: [Handover transient (behavioural). Without anti-windup: current overshoot and poor recovery. With it: clean.])

*To do:* design the anti-windup clamp (e.g. a Schottky from the CC/CV node to each
error-amp output) and verify the handover on the bench / in SPICE — the Bode model cannot
quantify the overshoot.

= Status, models, open items

*Done:* topology reverse-engineered; device dynamics from datasheets; CV and CC loops
modelled and re-compensated to ≥ 45° worst-case; pass-device comparison.

*Open items:*
+ Anti-windup clamp for clean CV/CC handover (design plus bench/SPICE verification).
+ Pre-regulator tracking-loop (V#sub[out] + 1.7 V) interaction with the post-reg — not yet analysed.
+ Bench validation of the re-comp (load-step response, CV/CC transition) on real hardware.

*Model files* (in `Calc/Stability/`, run with `py <file>`):

#table(
  columns: (auto, 1fr),
  stroke: 0.5pt + luma(180),
  align: (left, left),
  table.header([*File*], [*Purpose*]),
  [`loop_stability.py`], [baseline CV + CC Bode (worst-case 4 MHz β-pole)],
  [`loop_fixes.py`], [fix exploration (faster device, re-comp, output cap)],
  [`loop_tip125_recomp.py`], [typ-vs-worst β-pole sweep + CV re-comp verification],
  [`cc_and_handover.py`], [CC re-comp Bode + behavioural handover transient],
)

#v(4pt)
#line(length: 100%, stroke: 0.5pt + luma(180))
#text(8pt, style: "italic")[Component values from `Linear_PostReg.kicad_sch`. Datasheets: TLV2387/4387 (SBOSA91B), onsemi TIP120-D, ST DS0854 (TIP125), onsemi BDX53B-D, ST DS0845 (BDX54C). All phase-margin figures are model results; bench validation pending.]
