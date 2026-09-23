# Move a fixture with arrows, and let it snap onto the stock

## The problem

The two-click seat (fixture-seat-plan.md) works, but the person it was built
for could not tell how to use it. It asks for a face and then a spot, in a
viewport that shows neither prompt near the fixture. The rest of the app moves
things with arrows:
- the assembly gizmo moves occurrences;
- the job tab's placement gizmo moves the parts in the stock.

A fixture had no gizmo at all, only numbers in bed coordinates.

## The fix

### 1. A fixture gizmo (`src/ui/GizmoInput.cpp`)

While a fixture's editor is open, it gets the gizmo that parts get on the job
tab. It needs no pick: the fixture being edited is the one the handles move.
It is drawn at the top centre of the fixture as drawn.

**Handles:**
- X, Y and Z arrows move it;
- the Z arc spins it.

Nothing tilts it, because a fixture turns about Z only.

**Snapping to faces.** It snaps unless Ctrl is held, the app-wide polarity.
- **What moves:** the fixture's own FLAT faces square to the dragged axis,
  gathered once at grab from the part as placed. These are real faces, not
  its bounding box: a step clamp's pad underside, its bar underside and its
  stud's bottom are three separate candidates in Z.
- **What it snaps to:** the stock's planes on that axis. That is both
  sides in X and Y; the top and the bed (the stock's bottom) in Z.
- **How close:** within a screen-sized threshold. The nearest pair wins,
  whether that is contact or flush.
- **Spin** snaps to 45° (`SnapAngleDeg`), as a part's does.

**The editor says what it snapped to** while you drag, e.g. "Pad on the stock
top". The label is kept on `gizmoDrag`.

### 2. The seat comes from where you put it

A mate in the assembly sense, made by snapping rather than by a separate
tool.

- **On release,** the fixture is seated when a flat, named, downward face of
  it lies ON the stock top and over it. If several do, the one with the
  largest overlap wins.
- **The seat's numbers are read from the pose:** `cam::SeatFromPose`, the
  inverse of `SolveFixtureSeat`.
  - Edge: the one the nose points in over.
  - Spin: what is left over from pointing straight in.
  - Over and along: measured from that edge.
- A seated fixture keeps following the stock exactly as before.
- **Dragging a seated fixture:**
  - in X or Y, it slides and is re-seated on release;
  - off the top in Z, it unseats;
  - spun, its spin is re-read.

There is no separate "rest it" gesture any more. The two-click arming is
REMOVED: the button, `state.camSeat`, the selection-pipeline ray hook,
`ArmFixtureSeat` and `FixtureSeatClicked`. It is dead code once the arrows
do the job, and a second way to do the same thing is what made this
confusing.

The editor keeps:
- the seated numbers (Over the edge, Along, Over the stock, Spin);
- Unseat;
- the bed position and "Put it" for a fixture standing on its own.

The model, the format (v127) and the RPC are unchanged.

## Files

- `src/core/cam/CamToolpath.h`, `CamSetup.cpp`: `SeatFromPose`.
- `src/ui/AppState.h`: `gizmoDrag` gains the fixture id, its face planes per
  axis at grab, and the snap label. `camSeat` goes.
- `src/ui/GizmoInput.cpp`: the fixture gizmo, with its publish and its
  dispatch.
- `src/ui/CamTool.{h,cpp}`: the two-click code removed; the editor states
  the snap.
- `src/ui/SelectionPipeline.cpp`: the hook removed.
- Tests; docs/cam/gui.md; and a note in fixture-seat-plan.md that the
  gesture was replaced.

## Verification

- **`test_cam_workholding.cpp`:**
  - `SeatFromPose` inverts `SolveFixtureSeat`, for every edge and with a
    spin.
  - The edge it picks is the one the nose points in over.
- **GUI test `cam/a_clamp_is_dragged_onto_the_stock`,** with rays handed to
  `HandleGizmoInput`, as `a_placement_is_dragged_in_the_viewport` does:
  - Z-drag the clamp down near the stock top: the pad snaps onto it and it
    is seated on release.
  - X-drag it in: along and over change, and it stays seated.
  - Grow the stock: it follows.
  - Z-drag it up: it unseats.

  The spin arc is NOT driven by the test: hitting an arc with a scripted
  ray is its own problem. The spin reuses the placement gizmo's snapping
  (`SnapAngleDeg`) and `UnwrappedTurn`, and turns about the handle.
- **The existing tests move off the two clicks:**
  `a_clamp_is_seated_by_two_clicks` is replaced by the above.
- **A screenshot** of the gizmo on a clamp, looked at.
- **Suites:** `tools/test-fast.sh`; the GUI suite; the pygui smokes.
