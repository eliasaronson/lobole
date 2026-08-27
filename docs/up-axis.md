# Which axis is up — the Y-up decision, and undoing it

> Status: **PLANNED** (2026-08-27). Not started. Written after the CAM cutting
> simulation drew a solid spindle lying on its side
> ([cam/simulation.md](cam/simulation.md)) — the fourth subsystem to collide
> with the same choice.
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

- gravity defaults to `(0, −9.81, 0)` (`StudyPanel.cpp:579`)
- the **XZ** plane is named "Top Plane", the **XY** plane "Front Plane"
  (`SketchPlane.cpp:47-57`)
- the Top drawing view looks along `(0,−1,0)` (`DrawingDocument.cpp:154`)

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

There is no reason to expect the fifth subsystem to be different. Every
convention we have to interoperate with — G-code, DXF, drawing practice, other
CAD — is Z-up, and Y-up buys exactly one thing, which is matching a habit in
a layer that does not care.

## The insight that makes it affordable

**This is a convention change, not a data migration.** No stored coordinate
has to change. What changes is which axis the application *calls* up, and
every default derived from that.

Consequences worth stating plainly:

- **Document goldens survive.** Geometry is untouched, so `test_golden.cpp`
  and the serialized corpus do not move. No `kVersion` bump.
- **CAM's bug disappears with no CAM change.** `BodiesInSetupFrame` at
  `rotationA = 0` becomes correct again, because setup space (Z = spindle) and
  world space (Z = up) finally agree. Do NOT write the CAM-local frame
  workaround: it is exactly the code this deletes.
- **The drawings fix gets reverted.** `OrientationAxes` goes back to the Z-up
  table it originally was — which is itself the evidence that the CAD-habit
  code was right and we bent it to fit.
- **Existing projects come up lying down.** The scratch projects (the dock, the
  enclosures, the C-clamp, the screwdriver set) keep their geometry and change
  their apparent orientation. One-shot re-orientation, or accept it.

## The durable fix, which is not the flip

**There is no `kWorldUp` anywhere in this codebase.** I grepped: the axis is
implicit in **69** literal `vec3(0,1,0)`s and in every table that names a view,
a plane or a direction. *That* is why it has been got wrong four times — not
because Y was the wrong choice, but because the choice is nowhere stated.

So the first phase is not the flip. It is:

```cpp
// core/Conventions.h
inline constexpr glm::dvec3 kWorldUp{0, 0, 1};
inline constexpr glm::dvec3 kWorldForward{0, -1, 0};
```

plus a **conventions gate** (`tests/test_conventions.cpp` already exists and
already scans source for this class of rule) asserting that the named views,
the plane names, the default gravity and the CAM setup frame all agree with
`kWorldUp`. With that in place the flip is a one-line change plus fallout, and
the *fifth* module cannot drift.

## Phases

| | scope | size |
|---|---|---|
| **Z1** | `kWorldUp` + the conventions gate, asserted against the CURRENT (Y-up) value. Nothing changes behaviour; the gate goes green on today's code | ~½ session |
| **Z2** | Triage the 69 literals: which are world up, which are a local basis, a UV axis, a face normal. Route the world-up ones through `kWorldUp` | ~½ session |
| **Z3** | **The flip.** `kWorldUp` becomes +Z; fix everything the gate now fails on: standard views, view cube, `orient_to_face`, plane naming, gravity default, drawing orientation table (a revert) | ~½ session |
| **Z4** | The render layer, which genuinely bakes Y-up in and is NOT a revert: three light directions (`MeshRenderer.cpp:292-304`), the shadow light `normalize(1.0, 1.5, 1.0)` (`ShadowRenderer.cpp:128`), and the equirectangular environment map — `dirToUV` uses `d.y` as its polar axis in TWO places that must match (`mesh.frag:53` and `IBL.cpp::DirToEquirectUV`) | ~½ session |
| **Z5** | The screenshot pass: CAM, drawings, sketching, assemblies, FEM, exploded views, sections. This is the phase that actually finds the misses | ~½–1 session |
| **Z6** | Re-orient the scratch projects, or decide to leave them | small |

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
  Mitigation: Z2 is a read, not a replace-all.
- **A half-done flip is worse than either end.** If Z3 lands and Z4 does not,
  the model is Z-up and the lighting is sideways. Z3 and Z4 ship together or
  not at all.
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

- `kWorldUp` is the only place the up axis is stated, and `test_conventions.cpp`
  fails if a named view, a plane name, the gravity default or the CAM setup
  frame disagrees with it.
- The drawings case that started this: a plate 80 × 8 × 60 shows **80 × 8** in
  the front view and 80 × 60 in the top view.
- CAM: `cam_derive_op` on the visual top face of a beam derives a pocket
  instead of refusing it as a wall; the simulated spindle stands vertical in
  the default view.
- FEM: gravity points at the floor.
- No `kVersion` bump, and the golden corpus is untouched.
