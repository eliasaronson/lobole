# Woodcutter automaton

A print-only dogfood round: a hand-cranked automaton in a printed box. The
crank turns the camshaft directly, one chop per crank turn. Two cams lift
followers that fall back by gravity, and the followers move two figures: a
woodcutter whose axe rises slowly and drops onto a log, and a bird that
pecks on its own cam, phase-shifted from the chop so the two motions take
turns. Everything is printed, the box included, on the P1S profiles. Nothing
is bought, there is no motor, and there is no CAM.

The design targets:

- **The box's inside width is about 120 mm.** It fits a P1S plate as flat
  panels and leaves room for two cams side by side on one shaft. It is the
  generator's one size argument; every other size is derived from it.
- **A comfortable hand turn is about 1 rev/s (60 crank rpm).** At one chop
  per turn that is 60 cam rpm. The followers must stay on their cams with a
  margin of two: `liftoff_rpm ≥ 120`, and every drop lands within its dwell
  at 120 rpm.
- **Followers are rods with rounded knife tips.** No roller and no pin.
  Printed rollers on printed pins are a later variant of the same
  generator, which the contact relation and the cam feature already serve.
- **The box is printed panels pegged together.**

Design and verification come first, in the app, with no print and no
measurement. Only the last phase prints and measures.

The round needs app work first (phase 1). An automaton is cams and
followers, and the app cannot model a follower from the shapes it touches.

## The problem

- **A cam relation is a table computed outside the app.** `Relation::lift`
  (`src/core/Occurrence.h`, the cam kind) maps the driver's angle to the
  follower's lift. `CamLift` interpolates it (`src/core/Occurrence.cpp`),
  forward only. Every generator that uses one works out the table in Python:
  `pes_spec.py`'s `table`/`plunger_lift`, `engine_spec.py`'s `cam_table`,
  `clock_spec.py`'s `cam_table`, the watch's rides. The generator then draws
  the cam to match. `pes_spec.py`'s `lobe_profile` builds the roller
  envelope and `pes_parts.py` sketches it as a closed spline. The relation
  and the shape are two copies of one intent. Edit the cam and the follower
  still follows the old table. The motion study does not notice, because
  nothing in the study touches. It only finds a clash where the table is
  higher than the cam, and finds nothing where the follower floats above it.
  The roadmap lists this ("A follower driven by contact",
  docs/roadmap.md#3-assemblies-and-cross-part-design).
- **Nothing cuts a cam from a motion.** No feature makes a profile from a
  lift law. A sketch spline is the only way to draw one, and it has at most
  255 poles. A 1° table of 361 points is refused ("a spline has at most 255
  poles"), so a generator thins its table before drawing it.
- **Nothing says whether gravity keeps the follower on the cam.** A gravity
  follower leaves the cam when the profile falls away faster than the
  follower can fall. The app has no gravity in assemblies (`kGravity` in
  `src/core/Conventions.h` is used by FEM only). It has no speed and no time
  in a motion study (`Sample::t` runs 0..1), and no pressure angle. A snail
  cam's drop is a free fall by design. The axe chop is one. Nothing can say
  whether the follower lands before the next rise reaches it at the speed a
  hand turns the crank.
- **No motion path measures distance.** A motion study reports clashes and,
  in mesh mode, `touching` pairs. Exact mode asks
  `Measure::ContactBetween(..., wantDistance=false)` for a volume only. The
  `clearance` requirement's `MeasureClearance` (`RemoteRequirements.cpp`) is
  the one place that measures a gap over a study's poses. Nothing can show
  that a follower stays on its cam: touching at every sample, never a clash,
  never a gap.

## Phase 1: app work

Phase 1 has three pieces: a relation solved from contact, a feature that
cuts a cam from a law, and a check that compares the two and adds gravity
and speed. With all three, a design can go from the motion, to the cam cut
from it, to the follower driven by that cam's real shape, to a check that
the follower stays on.

### 1a. The `contact` relation (size: L)

A new `RelationKind::Contact`. Mate `a` (the cam's revolute or cylindrical
rotation) drives mate `b`, the follower's DOF (a slider's offset, or a
revolute's or cylindrical's rotation). It does this through the geometry of
two occurrences, not a table:

- **`cam`**: the occurrence mate `a` turns. Derived as the moving side of
  `a`. It is refused, naming the candidates, when `a` moves more than one
  occurrence that could touch.
- **`follower`**: the occurrence that touches the cam. Derived as the moving
  side of `b`. It may be any occurrence rigidly carried by `b`'s moving
  side, such as a roller on its own pin, whose spin does not change its
  outline. A part with several bodies takes `cam_body`/`follower_body` by
  feature id. Without them it is refused, naming the bodies.
- **`toward`** (`positive|negative`): the direction the follower is held in,
  meaning the way it moves until it touches. When omitted it is derived once,
  at creation, from gravity, as the sign of the generalised gravity force on
  `b` at the current pose (below). It is refused when that force is too
  small to choose a sign, such as a horizontal slider, naming `toward`.
  `toward` is stored. It is not re-derived per pose, so a file re-solves to
  the same placement. A pose where gravity stops holding the follower is
  reported by the check (1c). It never changes the kinematics.

**At every pose, `b` is the value where the follower first touches the cam,
moving from clear in the `toward` direction.** It is computed as follows.

1. **The planar case, and only that case.** The cam's axis is mate `a`'s
   connector Z. A turning follower's axis must be parallel to it, within the
   tilt that moves the contact by less than the contact tolerance across
   the shared width. A sliding follower's direction must be
   perpendicular to it. Anything else is refused at `add_relation`, naming
   the case it is (a barrel, face or conical cam). Every printed disc cam is
   this case. It is also the case the 2D solve below makes exact.
2. **The outlines.** Each body is sectioned perpendicular to the cam axis, at
   the middle of the width the two bodies share along that axis. It is
   refused when they share none ("the follower does not overlap the cam
   along its axis"). The cam's outline is taken in the cam's own frame, and
   the follower's in `b`'s moving frame, as `Geom2d` curves from the
   section's edges, not a polyline. A chamfer or a draft that intrudes into
   the shared width is not modelled. The exact 3D check in the motion study
   catches it as a clash (verification, below), so it cannot pass silently.
3. **The first touch.** For driver angle θ, the cam outline is turned by θ.
   The follower outline is carried along its own path, a translation along
   the slide or a rotation about its pivot, both in the shared plane. The
   travel to first contact is the smallest travel at which the two outlines
   meet. Coarse: both outlines are sampled at a chord error derived from the
   smaller outline's size. Each follower sample is carried along its path
   against the cam's segments, and each cam sample along the reverse path
   against the follower's segments. A box tree keeps this fast. Exact:
   `Geom2dAPI_ExtremaCurveCurve` is run between the two edges found, and
   Newton is run on the travel until the gap is under the contact tolerance.
   The approach is always from the clear side, so the answer is never an
   overlap. The tolerance is derived from the smaller outline's size (1e-5
   of it, about 0.4 µm on a 40 mm cam), not a fixed value. The contact point,
   the outline normals and curvatures there, and a second contact when two
   points touch within the tolerance (the follower bridges a hollow) are
   kept with the answer.
4. **A table, built once per geometry.** The kinematics needs a smooth
   function of θ. The loop row differentiates the relation by finite
   differences (`AssemblyLoopSolve.cpp`'s cam row). A drag needs it inside a
   frame. So the contact solve fills a periodic table over one turn,
   bisected adaptively until the monotone cubic `CamLift` already uses
   reproduces an exact midpoint solve within the contact tolerance. That
   bound holds everywhere except at a drop (below), and the table records
   it. The solver then reads the table exactly as it reads a cam's:
   `RelationValue` in a tree and the cam row in a loop. A contact relation
   is propagated forward only, as a cam is.
5. **A drop is a step, not a slope.** A snail cam's step face makes the
   travel jump. Adaptive bisection that keeps halving one interval without
   converging, down to a width derived from the tolerance, marks a drop. The
   table then holds a discontinuity, which the interpolation steps across
   without smoothing it. The step is stored as the drop's angle, its height
   and the side it falls to. The kinematics shows the follower landed. The
   check (1c) says whether it can land in time.
6. **Cache, not file.** The table is derived state, cached the way contacts
   are (`collide::ExactContactCache`). It is keyed on both bodies'
   `CADShape::GeometryId`, the relative frame of the two mates' bases, and
   the configuration. A motion run's copied documents keep their identities
   (`Document::CopyWithGeometry`), so the run hits the cache. An opened
   project rebuilds the table on its first solve. The file stores the
   relation (mates, occurrences, bodies, `toward`) and nothing derived.
   The mate values are stored as today, so a file re-solves to identical
   placements.
7. **What the solve needs that it does not have today: geometry.**
   `PropagateRelations` (`src/core/AssemblySolve.cpp`) works on mate values
   only. It gets the same access to the placed parts' bodies that a
   thread-backed screw uses to re-read its Thread feature on every solve.
   It also gets one invariant, checked at creation and on every solve: the
   bases of mates `a` and `b` are rigid to each other. Their relative frame
   must not depend on any free DOF, as when both sit on the box. The
   relation's `error` reports it otherwise: "the cam's axle and the
   follower's guide must not move relative to each other". A follower driven
   by another follower (an arm resting on a rod resting on a cam) is
   therefore not built. The woodcutter joins its rod to its arm through a
   link, which is a mate loop and is already solved.
8. **An error, never half-applied** (relations.md#enforcement). No contact
   in a whole turn: the follower can travel to infinity, or past a stop the
   outline cannot reach. Outlines that overlap at θ = 0 whichever way the
   follower moves. A body gone. Each is reported in the relation's `error`
   and the relation is skipped.

**Surface.** `add_relation {type: "contact", a, b, cam?, follower?,
cam_body?, follower_body?, toward?}`. Refused: `ratio`, `phase`, `lift`,
`rise`, or a driver DOF that is not a rotation. Each refusal names what a
contact relation takes. `set_relation` changes `toward` and the bodies.
`list_relations` reports, per contact relation, the table's size, its
interpolation bound, the drops (angle, height), and `bridges` (θ ranges
with two contact points). The `RemoteAssembly.cpp` MethodDoc row names the
units. The SDK gets `Asm.contact(a, b, …)`. The occurrence panel's Add
relation window gets the kind. Contact is the first relation that is picked
from parts, not typed, which suits the window. Serialized in the
relation's file under `relations/` (`SerializeAssembly.cpp`'s
`kRelationKinds`, `RelationToJson`, `RelationFromJson`). Format `vNEXT`,
golden `tests/golden/assembly/vNEXT-contact-follower.json` + `.expect.json`,
with a row in format.md's *Pending changes*.

### 1b. The `CamProfile` feature (size: M–L)

A part feature that cuts a disc cam from a follower motion. It is the
inverse of 1a.

- **Placement.** A cylindrical face (the shaft or hub the cam goes on) gives
  the axis, a seed for the base radius, and the width. A Frame can be given
  instead. With nothing picked, the GUI tool refuses visibly ("pick the
  shaft or hub the cam goes on"), per the `defaults/*` rule.
- **The motion.** One of:
  - `lift`: a table in the cam relation's form,
    `[[angle_deg, lift_mm | lift_deg], …]`;
  - `segments`: `[{to_deg, rise_mm | rise_deg, law:
    cycloidal|harmonic|poly345|dwell|drop}]`. `drop` is a step: a snail
    cam's face, radial for a sliding follower and along the follower's arc
    for a turning one.

  A table that does not wrap is refused, the same way the cam relation
  refuses one. So is a segment list that does not close a turn, or a
  `rise_deg` on a sliding follower.
- **The follower it is cut for.** `follower: {kind: roller|flat|knife,
  radius_mm (roller; a knife's tip radius), motion: slide|turn,
  offset_mm (slide: the line's offset from the axis), pivot_mm and arm_mm
  (turn: the pivot's position and the arm to the contact)}`. `turn`
  (cw|ccw) is the sense the cam is driven in.
- **Built exactly, not through a sketch.** The pitch curve is computed per
  degree of θ from the law and the follower's kinematics. The profile is its
  envelope: the pitch curve offset by the roller radius, the envelope of the
  flat face's lines, or the knife's tip path offset by its tip radius. It is
  fitted with `GeomAPI_Interpolate` as one periodic B-spline, as `Curve3D`
  FitSpline does (`CurveFeature.cpp`), and its deviation from the computed
  envelope is checked and reported. It is then extruded to the width, as a
  new body or added to the hub. The 255-pole limit is the sketch's and does
  not apply. A drop makes two edges, the step face plus the spline, not one
  smooth curve.
- **Undercut is refused, with its angles.** A roller cusps where the pitch
  curve's convex radius of curvature is under the roller radius. A flat face
  cusps where `R_base + s + s''` ≤ 0. The feature fails, naming the θ ranges
  and the smallest base radius that clears them. It does not build a cusped
  cam. The report gives the flat face's required width (the range of `s'`),
  the maximum pressure angle and its θ, and the profile's smallest convex
  and concave radii.
- **Defaults derived from the pick.** The base radius is the picked
  cylinder's radius. The width is that face's length along the axis. The
  rise is half the base radius. The roller radius is a third of the base
  radius. The lobe is rise, dwell, fall, dwell, cycloidal over
  120/60/120/60°. All are scale-free against the hub, so the tool changes
  the document with its defaults (docs/ui/tool-sessions.md#defaults-and-refusals).
- **The cam relation reads it.** The cam relation gets
  `profile: {part, feature}` as the alternative to `lift`. On every solve it
  reads the feature's motion, as a thread-backed screw reads its pitch. A
  design can then animate its intended motion before it has a follower
  shaped to the contact.

Code: `src/core/features/CamProfileFeature.{h,cpp}` (the class, `Execute`,
`Report`) with the law and envelope math in
`src/core/CamProfileMath.{h,cpp}`, so 1c can evaluate the designed motion
without a regen. Then follow the Worm feature's checklist:
`SerializeFeatures.cpp` (`kFeatureTypes`, `kTypeKeys`, `FeatureToJson`,
`FeatureFromJson`), the get_state report (`RemoteDispatch.cpp`,
`RemoteModeling.cpp`), `ui/CreateActions.cpp`, `ui/Commands.cpp`,
`ui/FeatureEditor.cpp` (`kToolSessions`), `ui/FeatureTreePanel.cpp`, a GUI
test in `app/guitests/GuiTestsModeling.cpp`, `python/lobole/{sdk,rpc,mcp}.py`,
`tests/test_cam_profile.cpp`, `test_regen_cache.cpp`. Golden
`tests/golden/vNEXT-cam-profile.json`. The name avoids `src/core/cam/`,
which is machining. Docs say "cam (mechanism)" wherever both could be
meant.

### 1c. The follower check (size: M)

`check_followers {study, speed_rpm?, relations?}` runs over a motion study's
stored samples. It is answered from a stored run, as `leave_at` is. Per
contact relation it reports the following.

- **`touching` at every sample.** An exact distance between the follower and
  the cam at the sample's poses, through `Measure::ContactBetween(...,
  wantDistance=true)` on the pair only. That makes it the 3D check of the 2D
  solve. A gap over the contact tolerance, or any overlap volume, is a
  failure that names the sample. The study's own exact clash run must also
  list the pair as touching, not as a clash, at every sample.
- **`deviation_mm` | `deviation_deg`.** When the cam was cut by a
  `CamProfile` feature, this is the realised table against the feature's
  designed motion, as the maximum and its θ. This is where the
  design-to-verify loop closes. It catches a roller bridging a hollow, a
  follower that is not the one the cam was cut for, and an edited cam.
- **Pressure angle.** The angle between the contact normal and the
  follower's direction of motion at the contact point: the slide direction,
  or the tangent of the arc about the pivot. It is reported as the maximum,
  its θ, and per sample. The limit is the caller's (`max_pressure_angle_deg`
  or a requirement). With none stated, the reply says `unchecked`. It does
  not apply a default limit.
- **Curvature.** The cam's convex and concave radii at each contact against
  the follower's own radius at its contact point, read from the follower's
  outline, so a roller, a flat and a knife are not declared. The check
  reports a `bridges` range (concave radius under the follower's) and the
  margin.
- **Gravity, and the speed at which contact is lost.** Let q be the
  follower's DOF. It computes:
  - The generalised gravity force, Q(q) = Σᵢ mᵢ g·∂xᵢ/∂q.
  - The generalised inertia, M(q) = Σᵢ (mᵢ |∂xᵢ/∂q|² + ωᵢᵀ Iᵢ ωᵢ), with ωᵢ = ∂(orientation)/∂q.

  Both sums run over every occurrence whose pose depends on q: the follower,
  and the arm and figure it moves through a loop. The partial derivatives
  come from finite differences of the solve, with q pinned and the cam left
  out. Masses and inertias come from the occurrences' materials and printed
  mass (docs/tolerances/README.md#printed-mass). At a constant input speed
  ω, q̇ = q′ω and q̈ = q″ω², with q′ and q″ from the contact table. Contact
  holds while (M q″ + ½ M′ q′²) ω² ≥ Q in the lift sense. Where the bracket
  is negative, contact is lost above ω = √(Q / bracket). The minimum over
  the turn is **`liftoff_rpm`**, which needs no speed as input. It is the
  speed above which the follower leaves the cam, given in the study driver's
  rpm, through the driver-to-cam ratio read from the samples. The check
  reports:
  - Where Q pushes the follower away from the cam (over centre): `held: false`
    and the θ range. Contact is lost at any speed.
  - At each **drop**: the fall height, the free-fall time from M and Q, and
    the turn the cam makes meanwhile at `speed_rpm` (or the speed at which
    the follower lands on the next rise instead of the dwell, `drop_rpm`).

  Worked numbers for the woodcutter's scale:
  - A 10 mm cycloidal fall over 90° of cam needs at most 4.0 m/s² at a cam
    speed of 2 rev/s, well under g. The same fall over 45° needs 16 m/s² and
    leaves the cam above about 94 cam rpm.
  - A 10 mm free drop takes 45 ms, which is 32° of cam at 2 rev/s. The dwell
    after the drop must be longer than that.

  Friction in the guide is not modelled, so `liftoff_rpm` is an upper bound,
  and the docs say so. The last phase measures the real one.
- **Requirement.** A new measure kind, `follower`
  `{relation, study, quantity: liftoff_rpm|pressure_angle_deg|deviation_mm|gap_mm}`,
  in `req::kMeasureKinds` (`src/core/Requirements.h`, `Requirements.cpp`
  `Specs()`), measured in `RemoteRequirements.cpp` from the stored run.
  `verify_requirements` and `sweep` then hold "the axe never leaves its cam
  under 120 crank rpm" and "pressure angle ≤ 30°" against variable edits.

Code: `src/core/FollowerCheck.{h,cpp}` (pure: the table, the samples, the
mass properties in, a report out); the RPC in `src/remote/RemoteMotion.cpp`
with its MethodDoc row (units in the names; `tests/rpc_units_gate.py`);
`tests/test_follower_check.cpp`; a smoke in `tests/`.

### How phase 1 is verified (real numbers, no print)

- **Against the closed form.** `tests/test_contact_relation.cpp`:
  - A circle of radius R offset e from its axis (an eccentric), under a flat
    follower: lift = e(1 − cos θ) exactly. A roller of radius r on the same
    eccentric: the pitch-circle formula. The table must match both to the
    contact tolerance at 1000 random θ, not only at its nodes.
  - A rocker on an eccentric: the closed-form angle.
  - Pressure angle against tan φ = (s′ − e)/(s + √(R_b² − e²)).
- **The loop closes.** A `CamProfile` cut for a roller from a cycloidal law,
  driven through a contact relation by that roller, has `deviation_mm`
  under the feature's reported fit deviation. The cam relation reading the
  same feature puts the follower at the same pose at every sample.
- **It follows an edit.** Grow the cam's base radius by a variable edit. The
  follower rises by the same amount at the dwell, the table is rebuilt
  (cache miss), and nothing else is recomputed.
- **The 3D check agrees.** An exact motion study over the pair lists it
  `touching` at every sample and never as a clash. A cam given a chamfer
  that reaches the contact makes the check fail at the samples where it
  touches.
- **Refusals.** A crossed-axis follower. A horizontal slider without
  `toward`. A follower that never touches. Bases that move relative to each
  other. Undercut. Each refusal names what is accepted, in its own response
  (`refused_calls_smoke` covers the RPC).
- **Gravity.** A cycloidal fall with the follower's M and Q known by hand
  (one slider, one material): `liftoff_rpm` matches √(g β² / (2π h)) within
  1%. A rocker past vertical reports `held: false`.
- **Cost.** One table build is a single 2D job. Measured for this plan on
  an offscreen instance: a cam (a 180-point spline lobe, 6 mm wide) against
  a Ø8 roller costs 75–100 ms for one exact 3D `measure`, boolean included,
  and 5 ms off the meshes at 0.11 mm. A table of several hundred exact 3D
  queries would cost a minute. That is why the solve is 2D and the 3D check
  runs only at the study's samples. The plan's bound: the 2D table builds in
  under 100 ms for the woodcutter's cams (`tools/bench.py` workload), a drag
  frame reads it at no measurable cost, and the 48-sample check costs about
  48 exact pair distances per relation.

Docs when it lands: `docs/assemblies/relations.md` gets a `## Contact`
section (and the kinds table). `docs/assemblies/README.md#relations` and
`#not-built` lose "contact dynamics" where it no longer applies.
`docs/modeling/` gets the CamProfile page. `docs/requirements/README.md#measures`.
The roadmap entry goes.

## Phase 2: the mechanism (size: M)

`generators/woodcutter/` (`build.py`, `woodcutter_spec.py`,
`woodcutter_parts.py`, `README.md`, `test.json`). Output goes to untracked
`scratch/woodcutter/`: `woodcutter.lob`, a receipt `woodcutter.json`, and the
STLs and print job from phase 3.

- **Sizes come from one number, the box's inside width.** It is a
  generator argument. Everything else is derived from it and from the print
  rules (the profile's minimum wall, the coupon's clearances):
  - the camshaft's diameter;
  - the cams' base radii, set by the pressure-angle limit through
    CamProfile's report;
  - the follower rods and their guide bores, at the print clearance that
    `print_allowance` gives for a sliding fit;
  - the figures' scale.
- **The camshaft** turns on revolute mates in bores in the box's end walls.
  Each cam is a `CamProfile` keyed to the shaft by a printed flat:
  - the axe cam: a slow rise over most of a turn, then a `drop`;
  - the bird's cam: a cycloidal rise and fall, phase-shifted on the shaft so
    the bird pecks while the axe is rising, not as it drops. The two
    motions are two steps of one `SequenceStep` timeline, which tests the
    sequence checks on overlapping and alternating motions.
- **Followers** are printed rods with rounded feet (knife followers with a
  tip radius, so no roller and no pin to buy), sliding in guide bores in the
  box's top. Each is driven by a `contact` relation with `toward` derived
  from gravity.
- **Figures.** The woodcutter's arms turn at the shoulder on a revolute
  fixed to the figure, which is fastened to the box top. A link pinned to
  the rod's top and to the arm makes a mate loop (slider, three revolutes),
  which `AssemblyLoopSolve` closes. The log is fastened. The bird's body
  turns on a revolute at its feet and is tipped forward by its own rod the
  same way, so it pecks.
- **Checks in the receipt** (all numbers, all from the app):
  - an exact motion study over one crank turn (48 samples plus every drop's
    neighbourhood) is clean, with each follower–cam pair touching at every
    sample;
  - `check_followers`: `liftoff_rpm` per follower, the maximum pressure
    angle, `deviation_mm`, and the axe drop's fall time and `drop_rpm`;
  - the axe's head at the bottom of the drop is a stated clearance above the
    log (a `clearance` requirement over the study), so the blade meets the
    log without striking it;
  - requirements: `follower liftoff_rpm ≥ 120` for each follower (twice the
    60 rpm hand turn), `pressure_angle_deg ≤` the limit, `motion_clean`,
    and each drop's `drop_rpm ≥ 120`.
- **Negative controls**, as the hinge box keeps them:
  - the axe cam with its drop dwell shortened below the fall time must fail
    `drop_rpm`;
  - a cam with too small a base radius must be refused for undercut;
  - a follower guide moved off the cam's axis plane must be refused.

Verified by `test.json` (the generator test harness) and by the receipt's
numbers matching the closed forms for the eccentric cam within the contact
tolerance.

## Phase 3: the box, the crank, the plates (size: M)

- **The box** is printed as panels that print flat and peg together.
  They use printed pegs, not glue or screws, sized by the coupon's press
  fit. The end walls carry the camshaft bores. The top carries the guide
  bores and is the figures' stage. Teardrop holes are used where a bore
  prints sideways (`RegenContext::printUp`).
- **The crank** is a printed handle on the camshaft's end, outside an end
  wall: one chop per crank turn needs no gear pair. The handle turns on its
  own revolute, and its radius is derived from the box (a stated fraction
  of its depth).
- **Print plates.** Every part is laid out on P1S plates by
  `export_print_job`, a few plates in all. `check_print` must pass each
  plate, stacking included. Each part has its print direction chosen so its
  loaded member has its layers across the load: the rods along their length,
  the camshaft lying down. `split_for_printing` is used where a panel is
  over the bed. The slicer's time and filament come from the installed
  slicer, not the app.
- **An assembly sequence** puts the box together in printable order
  (`check_assembly_sequence` with insertion), and the shaft goes in through
  its bores.
- Verified: `check_print` clean on every plate; the sequence check clean;
  `check_followers` and the motion study still clean with the box and crank
  in.

## Phase 4: FEM of the crank and the shaft on pins (size: S)

This phase assumes the pin support (the `wtpin` branch's plan,
fem-pin-support) has landed.

- **The camshaft** sits on two pin supports at its bores. It is loaded by the
  followers' contact forces at the worst sample: the force from 1c's
  M q″ ω² − Q on the contact normal, at `speed_rpm`, plus the handle torque
  that balances them. The check reports those forces, so they need not be
  estimated.
- **The crank handle and the camshaft's crank end** sit on pins and are loaded by a hand's push
  at the handle. The push is derived from the torque the followers need at
  the worst sample, with a stated factor for a child leaning on it.
- PLA's interlayer strength applies across layers
  (`max_principal_mpa`, `layer_normal`) for the shaft printed lying down.
  The pin support's flag on a support-edge peak must not be the headline.
- Verified: the factors of safety come out of the receipt, with the reaction
  at each pin matching the applied load's statics within 1%.

## Phase 5: drawings and BOM (size: S)

- An assembly drawing (front, side, an exploded view, balloons through
  `layout_balloons`) and `export_bom`: every printed part with its material,
  printed mass and plate. Nothing is bought, so the BOM has no fastener rows.
- Part drawings for each cam, with its lift diagram. A drawing table of the
  CamProfile's motion counts if one is there to use. Otherwise the gap is
  recorded for the roadmap.
- `check_drawing` is clean.

## Phase 6: the film (size: S)

- `export_motion_video` of the crank turning two or three times, with the
  camera on the figures. A second cut runs with the box sectioned
  (`set_section` during motion) so the cams and followers show. It is driven
  at the hand speed (60 rpm), with the sample count high enough that the drop reads
  as a drop. A `SequenceStep` film of the assembly order comes from phase 3.
- Look at the frames. The axe must meet the log on the beat, with no figure
  passing through the box.

## Phase 7: print and measure (later, when the printer is free)

Print the plates, assemble by the sequence, and measure:

- each rod's stroke against the contact table, with calipers at the dwell
  and the drop;
- the crank speed at which the axe first chatters (a phone's slow-motion
  video against a metronome) against `liftoff_rpm`. This is the friction
  the check leaves out;
- the fits: the guide bores, the shaft, and the pegs.

The numbers go into the generator's README beside the predictions. Any
difference that is a bug goes into known-bugs.md or the roadmap.

## Files phase 1 touches

`src/core/Occurrence.{h,cpp}` (the kind, its fields), `AssemblySolve.cpp`
(geometry access, the invariant, `RelationValue`), `AssemblyRelations.cpp`
(creation checks), `AssemblyLoopSolve.{h,cpp}` (the table as a row), a new
`src/core/ContactFollower.{h,cpp}` (the 2D outlines, the first-touch solve,
the adaptive table, its cache), `src/core/features/CamProfileFeature.{h,cpp}`
and `src/core/CamProfileMath.{h,cpp}`, `src/core/FollowerCheck.{h,cpp}`,
`src/core/Requirements.{h,cpp}`, `src/core/io/SerializeAssembly.cpp`,
`SerializeFeatures.cpp`, `SerializationInternal.h` (`kVersionPending`),
`src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp`,
`RemoteRequirements.cpp`, `RemoteModeling.cpp`, `RemoteDispatch.cpp`,
`src/ui/OccurrencePanel.cpp`, the feature UI files listed in 1b,
`python/lobole/{sdk,rpc,mcp}.py`, the tests and goldens named above, and the
docs named under 1c.
