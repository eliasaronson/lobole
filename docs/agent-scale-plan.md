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

### G1b the SDK (two days)

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

Gate: `sdk_smoke.py` builds a piston (crown, ring grooves, pin bore, skirt
relief) in ≤ 15 lines and checks volume/extents; `generators/gear.py` and
`gear_train.py` are ported to it and `generators_smoke.py` still passes.
**Measure:** `gripper.py` re-written on the SDK: lines and wall time against
652 / 86 calls. **No benchmark, no claim:** the MCP surface changes
(`execute_script` prelude, `help`), so a sweep runs before the claim.

## G2 — Motion study: a driver, samples, a scrub, an export (three days)

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

Gate: `generators/slider_crank.py` — crank, rod, piston in a bore, revolute
+ revolute + cylindrical, a closed loop (the A6 four-bar's cousin, and the
engine's atom); `motion_smoke.py` runs 48 samples, asserts the piston's
stroke equals twice the crank radius at t=0.5, `clean: true`, and the GLB
loads back with 48 keyframes. A GUI test scrubs the slider and asserts the
piston moved. **Measure:** 48 samples with the mesh check < 2 s on the
slider-crank; the number on the gripper's 8 occurrences.

## G3 — Mesh interference for sweeps (one day)

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

Gate: `test_measure.cpp` cases — overlapping, touching (must NOT clash at
deflection tolerance: inflate nothing, report `touching` only from exact),
clear, enclosed; agreement with exact on the gripper assembly.
**Measure:** the arm's 253 occurrences, all pairs, in under 1 s against
23 s / 123 s exact.

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
