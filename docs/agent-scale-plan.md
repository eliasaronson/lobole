# Agent scale — the W16 gap (plan, 2026-09-04)

> **Status: planned.** Seven workstreams, ordered below. Each is one or two
> commits with the suites green and a number measured before and after.
> Acceptance for the round is the single-cylinder engine at the bottom:
> the W16 is that sixteen times over, and sixteen times over is a `for`.

## Why

A text-to-cad user built a quad-turbo W16 from one prompt: hundreds of bodies,
a crank animation, an explode. Their stack is build123d code, STEP out, a
browser viewer, and a hand-written JavaScript function that sets every
occurrence's transform from a `crank` parameter. No solver, no mates, no
kernel-checked collision. Designer could build the geometry today but at an
order of magnitude more agent effort, and the deliverable — a scrubbable,
kernel-verified motion — has no home here at all. The comparison, item by
item, is in the session notes; the gaps it left are these seven.

What this plan is **not**: a code-CAD interpreter inside the app (settled,
[capability-review-2026-08-31.md](capability-review-2026-08-31.md),
"scripted features"), or a chase after that render. The bet stays the shared
document between engineer and agent. This plan makes the agent's half of it
fast enough to build an engine, and gives motion a place to live.

## The measurements that justify it

| symptom | measured | where |
|---|---|---|
| two parts + one assembly on the RPC surface | 652 lines, 86 calls, a day | `scratch/so101/gripper.py` |
| one feature = one regeneration; no batch | `Document::BulkEdit` exists (Document.h:301) and is unreachable from `src/remote` | RemoteModeling.cpp attaches `regen` per call |
| an assembly sweep, exact booleans | 23 s / 123 s for 253 occurrences; one boolean between two imports 100 s | derived-followup-plan.md P2 |
| sweep × 48 crank steps | ~18 min per kinematic gate | arithmetic |
| relations | Gear, Rack, Screw, Linear — no cam, no chain | Occurrence.h:188 |
| motion | none: `set_mate` re-solves one pose; `export_visual` writes one static GLB | RemoteAssembly.cpp:607, MeshExport.cpp |
| benchmark ceiling | 8 parts T0–T3, 234/234, "cannot detect a regression" | benchmarks/REPORT.md |

## Order and dependencies

```
G0 datum frames ─► G1a batch RPC ─┐
                   G3 mesh clash ─┼─► G2 motion study ─► G4 cam + chain ─► G6 benchmark T4/T5
                   G1b SDK ───────┘                                     └─► G5 fan-out
```

G0 first because it is the blocker the gripper actually hit: every offset
in it was a number the agent transcribed or a centroid that was not a datum
(the "why" under G0). G1a next because every later gate is a script and pays for regen per call.
G3 before G2 because a motion study without a collision table is a slider.
G1b lands after G2 so its examples (a piston, a crank throw, a bolt circle)
are the parts the acceptance engine needs. G4 and G2 share ONE format bump.

---

## G0 — Datum frames: positions by name, never by number — SHIPPED 2026-09-04

**Why.** The gripper script records where its offsets came from, in its own
comments: twelve positions transcribed from `list_elements` at two decimals
("the one place numbers enter by hand"); a face-connector at a centroid that
was not the horn centre ("face-to-face shifted the body 7 mm"; "the hub's
underside is one face with the arm's"); sketch UV taken for world (the flange
7.57 mm off its axis); and parts modelled in the ghost's world coordinates
instead of their own frame, which made the other three possible. It also has
**zero constraints and zero variables** — a rectangle at typed coordinates,
extruded — so it is not a parametric model at all, only a script that made
one. text-to-cad avoids the whole class structurally: a part is built at its
own origin (a designed datum: bolt-pattern centre, shaft axis), placement is
a joint between two NAMED frames computed from parameters, and validation
returns a delta that is fixed in source. Underneath that, `face.center()` is
a call there and a JSON field here. The measuring half exists (`measure`
returns a signed delta; `contact_report` lists near misses); what is missing
is upstream of measurement.

- **`Frame` feature** (part scope, v86 with G2/G4): a named origin + axes.
  Kinds: `explicit` (origin/axes as expressions), `from_element` (a circular
  edge → centre + axis; a planar face → centroid + normal; a bolt circle →
  the centre of N circular edges), `offset` (another frame + transform).
  It is a feature so it regenerates: move the boss and the frame follows.
  Drawn as a small triad; `defaults/*` gate since it is a creation tool.
- **Frame connectors**: `ConnectorAnchor` gains `frame` beside `origin`,
  `face`, `snapEdge` (Occurrence.h:81) — `{occurrence, frame: "horn_seat"}`.
  A mate between two named frames is the mate as designed, not the mate as
  the nearest face allowed. The connector panel lists a part's frames first.
- **`frame` query**: the world frame of an occurrence, an element, or a
  named frame, on either tab — the missing third of their
  `measure`/`align`/`frame` triad.
- **The SDK holds references, not floats** (G1b, but the rule belongs here):
  `Face`, `Edge`, `Frame` objects with `.center`, `.axis`, `.normal`,
  `.plane()`; a `Plane` built from a face carries its origin, and
  `plane.uv(world)` / `plane.world(u, v)` do the arithmetic the flange got
  wrong. `asm.check()` raises on any near miss under the touch tolerance.
- **Rules the SDK enforces**: a part is modelled at its own origin (a sketch
  on a ghost's face is placed by `face_ref`, never by a typed origin);
  a sketch entity whose position is another entity's is CONSTRAINED to it
  (`concentric`, `coincident`), and a dimension that the spec names is an
  expression over a variable, not a literal. The SDK's sketch builder does
  this by default: `sk.circle(concentric=ghost_edge, d="bore")`.

### As built

All four items shipped, v86 (golden `v86-frames`, history row). The `Frame`
feature (`explicit` / `from_element` / `offset`), `ConnectorAnchor::frameId`
with `add_mate {a: {occurrence, frame: <id|name>}}`, the `frame` query in
part and world space, and the toolbar/editor/triad-overlay surface. The SDK
half moves to G1b, where the objects live.

**One real bug found on the way, and it was the exact failure this
workstream exists to stop.** Baked geometry (docs/versioning/baked-geometry.md)
installs the saved bodies and *executes nothing* — but a datum's output is
not a body, so nothing carried it. A reopened project therefore left every
datum at its DEFAULT: a plane at the origin, a frame at the identity. A mate
anchored to that frame then solved **cleanly** and placed the part at the
world origin, on a document that had just been saved correct. No error
anywhere. `Feature::IsDatum()` now marks the four features whose output is
state rather than geometry — `Frame`, `DatumPlane`, `DatumAxis`, `Curve3D` —
and the baked branch executes them against the bodies it just installed.
Pinned by "Datums are resolved on a baked open" in `test_baked_geometry.cpp`.
The other three were latently wrong the same way (a datum plane drew at the
origin after every baked open); nothing had noticed because their consumers
are overlays, and geometry that referenced them was baked.

Gates: `tests/test_frame.cpp` (11 cases — the frame kinds, every refusal, the
round trip, and the mate that follows its boss when a variable moves it),
`tests/frame_smoke.py` (the same over the RPC, plus the `frame` query and the
save/reopen), the baked-datum case above, plus the standing gates
(`rpc_params_gate`, the golden corpus, the `defaults/*` GUI row, the
keymap one-to-one registry). Suites: 1061 headless, 278 GUI.

**Owed:** the MCP surface changed (the `Frame` dialect in `add_feature`'s
description), so CLAUDE.md's "no benchmark, no claim" applies before any
claim that this makes an agent faster. The typed-coordinate count in a
rewritten `gripper.py` — twelve today, with zero constraints and zero
variables — is the measurement, and it belongs to G1b, which is what the
script would be rewritten against.

## G1 — Scripting: one batch, one regen, and an SDK that speaks geometry

### G1a `batch` RPC — SHIPPED 2026-09-04

`batch {calls: [{method, params}], stop_on_error: true}`:

- executes under one `Document::BulkEdit` (part) or one deferred
  `SolveActiveAssembly` (assembly): **one regeneration or solve** at the end;
- **one undo step**: the snapshot is taken once before the first call, and a
  failed call (`stop_on_error`) rolls the WHOLE batch back and says which
  call failed and why, in the error style (error-style.md). Precedent:
  `RemoveFeaturesCommand` is already one step for N features;
- response: `results[]` (each call's response, minus its own `regen`) plus
  ONE `regen` summary at the end — the honesty rule holds
  (`regen_honesty_smoke.py`): a broken feature is reported in the batch's own
  response;
- the strict unknown-key check runs **before** a call applies, not after
  (today `RemoteServer.cpp:2172` checks post-execution, so a junk key applies
  then errors — harmless alone, wrong inside a transaction);
- refuses `batch` inside `batch`, `ui_action`, screenshots, and the git verbs
  (each is its own transaction already).

#### As built

All of the above, plus one thing the plan had missed and the measurement
found: **a batch could not use an id an earlier call returned**, which is the
API's commonest pair (a sketch, then the extrude that names it) and would
have limited the whole verb to calls that happen not to depend on each
other. So a param value `{"$ref": n}` anywhere is the n-th call's result —
its `id` by default, or `{"$ref": n, "key": "..."}` for another field.
Backward only, and a reference to a call that failed or to a field it did
not return is refused naming what it did return.

Two more decisions worth recording. The regeneration is deferred only past
calls whose ANSWER does not depend on rebuilt geometry (the feature and
variable verbs); anything else — a `measure`, a `mass_properties` — flushes
first, so a query inside a batch can never answer from the geometry as it
was before the batch. And the per-call `regen`/`digest` are stripped from
the results, because under a deferred regeneration they would describe a
document that has not been rebuilt yet; geometry is answered once, by the
batch's own `regen`.

**Measured**, headless, same machine:

| what | one call each | one batch |
|---|---|---|
| 40 sketches appended | 0.11 s, 40 regenerations | 0.00 s, 1 |
| 40 holes drilled (80 calls, sketch + cut) | 0.86 s | 0.49 s |
| 12 edits EARLY in a 49-feature part | 1.89 s | **0.20 s** |

The third row is where the verb earns its keep and the second says why the
first two are modest: incremental regeneration already makes an APPEND cheap
(only the new feature runs), so batching appends saves the per-call overhead
and little else. An edit early in the history re-runs everything after it,
so twelve of them cost twelve full rebuilds — and one batch costs one.

Gates: `tests/batch_smoke.py` (the regeneration count, one undo step for 40
features, rollback on a failed call and on a rejected key, `stop_on_error:
false`, a feature that fails to BUILD not counting as a failed call, `$ref`
including its three refusals, a mid-batch query seeing current geometry, and
an assembly batch as one snapshot), plus `rpc_params_gate`,
`regen_honesty_smoke` and `rpc_gate_smoke`. 1062 headless, 278 GUI.

### G1b the SDK — SHIPPED 2026-09-04

`tools/designer.py` (198 lines: `Sketch`, `extrude`, `box`) becomes the
package S3 promised, absorbing `generators/_lib.py` (planes that spell out
the V axis, `arc_through`, `check`, `connect`). Not a new abstraction: the
gripper's friction list, turned into methods, each issuing one `batch`:

| method | replaces | the bug it retires |
|---|---|---|
| `Plane.at(origin, normal, u)` + `plane.uv(world_point)` / `plane.world(u, v)` | hand-computed UV | the flange 7.57 mm off its axis (UV measured from the origin, world = origin + u·U + v·V) |
| `sketch.polyline(points, close=True)`, `sketch.slot`, `sketch.polygon(n, r)` | 4 `line` calls per rectangle | dropped/reversed edges |
| `doc.revolve_profile(points, axis, angle)` | sketch + revolve, two dialects | a piston in one call |
| `doc.bolt_circle(face, n, pcd, standard="ISO 4762 M6", depth=...)` | Hole + N `fit_fastener` | random bolt placement |
| `doc.fillet(body, r, edges="all" \| predicate)`, `doc.chamfer(...)` | `list_elements` + names by hand | the missing-fillets class |
| `doc.pattern(feature, circular=(axis, n) \| linear=(dir, n, pitch))` | the two pattern dialects | |
| `asm.place(occ, frame)`, `asm.revolute(a, b, axis=...)`, `asm.slider`, `asm.gear(...)` | connector JSON | the connector-name detour |
| `doc.check()` | regen polling | silent breakage: raises on any errored feature |

Rules for it, written into its docstring and enforced by `sdk_smoke.py`:
every method is a thin composition of documented RPCs (the MCP `describe`
table stays the source of truth; the SDK never knows a field the RPC does
not); every method returns the ids it made; nothing is a default with a
unit unless derived from what it acts on (CLAUDE.md). `execute_script`'s
prelude imports it so an MCP agent gets it for free.

#### As built — and the design changed on one question

Asked mid-build: *can this be generated so nothing is duplicated?* It can,
and it should be, so the module is two layers:

- **The RPC surface is GENERATED at connect time from `describe`.** No
  wrapper is written per method. `app.add_feature(...)`, `app.frame(...)`,
  `app.contact_report(...)` — all 222 of them — resolve through the
  description the app publishes, with keyword arguments checked against the
  documented parameter list BEFORE the round trip. The enums come the same
  way. A new RPC is callable the day it lands and nothing here can drift
  from the server, which is the thing a hand-written client always
  eventually does.
- **Hand-written: only what `describe` cannot express.** The reference
  objects (`Face`, `Edge`, `Frame`, `Plane`, `Body`), the plane arithmetic
  (`plane.uv(world)` / `plane.world(u, v)`), the compositions
  (`revolve_profile`, `bolt_circle`, `polyline`, `slot`, `polygon`,
  `project`), the assembly verbs that mate BY FRAME NAME, `check()`, and a
  `batch()` context manager whose handles are `$ref`s.

`tests/sdk_smoke.py` gates the split itself: every documented method is
reachable, nothing hand-written on `App` shadows one, the enums are read
rather than copied, and no constraint-kind string is hard-coded in the
module.

#### The bug this found: a "live" variable that was frozen

`generators/gear.py` advertises a bore that stays adjustable. It was not.
A circle's expression field is `r_expr` (v38); the helper sent
`radius_expr`, which is not a field of any entity, so the app **dropped it
silently** and the bore was the literal it happened to start at. Measured
before the fix: setting `gear_bore` from 6 to 12 changed the gear's volume
by zero. `EntityFromJson` now refuses an expression field the entity does
not have, naming the ones it takes; `_lib.circle` takes `r_expr`; the gear's
bore is live (3234 mm³ → 2555 mm³ on the same edit). The SDK uses the app's
own fields for the same reason — a coordinate or radius given as a string
rides on the entity and the DOCUMENT evaluates it, so there is no second
expression language in the client. Same shape as G0's baked-datum bug: a
plausible substitute that loaded fine, looked right and was wrong.

Gate: `tests/sdk_smoke.py` — the generated-surface rules above, the plane
arithmetic that put the gripper's flange 7.57 mm off its axis (round-tripped
both ways), a **piston in 20 lines** (revolved body, three revolved ring
grooves, wrist-pin bore, every dimension a variable) checked by volume and
extents, a parametric edit moving it, `bolt_circle` with the frame that
names it, a hole pattern taken from another part's ghost by PROJECTION with
no coordinate transcribed, a batch of sixteen features in one regeneration,
and an assembly where the check catches a floating part, a mate onto a
named frame follows the face it was built from, and an edit that destroys
that face makes the mate REFUSE rather than reuse its last good pose.
1063 headless, 278 GUI.

#### The gripper, rebuilt on the SDK — the measurement

`scratch/so101/gripper_sdk.py` builds the same two parts and the same
assembly from the same STEP files.

| | original | on the SDK |
|---|---|---|
| lines of code | 652 | **228** |
| RPC calls | 86 | **138** |
| transcribed coordinates | 12 | **0** |
| wall clock, end to end | (a day of iteration) | 24 s |

**The call count going UP is the result, not a regression.** The original
knew where things were because a human had read twelve numbers off
`list_elements` and typed them in. This asks the app instead — one query per
face, per edge set, per plane — and those queries are what make the script
survive the file changing. It does not survive otherwise: the original's
occurrence ids (255/256/257/254) no longer resolve at all, because twelve of
the arm's parts fail to import and every id after them shifts. The rewrite
finds the wrist servo by name.

Every derived value matches what the original had transcribed: the servo end
face at x=22.56 (`END_X = 22.56`), the covers' outer faces at y=-18.78 and
16.92 (`-18.8` / `17.1`), the wrist-roll horn's face at (7.573, -0.208,
-23.4) (`HORN_C = (7.57, -0.21)`, `HORN_Z = -23.4`).

**Four SDK defects the rewrite found**, none of which the piston gate could
have:

1. `Part.bodies()` described every face of every body in one call, which a
   part carrying a derived 197-body camera answers with a refusal (17,842
   faces). Bodies are now listed from `get_state` and each body's geometry
   is read only when asked for, per body.
2. `edges(kind="circle")` silently misses a hole whose rim the kernel split
   into arcs — the camera's four mounting holes are 24 arcs, and the filter
   found none of them and drilled nothing. `circular=True` covers both.
3. The radius filter's default tolerance was a float epsilon (1e-4), not a
   manufacturing one. Now 0.05 mm.
4. No way to pass a timeout, so a 20 MB STEP import could not be made at
   all. `_timeout=` on any generated call.

**And one shipped bug, which is the important one.** The "this hole drills
empty space" warning added earlier this week fired on four holes that had
visibly drilled four standoffs. It reused `fit_fastener`'s ray bundle, which
documents its own assumption — that a ray starts OUTSIDE the material — and
a hole drilled into a boss breaks it: every offset ray begins inside the
boss, all four are discarded as "started inside", and the hole is reported
as meeting nothing. A false warning is worse than no warning, so the test is
now a direct classification of points down the bore
(`CADShape::ContainsPoint`), which depends on no ray origin at all.

**Not done, deliberately:** `generators/gear_train.py` was left alone — it
has uncommitted local changes, and rewriting a file someone is working in is
not the port's job.

## G2 — Motion study — SHIPPED 2026-09-04

The assembly grows a `MotionStudy`:

```
{ id, name,
  driver: { mate: <id>, dof: rotation|offset, from, to },   // 0..720°
  samples: 48,
  checks: { clash: true, method: "mesh", deadline_s: 20 } }
```

- **Sample**: for each t, `SetMateDof(driver)` → `PropagateRelations(driver)`
  → `Solve` → the `FlatInstance` worlds. A sample whose loops do not close
  reports its residual and is marked; the study never draws the last good
  pose in its place (refuse, don't substitute).
- **Check**: per sample, the G3 mesh sweep over all occurrence pairs; the
  result is the collision table: `[{t, dof, residual_mm, clashes:[{a, b}]}]`
  plus `worst` and `clean: bool`. Runs as an A3 background job (it is 48
  sweeps), so `ping` answers during it.
- **Scrub**: `set_motion_pose {study, t}` sets the driver and re-solves, the
  way `set_explode` drives the viewport (`state.syncedDocumentVersion =
  UINT64_MAX`, RemoteAssembly.cpp:1039). GUI: a slider beside the explode
  slider in the assembly panel, with play/loop; a `defaults/*` gate since it
  is a creation tool.
- **Export**: `export_motion {study, path}` writes a GLB with a glTF
  animation — one TRS channel per moving node, sampled at the study's
  samples, `LINEAR` interpolation, exactly periodic when `from`/`to` differ
  by a full turn. **The one unknown in this plan:** `RWGltf_CafWriter` writes
  no animations. Either the writer emits the static scene and we append
  `animations`/`accessors` to its JSON chunk (a GLB is JSON + one BIN
  chunk; appending accessors means rewriting the BIN), or we write the GLB
  ourselves from the tessellation cache we already own. Decide by trying
  the append first; it is a day either way.
- **Persisted**: the study definition, not the samples — format bump
  (v86, shared with G4), golden `assembly/v86-motion-study`, history row.
- RPCs: `add_motion_study`, `set_motion_study`, `remove_motion_study`,
  `list_motion_studies`, `run_motion_study {async}`, `set_motion_pose`,
  `export_motion`; MethodDoc rows; MCP gets `motion_study` as one tool.

### As built

A `MotionStudy` on the assembly (v87, golden `assembly/v87-motion-study`,
history row): driver mate + DOF, a range in the DOF's own unit, a sample
count, and the clash check's mode. Running it solves at every sample and
sweeps every occurrence pair; the result is transient, like a FEM result.
The surface is `add_motion_study` / `set_motion_study` /
`remove_motion_study` / `list_motion_studies` / `run_motion_study` (sync or
`async` on the A3 registry) / `motion_status` / `set_motion_pose` /
`export_motion`, plus a panel section with a scrub slider and Run.

Decisions worth recording:

- **The RPC speaks degrees, the document stores radians.** `set_mate` takes
  radians, so the study stores what it will write; the conversion happens
  once, at the boundary, and every response says which unit it is in.
- **A scrub is a look, not an edit.** `set_motion_pose` pushes no undo step,
  the explode slider's rule — 48 snapshots per drag would bury the history
  that matters.
- **The worker shares nothing.** An async run copies the assembly AND every
  document under it, and pre-builds the collision meshes on the main thread,
  because tessellating is the one step that mutates a shape. A synchronous
  run skips the copies (it is blocking the caller anyway) — copying a
  Document regenerates it, and an Import re-reads its STEP.
- **A sample that does not solve is reported unsolved with its residual**,
  and `export_motion` REFUSES rather than writing an animation that holds a
  frame there. The GLB is the run's poses, not a guess at them.

The animated GLB is a writer of our own (`core/io/GlbAnimation`), because
OpenCASCADE's glTF writer emits a static scene and nothing else. It is small:
glTF 2.0 is a JSON index over one binary buffer, and a rigid mechanism needs
three accessors per node (times, translations, rotations) with LINEAR
interpolation. The scene root carries the one Z-up-to-Y-up rotation.

### The bug the acceptance rig found, and it was in G3

`generators/slider_crank.py` is the plan's atom: block, crank, rod, piston,
a closed loop the A6 solver closes numerically. Built on the SDK, every mate
between named frames. Its first run reported a clash at every angle — and
the study was right twice and wrong once:

- the rod's tip really did hit the piston body ahead of its slot (an exact
  360 mm³, which the boolean confirmed), and the rod's swing really did
  leave the bore where the barrel started. Both were the rig's geometry, and
  fixing them is what a kinematic gate is for.
- but `crank x rod` was reported clashing where the exact boolean said
  0.5 mm clear, at a 0.17 mm deflection. **G3's triangle test was wrong for
  COPLANAR triangles.** The usual statement of triangle-triangle SAT gives
  eleven axes — two face normals and nine edge-edge crosses — and those are
  enough only when the triangles are not coplanar. When they are, every one
  of the eleven is parallel to the shared plane normal, nothing can separate,
  and any two coplanar triangles read as intersecting however far apart they
  lie. A pin whose end cap is flush with the face it passes through is the
  ordinary way parts are drawn, so this was not a corner case. Six more axes
  — each triangle's in-plane edge normals — decide it. Pinned by "A pin in a
  clearance hole is clear" in `test_meshcollide.cpp`, which fails on the old
  code.

That is the second time this round that a new gate found a defect in the
thing built just before it, and it is the argument for building the gate.

**Measured** on the slider-crank, 49 samples of a four-body loop, 196 pair
checks:

| | exact | mesh |
|---|---|---|
| a full turn | 0.35 s | **0.08 s** |

Both report the same verdict (clean), and the piston's stroke is 39.991 mm
against a crank radius of 20 — the loop solver's own tolerance.

Gate: `tests/test_motion.cpp` (the sampling, the clash the table must find
and where, exact and mesh agreeing, the refusals, the round trip, the v87
golden, and the GLB container parsed back), `tests/motion_smoke.py` (the
whole RPC surface, sync and async, the scrub, the export, the reopen),
`a_motion_study_scrubs_and_runs` in the GUI suite, and `slider_crank.py` as
the standing acceptance rig. 1075 headless, 279 GUI.

## G3 — Mesh interference for sweeps — SHIPPED 2026-09-04

`src/core/MeshCollide.{h,cpp}`: an AABB tree per body over
`CADShape::GenerateMesh(deflection)`, deflection **derived** from the body's
bbox (diagonal / 2000, clamped), cached by `GeometryId()` in core (the UI's
`BodyTessellation` cache is not reachable from `src/core`, and this one is
small: triangles, no normals). Pair test under two transforms:
tree-vs-tree descent to triangle-triangle intersection, plus a containment
test (a ray parity test on one vertex) so a bolt entirely inside a block
counts as a clash with no crossing triangles.

- Result: `{intersects, method: "mesh", deflection}` — never a distance
  (that is what exact is for) and never called "clear" without saying which
  method cleared it.
- `interference {method: "mesh"}` opts in; `contact_report` stays exact; the
  motion study defaults to mesh and the final gate re-runs the worst sample
  exact.

### As built

`src/core/MeshCollide.{h,cpp}`: an AABB tree over each shape's triangles,
descended pair-wise with the separating-axis test on the leaves. Deflection
is the renderer's own (0.2% of the body's bounding diagonal), so a 2 mm screw
and a 2 m dock are meshed to the same RELATIVE fidelity and neither carries a
millimetre constant that is wrong for the other.

Three decisions worth recording:

- **It never reports a distance or a volume.** A volume measured off a
  tessellation is the tessellation's volume. `interference {method: "mesh"}`
  returns the pair and `deflection_mm`, and `exact` stays the default and the
  only thing that reports a number.
- **A body wholly inside another is a clash**, which no surface-intersection
  test finds on its own — a bolt buried in a block crosses nothing. The
  containment check is gated on the boxes NESTING, because it is a ray parity
  count over every triangle and running one on each side of every *clear*
  pair would cost more than the tree just saved.
- **The cache keys on PLACEMENT, not geometry.** A rigid `Transformed()`
  shares the underlying shape by design, so two occurrences of one part have
  the same `GeometryId` and sit in different places; keying on geometry would
  hand the second occurrence the first one's mesh and test every later pair
  against a body somewhere else. `CADShape::PlacementId()` is new for this.

The RPC parameter is `mode`, not `method`: the python client takes the RPC
name as its own `method` argument, so a parameter of that name can only be
reached through `request()`. `help` and `describe` are already stuck there
and it is not a club to grow — the smoke test found this by failing.

**Measured**, a bored plate swept past a pin over 64 poses, every pose
agreeing with the exact boolean:

| | exact | mesh |
|---|---|---|
| 64 pair tests | 689 ms | **68 ms** |

10x, not the 100x this plan guessed before measuring. That is one sample of
a 48-step motion study in miniature, and it is the difference between a
kinematic gate that runs and one nobody waits for.

Gate: `tests/test_meshcollide.cpp` (agreement with the exact boolean on
overlapping / clear / wholly-enclosed pairs, the deflection scaling with the
body, the placement-keyed cache, and the speed ratio) plus the `mode: mesh`
case in `contact_smoke.py`. 1067 headless, 278 GUI.

## G4 — Cam and chain relations (one day)

- `RelationKind::Chain`: gear with the sign kept (a chain does not reverse):
  `ratio = +teeth_a / teeth_b`. One row in `add_relation`; the chain itself
  is geometry, `generators/chain.py` (frozen roller chain around N sprockets).
- `RelationKind::Cam`: `dofB = lift(dofA)` with a periodic table
  (angle → mm) interpolated by cubic spline so the loop solver's
  finite-difference Jacobian sees a smooth function. Propagation
  (AssemblySolve.cpp:581) evaluates the table forward; the reverse branch is
  refused (a cam is not invertible). Inside loops it is one residual row
  like the others (AssemblyLoopSolve.h:71). `add_relation {type: "cam",
  a: <camshaft revolute>, b: <valve slider>, lift: [[deg, mm], ...]}` or
  `{base_radius, lift, duration_deg}` for a cosine lobe built for you.
  Deriving the table from a lobe FACE is deferred — named so it is not
  rediscovered.
- Serialization: `RelationToJson` gains `table` (SerializeAssembly.cpp:291);
  the same v86 bump as G2.

Gate: relation unit cases (a full turn returns the valve to seat; the table
is periodic or refused); `slider_crank.py` gains a camshaft on a 2:1 gear
relation and two valves; `motion_smoke.py` checks lift at the lobe's peak.

## G5 — Fan-out: N builders on N parts (half a day)

Nothing to build, one thing to prove and one to fix:

- `docs/scripting/fanout.md`: the recipe — N headless instances on scratch
  ports (`tools/scratch_port.py`), one worktree each (F9), one part each,
  the assembly referencing parts by stable id, integration serial through
  `branch_merge` / `conflict_list` / `merge_finish`. The MCP server pins an
  instance with `DESIGNER_PORT`, so a subagent per instance is one env var.
- `tests/fanout_smoke.py`: three instances build three parts in three
  worktrees; merge; the assembly opens with all three placed. Also the
  refusal: two instances on ONE project directory, the second save is
  refused by the F8 stamp, not corrupted.
- Fix the client's comment that the server "handles one client at a time":
  it accepts up to 8 and executes serially (`RemoteServer.cpp:277`).

## G6 — Benchmark above T3 (one day + a sweep)

The report says the harness cannot detect a regression; the fix is tasks
that can fail.

- **T4 `t4-slider-crank`** (assembly): three described parts, three mates,
  the crank at a stated angle. Oracle: solids = 3, the assembly bbox, and a
  new check kind `solid_com` (centre of mass of the i-th solid by volume
  rank), which pins the piston's position without asking the app.
- **T5 `t5-crank-motion`**: T4 plus "run the crank through a full turn and
  report whether anything collides". Scored: `export_step` at t = 0 and
  t = 0.5 → two oracle reads → the piston's `solid_com` moved by the stroke.
  Labelled, not scored: the app's `clean: true` (`"source": "app"`).
- `reference.py` builds both by hand (39 → 47 checks); a sweep before G1b
  and after; results committed, never overwritten.

## Acceptance: the single-cylinder engine

`generators/single_cylinder.py`, on the SDK: a block with one bore, a
piston with rings and pin, a rod with bolts, a crank throw with
counterweights, a head with two valves, springs (helix sweep), a camshaft on
a 2:1 chain relation, a bolt circle from ISO fasteners. A motion study over
720° at 15°, mesh-checked, exact on the worst sample, exported as an
animated GLB, scrubbed in the GUI, and drawn on a sheet. Findings go in
`docs/modeling/single-cylinder-dogfood.md` like the gripper's. The W16 is
sixteen of these on one crank with the throws phased — a loop, not a plan.

## Not in this plan

Roughly eleven days of sessions for G0–G6 plus one paid benchmark sweep.

- The parametric quality of what an agent builds is G0's rule set, not
  a checker: a lint that counts literals where an expression could be is a
  later, cheaper idea than a rule the SDK follows.

- An interpreter in the regeneration loop (settled).
- Deriving a cam table from a lobe face; belt/chain path geometry that
  follows sprocket moves (the chain is frozen geometry).
- Persisting tessellations beside baked B-Reps (derived-followup-plan.md).
- Inspection sweeps as A3 jobs for `interference`/`contact_report` beyond the
  motion study (same file).
- glTF skinning, cameras, or lights in the export — TRS channels only.
