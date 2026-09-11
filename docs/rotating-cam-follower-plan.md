# A cam's follower may turn

## The problem

A cam relation drives a SLIDING follower only: `add_relation(type: "cam")`
fixes `dof_b` to the offset, and a revolute follower is refused. A cam that
rocks a lever is at least as common as one that lifts a tappet — a rocker
arm, a clamp, an indexer, a pallet fork — and every model with one works
round the refusal: the OM615's valves and its vacuum pump drive a slider
from the cam and then run a RACK backwards from that slider to the rocker.
That works only where a sliding part exists to hang it on.

The watch has no such part. Its escapement is placed, not animated: the
balance and the pallet fork stand still while the train turns
(docs/modeling/watch.md, "An animated escapement: no").

## The shape of the fix

**App.** A cam's follower is whichever degree of freedom mate b has:

- a slider's offset, a revolute's rotation;
- a cylindrical mate has both, so `dof_b: "offset" | "rotation"` names it.
  Omitted, it stays the offset — what every existing caller means — and the
  describe row says so.
- mate b with neither is refused, naming both.

The lift table's second column is in the FOLLOWER's unit: mm for a slide,
DEGREES for a turn, as the first column is degrees of the cam's turn. The
table is stored as authored; the solve converts degrees to radians where it
reads it — `RelationValue` (propagation) and the loop solver's relation
row. The cosine-lobe shorthand takes `rise` (mm) for a slide and `rise_deg`
for a turn; the wrong one for the follower is refused, not converted.
`phase` stays in b's own unit (radians or mm), as it is for every relation.

A rotating follower is still forward-only, like every cam.

**Format: v102.** Nothing in a file's schema is new — `dof_b` is written for
every relation — but a v101 build reading a turning cam would take its
degrees as radians. Bumping makes it refuse the file instead. Golden
`tests/golden/assembly/v102-rotating-cam.json`.

**Watch.** The escapement animates from the train:

- The escape wheel turns 12 times a minute and one full oscillation of the
  balance releases one tooth, so the balance is 3 Hz — 21,600 beats an hour,
  the standard rate. A cam from the escape wheel to the balance gives it a
  harmonic swing of one period per tooth pitch (24 degrees of the escape
  wheel). The motion is harmonic in time, like a real balance.
- A second cam, also from the escape wheel, gives the pallet fork the angle
  the impulse pin puts it at: while the pin is in the notch, the fork points
  at the pin; outside, it rests at the banking angle where the pin left it.
  The table is computed from the fork and roller geometry and sampled
  densely enough that the spline stays within the notch's play.
- **What is still approximate:** the escape wheel turns continuously, not in
  steps, so no stone can lock or impulse a tooth. The stones stand clear of
  the tip circle through the whole swing, and the fork is cut clear of it at
  both banking angles. The doc says so.

## Files

- `src/remote/RemoteAssembly.cpp` — `add_relation`'s cam branch: follower
  DOF, `dof_b`, `rise_deg`, refusals, describe row.
- `src/core/AssemblySolve.cpp` (`RelationValue`),
  `src/core/AssemblyLoopSolve.cpp` (relation row) — degrees to radians for
  a turning follower.
- `src/core/Occurrence.h` — the Relation/cam comments.
- `src/core/io/SerializationInternal.h` — v102;
  `docs/versioning/format.md` — the row.
- `tools/designer_sdk.py` — the `cam()` docstring.
- `generators/watch/` — balance and pallet cams, stones and fork clearance,
  an escapement motion study.
- `docs/modeling/watch.md`, `docs/remaining-watch-items-plan.md`.

## Verification

- `tests/test_assembly_relations.cpp`: a cam on a revolute follower turns
  it to the table's degrees; the same relation inside a mate loop solves to
  the same angle; a v102 round trip and the golden drive.
- `tests/assembly_relations_smoke.py`: a revolute follower is ACCEPTED and
  turns; a cylindrical follower takes `dof_b`; `rise` on a turning follower
  and `rise_deg` on a sliding one are refused; a fastened follower is
  refused naming both DOFs.
- The watch: a motion study over one escape pitch, sampled finely enough to
  land inside the impulse window, with the mesh clash check — the pin never
  enters the fork's horns and no stone reaches a tooth — and the full run's
  exact sweep and declared clearances still VERIFIED.
- Headless and GUI suites; `benchmarks/runners/reference.py` (the MCP
  surface changed).
