# tea_alarm

An alarm that clips on a mug's rim and rings when the tea has cooled
enough to drink. A straight KANTHAL 94S thermostatic bimetal strip hangs in
the tea and straightens as it cools; its free end lifts a vertical probe;
the probe tips a balanced ramp past the slope of its detent at the set
temperature; a steel ball rolls off the ramp's lip onto a chime and into a
dish. An M3 screw under a printed knob sets the temperature. Adapted from
Chronova Engineering's tea timer, not copied from it.

This is a dogfood round for the simulation stack, and the alarm is designed
and checked in the app: every study is held to its closed form, and two
side cases exercise the study types the alarm does not use. Building it is
the round's last phase ([the plan](../../docs/plans/simulation-dogfood.md)):
the strip in a water bath, the trigger in a cooling mug, the block's
temperature.

```sh
tools/headless-preview.sh --start 8800
python3 generators/print_coupon.py --port 8800 --seed   # the P1S profiles
python3 generators/tea_alarm/build.py --port 8800 --profile "K5 PLA on P1S" \
    --strip-mm 5 0.3 --rod-mm 3 --vessel-mm 80 95 3 --ready-c 55 --room-c 22 \
    [--no-render] [--fresh] [--out DIR]
```

Every argument but `--out` and the flags is required, because each is
something only the buyer and the mug know: the strip and the rod as
ordered (width and thickness; diameter), the mug (inside diameter, inside
depth below the rim, the wall's thickness at the rim), the temperature to
ring at, and the room the dry rod and the block cool into. Output
(untracked) goes to `scratch/tea_alarm/`: `tea_alarm.lob` (the parts, the
assembly and its studies), `strip_check.lob`, `latch.lob`,
`latch_control.lob` and `bracket.lob` (the side cases), the receipt
`tea_alarm.json` (every number below, from the app), the print job
`tea_alarm.3mf`, the bill `tea_alarm_bom.csv`, the rod's bend schedule
`tea_alarm_rod_bends.csv`, and the sheets as PDF and SVG: `tea_alarm_clip`,
`tea_alarm_rod` and `tea_alarm_knob`.

Files: `ta_spec.py` (every size and number, no app), `ta_parts.py` (one
function a part), `ta_mechanism.py` (the parts built, the statics, the
assembly and the trigger), `ta_fem.py` (the strip check, the heat path, the
clip and the chime), `ta_cases.py` (the side cases), `build.py` (the run,
the print, the bill and the drawings), `test.json` (the
[generator test](../README.md#the-big-generators-as-tests)).

## The strip

KANTHAL 94S, straight, cut from bought strip `--strip-mm b s` (5 × 0.3 in
the test). Why this grade: Kanthal recommends it, and only it, for water
("both components consist of NiCr steels. KANTHAL 94S is suitable for use
in water and steam", handbook §4); its linear range, 0 to 200 °C, covers
the tea; and it is roll-bonded clad, so it bends as one strip (a riveted
pair slips between its rivets, which no closed form answers).

It is a horizontal cantilever in the tea, clamped against the frame rod
inside the wall and free on the mug's axis, where the probe stands on it.
**Its high-expansion side faces UP.** Heated, a bimetal bends toward its
low-expansion side, so at the pour its tip is lowest and it rises as the
tea cools, lifting the probe. (The plan had it facing down; that curls the
tip up as it heats, and the probe would fall as the tea cooled.) Bought
flat at room temperature, its constants are from 20 °C, and the set screw
takes out any offset.

From the handbook (catalogue 3-A-1-3; the library row
`bimetal_kanthal_94s` carries the same, and `build.py` checks them):

- **Tip travel**, eq. [9]: A = a ΔT L² / s with a = 9.5e-6 /K. At s = 0.3
  and L = 36.5 mm (from the vessel, below), 0.0422 mm/K: 1.90 mm from 100
  to 55 °C.
- **Force costs temperature**, eq. [16]: a tip force F takes 4 F L³ /
  (b s³ E) off the travel, 4 F L / (a E b s²) in temperature: 180 K per
  newton, **0.180 K per mN**. A constant force is an offset the screw takes
  out; friction at the probe's guide and the ramp's pivot is not constant,
  and it is the trigger's scatter at 0.18 K for every mN of it.
- **Kanthal's minimum-volume rule**, eq. [20]: the force may take at most
  half the free travel. The probe's 9.9 mN takes 1.78 K, 3.0 % of the
  travel from the pour to the coolest setting.
- **Stress**, eq. [23]: σ = 6 F L / (b s²) = 4.8 MPa, against the
  handbook's general 400 to 500 MPa yield of thermostatic bimetal (§2.1;
  its permissible stress per grade is a graph with no values in its text,
  so the bound used is 400).
- **The strip's tolerance**: k is ± 4 %, so the trigger at T is ± 0.04 (T −
  20) K: ± 1.4 K at 55 °C. The strip as cut is measured in the bath
  (later, below).

A structural study of a body of the bimetal is **refused**, naming its
missing Poisson's ratio (Kanthal publishes none, nor either layer's data):
`build.py` asks for one as a negative control. The strip's motion is the
maker's measured constant, not a layer model.

### The strip check

The bought strip has no published layers, so the app's bimetal physics is
checked on a pair whose layers are all cited: a library `steel` layer under
a C26000 `brass` one, 0.5 × 5 × 40 mm each, bonded in an assembly
(`strip_check.lob`), clamped across one end and heated from 20 to 100 °C (a
[uniform thermal load on an assembly](../../docs/simulation/README.md#a-thermal-load-on-an-assembly)).
Timoshenko's curvature, with m = 1 and n = 210/110.3, is 8.4143e-4 /mm: a
tip rotation of 0.033657 rad and a drop of 0.67315 mm. The free end, read by
`probe_study` as a rigid body (on an assembly study, by its occurrence):

| | tip rotation | drop |
|---|---|---|
| in-house, 0.5 mm mesh | 0.034261 rad (+1.79 %) | 0.69700 mm (+3.54 %) |
| CalculiX, the same mesh | within 5.8e-8 | |
| control, both layers steel | 5.4e-6 rad | |

Away from the clamp the strip curves as the closed form says (+0.35 % in
`[thermal]`); the clamp holds both layers' ends across the width and the
thickness and the root turns a little more than the free span, so the
check holds the rotation to 2 % and the drop to 4 %.

## The trigger

- **The probe** is a stub of the rod on a slider mate in the printed
  block's guide, standing on the strip's tip. **The ramp** is printed, on a
  revolute on a stub of the rod pressed into the block's cheeks. Its probe
  arm carries the knob's screw over the probe; its ball arm, behind the
  pivot, rises by φ toward its lip, and a stop by the pivot holds the ball.
  The probe arm is heavier, so the ramp rests on the block until the probe
  lifts it; when the probe has turned it by φ the ball arm is level and the
  ball goes (and as it rolls out the ramp tips onto its far side).
- **The screw**, an ISO 4762 M3 (pitch 0.5, ISO 261) in a tapped hole in
  the probe arm, stands out under the arm by p. More stand-out, an earlier
  (hotter) touch: p = a pitch rings at 40 °C, a pitch and the strip's
  travel over 30 K at 70 °C. One turn moves the trigger by a pitch's
  travel, 11.85 K, so the knob's 2.53 turns span 40 to 70 °C, and its dial
  is marked every 5 °C (the knob sheet).
- **φ, the detent's slope**, is no sharper than the strip itself: the
  ramp turns by φ over the strip's ± 4 % of the cooling from the pour to
  --ready-c (1.8 K at 55 °C).
- **The statics.** The ramp's static moment about its pivot is its printed
  mass at its centre (`mass_properties`, as printed on the profile) and the
  screw's; the ball's moment behind the pivot opposes it. The largest ball
  that still leaves the ramp resting on the probe is 8.91 mm; the build
  uses a 7 mm one (1.41 g; the largest whole millimetre under, less one,
  to be weighed on arrival), and the probe then carries 9.9 mN.
- **The sweep.** `tea_c` (a project variable) is stepped from the pour
  down by a tenth of φ's band; at each step the probe is posed where the
  strip's tip puts it (its offset on the slider) and the ramp where the
  probe lifts it, the assembly solved and the ramp's angle read back; the
  trigger is where that angle reaches φ, interpolated. At the knob's mark
  for 55 °C it rings at **55.00 °C**, and at the knob's ends at 40.0 and
  70.0 °C.

The ramp is posed by hand because the app cannot drive it: a `contact`
relation takes a cam's TURN as its driver, and the probe slides (the
refusal is asked for and printed). So the sweep reproduces its own closed
form; what the app answers is the statics, the geometry and the pose.

**The ball's path** is not simulated (the app has no dynamics). A solid
sphere leaving the lip after rolling down a radius leaves at v² = (10/7) g
Δh, 0.22 m/s for the 7 mm ball, and falls 9 mm onto the chime, landing 11
mm past the lip; every trigger is at the same tilt φ, so the landing does
not spread across the knob's range. The dish hangs under the landing.

## Where every size comes from

From the strip and the rod as ordered, the vessel, the trigger and the
room, and nothing else in millimetres:

| size | rule | in the test |
|---|---|---|
| strip's free length L | from its clamp at the rod to the mug's axis: R − t_clip − 1.5 d | 36.5 |
| strip's height | its tip at the pour clears the bottom by its own drop | 6.75 above the bottom |
| clip | 1.4301, the thinnest gauge the sheet-metal rules stock (0.5 mm), two rods wide, bent at the rules' least radius 1.5 t | 0.5 × 6 |
| clip's leaves | opened from closed by the wall (and the inner face's sagitta over the clip's width, δ = 3.11), long enough for the root stress, 6 F h / (b t²), to keep 1.5 under 230 MPa | 49.1 |
| frame rod | `--rod-mm`, its bends on the tube rules' least former for stainless rod, 2 × 1.2 × d | Ø3, R 7.2 |
| riser | the dry rod a fin long enough to keep the profile's material its 10 K margin in still air (below) | dry path 144.6; one 90° bend, 218.7 from the foot, cut 239.0 |
| block | its guide a sixth of the rod looser, its cheeks a rod thick, three rods deep, the rod's end two rods into it | 19 wide, 9 deep |
| ramp | four rods wide, two deep, the probe arm five rods, the ball arm two thirds of it | 12 × 6, 15 + 10 |
| chime | the rod, as long as puts its first mode at the band's geometric middle (√(500 × 5000) = 1 581 Hz) | 36.5 |
| ball | the largest whole millimetre under the largest the ramp allows, less one | 7 |
| screw | the shortest ISO 4762 M3 stock length that reaches the arm and the knob's whole range | M3 × 10 |

## The heat path

Tea is poured at up to 100 °C (boiling at sea level), so every part
touching the tea or the rim is metal: the strip, the frame, the probe, the
clip. The printed parts sit on the frame above the rim, and the heat path
decides their material.

- **The frame rod** (1.4301) is wetted up to the rim, the hottest it can
  be, and held there at 100 °C; the rest convects to the room; its end in
  the block's bore is adiabatic, the bound for an insulating mount. That is
  the pin fin with an adiabatic tip (Incropera §3.6). The film coefficient
  has no default: both ends of Incropera's free-convection range (Table
  1.1, 2 and 25 W/(m² K)) are run, and the design is to the hotter, 2.
- **The riser** is sized by that fin: its dry length is what keeps the
  profile's material (PLA, 55 °C HDT) its 10 K margin, less the fin check's
  own 1 %: 144.6 mm of dry path, so the block stands 142 mm over the
  rim.
- **The check:** the rod's end at 44.16 °C against the fin's 44.22
  (−0.29 % of the rise over the room) at 2 W/(m² K), 22.17 against 22.17
  (−0.80 %) at 25; on CalculiX within 8.3e-8.
- **The block's own study** holds its mount (the rod's bore) at the rod's
  end and convects in still air; a requirement holds `temperature_margin
  at_least 10`. PLA keeps 10.8 K, PETG 23.8 K: printed in **PLA**, the
  profile's material.
- **What the bound leaves out:** steam rising off the tea warms the
  printed parts too, and the app has no air: the margin covers conduction
  only. The block is measured with a probe (later, below).

## The clip

The clip is a 1.4301 sheet-metal part (a base flange over the rim, an edge
flange each side), bent by hand from bought sheet; its sheet shows the
blank (102.8 × 6 mm of 0.5 mm sheet), its flat pattern with the bend lines,
and the bend table (two 90° bends down at R 0.75, K 0.45). The rod's sheet
is its bend table, read from the app's tube schedule: one 90° bend on a
R 7.2 former 218.7 mm from the foot, then the run, cut 239.0 mm.

- **The grip** is the outer leaf opened by the wall: the clip as a U frame
  held along its inner leaf (the rod clamps it), F = δ E I / (h³/3 + h²
  L_b), the leaf's bending and the bridge's turn under F h. The
  interference itself cannot be modelled (no preload, no prescribed
  displacement), so the force is computed: 0.765 N.
- **The linear study**: that force at the leaf's tip, the clip meshed at
  its thickness and its bends at half of it, moves the tip 3.083 mm
  against the wall's 3.113 (−0.95 %); peak 187 MPa against 1.4301's 230
  (beam theory 153 at the root; the peak is in the bend). The same study
  on CalculiX moves the tip the same 3.083 mm; it was refused, its answer
  balancing the in-house stiffness only to 4.2e-5, and that was our check's
  bug: the seven digits ccx prints leave a second-order term on a part
  this soft that its floor did not count
  ([reading the answer back](../../docs/simulation/README.md#reading-the-answer-back)).
- **The contact study** (the `clip on the rim` assembly): the clip meshed,
  its inner leaf held where the rod clamps it, its jaw (the outer leaf's
  tip) pressed onto the vessel's curved outside by the grip, frictionless,
  on CalculiX. The vessel carries 0.7684 N along the grip (0.48 % off: the
  rod's hold takes the rest), 4.4 % of it across, its line 0.24 mm up the
  2 mm jaw, closed over 0.042 mm²; the study balances to 3.3e-9. The
  plan's check -- the clip resting on the vessel alone, its resultant the
  grip on the grip's line to 1e-3 -- is not reachable, and for a reason of
  statics now: the clip touches the vessel at its jaw only, so a pull at
  its inner leaf turns it about the jaw, nothing but the weak springs
  resist that (91 % of the load), and the study is refused, as it should
  be. It was refused before for the springs' stiffness: scaled as `1e-6 E
  L` they were stiffer than a 0.5 mm spring clip and carried 62 % of its
  grip; they are scaled from the part's own stiffness now
  ([a part held by a contact alone](../../docs/simulation/README.md#a-part-held-by-a-contact-alone)).

## The chime

A cantilever of the rod pressed into the block's far face, under the
ramp's lip. Its first mode: 1 564 Hz against Euler-Bernoulli's round
cantilever, 1 581 (−1.06 %), inside the 500 to 5 000 Hz band chosen to be
heard (a report, not a pass). A rod rather than the plan's strip: a strip
wider than it is thick is a plate near its clamp, and Euler-Bernoulli is
the beam's.

## The side cases

Each a project of its own, against its closed form, with a negative
control.

- **Snap-through** (`latch.lob`): a PETG arch latch between two pins, 60
  between them, 1.2 deep and wide (span over depth 50, the `[nonlinear]`
  arch's proportions), eps = 4, a uniform pressure on its crown at
  Timoshenko and Gere's limit (§7.6): it peaks at 0.9985 of it (−0.15 %, a
  limit point), its linear buckling factor 2.19. It does not yield on the
  way: the printed latch snaps before it yields. The control
  (`latch_control.lob`), eps = 1.5, has no limit point and ends at its
  deflection limit (its own rise).
- **Plasticity** (`bracket.lob`): a library-steel L bracket, its foot
  clamped, a load 5 depths off the upright; the upright's span, 12 depths
  between a thickened base and head, carries a uniform moment F a and F
  along it. At 1.25 × the load whose moment is M_p, plastic, at small
  deformation: it reaches its load; the span's outer fibre 0.04828 plastic
  against the bilinear section's 0.04728 (+2.1 %, the library's curve, EN
  1993-1-5 C.6(2), H = E/99); the head left turned 1.028 rad against that
  curvature unloaded and integrated over the span, 1.070 (−4.0 %). First
  yield, though, is the app's peak over the whole bracket: 0.459 of the
  load against the span's 0.516 (−11 %), because the span's ends, where it
  meets its thickened base and head, yield first. With large deformation a
  collapse **is** reported, at 0.695 of the load: the upright bends toward
  its load and lengthens its arm (geometric softening), which the hardening
  does not outrun. The control, under first yield: no plastic strain,
  nothing left.

## What is checked

`build.py --no-render` prints a marker for each (the
[generator test](../README.md#the-big-generators-as-tests) holds them):
the strip check and its control; the bimetal refused; the strip's budget
and the ball; the trigger at the knob's mark and across its range; the
contact relation refused; the fin at both films and on CalculiX; the
block's margin and its material; the clip's grip and the clip on the
vessel; the chime; each side case and its control; `check_print` for each
printed part and the plate; the bill (the four printed parts; the rest
bought); the drawings, each `check_drawing` clean. It ends with
`tea_alarm: every check passed`.

## What the round exposed

- **A thermal load on an assembly** was refused outright; it is now taken,
  uniform, and the strip check is its test (fixed in the round:
  [a thermal load on an assembly](../../docs/simulation/README.md#a-thermal-load-on-an-assembly)).
  A `set_study {thermal_load: {from_study}}` on an assembly dereferenced
  the part document an assembly tab does not have; it is refused now.
- **`probe_study` read part studies only**; it reads an assembly study's
  face by its occurrence now, and a plastic result's strain and residual
  motion ([probing a face](../../docs/simulation/README.md#probing-a-face)).
- **No heat across bodies or occurrences**: the rod-to-block chain of
  bounds is the workaround (the rod's end adiabatic, then the block held
  at it).
- **No air and no steam**: the printed parts' margin covers conduction
  only.
- **No interference preload and no prescribed displacement**: the clip's
  grip is computed by hand.
- **A contact's weak springs** outweighed a thin spring part (`1e-6 E
  L`); they are scaled from the part's own stiffness now
  ([a part held by a contact alone](../../docs/simulation/README.md#a-part-held-by-a-contact-alone)).
  The clip still cannot rest on the vessel by its contact alone: touching
  at its jaw only, it turns about it.
- **A contact relation driven by a sliding pusher** is refused (a contact
  relation's driver is a cam's turn), and **a mate's value cannot be an
  expression** of a variable, and **a motion study drives a mate, not a
  variable**: the ramp is posed by hand, and the sweep is the generator's
  set-and-solve loop.
- **No rigid-body dynamics**: the ball's roll and fall are by hand.
- **No arc-length method**: only the latch's limit point is solved.
- **`first_yield_factor` is the whole part's**: the bracket's thickened
  ends yield first, 11 % before its span; the span's own first yield is
  the closed form's.
- **The library's steel row** had E = 200 GPa against its cited clause's
  210; it takes the clause's 210 now (EN 1993-1-1 §3.2.6), which moved the
  strip check's closed form (n = 210/110.3) and the bracket's plastic
  strains and turns; every check still passes.
- **Brass and stainless were re-cited**; no generator read brass's factor
  of safety or stainless's yield, and stainless parts weigh 1.25 % less
  (7 900 kg/m³ for 8 000).
