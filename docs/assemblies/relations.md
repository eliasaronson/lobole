# Relations

A relation couples mates' degrees of freedom — two, or, for an epicyclic
train, several. Mates alone give each gear of a pair its own free rotation
on a carrier; a relation makes turning one turn the other. Tooth contact is not simulated — once module and centre
distance match, the ratio is the kinematic model — and a wrong ratio or phase
shows up as teeth overlapping in the clash checks, which is the static test
that stands in for contact.

Code: `Relation` in `src/core/Occurrence.h`; `PropagateRelations`,
`RelationValue` and the residual check in `src/core/AssemblySolve.cpp`; the
derivation from teeth and the checks before a relation is made
(`DeriveFromTeeth`, `RelationProblem`) in `src/core/AssemblyRelations.cpp`;
the loop row in `src/core/AssemblyLoopSolve.cpp`; `add_relation` /
`set_relation` in `src/remote/RemoteAssembly.cpp`; the panel rows and the
Add relation window in `src/ui/OccurrencePanel.cpp`. Tests:
`tests/test_assembly_relations.cpp`, `tests/assembly_relations_smoke.py`,
the `assembly` GUI tests named `*relation*`.

Either side may be a mate inside a
[flexible sub-assembly](README.md#flexible-sub-assemblies) placement, named
`{path, mate}` (`add_relation {a_path, b_path}`): an engine's chain drives
its pump's own camshaft mate. Freewheels and clicks cannot, since their
history would be per-placement state.

## Kinds

A relation names mate `a` and mate `b` by id and the DOF on each (`rotation`
or `offset`). Most kinds are one equation:

    dof_b = ratio · dof_a + phase

| kind | dof_a → dof_b | ratio |
|---|---|---|
| `gear` | rotation → rotation | `−teeth_a / teeth_b` (meshing gears counter-rotate); with `internal`, `+teeth_a / teeth_b` (a gear meshing inside a ring turns the same way) |
| `chain` | rotation → rotation | `+teeth_a / teeth_b` (a chain or belt keeps the sense) |
| `rack` | rotation → offset | the pinion's `pitch_radius`, mm per radian |
| `screw` | one cylindrical mate's rotation → its own offset (a = b) | `pitch / 2π`; or from a Thread feature (`thread_part`, `thread_feature`), re-read on every solve: a table designation's pitch or a custom thread's own (a printed thread is a custom one), negative for a left-hand thread |
| `linear` | offset → offset | a bare `ratio` |
| `cam` | rotation → offset or rotation | none: a lift table |
| `freewheel` | rotation → rotation | a bare `ratio`, plus `drives` and `lead` |
| `click` | one mate's rotation (a = b) | none: a one-way hold |
| `worm` | the worm's rotation → the wheel's, on crossed axes | `±starts / teeth`, the sign from the hand and the geometry ([Worm](#worm)) |
| `linear_n` | Σ coefficient · dof = constant, over two or more mates | none: a coefficient per term ([below](#relations-over-several-mates)) |
| `epicyclic` | sun, ring and carrier rotations, by Willis | from `teeth_sun`, `teeth_ring` |
| `planet` | a planet's turn on its carrier, from the sun's and the carrier's | from `teeth_sun` and the planet's teeth |
| `intermittent` | rotation → rotation or offset, one step a turn | none: a step, a window and a law ([Intermittent](#intermittent)) |
| `contact` | rotation → offset or rotation | none: the shapes of the cam and the follower ([Contact](#contact)) |

The ratio is derived from the kind's own numbers when they are given (tooth
counts, pitch radius, pitch) and otherwise taken as a bare `ratio`; a relation
given neither is refused, and so is a gear or a chain given both teeth and a
ratio (or `internal` with a bare ratio, which carries its own sign). A gear's
or a chain's tooth counts are kept (`teeth_a`, `teeth_b`), so `set_relation`
and the editor change the teeth and the ratio follows. `phase` is in b's
unit (radians or mm) and is what makes teeth mesh.

Refused at creation, naming what would do: a mate that lacks the DOF (a
fastened mate's `rotation` is an authored twist, not a DOF); a rack whose
slider runs *along* the pinion's axis (a slider slides along its connector
Z, and origin frames are Z-up). A gear pair whose axles are more than 5° off
parallel is accepted with a `warnings` entry (bevel and crossed pairs are
real), and so is a worm pair more than 5° off square.

## Enforcement

**In a mate tree, by propagation** — `PropagateRelations`, run at the start of
every solve. Its nodes are (mate, DOF) pairs, so a cylindrical mate's turn and
slide can each take their own relation. The walk:

1. seeds the mate the caller just drove (a drag, `set_mate`, a motion sample),
   then every DOF the active configuration holds;
2. walks outward, setting each unsettled node from its neighbour and settling
   it; a relation is applied forward (b from a) or inverted (a from b), except
   a cam, which is never inverted;
3. when the frontier empties, first finishes any relation over several mates
   the walk reached but could not complete, holding one member
   ([below](#relations-over-several-mates)); otherwise seeds **one** new
   root — the `a` of the first relation neither of whose ends is settled
   (the first member, for one over several mates) — and continues. One root
   at a time, not all at once: seeding every `a` would settle every mate
   that is both a follower and a driver before anything reached it, and a
   chain would be applied at its two ends only.

Driven-first is what lets dragging either gear turn the other, and keeps a
free mate from claiming a node the driver is about to set from a stale value.
An unprompted solve (load, `solve_assembly`) has no driven mate, so `a`
drives `b` — deterministic, and a no-op after a driven pass because
propagation is idempotent.

**Inside a loop, as a row.** Each equality relation whose mates are all in the
same looped component is one residual row in the loop solve
([solver.md](solver.md#closing-loops)), `dof_b − ratio · dof_a − phase`, with
rotation rows scaled by the component's characteristic length like the cut
rows. A relation over several mates is one row, Σ cᵢ · dofᵢ − constant,
divided by its largest coefficient so it weighs like a binary one (weighted by
the characteristic length when every member turns). Rank sees it: a four-bar
with a 1:1 relation on its ground joints drops from one DOF to zero.

**Validation.** A relation with a problem is reported in its `error` and
skipped, never half-applied: a mate gone, a DOF the mate no longer has, a DOF
coupled to itself, a cam table with fewer than two points, a freewheel or
click with no direction, a missing or unsupported screw thread. A relation
suppressed by the active configuration (or coupling a suppressed mate) is
skipped without an error.

### When relations disagree

After placement and the loop pass, every equality relation is checked: one
whose `b` (for one over several mates, its last member) does not equal what
it asks for (to 1e-4 of its magnitude, the room
a closed loop needs) is a DOF driven twice by relations that disagree, and
carries `not satisfied: mate N's rotation is X deg where this relation wants
Y`. In a tree the second relation was never applied; in a loop the solver
settled on a compromise. Either way it is said on the relation.

## Relations over several mates

A mate's DOF is measured relative to its parent. A planet's revolute sits on
the carrier, so its angle is relative to the carrier, and the coupling that
moves it involves three members — which no relation between two mates can
say. An epicyclic train is one equation over three:

    linear_n:   Σ cᵢ · dofᵢ = constant
    epicyclic:  zs · sun + zr · ring − (zs + zr) · carrier = constant    (Willis)
    planet:     (zs/zp) · sun − (zs/zp) · carrier + planet = constant

`add_relation {type: "linear_n", terms: [{mate, dof?, coefficient}, …]}`
takes two or more terms (`dof` is required only of a cylindrical mate, which
has both). `{type: "epicyclic", sun, ring, carrier, teeth_sun, teeth_ring}`
is the form a person writes: it is stored as the Willis terms with the tooth
counts kept, so `list_relations` and the panel show what was written
("epicyclic 14/40"). With `planets: [{mate, teeth}, …]` it also adds one
`planet` relation per planet — each planet's own turn on the carrier,
−(zs/zp)(sun − carrier) — returned in `planets`; `type: "planet"` makes one
alone. Members are mate ids, or `{mate, path}` for a mate inside a flexible
placement. A relation over several mates stores `terms` and a `constant` in
place of a and b, ratio and phase; `ratio` and `phase` are refused, and the
`constant` (in the members' units) defaults to where they stand, so adding
one moves nothing.

The three members' rotations must be measured about one axis in one frame:
the sun, ring and carrier each on a revolute to the housing, the planets on
revolutes to the carrier. A member more than 5° off the sun's axis is named
in `warnings`.

Refused before anything changes (`RelationProblem`, which the Add relation
window asks too): a member missing, a mate used twice (across the planets
too), a member whose mate lacks the DOF, a tooth count that is not a whole
number above zero, fewer than two terms, a zero coefficient.

**Propagation.** A relation over several mates is a hyperedge. When every
member but one is settled, the walk solves for the last. With two or more
open it waits — for the walk to reach them, and, once the frontier is empty,
it holds its first open member **in term order** where it stands and goes
on. That hold comes before any new component is started, so a gear on the
carrier cannot claim the carrier from the far side. The relation says what
it held in `held` ("ring (ring axis) held where it stands: nothing drives
it"), in `list_relations`, `add_relation`'s and `set_relation`'s answers,
the row's tooltip and the editor. So:

- the sun driven: the ring (second) is held and the carrier turns
  zs/(zs+zr) of the sun; each planet relation, its sun and carrier now
  settled, turns the planet;
- the carrier driven: the sun is held and the ring turns;
- two members driven (one by a configuration's hold, which settles a DOF
  like the driver's): Willis sets the third — a differential.

A planet's terms are stored as {sun, carrier, planet} so that, without the
Willis relation, driving the sun holds the carrier and turns the planet.
Order matters only where several relations over several mates wait at once:
they are completed in the order they were made, which is why `epicyclic`
adds the Willis relation before its planets.

**In a loop** it is one row; with every member in the looped component, the
same relation solves there to the answer propagation gives
(`test_assembly_relations.cpp`, "An epicyclic inside a closed loop…").

**Everywhere else a relation is touched**, every member counts: a
configuration that suppresses any member (or hides its occurrence) takes the
whole relation out; a flexible sub-assembly's relation over several mates
moves with the placement only when all of its members do, and a parent's
term may name an inner mate by path; the freewheel cycle search walks from
each member to every other.

## In the app

The occurrence panel's Relations section lists each relation with what it
was made from as its tail — "gear 14:13", "internal 13:40",
"epicyclic 14/40", "planet 13 on 14", "geneva 3" — and its members, ratio or
coefficients, `held` and any error in the tooltip; a relation that is not
satisfied keeps its red glyph. A double-click opens it in the tool window:
a pair's Drives and Driven combos, its teeth when it was made from teeth
(the ratio shown, derived) or else its ratio, and its phase; a relation over
several mates shows each member's combo by role, its teeth and its constant,
never its raw coefficients; an intermittent relation shows its step and
window and edits where the window opens, with no ratio.

**Add relation** (shown once the assembly has a mate that turns or slides)
opens the same window in create mode. Its Kind combo lists what the window
can make: Gear, Internal gear, Linear, Epicyclic and Geneva carry; the other kinds are
made by `add_relation` or the SDK. A gear takes its two sides (started on
the first two mates that turn), both tooth counts (no default: a count is
read off the part) and a phase; Linear takes two slides, a ratio and a
phase; Epicyclic takes Sun, Ring and Carrier combos over the mates that
turn, their teeth, and a Planets list — a row per planet, Add planet, and
the picked one's mate and teeth below it. A Geneva carry takes the driving
mate, the Geneva's, its slots (no default) and where the window opens; its
step and window are shown, derived, and it is made where the parts stand.
What is wrong with the draft — a
member not picked, one mate used twice, a tooth or slot count missing — is said on
the window's red line and Accept stays disabled. Accept is one undo step.

## Cam

A cam drives b through a **lift table** — `[[angle_deg, lift], …]` spanning
one turn, `0 <= angle_deg < 360`: the table wraps, its last point joining
its first, so it stops short of 360 (a point at 360 is the one at 0, and is
refused) — instead of a ratio. The lift is in the follower's unit: mm for a
slide, **degrees** for a turn. The table is stored as authored and converted
to radians where it is read (`RelationValue`, the loop row). The follower is
whatever DOF mate b has: a slider slides, a revolute turns, and a cylindrical
b takes `dof_b: offset|rotation` (offset when omitted). `{rise | rise_deg,
duration_deg?, start_deg?}` builds a cosine lobe; `rise` on a turning follower
or `rise_deg` on a sliding one is refused, not converted. `profile: {part,
feature}` instead reads the motion a CamProfile feature was cut for, on every
solve, so editing the cam moves the follower
([../modeling/cam-profiles.md](../modeling/cam-profiles.md#the-cam-relation-reads-it)).

The table is interpolated periodically (a crank past 360° sees the profile
again) by a cubic with Fritsch–Carlson-limited tangents: the slope is
continuous, which the loop solver's finite differences need, and the curve
never leaves the interval its neighbouring points define, so a dwell cannot
dip below zero. A cam is not invertible — many angles give one lift — so it
drives forward only.

## Contact

A contact relation drives a follower from the SHAPE of its cam: at every
angle of a (the cam's turn, a revolute or cylindrical mate) b stands where
the follower first touches the cam, approached from clear. b is a slider's
offset or a revolute's or cylindrical's turn. Edit the cam and the follower
follows the new shape; nothing about the motion is typed.

**Which bodies.** The cam is the side of mate a that is not rigid with mate
b's base, the follower the side of mate b not rigid with a's — "rigid" being
fixed, fastened or attached (`contact::RigidGroup`). `cam` and `follower`
(occurrence ids) name one when a mate moves several (a cam fastened to its
shaft), and `cam_body` / `follower_body` (the feature id that made the body)
when a part has several bodies; otherwise each is refused naming the
candidates. The bases of the two mates must be rigid to each other — the
cam's axle and the follower's guide may not move relative to each other — so
a follower driven by another follower is not built; join them through a link
and a mate loop instead.

**`toward`** (`positive|negative`) is the way b's value moves to reach the
cam. Omitted at `add_relation`, it is derived once from gravity: the sign of
the generalised gravity force on b over the follower and everything rigid
with it, at the current pose (`contact::TowardFromGravity`). It is refused
where gravity is under 2% of its full weight along the motion — a
horizontal slide, a pivot under its own mass centre — naming `toward`. It is
stored, never re-derived, so a file re-solves to the same placement.

**The planar case only.** The cam's axis is mate a's connector Z. A sliding
follower must slide square to it and a turning one turn about an axis
parallel to it, within the tilt that moves the contact by less than the
contact tolerance across the width the two share; anything else (a barrel,
face or conical cam) is refused. Both bodies are cut across the axis in the
middle of the width they share along it (refused when they share none), the
cam in its frame at a = 0 and the follower at b = 0. A chamfer or draft that
reaches into the shared width is not in the cut; the exact 3D check of the
motion study sees it.

**The first touch** (`contact::FirstTouch`). Each section's edges are
sampled within a chord error of an eighth of the contact tolerance, which is
1e-5 of the smaller outline's size (0.4 µm on a 40 mm cam). The follower is
carried along its path — a translation along the slide, a rotation about the
pivot — and every vertex of one outline is cast against the segments of the
other; the first hit in the `toward` sense is the contact. A turning
follower is looked for within half a turn either side of b = 0. The answer
is then solved again on the two curves' osculating circles (or lines) at the
contact, which is exact for arcs and lines and second order on a spline, and
gives the contact point, the common normal, both radii of curvature (signed:
convex or concave) and a second point when one touches within the tolerance
well apart from the first (the follower bridges a hollow).

**The table** (`contact::BuildTable`). The solve reads b from a periodic
table over one turn of a: 72 nodes, bisected until a cubic Hermite through
them (three-point tangents) reproduces an exact solve at every interval's
midpoint within a quarter of the tolerance. An interval that keeps failing
down to a microradian and jumps by more than ten tolerances is a **drop** — a
snail cam's step face — kept as a step the interpolation does not smooth.
`list_relations` reports, per contact relation, `contact {points,
tolerance_mm, bound_mm | bound_deg, drops [{at_deg, height_mm |
height_deg}], bridges_deg}`.

**Cached, never stored.** The table is derived state, cached on both
bodies' geometry, the frames between the two mates and `toward`
(`contact::ModelFor`, 64 entries): a solve, a drag frame or a motion run's
copied document hits it, and a cam edit misses it once. The file stores the
relation (`toward`, and the occurrences and bodies when named) and the mate
values as always; an opened project rebuilds the table on its first solve.
Like a cam it drives forward only, as a value row in a mate loop.

**Errors, never half-applied.** A follower that never touches the cam in a
whole turn (it would travel without end), bases that move relative to each
other, a body gone or a part not loaded: the relation's `error` says which
and it is skipped. `add_relation` builds the table before adding, so these
are refusals there.

Code: `src/core/ContactFollower.{h,cpp}`; tests
`tests/test_contact_relation.cpp` (an eccentric under a flat face, a roller
and a rocker's roller against their closed forms at 1000 random angles
between the table's nodes; the pressure angle; an edit rebuilding one
table; each refusal) and `tests/contact_relation_smoke.py`.

## Freewheel

A one-way clutch, rotation to rotation:

    b = ratio · a + phase + lead

`lead` is the only state — how far b has run ahead of a's drive line, in b's
unit — and it only grows in the `drives` direction (`positive` or `negative`,
required: a default would be a plausible wrong answer). Its position depends
on history, and `lead` is all of that history, saved with the document and
undone with the pose.

- **From a:** if the drive line is at or ahead of b in the drive direction, b
  follows (engaged). Behind, the clutch **slips**: b stays, and is left
  *unsettled* so another relation may move it.
- **From b:** forward, b overruns and a stays (unsettled). Backward, the
  clutch engages and drags a back — a bicycle's pedals turning with a wheel
  spun backwards — **unless** a is reachable from b through the other
  relations. There dragging would feed itself (turning one path back turns
  the other forward, which pushes the output further), so the clutch holds.
- **After the walk,** each freewheel is reconciled from where its two ends
  actually finished: output ahead of the line is slip, and `lead` takes it
  up; output *behind* the line is two drivers disagreeing, reported as
  `not satisfied`. Reconciling after the walk rather than in it is what lets
  two freewheels drive one output — a **reverser**: whichever way the input
  turns, one clutch drives and the other slips and absorbs the difference,
  order-independently (`test_assembly_relations.cpp`, "Two freewheels drive
  one output").
- **Idempotent:** after a pass the equation holds exactly (with a slack of
  1e-6 of its magnitude, so rounding and a loop's closing tolerance do not
  grow `lead`), so the many solves per action — `set_mate` then solve, every
  drag frame, load, tab switch — change nothing.
- **Created where it stands:** `lead` starts as the current gap between the
  two parts unless given, so adding a clutch moves nothing. `set_relation
  {lead}` puts the history back and moves b onto the drive line at that lead.
- **Teeth:** with `teeth`, it re-engages only where teeth meet — the drive
  line stands at the notch at or behind `lead`, so the input travels up to a
  pitch before driving again (a ratchet's dead band). `ride_mate`,
  `ride_dof`, `ride_mm | ride_deg` name what riding over the teeth lifts: the
  whole amount off a notch, nothing seated in one (solid dogs cannot stand part
  way over each other), added after the walk on top of whatever else drives
  that DOF, and allowed for by the residual check.
- **Not inside a loop:** a clutch that may slip is an inequality and the loop
  solve holds only equations. A freewheel is left out of the loop rows; if the
  loop moves one of its mates, it reports `a freewheel cannot be inside a mate
  loop`.

## Click

A pawl: ONE mate's rotation (a = b) that may travel only the way it `drives`.
`lead` is its mark, starting where the wheel stands. Moved the way it lets go,
the mark follows; moved against it, the DOF goes back to the mark and whatever
drove it is left disagreeing, which that relation's own residual check
reports. What rides over the teeth the click holds is a cam, which follows
their shape. Like a freewheel, it is left out of loop rows.

## Intermittent

A carry: per turn of `a` (or per `period_deg`), `b` advances ONE step
during a window of a's rotation and stands still the rest of the turn —
an odometer's carry, a Geneva's index, a film advance. Every other kind is
continuous, and a cam's table returns to where it started each turn, so
none of them can count.

    x = (a − start) / period,  n = ⌊x⌋,  u = (x − n) · period
    b = phase + step · (n + s(u / duration))   while u < duration
    b = phase + step · (n + 1)                 for the rest of the period

It is a pure function of a, with no history: the count is a's whole
turns, so a wheel turned back runs it back through zero, and a pose a
thousand turns on costs what one at zero does (`IntermittentValue` in
`src/core/Occurrence.cpp`). `s`, the window's progress from 0 to 1, is the
motion law:

- `uniform` — linear, a gear-driven step (a two-tooth carry):
  `{start_deg, step_deg | step_mm, duration_deg, period_deg?}`. The step
  is signed and in the follower's unit, and the wrong one is refused, as a
  cam's rise is; `period_deg` defaults to one turn.
- `geneva` — an external Geneva of `slots` N ≥ 3, exactly. The pin, at
  R = C·sin(π/N) from the driver's axis (C the distance between the axes),
  enters a slot radially and leaves it radially; with α the driver's angle
  from the line of centres the wheel's slot stands at
  β = atan2(R sin α, C − R cos α), so s = (β + π/N) / (2π/N). The window
  is 180 − 360/N degrees and the step −360/N (an external Geneva
  counter-rotates, as a gear pair does), once a turn; all three follow from
  `slots`, and giving one of them as well is refused. Being the geometry,
  a motion study's clash check on a real Geneva checks the shapes, not a
  guess at their motion (`tests/test_assembly_relations.cpp`, "A Geneva's
  law is the pin in its slot").
- `table` — any law a follower keeps to its driver's face by, given as
  points: `{table: [[driver, step], ...]}` beside a uniform step's keys,
  both fractions (of the window and of the step), s linear between them.
  It runs from `[0, 0]` to `[1, 1]`, the driver's fractions rising and the
  step's never falling; a table that is not that is refused, naming the
  point, and so is a `table` under another motion. An escape wheel turned
  by a pallet's impulse face is one: [generators/clock](../../generators/clock/README.md)
  derives the table from the face it follows.

`start_deg`, where a's window opens, is required for all three: the timing is
the whole of a carry, and no default would be the right one. `phase`
defaults to where b stands, so adding a carry moves nothing. A cylindrical
b names `dof_b`; a Geneva's b turns.

**Forward only.** A locked wheel says nothing about where its driver
stands, so, like a cam, an intermittent relation is never inverted: b
driven on its own leaves a where it is, and the relation reports `not
satisfied` — which is what a lock holding is. Turned that way in a motion
study, the parts that lock it clash, which is how
[generators/counter](../../generators/counter/README.md#what-is-checked-every-run)
checks its locks.
In a mate loop it is a residual row, dofB − value(dofA).

## Worm

A worm drives its wheel on crossed axes, `a` the worm's revolute and `b` the
wheel's: `add_relation {type: "worm", a, b, starts, teeth, hand: right|left}`.
A `ratio` is refused, and so is a pair whose axes are parallel or meet.

The size of the ratio is starts/teeth. Its **sign** is what a gear relation
cannot give. A right-hand worm turned +φ about its axis u screws its thread
back along u and carries the wheel's tooth with it. The wheel, on axis w,
therefore turns −φ·z1/z2 when the worm sits on the side c (from the wheel's
axis to the worm's) where (u × w)·c > 0, and the other way on the other side.
A left hand reverses it again.

A mate's DOF turns its B side about its A frame's Z. So a gear that is its
mate's A side (the carrier on B) turns the other way about that axis. The
carrier is taken as the occurrence the two mates share, else the fixed one,
else the A side.

`gear`'s fixed −za/zb is right for one of the ways a pair can be put
together. The pan/tilt head proved it wrong for the rest: the worm meshing
test with the sign reversed clashes within a quarter turn. That is why this
is a kind of its own (`worm::RelationRatio` in `src/core/WormGear.cpp`).

The ratio is derived once, when the relation is made, from the mates' frames
as they stand. Moving a mate's frame afterwards does not re-derive it. Once
made, it is an ordinary equality relation, propagated in a tree and a row in
a loop. Proved on real geometry in `tests/test_worm.cpp`: a motion study
turns the worm 1¼ turns, checks every sample exactly, and finds no clash,
right- and left-handed. Self-locking is reported by the features
([../modeling/worm-gears.md](../modeling/worm-gears.md#self-locking)), not
simulated.

## Surface

- `add_relation {type, a, b, …}` — kind-specific parameters as above;
  returns the relation, its `error` if the first solve found one, and any
  `warnings`.
- `set_relation {id, ratio?, phase?, name?, lead?, start_deg?, toward?,
  cam_body?, follower_body?}` — `toward` and the bodies only for a contact
  relation, which refuses a ratio and a phase;
  `start_deg` only for an intermittent relation, which refuses a ratio;
  `lead` only for a
  freewheel or click; `ratio` refused on a thread-backed screw (edit the
  thread) and on a gear or chain made from teeth, which takes `teeth_a`,
  `teeth_b` (and a gear `internal`); an epicyclic takes `teeth_sun`,
  `teeth_ring`, a planet `teeth_planet`, `teeth_sun`, and every relation over
  several mates `constant` instead of a phase.
- `remove_relation {id}`, `list_relations`.
- `set_mate` and a drag within a mate propagate from the mate they changed.
- The occurrence panel lists, edits and makes them ([In the app](#in-the-app));
  a freewheel's `lead` is read-only there. The SDK has
  `Asm.gear(…, internal=)/epicyclic/linear_n/chain/rack/screw/cam/contact/freewheel/click/worm/intermittent`.
- Stored in the assembly's `relations/` directory, one file per relation
  ([../versioning/format.md](../versioning/format.md)).

`generators/gear_train.py` is the worked example: three gears on a carrier
coupled by two gear relations derived from their tooth counts (the idler
phased by half a pitch so the teeth interleave), and a toothless bar on a
slider coupled to the wheel by a rack relation; turning the pinion through a
pitch, the interference check says whether the teeth ever clash.
