# A clamp sits ON the stock: fixture seats

> The two-click gesture in section 4 was replaced by the fixture gizmo: a
> fixture dragged onto the stock's top rests there
> ([fixture-gizmo-plan.md](fixture-gizmo-plan.md)). The seat, its solve,
> the format and the RPC below are unchanged.

## The problem

A step clamp's job is to rest its nose on the top of the stock and press it
down. The fixture editor cannot put it there.

- **Placement is a bounding-box rule.** `PlaceFixture` (CamDerive.cpp) has:
  - four sides, which stand the fixture's box on the bed against a side of
    the stock;
  - Centre;
  - Top, which rests the box's BOTTOM on the stock's top.

  A clamp's box bottom is the bottom of its studs. So "Top" stands the studs
  on the board with the nose in the air, and every side puts the clamp next
  to the board rather than over it.
- **The result is an absolute bed position** (`Fixture::pos`, `rotZdeg`).
  The stock moves when its margins, its mode or the parts change. A clamp
  placed against the old stock then floats or is buried, and nothing says
  so. That is the "defaults derive from what they act on" rule, broken by
  a placement.
- **The editor speaks in bed coordinates** (`-146.757, -50.000, 18.000`),
  which say nothing about where the clamp is relative to the work.

What a person means is a MATE: this face of the clamp rests on the top of
the stock, this far along that edge, reaching this far in. The assembly
mate system is not the thing to reuse:
- the job is not an assembly;
- the stock is a derived box with no named faces;
- a clamp needs one planar seat and a slide, not six mate types and a loop
  solver.

The idea to reuse is a face-to-face relation, stored and re-solved.

## The fix

### 1. A seat on the fixture (core, `CamJob.h` / `CamSetup.cpp`)

```cpp
enum class StockEdge : uint8_t { MinusX, PlusX, MinusY, PlusY };
struct FixtureSeat {
  ElementRef face;        // the fixture's face that rests on the stock top
  StockEdge edge;         // the edge of the stock top it reaches in over
  double alongMm = 0.0;   // from that edge's low end to the face's centre
  double overMm = 0.0;    // how far the face reaches in past the edge
  double spinDeg = 0.0;   // turned from pointing straight in
};
struct Fixture { ...; std::optional<FixtureSeat> seat; };
```

**`SolveFixtureSeat(stockLo, stockHi, fixtureModel, seat, pos, rotZdeg,
why)`** is pure, and lives in CamSetup.cpp because that file builds with
CAM off, the way fixtures are drawn.

- **The face must face down.**
  - The face is resolved by name in the fixture's own model (explorer order,
    as `CADShape::FacePlane`). It must be planar with its outward normal
    -Z.
  - A fixture only turns about Z, so any other face is REFUSED, with the
    reason. It is not tilted to fit.
  - A face that no longer resolves is refused by name.
- **Spin.** The nose direction is the XY vector from the fixture's bounding
  box centre to the face's centroid (+X if they coincide). The spin turns
  that direction to point INTO the stock, square to `edge`, plus `spinDeg`.
- **Position.**
  - The face's tight box, turned, fixes the inward reach, which is placed
    `overMm` past the edge.
  - The face's centroid, turned, is placed `alongMm` from the edge's low
    end.
  - The face's plane goes onto the stock top.

**`SeatAtPoint(stockLo, stockHi, point, seat, why)`** turns a click on the
stock top into a seat:
- the nearest edge;
- `overMm` = the click's distance in from it, so the face's innermost point
  lands where you clicked;
- `alongMm` = the click's position along the edge.

A click off the top is refused. Every number comes from the stock and the
click; none is a literal.

**Edge names** are `-x|+x|-y|+y` (the `FixtureSide` spelling). An unknown
one is REFUSED, naming the four.

### 2. Format, v127 (`SerializeCam.cpp`)

The seat is written as:

```
seat: {face: {feature, name}, edge, along_mm, over_mm, spin_deg}
```

It is omitted when absent. `pos` and `rot_z` are still written, holding
the last solved pose, so a reader that cannot solve still has a sane place
to draw it.

- `kVersion` goes to 127.
- New goldens: `tests/golden/job/v127-cam-fixture-seat.json` and
  `.expect.json`.
- A row in docs/versioning/format.md.

### 3. Every reader re-solves

**`ui::FixtureBedTransform(state, setup, f, why)`** gives the seated pose
from the stock as it is NOW (`StockBoundsFramed` over `JobPlacedBodies`).
It falls back to the stored pose only when the seat cannot solve, and then
says why. The three places that read a fixture's transform go through it:
- RendererSync's ghost;
- ViewActions' fit;
- `JobFixtureSolids`, which feeds verify and safe Z.

`JobRenderVersion` mixes the seat, AND the stock's definition (mode,
margins, size). A stock edit bumps no version, so without the stock in the
key the ghost would not follow the stock it is seated on.

Setting or editing a seat writes the solved pose back to `pos`/`rotZdeg`
through `CamFixturesChanged`.

### 4. The gesture (SelectionPipeline + CamTool)

In the fixture editor, a part fixture shows **"Seat it on the stock"**. A box
has no named faces, so it does not.

1. **Step one.** It arms the step "Click the face of <name> that rests on
   the stock".
   - The click is a ray, tested against the PLACED fixture solids with
     `IntCurvesFace_ShapeIntersector`.
   - Fixtures are not in the face selector, and do not need to be for this.
   - The face hit is named through the body's `ElementMap`.
   - A face that does not face down is a miss, and the miss says why.
2. **Step two.** "Click where it presses on the stock": the ray meets the
   plane of the stock top inside the stock, which gives `SeatAtPoint`.

The state is `state.camSeat` (fixture, step, face, miss). While it is armed,
`HandleSelectionInput` hands it the ray before any candidate is gathered, and
`ArmedPrompt` shows its prompt or miss in the pill. Esc disarms.

### 5. The editor and the row

**A seated fixture's editor shows:**
- the edge as four buttons;
- **Along** and **Over the stock** (mm);
- **Spin** (degrees, from pointing straight in);
- **Move: click a new spot**, which is step two again, with the face kept;
- **Unseat**, which keeps the pose as a free position.

The bed-coordinate Position field and the side buttons are for unseated
fixtures only.

**The panel row's tail** reads "over the -X edge" for a seated fixture. It
says "its seat is gone: <why>" when the seat cannot solve.

### 6. RPC (`RemoteCam.cpp`)

`set_job_fixture` takes:

```
seat: {feature_id, element, edge, along_mm, over_mm, spin_deg?}
```

or `seat: null` to unseat, and it refuses `seat` together with `pos` or
`side`.
- The response returns the solved `pos` and `rot_z`, or the refusal.
- Fixture listings carry the seat.
- The describe row documents it, with its units (rpc_units_gate).

Not in this round:
- **Dragging a seated clamp in the viewport.** Moving it is a click on the
  new spot, which covers the same need without making fixtures pickable
  through the face selector.
- **Guessing the contact face.** It is picked, which is never wrong.
- **Seats on a side face**, for vise jaws and stops. The model has room
  for them (a stock face instead of the top), but no gesture yet.

## Files

- `src/core/cam/CamJob.h`, `CamToolpath.h` (declarations), `CamSetup.cpp`: the
  seat, its solve, `SeatAtPoint`, edge names.
- `src/core/io/SerializeCam.cpp`, `SerializationInternal.h`: v127.
- `src/ui/JobActions.{h,cpp}`: `FixtureBedTransform`, `JobRenderVersion`,
  `JobFixtureSolids`.
- `src/ui/RendererSync.cpp`, `src/ui/ViewActions.cpp`: through it.
- `src/ui/AppState.h`: `camSeat`.
- `src/ui/SelectionPipeline.cpp`: the ray hook and the prompt.
- `src/ui/CamTool.cpp`: the gesture and the editor.
- `src/ui/CamPanel.cpp`: the row tail.
- `src/remote/RemoteCam.cpp`: the RPC.
- Tests and docs as below.

## Verification

- **`tests/test_cam_workholding.cpp`:**
  - A step clamp (a bar with a nose pad and two studs) seated on the -X edge
    has its pad's underside on the stock top. The pad reaches exactly
    `overMm` in, its centre is `alongMm` along, and its studs hang off the
    edge.
  - All four edges put the nose pointing inward.
  - Growing the stock's margin moves the seated clamp with the edge. An
    unseated one stays put.
  - A face that does not face down, or no longer resolves, is refused by
    name.
  - `SeatAtPoint` of a click 12 mm in from the +Y edge gives `+y` and
    `overMm` 12. A click off the top is refused.
  - An unknown edge name is refused, naming the four.
- **Golden v127:** a seat round-trips.
- **The GUI test `cam/a_clamp_is_seated_by_two_clicks`:**
  - arm, then a ray at the pad's underside, then a ray at a spot on the stock
    top, through the real selection path;
  - the fixture is seated where the spot was, and the ghost's instance
    transform matches the solve;
  - "Unseat" keeps the pose.
- **`headless_smoke.py`:** `set_job_fixture` with a seat, a refusal, and
  `seat: null`.
- **A screenshot** of the seated clamp on a real stock, looked at.
- **Suites:** `tools/test-fast.sh`; the GUI suite; `rpc_units_gate`; the
  golden gate.
