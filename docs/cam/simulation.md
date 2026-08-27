# Cutting simulation — watching the stock come off

> Status: **BUILT** (2026-08-27), S1–S5. From: *"a cutting simulation/visualizer
> similar to Fusion 360"*. What follows is the design as written, with an
> [as-built section](#as-built) at the end recording what changed and what the
> first real job measured.
>
> The engine ([plan.md](plan.md)) and the way in ([gui.md](gui.md)) are built.
> C5 already **simulates material removal** — it just never shows it. This is
> about turning a check that prints two numbers into a picture you can scrub.

## What is already here

`src/core/cam/CamVerify.h` is most of a simulator with no display:

| piece | where | note |
|---|---|---|
| G-code parser, modal, per-move line + T | `ParseGcode` | arcs already become chords at parse time, so playback gets G2/G3 free |
| stock as a height map | `MakeStock` | `x0/y0/res/nx/ny` + `vector<float>` |
| swept-tool removal, analytic per cell | `Simulate`, `CamVerify.cpp:240` | exact against the segment, not oversampled |
| the tool's real end shape | `TipHeight`, `CamJob.h:69` | flat / ball / V / drill from ONE function |
| the designed surface on the same grid | `SampleModel` | a flat-tool drop cutter already |
| compare + leftover regions | `Compare`, `LeftoverPoints` | Fusion's *compare* view, shipped as dots |
| cutting time from real feeds | `CuttingMinutes` | rapids excluded unless the setup states a rate |
| backplot, tool glyph, stock box | `CamPanel.cpp:167`, `CamTool.cpp:597` | already in setup space |

So this workstream is **display and transport**, not simulation math. The two
things genuinely missing are a *surface* for the height map and a *cursor*
through the program.

## Surveyed first (`~/Documents/CAD/CAM`)

FreeCAD ships **two** cutting simulators, and the difference between them is
the whole design decision here:

- **`PathSimulator/App/VolSim`** (821 lines) — the height map. `Array2D<float>`
  + a tool profile function, `ApplyLinearTool` / `ApplyCircularTool`, and a
  `Tessellate` that greedily merges equal-height cells into rectangles
  (`FindRectTop`, `TesselTop`, `TesselSidesX/Y`) instead of emitting a quad per
  cell. That merge is worth stealing outright.
- **`PathSimulator/AppGL`** (~4 500 lines) — the newer GUI one. It keeps **no
  stock model at all**: every frame it re-renders every swept tool segment from
  step 0 to the cursor, four times over (forward, reversed, back faces), and
  lets depth and culling do the subtraction (`MillSimulation.cpp:282`). Scrubs
  instantly, needs a whole shader/FBO stack of its own, costs O(steps) draw
  calls per frame — and, being pixels, **can answer no question**. FreeCAD's
  verdict and FreeCAD's picture come from two different engines.
- **CAMotics** — implicit field + AABB trees of tool sweeps + marching cubes.
  General enough for undercuts, which is exactly why it needs its own build
  system. We are 3-axis.
- **Kiri:Moto** (`grid-apps`) — animation is a small part of a 12.4k-line CAM
  mode; the lesson from C1 still holds: the display is cheap once the
  infrastructure exists.

**Decision: one simulator, not two.** The picture comes from the same height
map, fed by the same posted file, as the verdict. This is the C4/C5 rule
("a picture that can disagree with the file is worse than no picture") applied
once more — and it is where FreeCAD's split is a warning rather than a model.
The cost is that the surface must be meshed; the return is that every pixel on
screen is a number the panel can also state.

## The shape

A **transport**, in the CAM panel, under Generate/Post:

```
 ├────────────●──────────────────────┤   64 %   op 3/5 "Pocket floor"
 [◀◀] [▶] [▶|] [▶▶]      speed ×4      T2 6 mm flat · line 812 · 300 mm/min
```

- The **cursor** is a point in the program: `(moveIndex, fraction)`.
- The slider axis is **path distance**, not wall-clock. Cutting time we can
  defend (`CuttingMinutes`, from each move's own feed); a rapid traverse rate
  is a fact about the machine that the setup usually does not state, and
  inventing one would put a fabricated cycle time in front of a user. Distance
  is exact, needs no claim, and animates smoothly. Time is a *readout*, and it
  reads `cutting time`.
- Playback shows the **stock**, the **tool to scale where it actually is**, the
  path **already cut** dimmed and the path **still to come** bright, and the
  model bodies ghosted underneath (`SetInstanceGhost` exists).

A **session, like the CAM verb window — not a mode**, but an honest one: it
hides bodies, so it owns exactly ONE exit path that restores them. The U11
mode-exit bug class (gui.md) came from transitions scattered across call
sites; a single `EndCamPlayback(state)` called from every one of them is the
whole mitigation, and a GUI test asserts visibility is restored.

## Phases

| | scope | size |
|---|---|---|
| **S1** | Cursor + incremental simulate over the posted program | ~250 lines, ½ session |
| **S2** | Height map → surface mesh (run-merged, with skirts and walls) | ~300 lines, ½–1 session |
| **S3** | `StockRenderer` — a dynamic-buffer mesh path that does not stall | ~350 lines + 2 shaders, ½ session |
| **S4** | The tool, to scale, moving; ghosted bodies; past/future path | ~150 lines, ¼ session |
| **S5** | Transport UI, frame pacing, refusals, RPC | ~250 lines, ½ session |
| **S6** | What it can then answer for free — colour by op, holder collision, deviation map | ~200 lines each, on demand |

S1–S5 is the deliverable: **about one and a half sessions.**

### S1 — a cursor through the program

`Simulate` is already a straight loop over moves that only ever lowers cells,
so making it resumable is a signature change, not a rewrite:

```cpp
struct SimCursor { size_t move = 0; double t = 0.0; };  // t = fraction into `move`
void SimulateTo(const GProgram&, const Setup&, HeightMap&, SimCursor to);
```

Forward is incremental and cheap (the swept bbox is small). **Backward is a
re-run from zero**, because the height map is lossy — a cell that was lowered
cannot say what it was. Measure before optimising: the whole-program sim at
0.4 mm already runs inside a button press. If a real job at playback
resolution is too slow to scrub, keyframe the map every N moves (`nx*ny*4`
bytes a snapshot) — that is a cache, not a design change.

Two parser gaps to close while here, both small and both real bugs today:

- **`GMove` drops the F word.** The parser reads `F` into `default:` and throws
  it away, so the verifier cannot check feeds at all and playback cannot show
  the one number a machinist looks at. Add modal `feed` to `GMove`.
- **Tool changes are invisible.** `T` is read; `M6` is not. "Skip to the next
  tool" wants the boundary.

### S2 — the surface

One vertex per cell is 750 k vertices on a 200×150 mm plate at 0.2 mm. Milled
2.5D stock is overwhelmingly **flat**: an uncut top, a few floors, walls
between them. So mesh runs, not cells — merge equal-height cells along a row,
then merge identical rows, exactly as `VolSim::TesselTop` does with a 2D greedy
rectangle. Runs-per-row is most of the win for a fraction of the code; go 2D
only if a measurement asks for it. Between differing heights, emit the vertical
wall — a milled step is a wall, and smoothing it into a ramp is a lie about the
part. Skirt down to the stock floor so the block reads as a solid.

Regenerate the whole mesh per update once merged (small); dirty-rect
bookkeeping is an optimisation to defer.

### S3 — getting it on screen every frame

**`MeshRenderer::UpdateAsset` cannot be used for this.** It does
`vkDeviceWaitIdle` + destroy + recreate device-local buffers through a staging
upload, per call (`MeshRenderer.cpp`). That is correct for a regen and a stutter
machine at 15 Hz.

The pattern that fits already exists in this repo: **`FieldRenderer`** (297
lines) — its own pipeline and vertex format, host-visible persistently-mapped
vertex/index buffers *per frame in flight*, `SetMesh` + a dirty flag,
`EnsureCapacity` growing them, no device wait anywhere. `StockRenderer` is that
file with a lit shader. It must honour the section clip plane (FieldRenderer
does) and depth-test against the scene so the tool and the part interpenetrate
correctly.

Alternative considered: teach `MeshRenderer` a *dynamic asset* kind, which
would buy PBR, shadows and SSAO for the stock. Rejected for v1 — it touches the
shadow-caster collection and every draw path that binds `asset->vertexBuffer`,
which is a viewport-wide regression risk for a cosmetic gain. Promote it later
if the stock looks wrong beside the part; the FEM field sets the precedent that
it will not.

### S4 — the tool

Revolve the `TipHeight` profile and you have every tool the app models,
correct by construction and sharing its shape with the removal math. Add the
shank above the flutes (`fluteLength`) and a stub holder. One instance, moved
by transform per frame — free.

### S5 — transport

Play/pause/step/scrub, speed multiplier, jump to next operation and next tool
change, and the readout (op, tool, line, feed). Three things this codebase
specifically requires:

- **Frame pacing.** `FramePacer` (`src/app/FramePacer.h`) parks the app on
  `glfwWaitEvents` when nothing is happening; playback needs its own wake
  reason beside `cameraAnimating` or it will render one frame and stop.
- **Refuse visibly.** No toolpaths, or a job that failed to post → the
  transport says so where the buttons are. The `defaults/*` gate exists for
  exactly this.
- **Never regenerate during playback.** Op add/delete already freezes the app
  for ~a second (`docs/known-bugs.md`); playback reads the cached
  `camToolpaths` and the last posted text, and a model edit *invalidates* the
  playback rather than silently re-cutting under the cursor.

RPC: `cam_simulate {setup, at}` returning the height map's summary at that
cursor (and, with `render`, a still) — so the whole thing is drivable headless
and by an agent, like every other verb here.

### S6 — what the height map can then answer

Cheap, once S1–S3 exist, and each one is a question the panel can already
almost answer:

- **Colour by operation.** A parallel `vector<uint16_t>` of "which op last cut
  this cell", written in the same loop. Answers *which operation left that?*
  and makes a redundant op visible as an area nothing else touched.
- **Holder and shank collision.** `TipHeight` generalises to a radius-above-tip
  profile for the whole assembly; a cell whose stock stands above the shank
  envelope is a collision, reported with its G-code line — the same shape as
  the existing `rapidsThroughStock` check, which is already trusted.
- **Deviation map.** `machined - designed` on the same grid, painted on the
  stock. `Compare` computes it now and throws away everything but the extremes.

## Excluded on purpose

- **Undercuts, 5-axis, continuous 4-axis.** A height map cannot represent them
  and this CAM cannot cut them. When the A axis moves, this needs revisiting
  with the rest of C9's deferred half.
- **Machine/fixture collision, work-envelope checks.** A different model (the
  machine, which the app does not have) rather than a harder version of this
  one.
- **Chip load, force, deflection.** Would need feeds and speeds this app
  deliberately does not compute (`CamJob.h`: a wrong number here breaks a tool).
- **Real-time clock playback.** Requires a rapid traverse rate and acceleration
  the setup does not state. See the cursor decision above.

## Gates

- Headless: `SimulateTo(end)` is **bit-identical** to `Simulate` — the picture
  and the verdict cannot drift apart, which is the premise of the whole design.
- Headless: a known map (a 10 mm pocket in a 50 mm block) meshes to the right
  bounds and the right enclosed volume; run-merging changes the mesh, never the
  volume.
- Headless: the parsed feed is modal and matches what the post emitted; the
  distance axis is monotonic and ends at the toolpath's own length.
- GUI: play → scrub → exit restores body visibility and leaves no ghost (the
  U11 class), and a screenshot of a half-cut pocket goes in the CAM suite.
- **No format bump.** Playback is session state; nothing here serializes.

## As built

Built in the order above; S1–S5 landed together. What the code argued back:

**The file already said which operation a move belongs to.** The plan assumed
op attribution would need the toolpath list matched back against the program —
which cannot be done reliably, because the post interleaves retracts and tool
changes. But `Emitter::Comment` has been writing `(op 7)` since C4, and the
parser was throwing comment text away one line before it could read it.
Keeping the text costs six lines and the boundary comes out of the FILE, which
is the thing being simulated. (`(SKIPPED op 9: …)` means the opposite, so the
match is exactly `op <digits>` and nothing else — there is a test.)

**The parser was dropping the F word entirely.** `F` fell into the `default:`
of the word switch, so the one number a machinist looks at was the one number
nothing downstream could see: no feed check in the verifier, and no honest
clock here. It is on `GMove` now, modal, and a non-positive feed is reported
rather than adopted. `M6` was invisible too — both were real gaps, found by
needing them rather than by looking.

**One carve, not two.** `Simulate`'s inner loop became `CarveSweep(from, to,
tool, stock)` and everything calls it: `Simulate`, `Inspect`, and the
playback. That also removed a per-move `GProgram` allocation from `Inspect`'s
hot loop, which was building a two-move program for every cutting move.

**Every segment is carved from its own start.** The obvious implementation
carves the slice between the old cursor and the new one, and it is wrong in a
way that would never have been noticed: a sub-segment clamps the tool's Z at
the split, so a program with an ascending cut could finish a hair deeper than
the same program run in one pass. Carving each segment from its own start (the
current one truncated at the cursor) is idempotent, costs nothing, and makes
the end state **bit-identical** to the map `Inspect` reads the verdict from —
which is the property the whole "one simulator" argument rests on, and now a
test.

### What it measured

A 60 × 40 × 12 plate, faced and profiled with tabs — 5 677 mm of path, 316
lines of Mach3:

| | |
|---|---|
| play to the end, 0.25 mm grid | **25 ms** |
| play to the end, 0.1 mm grid | **46 ms** |
| scrub backwards (re-runs from pristine) | **3–8 ms** |
| step forward one frame | **2–4 ms** |
| the machined block, 24 249 cells | **28 triangles** |

Two things follow. The keyframe cache S1 hedged about is **not needed** — a
full re-run of a real program is a frame, so backward scrubbing can stay the
simple thing. And run-merging does what it was chosen for: a profiled block is
flats and walls, and 24 249 cells come out as 28 triangles. The gate that
pins it (`triangles < cells / 4`) has three orders of magnitude of headroom
today, which is the point — it fails loudly if the mesher ever degrades to
per-cell.

Removed volume on that job: **13 871 mm³** against 14 208 mm³ of stock-minus-
part, the difference being four tabs and grid quantisation. That number comes
back over the RPC (`removed_mm3`), so a headless caller can assert a
simulation did something without owning a renderer.

### Where it differs from the plan

- **The stock's `EnsureCapacity` does wait — but only on a GROW.** Buffers
  double, so a playback pays it a handful of times at the start and never
  again. The steady state is a memcpy into a mapped, per-frame-in-flight
  buffer, which was the point.
- **The tool is a `MeshRenderer` instance, not part of the stock draw.** It is
  static geometry that MOVES, which is the one thing that renderer is best at:
  built once per tool number, then a transform per frame. It gets PBR and
  shadows for free; the stock, which changes every frame, does not.
- **Bodies are ghosted at 0.22, not hidden.** Seeing the part inside the block
  is most of why the picture is worth having. Re-applied every frame, because
  instance ids change on every regeneration — an "already ghosted" early-out
  can hold only stale ids, which is the bug the FEM overlay shipped once.
- **`cam_simulate` generates the toolpaths if there are none.** A GUI user has
  pressed Generate; an agent has not, and refusing would only teach it to
  press an invisible button first.

### Still open

- The stock re-meshes whole on every advance. It is 1–2 ms on a plate and
  scales with the grid; a part four times the area at 0.1 mm would be worth a
  dirty-rect rebuild. Measure before building it.
- S6 is untouched: colour by operation, holder collision, deviation map. All
  three are now cheap — the height map, the op id per move and the tool
  profile are all in place.
