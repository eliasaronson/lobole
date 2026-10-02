# Fasteners follow their holes

## The problem

`fit_fastener` places each piece of a joint (screw, washers, nut) as an
occurrence that RIDES ON a part (`Occurrence::attachedTo`), at an
`attachOffset`/`attachRotation` fixed in that part's frame at placement
([attached, and mated](../parts/README.md#attached-and-mated)). The offset is
a number, not the hole: when the hole moves INSIDE its part — the part is
regenerated with a thicker plate, a moved bore — the hardware stays where the
hole used to be. On the pan/tilt head, `base_t` + 2 mm moves the yoke's
geometry 2 mm up its own Z; the 12 riders on it (joint screws, the pan
blocks' nuts and washers) stay put and clash 2 mm into the yoke
(generators/pantilt/README.md). Nothing reports it: the rider is exactly
where its offset says.

Bearings are not affected: `fit_bearing` holds a bearing by a fastened mate
anchored on named faces, which already follows them.

## The shape of the fix

**A rider remembers its hole by name and is re-placed from it on every
solve.** `Occurrence::attachHole` = the parent part's **seat** (the planar
face the piece bears on: the head's face, or the far face for a nut and its
washer) and **bore** (the coaxial cylinder, or the countersink's cone), as
toponaming `ElementRef`s, plus the `innerPath` when the parent is a
sub-assembly. The hole's frame is derived, never stored: origin where the
bore's axis meets the seat's plane, Z the seat's outward normal, X the
parent's X projected into the seat (Y when X is square to it), each face
resolved through `ConnectorLocalFrame` — the same resolution a mate
connector on those faces gets, so the rider behaves like a concentric +
coincident mate to them. With a hole set, `attachOffset`/`attachRotation`
are the piece's placement in the HOLE's frame (washer thickness, the head's
side); without one they keep their old meaning, the parent's frame.

**A resolved attachment, not mates.** A fastener is a zero-DOF leaf: as
mates, every piece would add rows to the mechanism the solver and the Mates
list read (four per joint, 400 for a 100-bolt frame), and frame alignment
could relocate hardware off the pose the fit computed. The attachment stays
a composition after the solve — two face resolutions and a matrix product
per rider — so 100+ fasteners cost nothing the solver sees.

- **Placing** (`ui::AddFastener` with a parent): the hole is found at the
  pose the fit computed (`assembly::FindAttachHole`): the nearest planar face
  of the parent square to the piece's axis, facing it, with a circular edge
  centred on the axis; and a coaxial cylinder (else cone) with a circle in
  that plane. Found, the offset is re-expressed in its frame. Not found (a
  piece placed off any hole), it rides at an offset and the fit SAYS so in
  `warnings`.
- **Solving** (`assembly::ApplyAttachments`, now given the part resolvers):
  a rider with a hole is placed from it. A hole that does not resolve — a
  face deleted or renamed — is an error ON THAT RIDER (`attach_error`,
  transient, like a mate's `error`), counted in the solve report
  (`errored_attachments`) and listed by `check_fasteners`; the rider stays
  where it last was, flagged, never silently.
- **Joint length.** A thicker plate moves the head or the nut along the
  axis, so the screw may no longer reach, or reach further than it needs.
  `check_fasteners` reports both: "too short" (exists) and a new through-joint
  **over-reach** finding — a shorter stocked length spans the stack.
- **Existing documents** (format ≤ v155, offset only) load unchanged. The
  first solve of an assembly in a session binds every unbound rider to the
  hole under it (`assembly::BindRidersToHoles`, from
  `ui::SolveActiveAssemblyReport`), re-expressing its offset — the same
  search, at the pose the old offset gives. A rider with no hole under it
  keeps its offset and carries `attach_warning` (get_state,
  list_occurrences).

## Saved form

v156: an occurrence's `attach_hole` {`seat`, `bore`: {`feature_id`,
`element`}, `inner_path`?}, written only with `attached_to`; with it,
`attach_offset`/`attach_rotation` are in the hole's frame. Golden
`tests/golden/assembly/v156-attach-hole.json`, a row in
docs/versioning/format.md.

## Files

- `src/core/Occurrence.h` — `AttachHole`, `attachHole`, transient
  `attachError`/`attachWarning`/`attachSearched`.
- `src/core/AssemblySolve.{h,cpp}` — `HoleFrame`, `FindAttachHole`,
  `BindRidersToHoles`, `ApplyAttachments` with resolvers,
  `SolveReport::erroredAttachments`.
- `src/core/io/SerializeAssembly.cpp`, `SerializationInternal.h` — v156.
- `src/ui/FastenerActions.cpp`, `src/ui/AssemblyActions.{h,cpp}` — bind at
  placement, the fit's `warnings`, bind before a solve.
- `src/ui/GizmoInput.cpp` — the free drag passes resolvers.
- `src/ui/Bom.cpp` — `check_fasteners`: lost holes, over-reach.
- `src/ui/OccurrencePanel.cpp` — the row tail says a lost hole.
- `src/remote/RemoteAssembly.cpp` — `fit_fastener` `warnings`,
  `errored_attachments` in solve answers, `attach_error`/`attach_warning` on
  occurrences, describe rows.
- `generators/pantilt/README.md`, docs/parts, docs/assemblies.

## Verification

- `tests/test_actions.cpp` `[parts]`: a bolted joint (screw, two washers,
  nut) where the top plate thickens and both bores move sideways — every
  piece follows its own hole; the screw reports too short through
  `check_fasteners`, and over-reach when the plate thins; the top plate's
  hole suppressed — the screw and washer error, the solve counts them,
  `check_fasteners` names them; a legacy offset-only rider binds on the first
  solve and then follows.
- `tests/test_assembly.cpp`: the v72 offset-only rider still composes.
- Golden v156 round trip; `test_golden` gates.
- Pan/tilt: regenerate into a scratch copy (`DESIGNER_SCRATCH`), raise
  `base_t` 2 mm: zero clashes in the standing sweep, every fastener moved
  with its hole, `check_fasteners` clean; `ctest -C generators -L generators`.
- Full headless suite, the GUI suite, `-L pygui`.
