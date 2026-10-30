# Simulation dogfood: a tea-ready alarm

A dogfood round for the simulation stack. The object is an alarm that
clips on a mug's rim and rings when the tea has cooled enough to drink:

- A bought, straight thermostatic bimetal strip hangs in the tea and
  straightens as the tea cools.
- Its free end lifts a vertical probe. The probe tips a balanced ramp past
  the edge of its detent at the set temperature.
- A steel ball rolls off the ramp and strikes a chime. A printed dish
  catches it.
- A set screw under the probe sets the trigger temperature.

The design is adapted from Chronova Engineering's tea timer, not copied
from it. It replaces a dial: a threshold mechanism exercises the
kinematics and the statics as well as the thermal and bimetal studies, and
a straight strip in water is exactly Timoshenko's case.

Two small side parts exercise the study types the alarm does not use:

- a snap-through clip (nonlinear);
- a steel bracket loaded past yield (plasticity).

This round breaks the print-only rule: the strip, the frame, the clamp, the
chime and the ball are bought or made from bought metal. The user has
accepted that. The strip and the probe sit in the drink and neither is rated
for food contact; a coating answers that if the alarm is ever built, and the
round does not depend on it.

Generator: `generators/tea_alarm/`. Output, untracked:
`scratch/tea_alarm/`. The layout follows `generators/woodcutter/`:
`ta_spec.py` holds every size and number with no app; `ta_parts.py`,
`ta_mechanism.py`, `ta_fem.py`, `ta_cases.py` (the side parts),
`build.py`, `README.md` and `test.json`.

## The problem

Each item names what the app cannot yet do, or has not yet been asked to.

- **A bimetal cannot be studied.** The solver can already give each tet its
  own material's expansion (`FemSolver.cpp`, `matOf(ti).expansionPerK`),
  and the CalculiX deck writes `*EXPANSION` per material domain
  (`Calculix.cpp`). But both routes to a two-material strip are closed:
  - a part study meshes each body on its own, so two touching bodies in a
    part are not bonded;
  - an assembly study bonds occurrences into one conforming mesh, each with
    its own material, but refuses any thermal load (`AssemblyStudy.cpp`:
    "a thermal study, or a thermal load, is a part study's").
- **Nothing has been designed against hot water.** Tea is poured at up to
  100 °C, the boiling point of water at sea level, which is the design
  temperature (the user is at sea level). Any part touching the tea or the
  rim is at most at the tea's temperature, and while it loses heat only by
  convection to cooler air, its hottest point is that contact. So every
  part touching the tea or the rim must survive 100 °C. That rules out
  every printed material we hold a datasheet for: PLA's HDT is 55 °C,
  PETG's 68 °C, Prusament ASA's 93 °C. These parts are metal. The printed
  parts sit on the frame above the rim, and thermal studies decide their
  material.
- **No threshold mechanism has been designed in the app.** Nothing has yet
  answered "at what temperature does this trip?" through mates, variables
  and statics.
- **The library lacks cited numbers.** Brass's modulus and yield are
  uncited and it has no expansion. Stainless steel's modulus, ν and yield
  are uncited. There is no thermostatic bimetal row.

## The shape of the work

### 1. App: a uniform thermal load on a bonded assembly study

`set_study {thermal_load: {uniform_c, stress_free_c}}` is accepted on an
assembly static study whose participants are meshed and bonded. It works
in-house and on CalculiX, and it combines with `nonlinear` as on a part.

Still refused, each naming why:

- `from_study`, and a thermal study on an assembly (an assembly does not
  solve heat yet);
- beside a contact participant;
- on buckling, modal and topology studies;
- on a member whose material has no expansion coefficient.

The bonded group's per-domain materials already reach the solver. The work
is the gate, and checking that each member's expansion reaches its domain
on both solvers. The input hash takes the members' expansion only under a
thermal load (`HashThermal`, as on a part).

Files: `src/core/fem/AssemblyStudy.cpp`; `src/core/fem/FemStudy.cpp` (the
bonded-group mesh path carrying `SolveOptions::thermal`);
`src/remote/RemoteSimulation.cpp` (the `set_study` describe row); and the
thermal-load control on an assembly study in `src/ui/StudyPanel.cpp`.

### 2. Library: cited rows

Material slugs are saved as strings, so no row here is a format change. In
`src/cad/CADMaterials.{h,cpp}`, each value carries its citation beside it:

| row | values | source |
|---|---|---|
| `stainless_steel` (1.4301), re-cited | E 200 GPa, ν 0.3 (G 76.9 GPa); f_y 230 MPa for cold-rolled strip t ≤ 8 mm (f_u 540); density 7 900 kg/m³. The cited 16e-6/K and 15 W/(m·K) stand | SCI P413, *Design Manual for Structural Stainless Steel*, 4th ed. (2017): §2.3.1 for E, ν and G; Table 2.2 for f_y; Table 2.7 for density, expansion and conductivity. The row today has 193 GPa and 215 MPa, both uncited |
| `brass` → C26000 cartridge brass, re-cited | E 16 000 ksi (110.3 GPa); G 6 000 ksi (41.37 GPa), so ν = E/2G − 1 = 0.333; CTE 19.2e-6/°C (20–300 °C); conductivity 121.2 W/(m·K) at 20 °C; density 8.53 g/cm³. No yield (see below) | Wieland Concast, *C26000* datasheet, "Physical properties provided by CDA" |
| `bimetal_kanthal_94s` (new) | specific curvature k 17.8e-6 K⁻¹ ± 4 %; specific deflection a 9.5e-6 K⁻¹; E 190 GPa at 20 °C; conductivity 12 W/(m·K); density 8.1 g/cm³; linearity range 0 to +200 °C; components 38/7 NiCr steel / 19/7 NiCr steel | Kanthal, *Thermostatic Bimetal Handbook*, catalogue 3-A-1-3, the KANTHAL 94S page (p. 40) and the type table (pp. 17–18) |

Notes on these rows:

- **The bimetal row has no ν and no per-layer data.** Kanthal publishes
  neither. So it is a row for the strip's mass, conductivity and stiffness,
  and a structural study of a body made of it is refused by name. The
  strip's motion comes from the maker's measured constant (§4), not from a
  layer model.
- **Brass's yield.** Concast gives yield for ASTM B927 **rod** only (H01
  205 MPa to H04 345 MPa, under ½"). Brass in this round is a check layer,
  never the strip itself, so the row keeps no yield: the factor of safety
  on brass is refused by name. Moving off the uncited 200 MPa may change
  another generator's brass factor of safety. The landing audit lists any
  that read it.
- **The steel row.** It cites EN 1993-1-1:2005 §3.2.6 for its expansion,
  and that clause gives E = 210 000 N/mm², ν = 0.3. The row's E is 200 GPa,
  which the plasticity curve is written from. This round does not change
  it, since the [plasticity] closed forms move with it. It goes into
  `docs/known-bugs.md` as an uncited modulus that disagrees with the
  clause its row cites.

### 3. The strip check (Timoshenko against the study)

The bought strip has no published layers, so the app's bimetal physics is
checked on a pair whose layers are all cited.

`ta_fem.py` and the tests build a test strip as an assembly: a library
`steel` layer and a `brass` (C26000) layer, each 0.5 mm × 5 mm × 40 mm,
bonded on their shared face. One end face is clamped, and the strip takes a
uniform thermal load from 20 to 100 °C (the pour).

Timoshenko's curvature (S. Timoshenko, "Analysis of bi-metal thermostats",
J. Opt. Soc. Am. 11, 233–255, 1925; the Kanthal handbook's eq. [1] is the
same) is:

    1/ρ = 6 (α₂ − α₁) ΔT (1 + m)² / ( h [3 (1 + m)² + (1 + m n)(m² + 1/(m n))] )

with m = t₁/t₂ = 1 and n = E_steel/E_brass = 200/110.3 = 1.813. That gives
1/ρ = 8.448e-4 /mm, a tip rotation L/ρ = 0.03379 rad and a tip deflection
L²/2ρ = 0.6758 mm.

- **The check:** the free end's motion from `probe_study` (as a rigid body)
  within 2 % of these. The clamp holds the layers across the width; at
  L/b = 8 that stays near the root, and the margin is measured.
- **CalculiX:** the same mesh, to the agreement the thermal `[calculix]`
  cases hold.
- **Control:** both layers steel. The strip does not curl: tip rotation
  under 1e-6 rad.
- **Tests:** a hand-made two-domain mesh in `tests/test_fem_thermal.cpp`
  and `[thermal][calculix]`, so the path is tested without a generator.
  The generator re-checks it through the RPCs.

### 4. The alarm

**The strip** is KANTHAL 94S, straight, cut from bought strip of width b
and thickness s. These are `--strip-mm b s`, a required argument read off
the order. Why this grade:

- Kanthal recommends it, and only it, for water: "both components consist
  of NiCr steels. KANTHAL 94S is suitable for use in water and steam"
  (handbook §4, Corrosion Resistance and Protection).
- Its linearity range, 0 to 200 °C, covers the tea.
- It is roll-bonded clad, so it bends as one strip. A riveted pair, as in
  the inspiration, slips between its rivets. No model here holds that, so
  no closed form would answer it.

The strip hangs in the tea, below the shallowest fill. The part above the
water is the stainless frame, so the whole active length sits in water. In
steady state it is at the tea's temperature: no air model is needed, and
the study chain stops at the tea.

- **Tip travel.** A = a (T − T₀) L² / s (eq. [9], p. 11, with A ≪ L). At
  s = 0.3 mm and L = 50 mm that is 0.079 mm/K, and 3.56 mm from 100 to
  55 °C. L derives from the vessel's depth below the shallowest fill
  (`--vessel-mm diameter depth`, required). The high-expansion side faces
  down, so cooling lifts the tip.
- **Force costs temperature.** From eq. [16] (p. 100), a tip force F takes
  4 F L³ / (b s³ E) off the travel, which is 4 F L / (a E b s²) in
  temperature: 246 K per newton at b = 5, s = 0.3, L = 50. Two
  consequences:
  - A constant force is an offset the set screw takes out.
  - Friction at the probe guide and the ramp pivot is not constant, and it
    becomes the trigger's scatter. `ta_spec.py` sizes b and s against
    Kanthal's minimum-volume rule (eq. [20], T_F = T_A = 0.5 at the most),
    and reports the K per mN the strip leaves.
- **Strip stress** at the probe's force: σ = 6 F L / (b s²) (eq. [23],
  p. 102). The handbook's permissible stress is a graph per grade with no
  values in its text, so `ta_spec.py` reports σ against the handbook's
  general 400–500 MPa yield (§2.1) and says that is the bound it used.

**The trigger mechanism** is modelled in the assembly:

- **The probe** is a stainless rod on a slider mate. Its height is an
  expression of the variable `tea_c`, through A(T) and the set screw's
  `gap_mm`.
- **The ramp** is printed, on a revolute. Its underside is driven by the
  probe's tip through a `contact` relation.
- **The ball** sits in a detent whose exit lip has slope angle φ. It is
  released when the ramp has turned by φ.

So the trigger temperature is where the probe has lifted the ramp by φ.
Two numbers feed the statics:

- the ramp's static moment about its pivot, with the ball in the detent;
- the ramp's centre of mass, from the app's mass properties;
- the ball's mass, weighed on arrival. Before that, `ta_spec.py` reports
  the largest ball that keeps the offset within the set screw's range.

They give the probe force F, and F gives the offset above. The generator:

- sweeps `tea_c` from 100 °C down (set the variable, solve, read the
  ramp's angle), and reports the trigger temperature;
- marks the set screw's knob in °C from the screw's pitch and A(T);
- checks that every `gap_mm` in the knob's range triggers between 40 and
  70 °C, the range the user's inspiration targets around 54–55 °C.
  `--ready-c` is a required argument.

The ball's path off the lip is not simulated (no dynamics in the app). A
solid sphere rolling off the lip leaves at v² = (10/7) g Δh. The chime sits
under the landing spread across the knob's range, and the dish under the
chime.

**The frame and the clamp:**

- **The frame** is 1.4301 stainless rod, bent. It carries the strip below
  the water and the printed parts above the rim.
- **The clamp** on the rim is a 1.4301 sheet-metal spring clip from the
  sheet-metal features, drawn with its flat pattern. It is the clamp's
  material because it touches the rim (100 °C), and the round's cited
  stainless is the one metal with E, ν, yield, expansion and conductivity
  all sourced.

**Thermal: the frame rod, covering the old heat-sink case.** The frame's
wetted length is held at 100 °C (a `temperature` load on a `region`), and
the rest convects. The face where the printed block mounts is left
adiabatic, the upper bound for an insulating mount. The film coefficient
has no default: the build runs both ends of Incropera's free-convection
range (Table 1.1, 2 and 25 W/(m²·K)) and designs to the hotter.

- **The check:** the 1-D fin with an adiabatic tip (the closed form the
  `[thermal]` fin case is held to), within 1 %, and `solver: calculix` on
  the same mesh.
- **Then:** the printed block's own thermal study holds its mount face at
  the rod's tip temperature and convects. `temperature_margin at_least 10`
  decides PLA or PETG for the ramp, the probe guide, the knob and the dish,
  and the build prints which one it chose and why.
- **What the bound leaves out:** steam rising off the tea warms the
  printed parts too, and the app has no air. The README says the margin
  covers conduction only. Phase 6 measures the block with a probe.

**Contact: the clamp on the rim, covering the old contact case.**

- **The grip** comes from opening the clip by the rim's wall. The
  interference cannot be modelled, so the generator computes the clip
  leaf's force from beam theory, F = 3 E I δ / L³, with δ the rim wall
  (`--vessel-mm` again).
- **A linear study** of the clip under that force must deflect δ within
  2 %.
- **The spring stress** σ = 3 E δ t / (2 L²) must stay under 1.4301's
  230 MPa.
- **The contact study:** an assembly study, the clip meshed and resting
  frictionless on the vessel (a rigid occurrence, curved) under that grip.
  The vessel's resultant must equal the grip and lie on its line, to 1e-3.
  The closed area must lie along the jaw.

**Modal: the chime.** The chime is a 1.4301 strip, a cantilever under the
lip. Its first frequency is checked against Euler–Bernoulli,
f₁ = (1.875² / 2π) √(E I / (ρ A L⁴)), within 2 % (the modal benchmark
`[fem]` already holds). The build reports whether f₁ lies between 500 Hz
and 5 kHz. That is a band chosen to be heard, not a cited one, and it is a
report, not a pass.

**Print, plate, bill and drawings:**

- `check_print` on the P1S profile for each printed part, and one plate.
- The bill: the strip, the stainless rod, the ball and the clip blank as
  bought; the rest printed.
- Drawings for bending by hand from bought stock: the clip's flat pattern
  with its bend lines, and the frame rod's bend table (straight lengths,
  angles and the order to bend them in).

### 5. Side cases (`ta_cases.py`, one `.lob` each)

| case | part | study | verified against |
|---|---|---|---|
| snap-through | a shallow PETG arch latch between two pins, at ε = 4 | static, `nonlinear {large_deformation, snap_through}`, uniform pressure on the crown | Timoshenko and Gere §7.6's limit, as `[nonlinear]` holds it (5 %); then `first_yield_factor` against the peak (does a printed latch snap before it yields?) |
| plasticity | a library-steel L bracket: the foot clamped, a load at arm a on the other leg, so the upright carries a uniform moment F·a plus F axially | static, `nonlinear {plasticity}` at 1.25 × the M_p load; then again with `large_deformation` | first yield at F/A + F a / (b h²/6) = f_y against `first_yield_factor` (2 %); past it, the bilinear section's moment–curvature under the library's cited curve (EN 1993-1-5 C.6(2), H = E/99) for the outer fibre's plastic strain in the uniform-moment span (5 %), and `residual_displacement` from that curvature integrated over the upright |

The bracket stays a side case because nothing in the alarm yields. It also
answers a question the docs leave open: whether a hardening steel ever
reports a collapse. At small deformation the run must reach its load. With
`large_deformation`, the round records whether a collapse is reported.

Each case ends with a negative control:

- the arch at ε < 2, which has no limit point and must end at its
  deflection limit;
- the bracket under its yield, with nothing left after unloading.

## What the round is expected to expose

Each item is either fixed in the round or entered in `docs/known-bugs.md`
or in `#not-done`, with the case that shows it:

- **A thermal load on an assembly** (§1, fixed in the round).
- **No heat across bodies or occurrences.** The rod-to-block chain of
  bounds is the workaround.
- **No air and no steam.** The printed parts' margin covers conduction
  only.
- **No interference preload and no prescribed displacement.** The clamp's
  grip is computed by hand.
- **A contact relation driven by a sliding pusher.** The woodcutter's
  relations are driven by rotating cams. If a translating driver is not
  accepted, that is the gap, and the generator solves the ramp's angle by
  hand as the stand-in.
- **A sweep over a variable.** If a motion study cannot drive `tea_c`, the
  generator's set-and-solve loop is the stand-in, and the gap is recorded.
- **No rigid-body dynamics.** The ball's roll and fall are computed by
  hand.
- **No arc-length method.** Only the latch's limit point is solved.
- **A library row with no ν.** The bimetal row: check that a structural
  study on it is refused by name.
- **The steel row's modulus** against its cited clause (§2).

## Verification

**The app:**

- the §3 cases in `[thermal]` and `[thermal][calculix]`;
- the assembly refusals that remain (`from_study`, a thermal study, beside
  contact) in `tests/test_fem.cpp` and `refused_calls_smoke`;
- the re-cited rows in the library test in `test_fem_thermal.cpp`;
- the headless suite.

**The generator:** `build.py --no-render` prints a marker for each of:

- the strip check and its control;
- the tip travel and the K/mN scatter;
- the trigger temperature at `--ready-c` and across the knob's range;
- the fin against its closed form, at both film coefficients;
- the printed block's margin and the material chosen;
- the clip's deflection, stress and contact statics;
- the chime's f₁ against Euler–Bernoulli;
- each side case against its closed form, and its control;
- `check_print`, and the bill.

It ends with `tea_alarm: every check passed`. `test.json` lists the
markers as the generator test (generators/README.md#the-big-generators-as-tests).

**Phase 6 (later, with the strip in hand):**

- the strip's tip travel in a water bath against a thermometer, measuring
  the 94S constant on the part as cut;
- the trigger temperature against a probe thermometer in a cooling mug,
  ten times, for its scatter against the K/mN prediction;
- the printed block's temperature.

The numbers go into the generator's README beside the predictions.

### On landing

1. `docs/simulation/README.md` gains the thermal load on an assembly, the
   re-cited rows in its material table, and the strip check in its
   Measured tables. `#not-done` loses the assembly thermal load.
2. The RPC describe rows and the materials docs are audited for the
   re-cited brass and stainless.
3. The plan goes.
