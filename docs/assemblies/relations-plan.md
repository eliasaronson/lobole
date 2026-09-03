# Relations — gear, rack, screw (the assembly's coupled DOFs)

> Status: **planned and built 2026-09-03** (format v83). From: *"does the
> assembly support a gear simulation with rotary mates?"* — it did not. See
> [As built](#as-built).

## What was missing

Six mate types (fastened, revolute, slider, cylindrical, planar, ball), and
since A6 a solver that closes loops in joint coordinates — but nothing that
relates two mates' degrees of freedom to each other. A revolute on each gear
to a common carrier gives each its free rotation; turn one and the other
stays put.

Every mainstream assembly does the same thing for gears, and none does
contact dynamics for them: a **relation**, a ratio between two mate DOFs
(Onshape's gear / rack-and-pinion / screw / linear relations, SolidWorks'
gear and rack mates, Fusion's motion links, FreeCAD 1.0's gear joint over
OndselSolver). Tooth contact is geometrically guaranteed by the pair once
module and centre distance match; the ratio is the honest kinematic model.
The contact-dynamics modules (SolidWorks Motion, Inventor Dynamic Simulation)
are separate products most users never open.

## The design

One relation type, four kinds that are labels and unit conventions on it:

    dofB = ratio * dofA + phase

| kind | dofA | dofB | ratio |
|---|---|---|---|
| `gear` | rotation | rotation | `-teeth_a / teeth_b` (dimensionless) |
| `rack` | rotation (the pinion's) | offset (the rack's slider) | pitch radius, mm/rad |
| `screw` | rotation | offset — the SAME cylindrical mate | pitch / 2π, mm/rad |
| `linear` | offset | offset | dimensionless |

`phase` is what makes teeth mesh (or where the rack sits at θ = 0). A
relation names two mates by id and the DOF on each; a mate that lacks the
DOF (a fastened mate has no rotation) is refused at creation, in words.

**Where it is enforced — two places, on purpose.**

- **Loop-free components** (the common case: two gears on a carrier): the
  relation is *propagated* before the closed-form tree walk. `A drives B`
  when the solve is unprompted (loading a file, `solve_assembly`), which is
  deterministic; when a DOF was just changed — a drag, `set_mate` — the
  changed mate drives, walking the relation graph outward from it, so
  dragging either gear turns the other. Propagation is idempotent, so the
  unprompted pass after a driven one changes nothing.
- **Looped components**: each relation whose mates are both in the
  component is one **residual row** in A6's system, beside the six per cut
  mate. Rank then sees it: a four-bar with a gear relation on its ground
  joints drops from one DOF to zero. Rotation rows are scaled by the
  component's characteristic length like the cut rows' rotational part, so
  radians and millimetres weigh alike.

**What stays out.** Motion (driving a DOF along time and playing it) is its
own small item; the CAM playback transport is the thing to reuse. Contact
is not simulated by anyone's assembly and is not here; the interference and
contact reports already answer "do these two gears overlap at this centre
distance", statically, which is the useful check.

## Surface

- `add_relation {type, a, b, ratio?, phase?, name?, teeth_a?, teeth_b?,
  pitch_radius?, pitch?}` — the ratio DERIVED from the kind's own numbers
  when given (two tooth counts, a pitch radius, a thread pitch); a bare
  `ratio` for the rest; refused when neither is given. `set_relation {id,
  ratio?, phase?, name?}`, `remove_relation {id}`, `list_relations`.
- `set_mate` and a drag-within-mate propagate from the mate they changed.
- The Occurrences panel lists relations with their ratio and phase editable
  and a remove button. Creating one is RPC-only in this round: it needs two
  mates picked, and the panel selects one. (No creation tool, so the
  `defaults/*` gate has nothing to hold.)
- Format v83: `relations` array (flat) / `relations/` files + index list
  (directory), absent when none, a golden at v83.

## Gates

- `test_assembly_relations.cpp`: two gears on a carrier turn together at
  the ratio with the phase, from either side; a rack's offset follows its
  pinion; a screw's own offset follows its rotation; a relation inside a
  four-bar loop is a row (DOF 1 → 0, converged); a mate without the DOF is
  refused; a relation loses its mate and says so; JSON round-trips.
- `assembly_relations_smoke.py`: the RPC path headless, including the
  derived ratio and the refusal.
- The golden corpus test at v83.

## As built

Built the same day, as planned, with three things the plan did not say:

- **Propagation walks from the driven mate with a settled set.** A gear
  train is a graph, and the walk marks each mate it reaches so a later
  relation that would move it again is left alone — an inconsistent set is
  the loop solver's business (its rows), not a fight between two
  propagations. A screw (one mate, both DOFs) is the one case where the
  walk cannot know which DOF was touched; A's DOF drives, and the rack test
  shows the other direction works where the mates differ.
- **A relation row is validated at creation, then skipped rather than
  guessed.** `add_relation` refuses a mate that lacks the DOF, in words
  naming which mate types would do; the solver's `PropagateRelations` sets
  `Relation::error` for a mate that is gone or has since lost the DOF, and
  `Solve` leaves such a relation out of the loop rows. The panel shows the
  error beside the row like a mate's.
- **The golden's connectors** are `{"occurrence", "anchor": {"origin":
  true}}` — the anchor is an object, which the first draft of the golden
  got wrong and the corpus test caught, which is what the corpus is for.

Measured in the tests: two gears on a carrier at 12:36 follow each other
from either side to 1e-5 rad; a rack slides its pitch radius per radian; a
screw advances 2.5 mm in two turns of M8 coarse; a parallelogram four-bar
with a 1:1 relation on its ground joints reports DOF 0 where it reported 1,
and with an inconsistent 2:1 relation still closes, at the one pose both
allow (both angles at zero). The RPC smoke derives −1/3 from `teeth_a: 12,
teeth_b: 36`, turns the wheel and finds the pinion three times as far
round the other way, and reloads a rack relation from the saved project.

**The demo, `generators/gear_train.py`**: two generator gears (m2, 12 and 36
teeth) each mated by its bore to a hole in a carrier 48 mm apart, a gear
relation derived from the tooth counts and phased by half a pitch on the
wheel (π/36). Turning the pinion through one pitch in eight steps, the
interference check reports *touching* or *clear* at every step with a worst
clash volume of 0.023 mm³ — the 8 µm arc approximation on a pair drawn
with zero backlash. A wrong ratio or phase shows up here as whole teeth
overlapping, so this is the static tooth check that stands in for
"contact", and it passes.

What is still missing, named: a creation tool in the panel (needs a
two-mate pick), a drive (a DOF along time, the CAM transport's shape), and
a phase derived from the two gears' geometry rather than typed.
