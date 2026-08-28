# Which axis is up — the Y-up decision, and undoing it

> Status: **SHIPPED 2026-08-28** — Z1+Z2 (the constant, the routing, the
> gate) in one commit, Z3+Z4 (the flip + render layer) in the next; all
> three suites green, screenshot pass done (upright iso/front/top, RT still
> with a horizontal floor and an upright HDRI room, CAM deriving a pocket on
> the visual top face with zero CAM changes). What the plan predicted held:
> the flip itself was one line, the named-view/plane/gravity fallout came
> out through `conv::` automatically, and the hand-work was exactly the
> predicted list — four equirect sites, the stock shader's light rig, the
> glTF swizzle, two smoke-test view expectations, four GUI tests staged
> Y-up, and one plate built thin-along-Y in two drawing tests. One surprise:
> the Camera CONSTRUCTOR seeded its roll from the identity quaternion's +Y
> and needed an explicit `SetUp(kWorldUp)`.
>
> (Original plan below, kept as written 2026-08-27, revised 2026-08-28 after
> a code audit. Written after the CAM cutting simulation drew a solid
> spindle lying on its side ([cam/simulation.md](cam/simulation.md)) — the
> fourth subsystem to collide with the same choice.)
>
> This is a **sensitive, wide, shallow** change. It touches almost nothing
> deeply and almost everything a little, and the test suite is known to be
> weak exactly here: the drawings round proved that RPC-level tests miss an
> axis convention entirely. Read the [risks](#risks) before the phases.

## The problem, stated once

Three conventions meet in this application, and only two of them agree:

| | up axis | not negotiable because |
|---|---|---|
| **Machine tools / G-code** | **Z is the spindle** | ISO 841 / RS-274. Every post, every controller. FreeCAD's `PathSimulator`, CAMotics, opencamlib, Kiri:Moto — all height maps and drop cutters in Z |
| **CAD & DCC modelling** | **Z is up** | FreeCAD's `Camera::top()` is the *identity* rotation (`Gui/Camera.cpp:34`) — Coin's camera looks down −Z, so "down Z" IS the Top view. Blender is Z-up. SolidWorks, Fusion, Onshape, solvespace, OpenSCAD all Z-up |
| **Real-time graphics** | **Y is up** | glTF is Y-up. Blender's exporter swizzles `(x, z, −y)` on the way out (`io_scene_gltf2/blender/exp/nodes.py:598`) precisely because it is crossing this boundary |

In FreeCAD, Fusion and SolidWorks the first two agree: world up = +Z = the
spindle axis, and CAM inherits the world frame for free. There is nothing to
reconcile.

**This application took the third row — the renderer's convention — for its
document model.** Evidence, from our own code:

- gravity defaults to `(0, −9.81, 0)` (`src/ui/StudyPanel.cpp:590`, with a
  comment defending it)
- the **XZ** plane is named "Top Plane", the **XY** plane "Front Plane"
  (`src/cad/SketchPlane.cpp:47-58`)
- the Top drawing view looks along `(0,−1,0)` (`DrawingDocument.cpp:154`) —
  while the *header* above it (`DrawingDocument.h:189-193`) still describes
  the original Z-up table it replaced. The comment and the code disagree
  today; the flip resolves it in the header's favour.

We adopted a **view** convention as a **model** convention. The renderer never
needed it — "up" there is one vector in a look-at.

## Why this is worth doing

Because it has now been got wrong four times, by four different pieces of work,
for the same reason. `docs/dogfooding.md:364`, written after the drawings
round, already names the pattern:

> This is the third module to get the axis convention wrong (CAM's Z,
> simulation's gravity) — **the pattern is that a module written against the
> CAD habit meets an app that chose Y-up.**

1. **Simulation gravity** was −Z while up was +Y (`docs/simulation/`, U11).
2. **Every named drawing view was the wrong view** — a Z-up `OrientationAxes`
   table survived in a Y-up app, so "front" showed the top on every drawing
   anyone had made.
3. **CAM's Z** — named in that same sentence, never fixed.
4. **The cutting simulation** made #3 undeniable: a thin backplot line looks
   like a line from any angle; a solid cutter lying on its side does not.

And a fifth, found by this audit: the MCP docstring for studies documents
gravity as "e.g. `[0,0,-9.81]`" (`tools/designer_mcp.py:1568`) — **already
written Z-up, wrong against today's app**. An agent copying the documented
example gets sideways gravity right now. (Fix the docstring immediately if
the flip stalls; the flip makes it correct as-is.)

There is no reason to expect the sixth subsystem to be different. Every
convention we have to interoperate with — G-code, DXF, drawing practice, other
CAD — is Z-up, and Y-up buys exactly one thing, which is matching a habit in
a layer that does not care.

## The insight that makes it affordable

**This is a convention change, not a data migration.** No stored coordinate
has to change. What changes is which axis the application *calls* up, and
every default derived from that.

Consequences worth stating plainly:

- **Document goldens survive.** Sketch planes are serialized as raw
  `origin/normal/uAxis` vectors, datum base planes as `"xy"|"xz"|"yz"`
  (`SerializeFeatures.cpp:530,438`) — geometric, not named. Camera state is
  not persisted at all. `test_golden.cpp` and the corpus do not move. No
  `kVersion` bump.
- **…with two named exceptions.** (a) Drawing views store an orientation
  *name* (`Serialization.cpp:1333`) resolved through `OrientationAxes` at
  render time — every already-saved drawing changes what it depicts when the
  table flips. (b) Saved CAM `safe_z` / stock margins were computed against
  Y-up world bodies and will mean something different. Both corpora are
  scratch projects; re-derive or accept, per Z6.
- **CAM's bug disappears with no CAM-core change.** `BodiesInSetupFrame` at
  `rotationA = 0` is an early-return identity (`CamToolpath.cpp:1117`), so
  setup space (Z = spindle) and world space (Z = up) finally agree. Do NOT
  write the CAM-local frame workaround: it is exactly the code this deletes.
  The workaround that DID ship is GUI-side: the "Look down the spindle"
  button and its explanatory strings (`CamTool.cpp:399-441`) — post-flip it
  duplicates Top view and its text becomes false. Delete or reword in Z3.
- **The drawings fix gets reverted.** `OrientationAxes` goes back to the Z-up
  table it originally was — which is itself the evidence that the CAD-habit
  code was right and we bent it to fit. The revert also re-trues the header
  comment above it and must be done together with `test_drawing_hlr.cpp`
  (`:45-46,156` pin the Y-up vectors — the very test that missed the bug).
- **Existing projects come up lying down.** The scratch projects (the dock, the
  enclosures, the C-clamp, the screwdriver set) keep their geometry and change
  their apparent orientation. One-shot re-orientation, or accept it.

## Where the flip *adds* code instead of deleting it

Two places, both at format boundaries, both easy to forget because they are
correct today by accident:

- **glTF/GLB export writes model coordinates unswizzled**
  (`MeshExport.cpp:130-137`, no `SetCoordinateSystemConverter`). Y-up model →
  Y-up format is accidentally right; after the flip every exported `.glb` lies
  on its side in every viewer until a `(x, z, −y)` converter is added — the
  Blender-exporter case from the table above, now on our side of the boundary.
  OBJ/PLY share the code path (weaker convention; decide, don't drift).
  STEP/STL/BREP write raw coordinates to Z-up consumers and *improve* with no
  change.
- **The Onshape importer remaps Z-up to Y-up by plane name**
  (`tools/onshape_import.py:1907-1910` `BASE_PLANES`, `:2071-2073`
  `NEUTRAL_PLANES`: `"Topplane"` → normal `(0,1,0)`, …). These tables must
  become the identity at the flip. They do not grep as vec3 literals.

## The durable fix, which is not the flip

**There is no `kWorldUp` anywhere in this codebase.** The axis is implicit in
**69** literal `vec3(0,1,0)`s (38 of them in GUI tests, ~31 in production
code) and in every table that names a view, a plane or a direction. *That* is
why it has been got wrong four times — not because Y was the wrong choice, but
because the choice is nowhere stated.

Worse, the tables are duplicated: the plane names exist in
`SketchPlane.cpp:47-58` **and** hardcoded again as `kPlaneNames[3]` in
`FeatureTreePanel.cpp:184-185`; the named-view table exists in
`Commands.cpp:353-360` (keyboard/menu) **and** `RemoteServer.cpp:636-643`
(the `view` RPC, `screenshot(view=)`, `render_sheet`); the plane vocabulary
again in `tools/designer.py:34-38` and the MCP docstrings
(`designer_mcp.py:113`). A flip that hits one copy and not its twin is the
drawings bug again.

So the first phase is not the flip. It is:

```cpp
// core/Conventions.h
inline constexpr glm::dvec3 kWorldUp{0, 0, 1};
inline constexpr glm::dvec3 kWorldForward{0, -1, 0};
```

plus a **conventions gate** — and NOT as a regex source scan.
`test_conventions.cpp`'s own header calls source-scanning "a blunt
instrument, deliberately narrow"; this gate checks *values*, so it is a normal
linking test that calls the real functions: `OrientationAxes("top")`,
`SetNameFromType`, the `kViews` map, the gravity and load-direction defaults,
the CAM setup frame — and asserts they all agree with `kWorldUp` **and with
each other's duplicates**. With that in place the flip is a one-line change
plus fallout, and the *sixth* module cannot drift.

## Phases

| | scope | size |
|---|---|---|
| **Z1** | `kWorldUp` + the conventions gate (linking test, cross-checking the duplicated tables), asserted against the CURRENT (Y-up) value. Nothing changes behaviour; the gate goes green on today's code | ~½ session |
| **Z2** | Triage the 69 literals (38 are GUI tests): which are world up, which are a local basis, a UV axis, a face normal. Route the world-up ones through `kWorldUp`. Collapse the duplicate plane-name and named-view tables to one source each. Include the non-literal `.y` reads: `Measure.cpp:1019-1031` (`facing()` labels faces "top"/"bottom" from `d.y` — reaches `describe` and selection UI) and `FemStudy.h:52-54` (load-direction fallback `(0,−1,0)`) | ~½ session |
| **Z3** | **The flip.** `kWorldUp` becomes +Z; fix everything the gate now fails on: both named-view tables, view cube (`UI.cpp:161-166` — labels and hit-test share the table) + its home-corner iso (`UI.cpp:186-189`), `orient_to_face` via `AnimateToPosition`'s hardcoded up (`Camera.cpp:195-198`), `DefaultIsometricPosition` (`Camera.h:17-20`) + Reset Camera (`MainMenuBar.cpp:186-189`), `set_camera`'s default up (`RemoteServer.cpp:1671-1681`), plane naming (both copies), gravity default + `GuiTestsSimulation.cpp:28`, drawing orientation table (a revert, with `test_drawing_hlr.cpp` and `test_camera.cpp`), the Onshape importer plane tables → identity, delete/reword CAM's "Look down the spindle", and the MCP/RPC doc text (`designer_mcp.py:113` plane mapping, `RemoteServer.cpp:629` comment, section help). GUI tests that click `"Top Plane"` by name (`GuiTestsSketch.cpp:182,192`) silently change which plane they sketch on — re-check their geometry assertions, don't just re-green them | ~½–1 session |
| **Z4** | The render layer, which genuinely bakes Y-up in and is NOT a revert. The equirect polar axis lives in **four** places that must match: `IBL.cpp:36` (`DirToEquirectUV`), `IBL.cpp:42` (`EquirectUVToDir`, its inverse), `mesh.frag:54`, and the ray-tracing copies `closesthit.rchit:80-85` + `miss.rmiss:25-28` (the background of every `render_still`). The key light `normalize(1, 1.5, 1)` lives in **three**: `MeshRenderer.cpp:292` (+ fill :298, rim :304), `ShadowRenderer.cpp:128`, `RayTracingRenderer.cpp:743-746`. Plus the CAM stock shader's own light rig (`stock.frag:19-21`) and the offline-render **shadow-catcher quad** built in XZ at `y = bbMin.y` (`AppActions.cpp:3866-3882`, on by default) — post-flip it is a vertical wall through the part. The HDRI itself is fine; it follows `dirToUV`, and the studio floor ending up under the part is the visual check | ~½ session |
| **Z5** | The screenshot pass: CAM, drawings, sketching, assemblies, FEM, exploded views, sections, `render_still` with ground plane. This is the phase that actually finds the misses | ~½–1 session |
| **Z6** | Re-orient the scratch projects, or decide to leave them; re-derive or accept saved drawing orientations and CAM `safe_z`/margins (the two stored-name exceptions above). Decide what Box's `height` means now — its RPC contract says "Y extent" (`SerializeFeatures.cpp:905-908`), which becomes horizontal; rename or re-document. Sweep the docs that state the old convention (`known-bugs.md:10`, `dogfooding.md:353`, `cam/gui.md:195-199` — that paragraph is simply deleted, `cam/simulation.md:342`, `ui/u11-simulation-setup.md:14`) | small |

**~2–3 sessions**, most of it shallow. Z1 and Z2 are worth doing **whether or
not the flip ever happens** — they are the part that stops the recurrence.

## Risks

- **The suite will not catch a miss.** This is the central risk and it is
  proven, not theoretical: 753 tests missed every drawing view being wrong,
  because the HLR tests pass direction and up vectors in by hand and never go
  through a named orientation. Mitigation: Z1's gate crosses the two, and Z5 is
  eyeballs on renders, budgeted as its own phase rather than assumed.
- **Wide, shallow changes hide in the tail.** The 69 literals are the visible
  part; an assumption written as `p.y` rather than `dot(p, up)` does not grep.
  Two are already found (`Measure.cpp` face labels, `FemStudy.h` load
  fallback) and promoted into Z2 as gated items precisely because they are
  checkable, not eyeball-able. Mitigation: Z2 is a read, not a replace-all.
- **Duplicated tables drift independently.** Plane names ×2, named views ×2,
  plane vocabulary ×2 more in python/MCP. The drawings bug was one copy bent
  to fit while its neighbours weren't. Mitigation: the Z1 gate asserts the
  copies agree; Z2 collapses what it can.
- **A half-done flip is worse than either end.** If Z3 lands and Z4 does not,
  the model is Z-up and the lighting is sideways — and Z4 is bigger than it
  looks (4 equirect sites, 3 key-light copies, 2 extra rigs). Z3 and Z4 ship
  together or not at all.
- **Green tests can lie after the flip.** GUI tests that pick planes by the
  *name* "Top Plane" keep passing while testing a different plane. Re-read
  what each renamed-artifact test now exercises.
- **Parallel work.** Another session is editing `src/ui/*` right now
  (expressions in drag fields). This change touches many of the same files;
  integrate serially, per the worktree rule in CLAUDE.md.

## Excluded on purpose

- **Migrating existing documents by rotating geometry.** The features are
  sketches on named planes and extrude directions; rewriting them is a
  different and much harder job than changing a convention, and the corpus is
  six scratch projects with no users behind them.
- **Making the up axis configurable.** Two conventions in one application is
  the disease, not the cure.

## Gates

- `kWorldUp` is the only place the up axis is stated, and the conventions
  gate (a linking test that calls the real tables, not a source scan) fails
  if a named view, a plane name — in either copy — the gravity or
  load-direction default, the face-labeling in `Measure.cpp`, or the CAM
  setup frame disagrees with it.
- The drawings case that started this: a plate 80 × 8 × 60 shows **80 × 8** in
  the front view and 80 × 60 in the top view.
- CAM: `cam_derive_op` on the visual top face of a beam derives a pocket
  instead of refusing it as a wall; the simulated spindle stands vertical in
  the default view; "Look down the spindle" is gone.
- FEM: gravity points at the floor, and the MCP docstring's `[0,0,-9.81]`
  example is now true.
- A `render_still` with the ground plane on shows a floor under the part, not
  a wall through it; an exported `.glb` stands upright in an external viewer.
- No `kVersion` bump, and the golden corpus is untouched — except the two
  named stored-orientation cases (drawing view names, CAM safe heights),
  which are handled or explicitly accepted in Z6.
