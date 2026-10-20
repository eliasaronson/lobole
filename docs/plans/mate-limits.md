# Mate limits

Not to start before the demo video and the open-source release are done.

## The problem

A mate's free values (`Mate::rotation`, `offset`, `dx`, `dy`,
`ballRotation` in `src/core/Occurrence.h`) have no range. Every revolute
turns forever and every slider runs to infinity, so:

- dragging moves a part through its stop (a hinge through its door
  stop, a vise jaw past its full opening);
- a motion study driven past what the mechanism allows runs on into a
  pose the real thing cannot reach, and says so only if a clash happens
  to be found;
- an assembly sequence's pose step (`SequencePose`) and its film accept
  any value: a pendulum swung to 30° where its case allows 5° is checked
  and filmed as if it could;
- the intent ("the anchor swings ±4°", "the crank turns freely") cannot
  be written down, so it cannot be a requirement.

## The shape of the fix

1. **The model.** An optional range on each free value of a mate:
   `rotation` (revolute, cylindrical), `offset` (slider, cylindrical,
   planar gap), `dx`/`dy` (planar); a ball takes a cone half-angle.
   Unset means free, as today. Angles in degrees at the RPC with a `_deg`
   suffix, millimetres with `_mm`; radians and mm inside, as the mate's
   own values are. A range whose min is above its max, or set on a value
   the mate type does not free, is refused naming what the type frees.
   Serialized (`vNEXT`), with its golden.
2. **Loop-free assemblies.** No iterative solve runs there: positions are
   computed from the mate values, so a limit is checked where a value is
   set. `set_mate` (and anything that writes a mate's value) refuses
   a value outside its range, naming the range; it does not clamp
   silently.
3. **Loops.** `AssemblyLoopSolve.cpp`'s Levenberg–Marquardt step projects
   each unknown into its range after each update (a bounded LM: the
   Jacobian, the costly part, is unchanged). A loop that cannot close
   inside its ranges reports which limit binds, on the mate, as a closure
   error does today. The branch the loop is on (open or crossed) is kept
   as now; a limit that makes the current branch unreachable says so
   rather than jumping to the other.
4. **Dragging.** The gizmo and drag-within-mate (`GizmoInput.cpp`,
   `DragMath.h`) stop at the limit, not past it and not by snapping back.
   In a loop the drag stops where the first limit binds.
5. **Motion studies.** A driver range that crosses a limit is refused
   before it runs, naming the limit; a follower in a loop that reaches its
   limit stops the run at that sample, and the result says which mate
   stopped it and at what driver value (`MotionStudy.h`'s result).
6. **Sequences.** A pose step outside a limit is refused by
   `set_assembly_sequence`, and the check and the film inherit it.
7. **Requirements.** A requirement kind (or a `measure` on an existing
   one) that a mate's range is reachable without a clash: the generator
   states "the anchor swings ±4°" and the clash check proves the case
   allows it.
8. **The UI.** The mate's panel shows and edits its range; the viewport
   draws the range as an arc (revolute) or a span (slider) while the mate
   is selected.
9. **Generators.** The clock's anchor and pendulum, the hinge box's lid,
   the Strandbeest's crank (free, stated) and the engine's crank (free)
   state their ranges, so the first users are real assemblies.

Contact constraints (parts stopped by their geometry touching rather than
by a mate's range) are not this: they need collision detection inside the
solve, and come after a mesh collision engine exists.

## Files

- `src/core/Occurrence.h` (`Mate`), `src/core/AssemblySolve.cpp`,
  `src/core/AssemblyLoopSolve.cpp`, `src/core/MotionStudy.{h,cpp}`, the
  sequence check; `src/core/io/SerializeAssembly.cpp` (`vNEXT`, golden).
- `src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp`,
  `RemoteRequirements.cpp` and their `describe` rows.
- `src/ui/GizmoInput.cpp`, `src/ui/DragMath.h`, `src/ui/MateActions.cpp`,
  the mate panel, a viewport overlay for the range.
- `generators/{clock,hinge_box,strandbeest,engine}/`.
- `docs/assemblies/README.md`, `docs/versioning/format.md`.

## Verified by

- Unit tests: a value outside its range refused, inside accepted; a range
  on a value the type does not free refused; a four-bar driven until a
  follower's limit binds stops there and names it; a loop seeded on one
  branch with the other made unreachable stays and says so; a ball's
  cone; save and reopen keeps every range.
- The solver's timings before and after on the existing loop tests
  (the 192-bolt pattern, the 64-link chain, the 199-unknown chain), in the
  docs: limits must not slow a solve that does not touch them.
- A GUI test: dragging a limited revolute stops at its limit.
- A smoke: a motion study refused across a limit; a sequence pose
  refused outside one; the requirement passing and failing.
- The generator tests with their ranges stated; the headless, GUI and
  pygui suites.
