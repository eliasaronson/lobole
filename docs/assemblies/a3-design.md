# Assemblies A3 — mates — design

> Status: **signed off 2026-07-13** (all six decisions below, as recommended).
> A3 is where assemblies stop being "eyeball
> it": occurrences become *constrained* to each other through mates, so a part
> edit or a re-solve keeps the assembly consistent. The model is **Onshape's
> mate-connector approach, not SolidWorks' constraint soup** (the plan's call):
> one mate = two coordinate frames + one mate type, solved by **direct frame
> alignment** — no 3D numeric solver. This doc leads with the mate model and
> the solve, then the save format (v24), picking, UX, and commands.

## What A3 is

A **mate** connects two **connector frames** — coordinate frames derived from
part geometry (a planar face, a cylindrical face, the part origin), each
carried by an occurrence — and constrains one occurrence's placement relative
to the other according to a **mate type**:

| Type | Constrains | Free (DOF) |
|---|---|---|
| **fastened** | everything | nothing |
| **revolute** | origins + Z axes coincident | rotation about Z |
| **slider** | Z axes coincident + twist | translation along Z |

v1 ships these three (the plan's recommended order); cylindrical, planar, and
ball follow in "the rest of A3" — the schema below is designed so they are
additive.

The payoff loop: mate a bolt into a hole once, then edit the base part — the
hole moves, the connector frame moves with it (toponaming), the placement pass
re-solves, the bolt follows. That is the whole reason mates reference *faces*,
not coordinates.

## The mate model

### Connector frames

A connector is **inline in the mate** (each mate carries two), not a
first-class named feature — Onshape's explicit "mate connector" features are a
later convenience, not a v1 need. Each connector is:

```
Connector {
  occurrence_id     // which occurrence of this assembly
  anchor            // where on the part the frame comes from:
                    //   origin                — the part's coordinate origin
                    //   face ElementRef       — {feature_id, element} into the part
}
```

The **local frame** (origin + Z + X, in part coordinates) is *derived* from the
anchor at solve time, never stored:

- **Planar face** → origin = face centroid, Z = face normal, X = the
  deterministic in-plane u-axis. This is exactly `CADShape::FacePlane`
  (`src/CADKernel.cpp:229-263`), already built in T3 and consumed by
  sketch-on-face via `ResolveFacePlane` (`src/core/SketchFeature.cpp:616-632`).
- **Cylindrical face** → Z = the cylinder axis direction, origin = the face
  centroid projected onto the axis, X = a deterministic perpendicular. **This
  query does not exist yet** — a new `CADShape::FaceAxis` sibling of
  `FacePlane` (`BRepAdaptor_Surface` → `GeomAbs_Cylinder` →
  `surf.Cylinder().Axis()`); the explorer-walk plumbing is shared.
- **Part origin** → identity frame. Free, and the natural anchor for a base
  part.

The **world frame** of a connector is `OccurrenceMatrix(occ) * localFrame`.

Resolution reuses the toponaming machinery verbatim: the anchor's `ElementRef
{featureId, name}` (`src/core/ElementMap.h:13-17`) is looked up in the
referenced part's regenerated bodies (`Body::createdBy` → `Body::elementMap`),
same as fillet/sketch-on-face do — except the *part* is chosen by the
occurrence's `part_id` instead of being the current document. A ref that no
longer resolves (the face's generating entity was deleted) puts the **mate**
in an error state; it never crashes and never guesses (toponaming's rule).

**Datum-plane/axis anchors are deferred**: the resolution side is trivial
(feature-id → `OutputPlane()` / `Origin()+Direction()`), but datums are not
*rendered* in an assembly viewport, so there is nothing to pick. They come
with in-context/visibility work, not with v1 mates.

### Mate schema (in-memory and on disk)

```
Mate {
  id            // stable uint64, same never-reused discipline as occurrences
  name          // cosmetic label ("bolt-in-hole")
  type          // "fastened" | "revolute" | "slider"
  a, b          // the two connectors; b's occurrence is the one the solve places
  flipped       // reverse b's Z against a's Z (default true for planar-planar —
                //   two touching faces have opposing normals)
  rotation      // twist about Z, radians
  offset        // translation along Z
}
```

`rotation` and `offset` do double duty, and this is deliberate:

- **fastened**: both are *fixed adjustment parameters* (rotate the part 90°,
  hold it 2mm off the face).
- **revolute**: `offset` is a fixed parameter; **`rotation` is the DOF's
  current value**.
- **slider**: `rotation` is fixed; **`offset` is the DOF's current value**.

So a mate always **stores its DOF pose**, and the solve is a pure function of
the file: load → re-solve → identical placements. This is the A2 principle
("an occurrence always stores an explicit transform — its last solved
placement") extended one level up: the *mate* also stores its last state, and
A2.5's drag-within-mate simply writes `rotation`/`offset` live. Later types
extend additively: cylindrical uses both as DOF; planar adds `dx`/`dy`; ball
stores a quaternion — new fields with defaults, no format break.

### The alignment math

One mate constrains occurrence *b* relative to occurrence *a*. With `Wa` = a's
connector world frame, `Fb` = b's connector local frame, and `Adjust` =
`Flip? * Rz(rotation) * Tz(offset)`:

```
T_b = Wa * Adjust * Fb⁻¹
```

— a closed-form rigid transform, decomposed back into the occurrence's
position + quaternion. No solver, no iteration, no residuals; a revolute
"solve" is the same formula with `rotation` free to be updated by UI/RPC.

### Posing notes (caster-round findings #6)

`Adjust` composes `T(dx,dy,offset)` on the **left** (`MateAdjust`,
`src/core/AssemblySolve.cpp`), so the translation always lands in **a's
connector frame**: `T_b`'s origin = `Wa.origin + dx·Wa.X + dy·Wa.Y +
offset·Wa.Z`. Two consequences worth stating because a dogfooding round tripped
over both:

- **Offset sign** is *not* inverted: a **positive `offset` moves b along +Z of
  a's connector frame** — a's *outward* face normal. The "−16 → +16 world"
  observation was the convention working against a reference face whose normal
  pointed into the assembly (so +Z of a's frame was −Z world). No solve or
  display bug; the sign is asserted by `test_assembly_solve.cpp`
  ("A positive offset moves b along +Z of a's connector frame").
- **Revolute takes an axial `offset`** just like slider/cylindrical (it shares
  `MateAdjust` — `offset` was always a fixed parameter for revolute, see the
  table above). Use it to seat a part up/down the shaft instead of collapsing
  onto the connector origin. This was a *discoverability* gap — the MCP schema
  read "offset = slider/cylindrical" — not a missing feature; the RPC and solve
  already applied it.
- **Lateral centering** (finding #6b) is *not* a separate mechanism, and none
  was added — it falls out of the connector choice. A **cylindrical**-face
  connector anchors its origin **on the axis** (`FaceAxis`), so two cylindrical
  faces mate **coaxially** (centered) for free; a **hole-center snap-edge**
  (A2.5 phase 4) does the same for a bolt into a bore. A **planar**-face
  connector anchors at the face **centroid**, so two planar faces coincide
  *centroids only* — which is why a wheel mated by planar faces landed off a
  fork leg. The fix is to mate on a cylindrical face / hole center (coaxial by
  construction) plus an axial `offset`, not to bolt a centering option onto
  centroid mates.

## The placement pass (the "solve")

Direct frame alignment over the mate graph — sequential, deterministic, and
loop-*detecting* rather than loop-*solving*:

1. **Anchors**: every `fixed` occurrence is placed at its stored transform.
   A connected component with no fixed occurrence anchors its **lowest-id
   occurrence** at its stored transform (deterministic; the component floats
   as a rigid group exactly where the user left it). Unmated occurrences keep
   their explicit transforms untouched — A2 behavior is the base case.
2. **Propagate**: repeatedly scan mates in list order; any mate with exactly
   one side placed computes the other side's transform by the alignment
   formula and marks it placed. Repeat to fixpoint (each pass places ≥1
   occurrence or stops).
3. **Loops / conflicts**: a mate whose *both* sides are already placed is
   checked, not solved — if the placed frames satisfy it within tolerance it
   is redundant (fine); otherwise the mate gets an **error state**
   ("over-constrained") and placements stand. Same story for a second mate
   reaching an already-placed occurrence. v1 reports; it does not iterate.
   (This is the plan's bet: most assemblies are mate *trees*; genuine loops
   are rare and can be an explicit later milestone.)
4. Solved transforms are written back to the occurrences (`position` +
   `orientation`) and `BumpVersion()` triggers the existing version-gated
   renderer resync (`SyncDocumentRenderers`, `src/ui/AppActions.cpp:958-985`).
   The renderer knows nothing about mates.

**Where it lives**: `src/core/AssemblySolve.{h,cpp}` — pure core, no UI
dependency. It needs the referenced parts' bodies, so it takes a resolver:

```cpp
namespace assembly {
  // partById: resolves an occurrence's part_id to its regenerated Document.
  // Returns per-mate status (ok / dangling ref / over-constrained / ...).
  SolveReport Solve(AssemblyDocument& asmDoc,
                    const std::function<const Document*(uint64_t)>& partById);
}
```

The UI passes `PartDocById` (`AppActions.cpp:794-799`); headless tests pass a
lambda over locally-built documents — mutation tests (edit the base part,
re-solve, assert the mated part followed) run pure-core like the toponaming
suite.

**Re-solve triggers**: any mate add/edit/delete; load of the assembly; a
referenced part's document changing (its regen moves connector frames); and an
occurrence `fixed`/transform edit (moving a *mated, non-fixed* occurrence is
overwritten by the next solve — the panel greys its transform fields and says
why). All triggers funnel through one `ui::SolveActiveAssembly(state)`.

### Failure semantics (mirror the feature error state)

A mate can fail two ways: **dangling ref** (a connector's face no longer
resolves after a part edit) or **over-constrained** (loop conflict). Either
way: the mate is marked errored with a message (transient, like
`Feature::m_error`), shown red in the mate list with a tooltip and — for
dangling refs — a *re-pick connector* repair action (fillet's re-pick
pattern); occurrences keep their last solved transforms. A file whose mates
are all broken still opens exactly like an A2 file. Errors are never
persisted.

## The save format (v24)

Mates are the second id-keyed list of `AssemblyDocument`, serialized in the
same file-per-item pattern as occurrences — the template is
`SaveAssemblyDir`/`LoadAssemblyDir` (`src/core/Serialization.cpp:1142-1234`):

```
assemblies/robot/
  index.json                 # {format, version:24, kind:"assembly",
                             #  occurrences:[ids], mates:[ids]}
  occurrences/
    1-base.json
    2-arm.json
  mates/
    3-hinge.json             # <mate-id>-<label-slug>.json
```

```json
{
  "type": "Mate",
  "id": 3,
  "name": "hinge",
  "mate_type": "revolute",
  "a": { "occurrence": 1, "anchor": { "face": { "feature_id": 4, "element": "4/g7" } } },
  "b": { "occurrence": 2, "anchor": { "face": { "feature_id": 2, "element": "2/cap1" } } },
  "flipped": true,
  "rotation": 0.7853981,
  "offset": 0.0
}
```

- **Occurrence and mate ids share one counter** (`m_nextId`) — ids are unique
  across the whole assembly, so an RPC/file/error message naming "id 7" is
  unambiguous, and the filename-prefix loader logic is reused as-is.
- The **origin anchor** serializes as `{"origin": true}`; the face anchor is
  the existing inline `ElementRef` shape (`{"feature_id", "element"}`) used by
  sketch `face_ref`/fillet edges — same keys, same reader.
- `kVersion` 23 → **24**, purely additive (a v23 assembly has no `mates` key ⇒
  defaulted empty; a v24 part file is unchanged). One new frozen golden at
  `tests/golden/assembly/` per the golden-per-bump policy; the v23 golden
  stays.
- File-per-mate keeps the F3 property: editing mate A and mate B touch
  different files; disjoint edits merge conflict-free.
- **A2's loadability rule is preserved**: occurrences still store their last
  solved transform, so any reader (or a mates-deleted file) opens at sane
  positions without running the solve.

## Picking: assembly clicks gain a face

Today the assembly branch of `ViewportInput` (`src/ui/ViewportInput.cpp:190-217`)
casts the ray, gets a `RayHit` — **and discards `faceIndex`**, keeping only
meshId → occurrence. The per-instance face data is already uploaded
(`SyncAssemblyRenderers` feeds `FaceSelector` per occurrence with transform,
`AppActions.cpp:943-945`), and `DocumentRenderMesh.bodyIndex` was retained in
A2 explicitly for this. So face-level assembly picking is a join, not new
infrastructure:

```
RayHit{meshId, faceIndex}
  → DocumentRenderMesh{occurrenceId, bodyIndex}
  → part = PartDocById(occ.part_id); body = part bodies[bodyIndex]
  → body.elementMap name of faceIndex  ⇒  ElementRef  (persistent!)
```

An unnamed face (patterns don't propagate names yet — toponaming's known gap)
is rejected at pick time with a status message, exactly like "Sketch on
Selected Face" falls back today. Normal click behavior (select occurrence) is
unchanged; face-picking activates inside the mate tool.

## UX

**Mate creation** follows the toolbar preview→accept pattern:

1. Toolbar: Fasten / Revolute / Slider (assembly tab only).
2. Pick a face on occurrence A → its connector frame is derived and drawn as a
   small axis triad (the existing `LineVertex3D` overlay path).
3. Pick a face on occurrence B → the solve **previews** live: B's occurrence
   jumps to the mated pose (flip defaulted so planar faces touch).
4. Accept ⇒ one snapshot + `AddMate` + solve; Escape/cancel restores.

**Mate list** joins the occurrence panel in the Model dock
(`src/ui/OccurrencePanel.cpp`): each mate shows name, type, the two occurrence
names, an error badge when red; selected-mate editor exposes name, `flipped`,
`rotation`/`offset` (DragFloat — for revolute/slider this *is* moving the
mechanism numerically, the pre-gizmo stand-in for drag-within-mate), re-pick
connector, delete. Edits are one undo step (snapshot on activate, the A2
panel's discipline). Undo/redo ride the existing `AssemblyHistory` full-copy
snapshots unchanged — mates live inside `AssemblyDocument`, so they are
snapshotted for free.

**DOF display + drag-within-mate** (spin the revolute by dragging) is
**A2.5**, per the roadmap — it shares the interactive-drag layer with the free
gizmo. Numeric DOF editing above is the v1 stand-in.

## Commands / RPC (scripting parity)

Follow the `set_occurrence_transform` template exactly (guard assembly →
`FindById` → `SnapshotAssembly` → mutate → solve → `BumpVersion` → echo JSON;
`src/remote/RemoteServer.cpp:901-928`):

- `add_mate {type, a:{occurrence, face|origin}, b:{...}, flipped?, rotation?, offset?}`
  → solves, returns the mate JSON (with assigned id).
- `set_mate {id, rotation?, offset?, flipped?, name?}` — numeric mechanism
  motion for the LLM ("rotate the hinge to 45°" = `set_mate {id, rotation}`).
- `remove_mate {id}`, `list_mates {}` (includes per-mate solve status).
- `solve_assembly {}` — explicit re-solve, returns the `SolveReport`
  (LLM-inspectable: which mates errored and why).

Plus MCP tools mirroring each (`tools/designer_mcp.py`), and an
`assembly_mates_smoke.py` headless test: build two parts, assemble, mate,
*edit the base part*, re-solve, assert the mated occurrence moved — the payoff
loop as a regression test.

### Why connector frames and not SolidWorks-style constraint pairs

Both models represent the same assemblies: the DOF states two rigid bodies can
be left in are the six classical lower kinematic pairs + fully-fixed, and a
SolidWorks constraint stack converges to one of them — the mate type is the
*normal form* of a constraint stack (a hinge is one revolute mate instead of
concentric+coincident whose "hinge-ness" is implicit). What the constraint
model buys — deliberately partial relations like parallel-only or tangent —
Onshape covers with extra mate types, addable to this schema additively.

The honest cost (raised at sign-off): Onshape's model is pleasant *because of
connector inference* — hover snap points (circle centers, edge midpoints,
corners) that place the frame origin well in one click. v1's coarse anchors
(centroid / axis) are sufficient for fastened/revolute/slider — the axis is
exact, and centroid misplacement is a single `offset`/`rotation` adjustment —
but **connector inference is a named "rest of A3" milestone, not a maybe**
(see below). Inference changes only the frame-origin derivation rule: same
solver, same mate types, same anchor `ElementRef` + an added qualifier —
an additive schema extension built on existing picking (`PickEdge`, B-Rep
vertices/circle centers). v1's creation flow can still take the cheap
SW-flavored sweetener: infer the *mate type* from the picked pair (two
cylinders → suggest revolute, two planes → fastened).

## Explicitly out of v1 (so their absence is a decision, not a gap)

- **Cylindrical / planar / ball mates** — rest of A3; schema extends
  additively (planar needs `dx`/`dy`, ball a quaternion).
- **Connector inference snap points** (circle centers, edge midpoints,
  corners, face center picker) — **the priority UX item of "rest of A3"**,
  likely alongside A2.5 (hover markers + drag handles share the interactive
  layer). Additive: anchor gains a qualifier next to its `ElementRef`.
- **Loop solving** — v1 detects + reports over-constrained; an iterative
  solve over genuine mate loops is a later milestone if real models demand it.
- **Expression-driven `rotation`/`offset`** — assemblies have no variable
  table yet; when one exists, the `_expr` twin pattern applies unchanged.
- **Explicit named mate connectors** (as reusable features) and **datum
  anchors** — need datum rendering in assemblies.
- **Mate limits** (min/max angle), **suppression**, and cross-*project* part
  references (F5 pins) — A4/cross-cutting.

## Decisions — SIGNED OFF (2026-07-13)

1. **Mate = two inline connector frames + one type** (Onshape model). No
   first-class connector features in v1; no SolidWorks-style pairwise
   constraint accumulation.
2. **v1 anchor sources: planar face, cylindrical face, part origin.**
   Faces via the existing `ElementRef` toponaming refs (cross-document:
   occurrence id + element ref); datum anchors deferred (not renderable in
   assemblies yet). New kernel query: cylindrical face → axis.
3. **v1 mate types: fastened, revolute, slider**, with `flipped` +
   `rotation` + `offset` on every mate; **the DOF's current value is stored
   in the mate** (revolute→rotation, slider→offset), so a file re-solves to
   the same pose — the A2 explicit-transform principle extended to mates.
4. **Solve = sequential direct frame alignment** from `fixed` anchors
   (lowest-id fallback per floating component), closed-form per mate, in
   `src/core` behind a part-resolver callback. **Loops are detected and
   reported (per-mate error), not solved.** No numeric 3D solver in v1.
5. **Format v24, additive**: `mates/` file-per-mate beside `occurrences/`,
   ids from the same never-reused counter, golden per bump, v23 files load
   with zero mates. Occurrences keep storing last-solved transforms.
6. **Failure semantics mirror features**: dangling connector refs and
   over-constrained mates put the *mate* (not the file) in a transient error
   state with a re-pick repair action; placements always stand.

## Phasing

1. **Core: model + solve + format** (headless). `Mate`/`Connector` structs on
   `AssemblyDocument`; `FaceAxis` kernel query; `AssemblySolve` with the
   propagation/loop-check pass; serialization v24 + golden + round-trip tests;
   mutation tests for the payoff loop (part edit moves mated occurrence).
2. **RPC/MCP + face picking**. `add_mate`/`set_mate`/`remove_mate`/
   `list_mates`/`solve_assembly`; the assembly face-pick join
   (RayHit.faceIndex → ElementRef); `assembly_mates_smoke.py`.
3. **GUI**: mate toolbar tool with connector-triad preview + accept; mate
   list + editor in the Model dock; error badges + re-pick; solve triggers
   wired (part edits, panel edits). Visual verification via RPC screenshots.

Then **A2.5** (drag gizmo + drag-within-mate + DOF display) per the roadmap,
then the rest of A3 (cylindrical/planar/ball, loops if needed).
