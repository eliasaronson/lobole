# Relations

A relation couples two mates' degrees of freedom. Mates alone give each gear
of a pair its own free rotation on a carrier; a relation makes turning one
turn the other. Tooth contact is not simulated — once module and centre
distance match, the ratio is the kinematic model — and a wrong ratio or phase
shows up as teeth overlapping in the clash checks, which is the static test
that stands in for contact.

Code: `Relation` in `src/core/Occurrence.h`; `PropagateRelations`,
`RelationValue` and the residual check in `src/core/AssemblySolve.cpp`; the
loop row in `src/core/AssemblyLoopSolve.cpp`; `add_relation` /
`set_relation` in `src/remote/RemoteAssembly.cpp`. Tests:
`tests/test_assembly_relations.cpp`, `tests/assembly_relations_smoke.py`.

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
| `gear` | rotation → rotation | `−teeth_a / teeth_b` (meshing gears counter-rotate) |
| `chain` | rotation → rotation | `+teeth_a / teeth_b` (a chain or belt keeps the sense) |
| `rack` | rotation → offset | the pinion's `pitch_radius`, mm per radian |
| `screw` | one cylindrical mate's rotation → its own offset (a = b) | `pitch / 2π`; or from a Thread feature (`thread_part`, `thread_feature`), re-read on every solve |
| `linear` | offset → offset | a bare `ratio` |
| `cam` | rotation → offset or rotation | none: a lift table |
| `freewheel` | rotation → rotation | a bare `ratio`, plus `drives` and `lead` |
| `click` | one mate's rotation (a = b) | none: a one-way hold |
| `worm` | the worm's rotation → the wheel's, on crossed axes | `±starts / teeth`, the sign from the hand and the geometry ([Worm](#worm)) |

The ratio is derived from the kind's own numbers when they are given (tooth
counts, pitch radius, pitch) and otherwise taken as a bare `ratio`; a relation
given neither is refused. `phase` is in b's unit (radians or mm) and is what makes teeth mesh.

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
3. when the frontier empties, seeds **one** new root — the `a` of the first
   relation neither of whose ends is settled — and continues. One root at a
   time, not all at once: seeding every `a` would settle every mate that is
   both a follower and a driver before anything reached it, and a chain would
   be applied at its two ends only.

Driven-first is what lets dragging either gear turn the other, and keeps a
free mate from claiming a node the driver is about to set from a stale value.
An unprompted solve (load, `solve_assembly`) has no driven mate, so `a`
drives `b` — deterministic, and a no-op after a driven pass because
propagation is idempotent.

**Inside a loop, as a row.** Each equality relation whose two mates are in the
same looped component is one residual row in the loop solve
([solver.md](solver.md#closing-loops)), `dof_b − ratio · dof_a − phase`, with
rotation rows scaled by the component's characteristic length like the cut
rows. Rank sees it: a four-bar with a 1:1 relation on its ground joints drops
from one DOF to zero.

**Validation.** A relation with a problem is reported in its `error` and
skipped, never half-applied: a mate gone, a DOF the mate no longer has, a DOF
coupled to itself, a cam table with fewer than two points, a freewheel or
click with no direction, a missing or unsupported screw thread. A relation
suppressed by the active configuration (or coupling a suppressed mate) is
skipped without an error.

### When relations disagree

After placement and the loop pass, every equality relation is checked: one
whose `b` does not equal what it asks for (to float precision) is a DOF driven
twice by relations that disagree, and carries `not satisfied: mate N's
rotation is X deg where this relation wants Y`. In a tree the second relation
was never applied; in a loop the solver settled on a compromise. Either way it
is said on the relation.

## Cam

A cam drives b through a **lift table** — `[[angle_deg, lift], …]` spanning
one turn — instead of a ratio. The lift is in the follower's unit: mm for a
slide, **degrees** for a turn. The table is stored as authored and converted
to radians where it is read (`RelationValue`, the loop row). The follower is
whatever DOF mate b has: a slider slides, a revolute turns, and a cylindrical
b takes `dof_b: offset|rotation` (offset when omitted). `{rise | rise_deg,
duration_deg?, start_deg?}` builds a cosine lobe; `rise` on a turning follower
or `rise_deg` on a sliding one is refused, not converted.

The table is interpolated periodically (a crank past 360° sees the profile
again) by a cubic with Fritsch–Carlson-limited tangents: the slope is
continuous, which the loop solver's finite differences need, and the curve
never leaves the interval its neighbouring points define, so a dwell cannot
dip below zero. A cam is not invertible — many angles give one lift — so it
drives forward only.

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
- **Idempotent:** after a pass the equation holds exactly (with a float-ulp
  slack so rounding does not grow `lead`), so the many solves per action —
  `set_mate` then solve, every drag frame, load, tab switch — change nothing.
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
- `set_relation {id, ratio?, phase?, name?, lead?}` — `lead` only for a
  freewheel or click; `ratio` refused on a thread-backed screw (edit the
  thread).
- `remove_relation {id}`, `list_relations`.
- `set_mate` and a drag within a mate propagate from the mate they changed.
- The occurrence panel lists relations with ratio and phase editable (a
  freewheel's `lead` read-only) and a remove button; creation is RPC or SDK
  only (`Asm.gear/chain/rack/screw/cam/freewheel/click/worm`).
- Stored in the assembly's `relations/` directory, one file per relation
  ([../versioning/format.md](../versioning/format.md)).

`generators/gear_train.py` is the worked example: three gears on a carrier
coupled by two gear relations derived from their tooth counts (the idler
phased by half a pitch so the teeth interleave), and a toothless bar on a
slider coupled to the wheel by a rack relation; turning the pinion through a
pitch, the interference check says whether the teeth ever clash.
