# U16 — Rotating a mate, and snapping a rotate drag

> Status: **SHIPPED 2026-09-12.** Written as a plan and built the same
> day; "The problem" below describes the app BEFORE the change.

## The problem

Two parts mate on the right faces and still land the wrong way round.

The mate tool window offers `Flip`, which is the only orientation control it
has: it reverses B's Z against A's (`MateAdjust`'s `rotate(pi, X)`). Flip
answers "which side", not "which way up". A side wall brought to a top wall
picks up whatever spin the two connector frames happened to disagree by, and
nothing in the session can take it out — the panel literally says
"Accept, Flip, or Cancel". Reported from use, with a fastened wall standing
90° out of its seat in both flip states.

There is no second path either. A fastened mate has no DOF, so
`BuildGizmoSpec` returns `false` and no gizmo appears; the mate inspector's
DOF switch ends `case MateType::Fastened: break; // no DOF`. So the twist is
unreachable from the GUI even after the mate is accepted.

Second, smaller: **a rotate drag in an assembly snaps to nothing.** Dragging
an occurrence's rotate ring, free or within a mate's DOF, lands on 87.3°.
The CAM job placement gizmo already snaps a spin to multiples of 45° in the
same file — assemblies, where right angles are the overwhelming case, do not.

## The shape of the fix

### 1. The twist already exists — expose it

`Mate::rotation` (radians, twist about the connector Z) is composed for
**every** mate type, not just the ones that call it a DOF:

```
MateAdjust = T(dx, dy, offset) · Rz(rotation) · R_ball · Flip_x(pi)
```

and `MateToJson` writes it unconditionally. So a fastened mate at
`rotation = pi/2` already solves and already round-trips through a save.
Nothing about the model, the solver or the format needs to change — **no
`kVersion` bump** — the field is simply not reachable from the GUI.

Three edits reach it:

- **Mate tool window** (`DrawMateToolWindow`): beside `Flip`, a `Rotate 90°`
  button that adds a quarter turn (wrapping into `[0, 360)`, so clicks cycle
  0/90/180/270), and an `Angle (deg)` field beside it so a non-right angle is
  reachable and the current twist is visible. Both edit `live->rotation` —
  the same object `Flip` edits — then `SolveActiveAssembly`, and snapshot
  first when the session is an edit of an existing mate, exactly as `Flip`
  does.
- **Mate inspector** (`DrawMatesSection`): `case MateType::Fastened` calls the
  existing `angleDof("Angle (deg)")` instead of `break`. Without this the
  tool window could set a twist that nothing could later read or undo.
- **The prompt**: "Accept, Flip, or Cancel" → "Accept, Flip, Rotate, or
  Cancel", and the over-constrained variant likewise.

Ball is the exception: its `rotation` is degenerate against `ballRotation`
(any Rz folds into the quat), and the inspector has never exposed it. The
control is shown for every type but Ball, which keeps the rule "wherever the
tool window offers Rotate, the inspector has the field".

The RPC surface needs no new parameter — `add_mate`/`set_mate` already take
`rotation`/`rotation_deg` — but their `MethodDoc` prose says rotation is the
starting position "along whatever it leaves free", which reads as *not*
applying to a fastened mate. It is corrected to say a fastened mate's
rotation is the authored twist about the connector axis.

### 2. One angle snap, shared

The snap the CAM placement gizmo spells inline

```cpp
const double nearest = std::round(deg / 45.0) * 45.0;
if (std::abs(deg - nearest) < 5.0) deg = nearest;
```

moves to `ui::SnapAngleDeg(deg, snap)` in `DragMath.h` — where the drag math
the gizmo and the preview handles share already lives — with a radians
sibling `SnapAngleRad` for the assembly callers, and the 45/5 literals as
named constants. A **capture window, not a quantizer**: the common angles are
sticky and everything between them is still reachable, so 37° remains typable
by drag. Ctrl frees it, which is what Ctrl already means in both gizmo drags.

Three call sites then use it:

- CAM placement spin — the literals it came from, so the refactor is a no-op
  there and `a_placement_is_dragged_in_the_viewport` proves it (it asserts an
  88° drag still lands on 90 to 1e-6).
- **Free occurrence rotate**: snaps the *turn*, `angleAxis(SnapAngleRad(
  UnwrappedTurn(...)), A) * g.startRot`. A free occurrence's starting
  orientation is arbitrary, so only the increment has a meaningful zero.
- **Drag-within-mate rotate**: snaps the *absolute* value,
  `mate->rotation = SnapAngleRad(g.startDof + turn)`, because the mate's own
  frame is the zero — a revolute joint should land on 90° from its seat, not
  90° from wherever the drag started. Ball keeps the turn form (it has no
  single angle).

### 3. One polarity, and one mechanism

Shipping (2) left the app saying opposite things with the same key: the gizmo
drags (CAM placement, and now both assembly rotate drags) snapped by default
with **Ctrl to free**, while the preview handles on a part tab ran free by
default and used **Ctrl to quantize** coarse (0.5 units / 5°). "Ctrl frees the
snap" was true on one tab and backwards on the next — an open question in
`direct-manipulation.md` since handles shipped, which this round finally
forced.

The handles are flipped to match: **snap is the default, Ctrl frees it,
everywhere a drag snaps at all.** The mechanism is unified on the gizmo's
capture window rather than the handles' hard quantum, because that is what
makes snapping-by-default free of cost — a quantizer would take 2.3 mm away
from anyone not holding Ctrl, and a capture window does not. So
`SnapAngleDeg` generalises to `SnapValue(v, step, capture, snap)` with two
faces over it: angles capture on multiples of 45° (one round set at every
scale and in every unit), lengths on multiples of 0.5 document units. The
length pair stays a literal, and is flagged as such in `DragMath.h` — 0.5 is
round for a 3 mm fillet and arbitrary for a 2 m beam, so deriving it from the
part is the obvious later move.

This is a rule about polarity, not coverage: a drag that snaps to nothing
(assembly translation, the section offset) still snaps to nothing.

## Files it touches

| file | change |
|---|---|
| `src/ui/DragMath.h` | `SnapValue` + `SnapAngleDeg`/`SnapAngleRad`/`SnapLength` and the 45°/5° and 0.5/0.1 constants |
| `src/ui/PreviewHandles.cpp` | the polarity flip: snap by default, Ctrl frees, capture window not quantum |
| `src/ui/GizmoInput.cpp` | CAM spin calls the helper; the two assembly rotate paths snap (Ctrl frees) |
| `src/ui/OccurrencePanel.cpp` | tool window `Rotate 90°` + angle field; inspector `Fastened` gets `angleDof` |
| `src/ui/MateActions.cpp` | the prompt names Rotate |
| `src/remote/RemoteAssembly.cpp` | `add_mate`/`set_mate` doc prose on `rotation` for a fastened mate |
| `src/core/Occurrence.h` | the `rotation` comment, which said "revolute/cylindrical DOF" |
| `src/app/guitests/GuiTestsAssembly.cpp` | two new cases |
| `src/app/guitests/GuiTestsViewport.cpp` | `a_preview_handle_snaps_unless_ctrl` |
| `tools/designer_mcp.py` | the hand-written mirror of the two RPC doc strings |
| `docs/ui/viewport-handles.md`, `docs/assemblies/{plan,a2_5-design,relations-plan}.md`, `docs/cam/job-document.md`, `docs/ui/direct-manipulation.md` | the snap and the twist, where they describe the gizmo and mates |

No format change: `rotation` is already serialized for every mate type, so
`kVersion` stays at 105 and the assembly goldens are unchanged.

## How it is verified

- `mate_tool_rotates_the_part` (GUI, assembly): fastened-mate two boxes,
  click `Rotate 90°`, assert the mate's `rotation` is `pi/2` **and** the
  driven occurrence's world orientation actually turned — the button must
  move geometry, not just a number. Click four times, get back to 0.
- `a_mate_rotate_snaps_to_the_common_angles` (GUI, assembly): drag a revolute
  ring to ~88° and assert the mate reads exactly 90°; repeat with Ctrl held
  and assert it does not.
- The existing `a_placement_is_dragged_in_the_viewport` (CAM) guards the
  extracted helper against behaviour change, and
  `a_ring_drag_unwraps_past_half_a_turn` (300°, which no multiple of 45
  captures) guards the assembly drag against the snap distorting an ordinary
  turn — it read 299.8° after the change.
- `a_preview_handle_snaps_unless_ctrl` (GUI, handles): the same revolve-arc
  drag, aimed deliberately off a multiple of 45 (360 - 88 = 272), run twice
  from the same mouse positions — plain it reads exactly 270, Ctrl-held it
  stays near 272. Both halves of the polarity, one test.
- Suites: `tools/test-fast.sh` and `./tools/gui-tests-xvfb.sh`.
