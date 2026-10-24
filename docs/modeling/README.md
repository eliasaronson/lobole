# Modeling

How a part is built: an ordered list of features that regenerates the part's
solid bodies from scratch, deterministically, on every edit. Sketching (the 2D
profiles features consume), sheet metal and tube frames are their own areas;
this page covers the feature model, regeneration, and the general solid
features.

Topic pages:

- [toponaming.md](toponaming.md) — how a face or edge keeps its name across
  regenerations, which is what every face/edge reference relies on.
- [holes-and-threads.md](holes-and-threads.md) — the Hole feature and what it
  records (fit, tapping), the Thread (cosmetic or modelled), the metric and
  unified thread tables, and thread engagement.
- [worm-gears.md](worm-gears.md) — the Worm and the hobbed WormWheel, cut
  from cylindrical blanks: sizes, how the wheel's envelope is computed, the
  phase a pair meshes at, and self-locking.
- [derived.md](derived.md) — the Derived feature: another part's bodies copied
  into this one as a snapshot.
- [templates.md](templates.md) — feature templates: a composite saved once as
  data, with named parameters, and placed as one `Template` feature that
  expands into its features in its own namespace.

## The model

- A **`Document`** (`src/core/Document.{h,cpp}`) holds the parameters
  (variables), the ordered feature list, and the bodies the last regeneration
  produced. Tolerances, studies and CAM setups also live on the document, but
  outside the regeneration chain.
- A **`Feature`** (`src/core/Feature.h`, concrete types in
  `src/core/features/`) has a stable id (`GetId()`), never an index, and
  `Execute(RegenContext&)` which reads and modifies the bodies built so far.
  `RegenContext` exposes only the features *before* the executing one, so a
  reference can only point backwards.
- A **`Body`** (`Feature.h`) is a `CADShape` plus transient data stamped each
  regeneration: `createdBy` (the id of the feature that created it — the
  whole-body identity booleans, patterns, ribs and CAM use), `elementMap`
  (face/edge names, [toponaming.md](toponaming.md)), `material`,
  `appearance`, `reference` (a ghost from a Derived feature), thread regions
  and bend records.
- References between features are stable ids: `sketch_id`, `source_id`,
  `body_id` (a `createdBy`), `ElementRef {feature_id, element}` for a face or
  edge. Every feature reports what it names through `VisitFeatureRefs`, and
  `Document::FirstConsumerIndex` uses that to find a feature's first consumer.
- Every numeric field a person would want to drive is an expression twin
  (`distance` + `distance_expr`). `VisitExprs` evaluates them against the
  parameter table before execution, so the evaluated value is part of the
  feature's JSON and therefore of the cache key. An expression that fails to
  evaluate is a feature error, not a silent fall-back to the literal.
- The kernel is OpenCASCADE behind `CADShape` (`src/cad/CADKernel.{h,cpp}`).
  `CADShape::Volume` is cached by TShape identity (plus the location's scale),
  because every mutating RPC digests every body's volume before and after, and
  an unchanged body — a large Derived snapshot especially — must not be
  re-measured each time. The cache holds the forward figure; a shape reversed
  in place reads it negated, which is what an inside-out solid encloses.

## Regeneration

`Document::Regenerate()` (`src/core/Document.cpp`):

1. Evaluates parameters, then every feature's expressions.
2. Computes a **chain hash** per feature: `h_i = H(h_{i-1}, FeatureToJson(f_i),
   effective suppression)`, seeded with the sheet-metal rules when the part has
   them. The serialized feature is exactly the state that determines its
   output, and chaining makes any upstream change invalidate everything after
   it. Stripped from the hash because they do not change geometry: sketch
   reference images, `appearance`, `face_appearance`, `material` (all are
   re-stamped onto bodies by a post-pass keyed on `createdBy`). An Import hashes its file's content
   instead of its path, so a save that moves the file into the project is not
   a changed part. A sketch standing on a face, a datum plane, another sketch
   or a frame leaves its `plane` out too: Execute works it out and writes it
   back, so it is an output, and keyed on it the hash carried the plane of
   whichever history last ran the sketch.
3. **Regeneration cache.** One entry per feature evaluation, keyed on its
   chain hash: the body list after it, and a `Clone()` of the feature taken
   right after it executed. Each feature is a lookup, so a hit is a hit
   wherever the entry came from — the current history, or one an undo, a
   redo, a value set back or a removal undone returns to. The chain cannot
   re-converge once it diverges, so a run is hits up to the edit and then
   all hits (a state seen before: nothing executes) or all misses (the tail
   executes); editing the last feature re-runs one feature.
   **A hit restores what Execute left on the feature**, because the object
   may last have executed in another history: `Feature::CopyDerivedState`
   (pure virtual, like `Clone`) copies the cut tool a Pattern reads, a
   datum's frame, a sketch's resolved plane and solved entities, a thread's
   overlay, the error and warning. Nearly every feature implements it as
   `RestoreExecuted<Self>`, a whole-object copy keeping only what the hash
   leaves out, so a member added later cannot be forgotten
   (`test_conventions.cpp`); Import and Template copy by hand. A feature's
   `DerivedFor()` names the evaluation its object holds, so a hit on the
   object's own evaluation skips the copy. The cache is bounded at
   `max(256, 3 × features)` entries, least recently used first, never an
   entry of the current chain: on the OM615 cylinder head (389 features) a
   full alternative history is about 130 MB resident.
   `LastRegenStats()` reports `{executed, cached}`; `ClearRegenCache()`
   empties it. `TipChainHash()` is the last feature's hash; baked geometry
   and the Derived stale check key on it (baked geometry is described in the
   versioning area). A baked open has no evaluations to return to, so the
   first edit and the first set back after it each rebuild. A sketch whose
   solve moves its entities keys on what they were before the solve, so the
   first regeneration after such a move is a miss once. The worm wheel also
   keeps its last few results by their exact inputs
   ([worm-gears.md](worm-gears.md#how-the-teeth-are-cut)).
4. **Continue past failure.** A feature that fails records its error, adds
   nothing, and regeneration continues; downstream features see the bodies as
   they were and fail on their own if they needed the missing output. A
   failure after an earlier failure is tagged (`after_earlier_failure` in the
   RPC `regen` block) so a client can point at the first red feature.
   `Regenerate()` returns false if any feature errored.
5. **Suppression.** A feature's `suppressed` flag (serialized) or suppression by the
   active configuration (`EffectiveSuppressed`) skips execution and keeps the
   parameters. A feature referencing a suppressed feature's output fails with
   its normal missing-reference error — the same rule as deletion.
6. **Rollback bar.** `Document::Rollback()` (persisted, -1 = end) executes
   features `[0, rollback)` only; the rest keep their parameters and hold no
   errors. A second, unpersisted marker, `ViewRollback()`, is set by sketch
   edit to the sketch's first consumer so the solid built from the sketch is
   not in front of it while editing; regeneration stops at the earlier of the
   two.

`Document::BulkEdit` defers regeneration while a loader adds many features, so
a load is one regeneration rather than one per feature.

A mutating RPC reports what it broke in its own response (`regen`), not only in
a later `get_state` (`tests/regen_honesty_smoke.py`).

## Features

The authoritative list, with every field, is `kFeatureTypes` in
`src/core/io/SerializeFeatures.cpp`, served as the RPC `describe`
`feature_types` rows; each row's typed `contract` is what `add_feature`
holds a spec to ([typed feature specs](../scripting/rpc.md#typed-feature-specs)). General solid features:

| type | what it does |
|---|---|
| `Box`, `Sphere`, `Cylinder` | a tool, not a stored type: `add_feature` expands one into a Sketch and an Extrude or Revolve ([Primitives](#primitives)) |
| `Import` | STEP/IGES/STL/OBJ/BREP bodies; centred on their bbox unless `keep_origin`, stood up by `up_axis` ([interchange](../interchange/README.md#up-axis)) |
| `Sketch` | 2D profile on a base plane, a `DatumPlane`, another `Sketch`'s plane or a `Frame` (`planeRef`), or a body face (`face_ref`); see the sketching area |
| `Extrude` | see [Extrude](#extrude) |
| `Revolve` | profile about an `AxisRef` (`x`, `y`, `z` through the world origin, `sketch_line`, `datum_axis`, `custom {origin, direction}`); `angle_deg`, `symmetric`, `reversed`. The axis must lie in the sketch plane and the profile must not cross it; a joint within 1e-4 mm of it is ON it, so an arc may end on the axis |
| `DatumPlane` | `offset` from a plane, `angle` about an axis, or `through_points`; sketches that reference it move with it |
| `DatumAxis` | two points, a sketch line, or a global axis |
| `Frame` | a named datum frame that mates anchor to (assemblies area) |
| `Boolean` | `union`, `subtract` or `intersect` of two bodies by `createdBy`; both operands consumed into one result, so booleans chain. The result must pass BRepCheck and [the boolean check](#a-boolean-is-checked-before-it-is-kept) |
| `Pattern` | `mirror` (about a base or datum plane), `linear`, `circular` of a source; see [Patterns](#patterns) |
| `Fillet`, `Chamfer` | edges by `ElementRef`; blend faces named from the spine edge |
| `Shell` | hollows a body, `open_faces` by `ElementRef` |
| `Draft` | tapers `faces` about a neutral plane |
| `Split` | a body by a face, a datum or an offset base plane |
| `Refine` | merges coplanar faces and boolean seams |
| `Rib` | see [Rib](#rib) |
| `Hole`, `Thread` | see [holes-and-threads.md](holes-and-threads.md) |
| `Worm`, `WormWheel` | see [worm-gears.md](worm-gears.md) |
| `Curve3D`, `Sweep`, `Loft`, `Pipe` | see [3D curves, sweep, loft and pipe](#3d-curves-sweep-loft-and-pipe) |
| `Derived` | see [derived.md](derived.md) |
| `Template` | an instance of a feature template; see [templates.md](templates.md) |
| `Fastener` | a generated standard part; only in a part of its own, placed in an assembly (the parts area) |
| `DeleteFace`, `OffsetFace`, `MoveFace`, `ReplaceFace` | change a body's faces where they are; see [Direct edit](#direct-edit) |

Sheet-metal features (`BaseFlange`, `EdgeFlange`, `Hem`, `Jog`, `Unfold`,
`Refold`, `FlatPattern`, `CornerRelief`, `NormalCut`) and `Cope` are
documented in the sheet-metal and tube areas.

Body modes: extrude, revolve, sweep, loft and pipe take `mode:
new_body|add|cut`, applied by `ApplyBodyMode` (`src/core/BodyMode.{h,cpp}`),
which propagates element names through the boolean. `add` fuses into the last
body its bounding box overlaps, and every other body the new solid actually
touches joins that one — a neck strung between two turned pieces makes one
body of them, where it used to overlap the second as a separate solid and the
part weighed the overlap twice. A body that only shares a bounding box with it
stays separate, and so does one that already touched the target before the add:
two overlapping bodies were made separate on purpose (a cavity drawn inside a
shell, to be subtracted from it), and growing one does not merge them. `add` never fuses into a reference (ghost) body, and `cut`
passes through one. A cut warns when it
removes no material, and when it consumes EVERY body: a part left with
nothing is a tool the wrong way round or a boolean the kernel could not do,
and without the warning the first sign is "no bodies" from `mass_properties`
with no feature named. Every add and cut result is checked before it is kept
([below](#a-boolean-is-checked-before-it-is-kept)); a refused one leaves every
body as it was.

### Primitives

A Box, Cylinder or Sphere is a one-call tool that stores what a person
would have drawn: a Sketch on what it is placed on, constrained and
dimensioned, and an Extrude (a Sphere: a Revolve) of it. A solid placed at
a world coordinate follows nothing (a boss at z = 24.7 stays there when the
plate becomes 30), reads as a number instead of an intent, and is a second
dialect a person has to rebuild to edit; this one is ordinary history on
what it stands on. It is for a fixture, a
stand-in or a first rough block; a part is modelled on its faces and planes
from sketches.

- **It names what it is `on`**, and there is no other placement: a planar
  face `{feature_id, element}`, `{feature_id}` of a `DatumPlane`, a `Sketch`
  (its plane) or a `Frame` (its XY plane, X as u: a mate connector), or
  `"Top"`/`"Front"`/`"Right"`. The sketch attaches the way any sketch does
  (`face_ref`, `planeRef`, or a baked base plane), so it resolves the
  reference's exact frame at every regeneration and moves with it. A face's
  frame is the sketch-on-face frame: origin at its centroid, normal outward,
  u from `CADShape::FacePlane`. A curved face is refused, pointing at a
  Frame of it.
- **The sketch is exactly determined (DOF 0).** Box: a rectangle
  (`AddRectangle`, or `AddRectangleCentered` when `centered`, the default),
  a `distance_x`/`distance_y` from the sketch origin to its centre or low
  corner (`offset` [u, v]), and its width (u) and depth (v); the extrude's
  distance is the height, along the normal (`reversed` the other way).
  Cylinder: a circle with a diameter and the two offsets. Sphere: a half disc
  over the u axis, its arc's ends on the straight side and its centre ON that
  side (point-on-line, not midpoint, which would be redundant), revolved 360°
  about that side, so the centre lies on the plane. Every dimension takes an
  expression (`width_expr`, `offset_expr` [u, v], ...).
- **Defaults derive from the reference**: half the face's smaller span on a
  face (`CADShape::FaceExtent` samples its outline in the face frame), a
  quarter of the part's largest dimension on a plane or a frame. On a plane
  in an empty part there is nothing to measure, so the RPC refuses and asks
  for the sizes; the GUI passes the viewport's reach.
- **One undo step.** `add_feature` adds the sketch, then the solid, and
  collapses both into one command (`primitive::Add`,
  `src/core/Primitives.{h,cpp}`). It returns `id`/`index` of the solid
  (the body's `created_by`) and `ids` [solid, sketch]. A request may pin
  both with `ids` [solid, sketch], which is how a component builder keeps
  their identity across rebuilds (`python/lobole/components.py`).
- **Edited through what it stored.** `modify_feature` on the sketch with a
  dimension's `value` changed re-solves the sketch before committing it, so
  the geometry follows the number (a literal dimension is not re-solved at
  regeneration, only an expression-driven one), and refuses a value the
  sketch cannot take or that would flip it
  ([sketching](../sketching/README.md#refusals)); a size given as an
  expression follows `set_variable`; the height is the extrude's
  `distance`.
- **Refused, naming what is accepted**: a missing or unknown `on`, the old
  `position`/`center`/`direction`, an unknown field, a size given as both
  diameter and radius, a size that is not positive, a feature that is not a
  plane. `modify_feature` refuses `type: Box|Cylinder|Sphere`; a primitive
  is changed through its sketch's dimensions or its extrude.
- **On a world plane over a face, the lint says so.** A primitive placed
  on Top/Front/Right at an offset, or on a datum plane, where a planar face
  of the part already lies is a `check_literals` placement finding naming
  that face ([../variables/README.md](../variables/README.md#literals-that-should-be-expressions)).
- **A lost reference is the sketch's error**, never the world plane: a
  deleted or suppressed datum, sketch or frame ("the plane this sketch is on
  ... is gone"), a face that no longer resolves. A feature whose sketch
  failed (Extrude, Revolve, Loft, Rib, Hole, Normal Cut, Jog, Base Flange,
  Sweep's profile) fails too, rather than building from the sketch's last
  plane ([../sketching/README.md](../sketching/README.md)).

The stored `BoxFeature`, `CylinderFeature` and `SphereFeature`
(`src/core/Feature.h`) remain for two readers only: older documents that carry
one (they load, regenerate and save as they were; their faces keep the
names `<id>/face<i>`, and the feature editor still edits one), and core
tests that want a one-line solid. Nothing an agent or a person reaches
creates one: not `add_feature`, `modify_feature`, the SDK, the MCP surface
or the GUI. The saved form did not change, so this is not a format version.

### Extrude

`ExtrudeFeature` (`src/core/features/ExtrudeFeature.{h,cpp}`,
`src/core/sketch/SketchProfile.cpp`):

- **Profiles.** Every closed loop of the sketch is classified by containment
  depth: even depth is an outer, odd depth a hole of its immediate container,
  and an island inside a hole is an outer again. Each outer with its holes is
  one prism; the prisms are assembled into one result. Loops that partially
  overlap are refused ("profile loops overlap"). `regions` restricts the
  extrude to picked regions (each named by the sorted entity ids of its outer
  loop); a region that no longer matches a loop is an error.
- **End conditions** (`end_mode`): `blind` (`distance`), `symmetric` (centred
  on the plane: half of `distance` each way; the tool window's *End* combo,
  and there is no separate `symmetric` key), `two_sided` (`distance` forward, `distance2` back),
  `up_to_face` (`end_target`, a **planar** face by `ElementRef`, followed
  across upstream edits), `through_all` (cut only; past every body's extent).
  Every variant builds one prism of the computed length, translated back along
  the normal, so element names are the same as a blind extrude's. A positive
  `distance` runs along the sketch plane's normal; for a feature on the far
  side of a part, face the plane the other way (the SDK's
  `Plane.yz(x, flip=True)`, [axes](../scripting/sdk.md#the-principal-planes))
  rather than passing a negative distance.
- **Thin** (`thin: {thickness, side: out|in|both}`): every loop becomes a wall
  ring (OCC `MakeOffset`), composing with any end condition.
- **Twist** (`twist_deg`, `herringbone`): the profile turns about the sketch
  normal as it goes ([twisted extrudes](#twisted-extrudes)).
- **Taper** (`taper_deg`): the walls lean in or out as it goes
  ([tapered extrudes](#tapered-extrudes)).
- A cut or add extrude keeps the prism it swept (`ToolShape()`), which is what
  lets a pattern repeat it; a Hole keeps its drilled tool the same way, so a
  bolt circle is one Hole and a pattern.
- A cut that removes nothing, or an add that adds nothing, leaves the body as
  it was, with a warning: the boolean would still imprint the tool's outline
  where the two touch, splitting a face and renaming it.
- **A tool starts on its plane, and a face typed at the same height is on it
  too.** The model is double
  ([../conventions.md](../conventions.md#the-model-is-double)), so a face
  built at z = 24.7 sits at the double nearest 24.7, where a sketch
  plane typed at 24.7 is, and OpenCASCADE cuts and fuses an exactly coplanar
  face cleanly. While primitives were placed in float the face sat at
  24.70000076: a pocket cut from the plane kept a sealed skin over itself and
  an add left two solids, and a cut or add then started a hair behind its
  plane whenever a face lay within float rounding of it. Nothing model-side
  is float any more, and that mitigation is gone with it
  (`test_model_precision.cpp` cuts and adds at 126 awkward heights,
  `test_hole.cpp` drills at six). An STL is float by its format: a plane typed
  at the decimal height of one of its faces is off it by that rounding, so
  sketch on the face, which takes its exact frame.

### Twisted extrudes

`twist_deg` (+ `twist_expr`, live like `distance_expr`) turns the profile
about the extrude direction over the extrude's length: a helical gear, a
twisted vase, a spiral flute. The axis is the sketch normal through the
**sketch origin**, so a gear is drawn about its sketch's origin. A positive
twist is right-handed about the direction of travel, which makes it a
right-handed helix whichever way the extrude runs.

- **The sweep is one screw motion.** Each wire of the profile is swept along
  a helix about the axis with the axis as the pipe's constant binormal
  (`ScrewSweepFace`, `src/core/HelicalSweep.{h,cpp}`; the builder is
  `BuildTwistedPrismNamed` in `SketchProfile.cpp`). With a constant binormal
  the trihedron turns about the axis exactly as the helix point does,
  whatever the helix radius, so every point of the profile follows a helix
  of the same lead and the side faces are helicoids: one per sketch edge,
  named as a prism's (`<id>/g<entity>`, `cap0`, `cap1`). The holes of a
  profile are swept the same way and cut from the outer. The surfaces are
  the pipe's B-spline approximation, within its 1e-4 mm tolerance: a twisted
  rectangle's volume is the prism's to a few parts in a million.
- **Herringbone** (`herringbone: true`) climbs the first half at the plain
  twist's rate and turns back over the second, so the helix angle does not
  change when it is switched on: the apex is turned by half of `twist_deg`
  and both ends are the profile. The upper half is the lower mirrored in the
  mid-plane — a mirror reverses the twist and meets the lower half on an
  exactly coincident face — and the two are fused with OCC's full glue. Its
  upper side faces are named `<id>/g<entity>@h2`.
- **Symmetric** centres the solid on the sketch plane and turns it back by
  half the twist, so the section on the sketch plane is the sketch (for a
  herringbone, the apex is there).
- **Refused:** a twist with `two_sided`, `up_to_face` or `through_all`, and a
  thin twist. `WhyIncomplete` names what is accepted, so the tool window
  shows it in red and disables Accept and `add_feature` refuses it; a file
  that carries one fails the feature with the same words. A twist of 0 is a
  plain prism, and `herringbone` is ignored there.
- **In the GUI:** the Extrude window's *Twist (deg)* field (Blind and
  Symmetric) and, once there is a twist, a *Herringbone* checkbox; the
  preview carries a twist arc ([viewport
  handles](../ui/viewport-handles.md#preview-handles)).
- **Measuring two meshing helical gears is slow.** Exact distance between two
  conjugate helicoidal flanks took about 28 s for a sun and planet against
  about 12 s for the same pair as lofts through turned sections: the flanks
  nearly touch along a line, which is the hard case for OCC's
  surface-to-surface extrema.

### Tapered extrudes

`taper_deg` (+ `taper_expr`) leans every wall of a blind extrude IN by that
angle from the extrude direction over its length (negative leans out), about
the sketch plane: the section on the plane is the sketch, and the far end is
the profile offset by `distance · tan(taper)`. A boss whose underside prints
at 45°, a cam lobe printed standing, a draft for a moulded or cast pocket.

- **Built by drafting the straight prism.** `ExtrudeFeature::TaperPrism`
  runs `BRepOffsetAPI_DraftAngle` on each side face of the prism
  `BuildPrismNamed` swept, about the neutral plane of the sketch with the
  extrude direction as the pull: a line's wall becomes an inclined plane,
  an arc's a cone. A profile of several regions is a compound, drafted
  solid by solid. A hole's walls lean the other way round from the
  outer's, so the material narrows (or widens) everywhere alike.
- **Names are a straight extrude's.** Each wall's name moves to the face
  the draft made of it (`ModifiedShape`), the caps keep `cap0` and `cap1`,
  and the edges are named again from their faces: a fillet, a sketch on a
  wall or a face reference survives turning a taper on, changing it, or
  changing the length (`test_printed_features.cpp`,
  "a taper keeps every wall's name across an edit").
- **Refused past the angle at which the profile closes up**, naming it: the
  walls move in by `L tan(taper)` at the far end, so the profile is gone
  when that reaches its inradius (the largest circle inside its material,
  found on its tessellated boundary by a grid and a pattern search), or,
  leaning out, a hole's inradius. A 20 mm square over 10 mm is refused at
  45° and over: "the taper must be under 45 degrees". A profile with a
  feature smaller than that — a cam lobe whose nose circle is narrower than
  the taper's offset — can still fail in the kernel before it, and says so.
- **Refused with** `two_sided`, `symmetric`, `up_to_face` or `through_all`,
  thin, or a twist (`WhyIncomplete`, so the tool window shows it and
  `add_feature` refuses it), and at ±90° and past.
- **In the GUI:** the Extrude window's *Taper (deg)* field, on a blind solid
  extrude.

### Patterns

`PatternFeature` (`src/core/features/PatternFeature.{h,cpp}`) references its
source by feature id. `count`, `spacing` and `angle_deg` are expressions.
`angle_deg` is the angle BETWEEN neighbouring copies, not the sweep: a full
ring of n is 360/n, and 360 puts every copy on the seed (the cut "removed no
material"). A pattern makes at most 10 000 copies, its angle is within ±360,
and a linear one reaches no further than the model space (below).

- A **body source**: every instance (seed plus copies) is fused and split back
  into connected solids, so touching instances merge and disjoint ones become
  separate bodies. The seed is consumed.
- A **cut, add or Hole source** — an Extrude, a Revolve (which a Sphere placed
  on a body is), a Box or Cylinder in add or cut mode, a Hole — leaves no
  body of its own, so the pattern
  repeats the source's tool (`Feature::ToolShape`) and subtracts or fuses each
  transformed copy. In the GUI the pick for this is the cut's tree row with the
  Source slot armed; a body pick resolves to the body's `createdBy`, which is
  the plate, not the cut.
- `angle_deg` is the STEP between instances (six round a circle is 60). A
  step that puts every copy on the seed — a whole number of turns, or a
  linear `spacing` of 0 — is refused: it builds nothing while reading as a
  pattern that worked.
- Every instance carries the source's element names with an instance suffix
  (`4/g7` becomes `4/g7#2`, a mirror's `#m`), so a face of the third copy is as
  referenceable as one of the first.

### Rib

`RibFeature` (`src/core/features/RibFeature.{h,cpp}`) grows a wall from an
**open** sketch profile toward a body: the profile is offset by `thickness/2`
each side into a closed slab, prismed along the sketch normal (`flip` for the
other way) far enough to pass through, cut against the target body, and the
component touching the profile plane is fused in. That component is exactly
the material between the profile and the body wall, so it conforms to curved
walls. A profile that never reaches the body is an error. The target is
`body_id` (a `createdBy`), or the nearest overlapping body when 0.

### Thread

`ThreadFeature` is an annotation on a cylindrical face, with pitch and
diameters from the ISO metric and unified (UNC/UNF) tables in
`src/core/ThreadStandards.{h,cpp}`. Cosmetic by default, it changes no
geometry; `modeled` cuts the real helical groove, right- or left-handed, and
a cut that fails keeps the body. Details, and how threads engage in
interference checks: [holes-and-threads.md](holes-and-threads.md#thread).

### 3D curves, sweep, loft and pipe

- **`Curve3D`** (`CurveFeature`, `src/core/features/CurveFeature.{h,cpp}`) is
  a free curve that is neither a sketch entity nor a body. Like a datum it
  contributes no body; `Execute` caches an OCC edge and a polyline, and a
  degenerate definition is an error. Kinds (`Curve3DKindName` is the one
  spelling): `fit_spline` (`GeomAPI_Interpolate` through `points`, `closed`
  for periodic), `control_poly` (clamped B-spline over `points` as poles,
  `degree`), `helix` (`origin`, `axis`, `radius`, `pitch`, `turns`,
  `left_handed`, `start_deg` measured from `HelixReference(axis)`), and
  `bent_path` (the corners of a tube's centreline and one bend `radius`, built
  line-arc-line with exact circular arcs; a straight too short for its two
  arcs or a corner that doubles back is refused, and refused where it is
  ADDED: `add_feature` asks `WhyIncomplete`, so nothing is committed for a
  pipe to be swept along). A bent path reports its
  `Bends()` — angle, radius, straight before, roll — and its cut `Length()`.
  RPCs: `curve_points` (the polyline actually built, with arc length) and
  `tube_schedule` (cut list and bend schedule per tube member).
- **`Sweep`** (`SweepFeature`) skins a closed profile sketch along `path_id`,
  a sketch (an open chain of lines, arcs and splines) or a `Curve3D`
  (`ResolveSweepPathWire`), with `BRepOffsetAPI_MakePipeShell` and rounded
  corner transitions. `frame`: `corrected_frenet` (default; cannot self-twist
  on a helix), `frenet`, `fixed`. An optional `guide_id` reorients the section
  along a second curve (`BRepFill_NoContact`).
- **`Loft`** skins `sketch_ids` (two or more sections, in order) with
  `BRepOffsetAPI_ThruSections`. With `path_id` the sections are instead swept
  along that spine by `MakePipeShell` with several sections, and an optional
  `guide_id` scales them to keep contact with the guide
  (`BRepFill_ContactOnBorder`) — `ThruSections` has no guide support. A
  section whose sketch failed fails the loft, naming it ("loft section 'S'
  failed: …"), rather than lofting what the sketch left behind.
- **`Pipe`** is a circle of `outer_diameter` (hollow when `wall_thickness` >
  0) swept along a path.

All four share `SweepSectionsAlongSpine` (`SweepFeature.h`). A path or guide
that gives no spine is reported by `SweepPathError`: when the path feature is
itself broken (a bent path edited into one that cannot be bent) the consumer
names it and defers to its error, instead of claiming the path is the wrong
kind of thing.

## Direct edit

A body changes by features added on top of it, and an imported STEP solid
arrives as `Import` with every face `<import>/face<i>` and no history
behind it: a hole in the wrong place, a cosmetic round in the way, a wall
too thin. Four features change a body's faces where they are: `DeleteFace`,
`OffsetFace`, `MoveFace` and `ReplaceFace`. Each is an ordinary history
feature (edited, suppressed, rolled back and undone like any other), takes
faces by `ElementRef` on ONE body named by its `createdBy` (as Shell does),
and changes that body in place. One file pair each under
`src/core/features/`; what they share is `src/core/DirectEdit.{h,cpp}`
(`directedit::`), not under `features/`, where every header is a feature's
(the `VisitFeatureRefs` convention scans them).

What every one holds to:

1. **It names through its builder's history** (`topo::NamesThroughHistory`
   with `keepWholeFaces` false: [toponaming](toponaming.md#how-each-operation-names-its-output)). A
   face it moves, extends, trims or stands in for is `<name>;M<id>`; a face
   it leaves alone keeps its name exactly. Builders report faces they merely
   rebuilt (a boolean of two bodies sharing every untouched face reports
   each one modified), so `directedit::RestoreUntouched` gives a face its
   name back when it is the input's face unchanged (surface kind, area and
   centroid to 1e-9).
2. **It refuses what it cannot do, naming the face**
   (`directedit::CheckResult`): `Measure::InvalidReasons` empty, the solid
   count the input's, every face asked for gone (Delete) or carrying
   `;M<id>`, the volume moving the way the operation says, and the tight
   bbox growing by no more than the operation allows. Builders run in
   `occsig::RunCatchingOccSignals` (`directedit::Guarded`). A refusal is the
   feature's error and reads "planar face `7/face12`, 120 mm²", the body left
   as it was.
3. **`add_feature` and `modify_feature` refuse one whose own Execute fails**
   (`RefuseFailingDirectEdit`, `src/remote/RemoteModeling.cpp`): the edit is
   built first on `Document::CopyWithGeometry` of the part (its bodies
   reused, so only the edit and what follows it runs), and its error, when
   it is its own and not after an earlier failure, is the call's: "DeleteFace
   refused: … The document is unchanged". Built on a copy, the refusal
   changes nothing, the revision included, inside a `batch` or out of one
   ([a refused call changes nothing](../scripting/rpc.md#a-refused-call-changes-nothing)).
   An upstream edit that later breaks a stored one is a broken feature,
   named in that edit's `regen`.
4. **A face gone upstream is an error, never skipped**
   (`topo::UnresolvedReason`). The tree's context menu has **Pick faces…**
   (Pick face… on a Replace), which opens the editor with the face slot
   armed: the dead ref goes with its "x", its successor is picked in the
   viewport.
5. **An imported face's index is checked against what was picked.** A face
   ref whose name `topo::DecodeElementName` calls positional AND whose seed
   is an `Import` stores the face's `fingerprint` {surface, area_mm2,
   centroid_mm} (`directedit::FaceFingerprint`), taken the first time the
   edit sees the face. A changed file read again renumbers its faces; when
   the face under the name is no longer the fingerprint's (to 1e-6
   relative), the edit fails: "face `7/face12` is no longer the face that
   was picked (was a plane of 120 mm² at …, now …); re-pick it". A pick in
   the editor drops the old print. A primitive's `<id>/face<i>` is
   positional too, but a box keeps its numbering whatever its size, and
   names that say why a face exists are resized by upstream edits on
   purpose, so neither is fingerprinted.
6. **Faces belong to one body.** Refs on two are refused. Bodies that share
   a `createdBy` (a multi-body import's `<id>/b<k>/…`, a split's pieces) are
   told apart by the name: the edit acts on the body that holds its first
   face (`directedit::BodyHolding`).

### Delete Face

`DeleteFace {body_id, faces[]}` removes the faces and heals the gap by
extending the faces around it (`BRepAlgoAPI_Defeaturing`, history on,
single-threaded, on a `BRepBuilderAPI_Copy`): a hole's bore and floor fill,
a boss levels, a fillet or chamfer face gives back the sharp edge.
Defeaturing reports a feature it could not remove as a WARNING
(`BOPAlgo_AlertUnableToRemoveTheFeature`) and returns success with the
feature still there, so any warning refuses, its shape mapped back to names
(`topo::FaceNamesIn`): "Delete Face could not heal around `7/face12`: the
faces around them do not meet when extended (parallel or tangent)". A set
that takes every face of the body is refused before the builder runs, and so
is one beside a freeform face (Measure's kind `other`: a modelled thread's
flank, a spline), which healing would have to extend: Defeaturing's
edge-face intersections on a thread ran for more than fifteen minutes on one
face. Offset and Replace Face refuse the same way
(`directedit::FreeformNeighbour`); pick the freeform faces with it, or edit
the feature that made them.

Every direct-edit builder (Defeaturing, the offset builder, MakerVolume) also
runs under a deadline, `directedit::BuilderBudgetS` (30 s and 0.5 s a
face), through a `Message_ProgressIndicator` whose `UserBreak` turns true
once it has passed: past it the edit is refused ("gave up after 30 s …").
The builders check it only at their progress points, so a call can run
past the budget before it stops. It is a backstop against a frozen app, far
past any edit that builds, not a timer whose answer could depend on the
machine's load.

The bbox may grow by no more than the deleted faces' own extent. Defeaturing
trims the host face's own surface, so a pocket in an imported face whose
edges lie 5e-5 mm off its plane fills and the host stays that plane
(`test_direct_edit`).

The editor's **Add the feature** (`###add_feature`) and the RPC
`grow_faces {faces}` grow picks to the faces of the one feature they belong
to (`directedit::FeatureFaces`): across edges to every face reachable
without stepping onto a host, a face whose inner loop (a hole in it) the
edge lies on. A through bore stays itself, a blind hole's bore takes its
floor, a boss's side takes the boss; a set no inner loop bounds (a fillet
runs on into the body) is refused, saying why, and the picks stay as they
were. `grow_faces` also answers `host_normal`, the largest planar face the
feature sits in.

All 100 bores of SolidCraft's `plate_100_holes.step` delete in one feature
in about 20 s on a quiet machine (nearly 40 s on a loaded one), nearly all of
it Defeaturing; the test allows 90 s.

### Offset Face

`OffsetFace {body_id, faces[], distance_mm (+_expr)}` moves each face along
its own normal, positive outward (adding material); its neighbours follow
along their own surfaces, so a drafted wall stays drafted and a cylinder
changes radius. Each face is put onto its own surface offset by the distance
-- a plane moved, a cylinder of another radius (`Geom_OffsetSurface`, its
canonical surface where it has one) -- by Replace Face's cells
(`directedit::ReplaceOnto`), one face at a time, its neighbours extended or
trimmed to meet it; a face a later step trims again keeps one `;M<id>`.

A set whose faces meet one another tangentially (a top, its round and the
wall below) goes to `BRepOffset_MakeOffset` instead (skin mode, intersection
joins, zero offset on the shape, `SetOffsetOnFace` per face): one face at a
time, a moved face no longer meets its unmoved tangent neighbour at all. It
is named through an adapter that reads the face the builder GENERATES from a
picked one as that face modified; a shell that comes back unclosed is made
a solid again. It takes a guard like Shell's for the unpatched apt OCCT (up to five
draws on a copy when a result fails the checks, under one deadline), but
none after a fault: a builder that long-jumped out of its frames is not run
again on that heap. The builder is not used elsewhere because elsewhere it
was the problem: it refused planar walls it should move (an imported
battery holder with cradles cut across it: no error, no shape), faulted in
`BRepOffset_Inter2d` and ran its 3D intersections past any budget on the
action matrix's fuzz designs, where the cells answer in a fraction of a
second.

Checks: outward never removes volume, inward never adds; the bbox grows by
at most the distance; an inward offset through the body is refused
("offsetting `4/cap1` by -20 mm consumes the body: it is 20 mm thick
there", measured along the inward normal from the face's inside point,
`directedit::ThicknessUnder`). A face with a tangent neighbour that is not
picked is refused naming it ("… would tear it from cylindrical face `…`,
which meets it tangentially (a fillet?): offset both together"): left to
itself the builder offsets the whole tangent chain, faces nobody picked. A
fillet is tangent to both faces it joins, so a top face, its round and the
wall below go together.

Default: a tenth of the smaller of the face's inset (list_elements'
`inset_mm`, `Measure::FaceInsetAt`) and the body's thinnest extent, rounded
down to 1, 2 or 5 × 10ⁿ, outward (`ui::SizeFromFace` beside
`SizeFromBody`), derived again from the first face picked into an empty
tool. The window has **Flip**; an arrow on the first face drags the
distance (`HandleParam::Offset`, `src/ui/PreviewHandles.cpp`).

### Move Face

`MoveFace {body_id, faces[], translate_mm [x,y,z] (+_expr each),
rotate_deg (+_expr), axis (AxisRef)}` moves a set of faces forming one
feature (a hole, boss, pocket, slot) rigidly, rotated about the axis first,
then translated; the body closes behind it and opens where it lands. Built
from Delete Face and named booleans: `H` = the body `B` with the faces
deleted; the feature's material `cut = H − B` (a hole's) and `add = B − H`
(a boss's), through `topo::BooleanNamed`; the result `(H − T(cut)) ∪
T(add)`, `T` the move as a location on the shape, so its faces keep their
index order and names. The chain runs under the edit's one tag, so its
repeated `;M<id>` steps collapse to one (`directedit::CollapseSteps`), and a
moved wall the booleans left whole is given its `;M<id>`: each moved wall is
`<original>;M<id>`, and a thread on a moved bore or a mate on a moved boss
follows it.

Checks: the volume unchanged to 1e-9; a moved hole that leaves the body is
refused naming the faces it crosses and by how much ("moving `9/p3/bore` 27
mm along X takes it 4 mm past `2/g1`"); a moved boss that sinks into the body
likewise; one that floats off it changes the solid count. A face that does
not lift out (a box's end face) fails Delete Face's heal and is refused
pointing to Offset Face; a slide within the feature's own surfaces (a bore
along its axis) changes nothing and is refused saying so.

Default: in the host face's plane (`directedit::HostNormal`, the largest
planar face the picked ones meet), along the body's largest extent in that
plane, a tenth of it, rounded as Offset's is (`ui::DefaultFaceMove`); no
rotation. Three arrows at the picked faces' centroid drag X, Y and Z
(`HandleParam::MoveX/Y/Z`), the preview-handle drag the extrude's arrow
uses. The occurrence gizmo (`GizmoInput.cpp`) is an assembly's and drives
occurrences, not features, so it does not drive this.

### Replace Face

`ReplaceFace {body_id, face, target}` puts a face onto another surface, its
neighbours extended or trimmed along their own surfaces to meet it.
`target` is `{kind: "plane", base_plane | datum_id}`, `{kind: "sketch",
sketch_id}` (the sketch's plane) or `{kind: "face", face}` (any body's face:
its whole surface); an unknown `kind` is refused naming the three. A
parallel planar target onto a planar face is Offset Face by the signed
distance (refused when the face already lies on it). Otherwise
`directedit::ReplaceOnto` hands `BOPAlgo_MakerVolume`:

- the body's faces less the replaced one;
- its neighbours extended past the target (`BRepLib::ExtendFace`, by the
  body's bbox diagonal, across only the UV sides the edges they share with
  the replaced face lie on: grown every way, a cradle's end wall ran on
  through the bottom of the part);
- the target surface bounded to twice that size: a plane as a square
  centred on the body, a cylinder, cone or sphere all the way round, any
  other surface `ExtendFace`d.

The cells holding a point just inside the faces the edit leaves alone are
kept; more than the body's solid count is refused ("leaves 2 separate
pieces"). The new face is `<replaced>;M<id>`. Checks: every untouched face
keeps its name (one the target cuts is refused naming it); the target is
there (a target the extended neighbours do not reach is refused naming
them); the bbox grows by at most the diagonal. The volume may go either
way, or not move at all (a tilt about the face's middle). The target slot
(`###slot` `ReplaceFaceTarget`) takes a face in the viewport, or a datum
plane's or a sketch's row in the tree; there is no numeric default.

## Sizes past the model space are refused

`kModelSpaceMm` (`src/core/ModelSpace.h`) is a kilometre: OpenCASCADE's
confusion tolerance is 1e-7 mm and a double carries about 16 digits, so past
about 1e9 mm a coordinate cannot tell two points apart and an algorithm loops
or crashes instead of failing. A Hole's diameter and depth, a linear
pattern's reach (spacing × (count − 1)), a Shell's thickness, an Offset
Face's distance and each component of a Move Face's translation are refused
past it, by name, and so are a sketch's plane origin and its entities'
coordinates, sizes and angles (naming the entity), a 3D curve's points,
axis, radius and pitch (and a helix of more than 10 000 turns), and a
stored Box's, Sphere's or Cylinder's position or centre, direction and
sizes: a file can hold any of them, and OpenCASCADE's loft crashes on a
sketch at 2⁶⁴ mm. A Shell is also refused at a wall of half the body's
thinnest extent or more, which leaves no cavity.

## Refused on the bodies

`add_feature`, `modify_feature` and the GUI's Accept refuse, with the
document unchanged, a feature the bodies it would act on cannot take
(`Feature::WhyRefusedOn`, given the tip's bodies for an add and the cached
bodies before the feature for a modify; inside a `batch`, whose
regeneration is deferred, it is left to the feature's own Execute, which
runs the same checks and reports them as its error). The checks cost a
look at the bodies and nothing like a build, and each names what would be
accepted:

- **The body an element is on** (`topo::FindOwningBody`,
  `src/core/ElementHistory.h`). A pattern's copies and a split's pieces keep
  the `createdBy` of the feature that made them, so an id alone does not say
  which body: a reference lands on the one body with that id whose element
  map names it. A name two of them carry is refused, naming both; a name on
  none is the usual "no longer exists"; and a name given with the id of the
  feature that MADE the element (its `made_by`, a Fillet's) rather than the
  one that created its body is refused naming the id to use. Every ref of
  one feature must land on one body. Fillet, Chamfer, Draft, Shell, Thread
  and Split's face knife find their body this way; Boolean, Refine, Rib and
  a plane Split name no element and take the first body with their id.
- **A blend edge that starts no blend**: degenerated, a face on one side
  only, or its two faces tangent by OpenCASCADE's own test
  (`ChFi3d::IsTangentFaces`), when every picked edge is one ("There are no
  suitable edges for chamfer or fillet" said it without saying which). One
  among others may be blended on another's tangent chain; an edge the blend
  builder then took into no contour fails the feature instead of being left
  out in silence.
- **A Draft face square to the pull.**
- **A Shell wall that breaks through an open face**: under each open face
  the body is as thick as a line from a point well inside it, along its
  inward normal, runs before it leaves the solid
  (`directedit::WallsUnder`), and the face it leaves through moves toward
  the opening by the wall. Only under the open faces: where two kept faces
  face each other across less than two walls the offset builder can still
  make a valid shell (a bore beside the outside merges into the cavity).
  The Shell tool's default wall is a tenth of the body's thinnest wall
  (`ThinnestWall`, the least of those lines over every face), rounded down
  to 1, 2 or 5 × 10ⁿ: a bounding box is not the wall of a body that is
  already thin-walled.
- **A Pattern or Mirror of a feature that made no body and swept no tool**
  (a Fillet, a Shell): refused naming the body it changed and that body's
  `created_by`, the source to give instead.

## Kernel calls build on a copy of the cached body

The body a feature acts on is the previous feature's cached result, which
undo serves back and every later feature builds on. OpenCASCADE's booleans,
splitter and blend, draft and offset builders write the p-curves and
tolerances they compute onto their arguments, so a call that ran on the
cached shape changed what every later call on it built: a split of random
design 33 lost 5.6 mm³ on a fresh body and none after a fillet had been
tried on it (plain OpenCASCADE does the same on the exported body). Booleans
(target and tool copied together, so what they share stays shared), fillets,
chamfers, drafts and shells therefore build on a topology copy
(`BRepBuilderAPI_Copy`, the geometry shared, the explorer order and so every
index-keyed name kept), and a split runs non-destructive (its pieces are
named fresh).

## A caught kernel fault

The few kernel calls known to fault rather than fail -- a blend, a draft, a
shell, an offset -- run in OpenCASCADE's signal scope
(`occsig::RunCatchingOccSignals`, `src/cad/OccSignals.h`), where a SIGSEGV
becomes that feature's error. The scope is re-armed whenever something
replaced its handlers (Catch2 does, around every test case). A caught fault
is a process event, not only the call's failure: the long jump skipped
destructors, and a wild address may have written somewhere first. So
`occsig::FaultCount()` counts them; the regeneration that saw one drops the
whole regen cache and does not cache the faulted evaluation; the feature's
`regen.failed` entry carries `kernel_fault: true`; `ping` reports
`kernel_faults`; and the GUI says once that the work should be saved and
Loböle restarted. The calls build on a copy of the body
([above](#kernel-calls-build-on-a-copy-of-the-cached-body)), so a fault
damages only throwaway topology.

The faults the [action matrix](#action-matrix) found are OpenCASCADE's, and
patched in every build we make but the Debian package, which links
Ubuntu's own (`nix/patches/`, written up for upstream with
their reproducers in `nix/patches/upstream/`, the bodies in
`tests/fixtures/occ/`): a draft past a sphere's pole dereferenced a missing
curve, a draft whose edge runs parallel to a face read an extremum's absent
point, a fillet through the old boolean's wire repair freed the list it was
walking, and a fillet beside a bore's seam walked round its vertex for ever
inside `BRepFilletAPI_MakeFillet::Add`. Each is now an ordinary failed
feature (`tests/test_occ_patches.cpp`, skipped on a kernel without the patch
-- CI's apt OpenCASCADE).

## A reshaping feature keeps only a valid solid

A Fillet, Chamfer, Shell, Draft or direct edit
([`directedit::CheckResult`](#direct-edit)) whose builder reports success on
a solid that crosses itself is refused with `Measure::InvalidReasons` ("draft
produced an invalid solid (self-intersecting wire, unorientable shape)"),
and the body is left as it was; check_geometry would have called it invalid.
A Shell that reaches past the body's own measured outside — the offset
builder moving a face outward — is refused too: a shell hollows inward.

Stock OpenCASCADE's offset builder answers by where its input lives in
memory, not only by what it is. A drilled box shelled through one bore's
floor (`tests/fuzz/cases/rebuild-139.jsonl`) came back an invalid solid of
several times the volume about one build in seven, in a copy, in another
process or a second time in the same one. `MakeShells` quilts the offset
faces in the order of a map hashed on shape addresses, the split of a cap
that the offset extends (a bore's floor) is oriented against them, and the
quilt keeps the orientation of whichever face comes first. Every build here
(nix, the Flatpak, the AppImage, the Windows cross build; not the Debian
package) carries
`nix/patches/occt-shell-cap-order.patch`, which quilts the caps last; the
overlay in `nix/occt-overlay.nix` applies it, and netgen is built against
the same patched library.

The CI Ubuntu job links apt's OpenCASCADE, which is unpatched, so the Shell
also keeps its own guard (Offset Face, on the same builder, has one like it:
five draws whatever the kernel, under one deadline). Every such answer fails one of the checks above,
so a Shell that fails them is built again on a `BRepBuilderAPI_Copy` of its
input (the same solid at new addresses, sub-shape for sub-shape, so its
index-keyed names are the body's), up to five draws on apt's kernel and two
on a patched one (`occpatch::Carries`, `src/cad/OccPatches.h`: the install
lists its patches), and is refused only when every draw failed. On the
patched kernel the first draw is nearly always the answer -- two shells of
the action matrix's 1,800 still built only on a later one -- and an honest
refusal (a wall too thick for the geometry) cost five draws for nothing, a
fifth of a full sweep. The offset builder's Intersection mode (offset faces
intersected over the whole solid, not only between neighbours) was measured
as a further try: it built 106 of the matrix's shells the arc joins refused,
but the shell it built on random design 33 left a body the next cut could
not cut, so it is not used. Every draw shares one budget, the direct edits'
(`directedit::BuilderBudgetS` by the body's faces): the builder stops at its
progress checks, so a wall far too thin for a worm's flank gives up instead
of running for minutes, though it overshoots to the end of whatever step it
was in.
`test_shell`'s "A shell builds the same wherever its input lives in memory"
shells 60 copies of that box and requires one solid with one set of names.

The patch fixes which way the shell's faces point, not the order they come
in, which still follows those addresses (upstream's
Open-Cascade-SAS/OCCT#1600 fixes the order). A feature after it sees the
same solid with its faces in another order, which mattered to one check: a
Draft that tilted the wall of a sealed cavity through the outer wall passed
or failed BRepCheck by face order, and is now refused every time
([inspect](../inspect/README.md#geometry-checks)).

Their builders can also fault outright on input they should refuse.
`ArmOccSignalHandlers` (`src/cad/CADKernel.h`, the one caller of
`OSD::SetSignal`) installs OpenCASCADE's handler without losing the crash
handler: OCC's turns a signal into an exception only inside an
`OCC_CATCH_SIGNALS` scope, and anywhere else prints "no catch was found" and
calls `exit(1)`, so the handler that was there before stays for every fault
outside one. That macro is empty in code not compiled with
`OCC_CONVERT_SIGNALS`, which is all of ours, so the blend, shell, draft and
direct-edit (`directedit::Guarded`) builders run inside `occsig::RunCatchingOccSignals` (`src/cad/OccSignals.h`),
set up where the macro is real: a SIGSEGV in `BRepOffsetAPI_DraftAngle`
becomes "draft failed inside OpenCASCADE (OSD_SIGSEGV …)" (on Windows
`OSD_Exception_ACCESS_VIOLATION …`, converted by our own vectored handler:
[architecture](../architecture.md#crashes-and-stalls)). The fault
long-jumps out of the algorithm's frames, so what they held may leak; it is
the last line of defence, not a substitute for refusing the input.

## A boolean is checked before it is kept

OpenCASCADE can return a boolean result that is not null, passes
`BRepCheck_Analyzer`, and is wrong. The case that found it: an intake flange's
paired-port slot whose round ends were the same cylinders as the two branch
bores (swept round elbows), drawn 1 mm on past the pad face. Drawn from one
face the cut "removed no material"; drawn from the other it kept the bores'
floors inside the slot and stood the slot's far end in the empty bores. That
is the bores turned inside out: a section is solid where the holes are, and
the part came out 13,800 mm3 lighter than any cut could have made it.

`BooleanRefusal` (`src/core/BodyMode.{h,cpp}`) runs on the result of every
`ApplyBodyMode` add and cut and every `Boolean` feature, before anything is
kept:

- **Volume.** Not negative; a cut no larger than its input; an intersection no
  larger than either operand; a union no smaller than its input (the old
  "add LOST material" refusal). Tolerances are relative to the volumes
  compared. These read volumes the callers compute anyway.
- **Shells.** Per solid, exactly one closed shell encloses positive volume and
  every other (a void) negative. A single-shell solid is covered by the
  total's sign; otherwise the shell with the most faces is inferred from the
  total, and the rest go through the volume cache, so a void the boolean did
  not touch costs nothing.
- **Faces.** A cut or a union keeps no face strictly inside its tool, and an
  intersection none outside it. Judged at one interior point
  (`BOPTools_AlgoTools3D::PointInFace`) for every face the boolean made and
  every face of the target it kept whole that has a vertex in the tool's box,
  against the TOOL — a sound solid, and for a feature a small one.
  Classifying against the target instead (hundreds of faces) cost 8% of the
  OM615's build; this costs about 2%. Each of those made faces, and each of
  the tool's it kept whole, must also pass `BRepCheck_Analyzer` on its own: a
  cut through a thin wall that left a self-intersecting wire passed every
  other test ("left body 'Base' with an INVALID face at …").

The refusal is the feature's error, so it reaches the `regen` block of the
call that made it. It names the feature, the operation, the body, both
volumes and a point, and says what usually causes it: a tool that shares a
surface with the body. Stop the tool at the face where the surfaces meet, or
make it clearly larger or smaller than what it coincides with.

The check is not a proof. It samples one point per face, and a target face the
kernel should have split but did not is judged only if a vertex of it lies in
the tool's box.

### A refused boolean gets one fuzzy retry

Most refusals are coincident geometry, which OpenCASCADE's exact booleans
handle badly, and the model is usually reasonable: a slot exactly as wide as
the seat it opens, with its floor on the seat's axis (the cut came back
uncut), or two equal spokes whose axes cross 8 µm apart (the union counted
their overlap twice). So a refused result is not the last word.
`CheckedBooleanNamed` (`src/core/BodyMode.{h,cpp}`) runs the boolean, checks
it, and on a refusal runs it ONCE more with OCC's fuzzy option
(`SetFuzzyValue`), then checks that result the same way. It serves every path
the check runs on: `ApplyBodyMode` add and cut, and the `Boolean` feature
(union, subtract and intersect), where the retry must also pass the feature's
BRepCheck, and a result that fails BRepCheck is retried too.

- **Kept:** the feature carries a warning, so it reaches the `regen` block of
  the call that made it: the operation "needed OpenCASCADE's fuzzy option at
  0.0029 mm", followed by what the exact result got wrong.
- **Refused again:** the refusal is the exact result's, followed by
  "Retried with OpenCASCADE's fuzzy option at X mm (1e-05 of the smaller
  operand's size), and that was refused too:" and the retry's own finding.

**The tolerance** is 1e-5 of the smaller operand's bounding-box diagonal
(`FuzzyTolerance`), so it scales with the feature and stays far below any
dimension of it (0.0029 mm for a 290 mm spoke). The fraction was measured,
because both ways of getting it wrong produce a result that PASSES the check:

- too small, and the kernel still misreads the near-coincidence. The jig slot
  came out 50-70 mm3 heavy at a fuzzy value of 1e-5 to 1e-4 mm, and right
  from 1e-3 mm up;
- too large, and it merges geometry it should keep. The flange and two spokes
  came out right up to 0.01 mm and 14 mm3 light at 0.029 mm (1e-4 of their
  size).

Both operands in those cases are about 290 mm across, so 1e-5 sits inside the
window with room either way. `tests/test_boolean_volume.cpp` pins both volumes
against an independent answer: the jig's section, and the wheel with the spoke
moved 0.01 mm off the crossing, built exactly.

**The retry is non-destructive.** It goes through `cad::BuildNonDestructive`
(`src/cad/NonDestructiveBoolean.h`), which sets the fuzzy value and
`SetNonDestructive` before the build. A refused retry leaves the operands in
the document, and a destructive fuzzy build would have written what it
computed at the fuzzy tolerance onto their edges for every later feature to
inherit.
A test counts the curve representations on the operands' edges across a
retry, and checks the same count sees the exact build's writes.

A retry runs only after a refusal, so a document that builds cleanly pays
nothing for it.

## Bounding boxes are memoized on the shape

`CADShape::GetBoundingBox` is memoized on the `CADShape` itself, one slot per
flavour (conservative, tight). A slot is filled lazily and dropped by
`CADShape::SetShape`, the only thing that replaces `m_shape` (gated by
`tests/test_conventions.cpp`), so an in-place transform cannot bypass it. A
regeneration builds new shapes, which start cold.

A copy made by `Transformed` through quarter turns and a translation (every
basis entry 0 or +-1) carries the tight box, permuted: the permutation is exact,
and CAM frames every body of a job through such moves (laid on a face, flipped,
a placement turned). Any other angle starts cold. The conservative box is not
carried: it is bounded in the frame it was taken in. A job's bodies are warmed
on their SOURCE documents' bodies (`ui::JobBodies`), which outlive the job's
copies, so returning to a job tab framed by quarter turns pays nothing; the
stock and machine boxes a job tab draws every frame are memoized in
`CamTool.cpp` on the job's geometry and the setup less its operations.

A copy at any other angle (a rotary index of 30 degrees) is a new object each
time a job tab frames it, so the TIGHT box of a LOCATED shape is also
remembered beside the object, keyed on its TShape and its location's
transformation: the same TShape at the same placement is the same point set,
so a framing done again reads the box the first one paid for, and returning to
such a job costs what a quarter-turned one does
(docs/cam/gui.md#job-tab-frame-cost). `CADShape::RememberTightBox` puts a box
computed elsewhere -- by the worker that pays a project's job tabs off the
frame, on detached copies -- under a framing's identity.
`frametime/job_tab_idle_and_switch` bounds both costs
(docs/rendering/frame-pacing.md#tests).

The per-shape memo lives inside the object: a shape's address-based id is
recycled by the allocator, so an outside cache keyed on the address alone
could hand a freed shape's box to whatever lands at that address next. The
located-box memo beside it holds a handle to each TShape it keys on, as the
volume cache does, so an address it keys on is never reused while the entry
lives; it is a bounded ring (1024), and opening a project empties it.

The conservative box uses `BRepBndLib::Add` with `useTriangulation = false` and
the tight one `AddOptimal`. Neither is mesh-derived: a box from the
tessellation is inflated by the deflection, so a part's size would change the
first time anything drew it.

Per-frame callers (preview handles, the section tool's anchor and hit test,
`ui::VisibleSceneBounds`) ask for the same bounds several times a frame;
without the memo each call walks the whole B-Rep. Filling a slot writes to the
shape, so it follows the OCC lazy-cache rule: the parallel sweeps warm bounds
single-threaded before workers start.

## Hole cutters: compound when disjoint

`HoleFeature` combines its cutters into one compound when no two holes overlap
(checked on each hole's outer radius, counterbore and countersink included);
fusing them one at a time is O(n²) in the accumulated tool. If any pair
overlaps it fuses all the cutters, because a boolean against a compound whose
pieces interfere fails outright.

## Code map

| where | what |
|---|---|
| `src/core/Document.{h,cpp}` | feature list, parameters, `Regenerate`, cache, rollback |
| `src/core/Feature.h` | `Feature`, `Body`, `RegenContext`, the stored (legacy) primitives, `ImportFeature` |
| `src/core/Primitives.{h,cpp}` | the Box/Cylinder/Sphere tool: `on`, defaults, the sketch and solid it expands to |
| `src/core/features/` | one file pair per feature type |
| `src/core/DirectEdit.{h,cpp}` | what the direct edits share: resolving and fingerprinting faces, the checks, naming, Add the feature |
| `src/core/BodyMode.{h,cpp}` | new-body/add/cut, pattern instance fusing |
| `src/core/ElementMap.h`, `src/core/ElementHistory.{h,cpp}` | toponaming |
| `src/core/sketch/SketchProfile.{h,cpp}` | loops, profile faces, named prism/revolve builders |
| `src/core/AxisRef.h` | the shared axis reference |
| `src/core/io/SerializeFeatures.cpp` | feature JSON, `kFeatureTypes` |
| `src/cad/CADKernel.{h,cpp}`, `src/cad/FaceSelector.cpp` | OCC wrapper, face/edge picking |
| `src/remote/RemoteModeling.cpp` | modeling RPCs |
| `src/ui/FeatureEditor.cpp`, `src/ui/FeatureTreePanel.cpp`, `src/ui/PreviewFeature.cpp`, `src/ui/Toolbar.cpp` | editors, tree, create-with-preview, toolbar |

## RPCs

`add_feature` (JSON dialect = `feature_types`), `modify_feature` (replaces the
whole spec, keeps the id), `delete_feature`, `suppress_feature {feature_id,
suppressed}`, `set_rollback {feature_id | index}`, `set_material` and
`set_appearance` (`{feature_id | body | body_name}`, on the body's creating
feature; a Boolean, pattern or Split's body continues its source's material:
[the feature that holds a material](../rendering/materials.md#the-feature-that-holds-a-material)),
`project_geometry`, `update_derived`, `derive_occurrence`
(`src/remote/RemoteModeling.cpp`), and there too `check_literals` and
`extract_variables`
([../variables/README.md](../variables/README.md#literals-that-should-be-expressions));
`list_elements`, `curve_points`,
`tube_schedule` and `grow_faces` ([Delete Face](#delete-face))
(`src/remote/RemoteInspection.cpp`). A direct edit that would not build is
refused by `add_feature` and `modify_feature` ([Direct edit](#direct-edit)). `get_state` lists each
feature with its error, warning, `suppressed` and, for a Derived, `stale`.

## GUI

The part toolbar (`DrawModelToolbar`, `src/ui/Toolbar.cpp`) has one
`###tb_*` button per creation tool; each opens a preview with a tool window
and Accept. The feature tree's context menu has Edit, Rename,
Suppress/Unsuppress, Rollback to here / Roll to end, and, where they apply,
Reattach face… (a face-attached sketch), Pick edges… (a fillet or chamfer
with a dangling edge) and Pick faces… (a direct edit). The four direct
edits share one toolbar flyout, `direct` (`###tb_delete_face`,
`###tb_offset_face`, `###tb_move_face`, `###tb_replace_face`). An errored feature is red with its message as the
tooltip. View > Show Element Names labels the hovered face with its name.

## Random designs

`tools/fuzz.py` (the command line) and `tools/fuzz_design.py` (the generator,
the checks and the minimiser) build random designs against headless
instances and check every one from outside.

A **design** is a seeded sequence of steps chosen by `random.Random(seed)`
against the state the steps before it left (list_elements, sorted by name):
a base — a Box on Top, a sketched profile (a rectangle, a polygon, a
rectangle closed by an arc, a notched one) extruded, or a section revolved
part or all the way round, sometimes touching the axis — then up to eight of
an extrude add, cut or new body on one of the body's own planar faces, a
Box, Cylinder or Sphere on a face, a Fillet or Chamfer of one to three
edges, a Shell with one open face, a Hole (through, blind or drilled), a
Pattern of the last step (linear, circular or mirror), a Draft and a
Boolean. Placements snap to a 2.5 mm grid and sizes are fractions of the
body's own extents, so coincident faces, tangencies and seams come up as
they do in real parts. Every feature's id is pinned from its step's index,
a step names an earlier one as `{"$step": k}` and a face, an edge or a body
by where it is (`$face`, `$edge`, `$body`), resolved at run time — so a
replay, a reopen and a minimised design all name the same features.

**After every step**, from the call's own answer and one `check_geometry`:
the answer is well formed with no `nonfinite`; every failed feature states
its measured condition; every body has a finite positive volume and a solid;
the geometry is valid; the volume moves the right way (a cut, a Hole or a
Shell never adds, an add never removes, a new body adds exactly one body and
leaves the rest alone, to 1e-6 of the part's volume); and a cut stays inside
the MEASURED box (`mass_properties`) of what it cut. **At the end**: the
accepted steps replayed as one `batch` into a new part tab equal the
incremental part (every body's volume, bbox and solid count to 1e-9
relative, and the replay's own regeneration clean); undo to the start leaves
no bodies and redo to the end gives the same bodies; and a save and reopen
gives them again. **Three Boolean identities** per seed, from two primitives
made as new bodies on the grid: `V(A∪B) + V(A∩B) = V(A) + V(B)` and
`V(A−B) = V(A) − V(A∩B)` to 0.2 % (an empty intersect or subtract counts as
0).

A seed comes out `clean`; `limit` (a Fillet, Chamfer, Shell, Draft or Hole,
or an intersect or subtract whose result is empty, refused with a message:
undone, counted by its gist, and the design goes on — not a failure);
`generator` (the RPC refused a call the generator made: the generator's
bug); or a finding: `boolean_failed`, `invalid`, `wrong_way`, `identity`,
`rebuild`, `reopen`, `undo`, `message`, `internal`, `nonfinite`, `crash`,
`hang`, or `leak` (failed in a reused instance, clean in a fresh one: state
the previous seed left behind, or a kernel answer that follows shape
addresses, for which the fresh instance was only a luckier draw —
[known-bugs](../known-bugs.md#a-blend-faces-edge-names-depend-on-what-the-process-did-before)).
A message's **gist** is its class and first
60 characters less digits, and the report groups seeds by it.

    tools/fuzz.py run --from 0 --count 1000 [--jobs N] [--timeout 90] [--out DIR]
    tools/fuzz.py one SEED [--design] [--minimise] [--asan] [--keep]
    tools/fuzz.py replay CASE.jsonl
    tools/fuzz.py self-test

`run` gives each worker one headless instance (`smoke_app`'s isolation),
`new_project` between seeds, a fresh one every 50 seeds and after any
failure; a failing seed is run again fresh before it is reported. `--jobs`
comes from free memory, never the core count. It writes one JSON per seed,
the crash handler's report beside a seed that crashed, and `report.md` (the
outcome table, the gists by count, the slowest seeds) under untracked
`scratch/fuzz/`. A run longer than the 600 s tool cap goes in the
background. `one --minimise` drops steps (with the steps that name them)
while the failure keeps its class and gist, then rounds the numbers, and
writes the case: a header `{"fuzz": {seed, class, gist, generator,
design}}`, then the minimised design's `--record` session, which
`python/lobole/replay.py` plays by hand (it skips the header). A failure
that depends on timing is replayed under `tools/with-cpu-load.sh`.

A fixed case lands in `tests/fuzz/cases/<class>-<seed>.jsonl`, which
`fuzz_corpus_smoke` replays (both as recorded and with every check); one not
fixed goes to [known-bugs.md](../known-bugs.md) with its case in
`tests/fuzz/open/`, which nothing runs until it moves. `tools/fuzz.py case
RUN/<seed>.json` writes a case cut at its failing step, for a failure fixed
before it was minimised.
`fuzz_seeds_smoke` (headless suite) runs a fixed list of clean seeds, each in
an instance of its own, and fails on any finding and on `generator`; the list
belongs to one `GENERATOR_VERSION`, and changes only with the generator.
`fuzz_design.py`'s self-test, run first, feeds the checks a cut that adds, a
union that breaks the identity and a reopen with another volume, and a
failure planted at step 5 of 8 that needs step 2 must minimise to those two.

## Action matrix

Every test above builds the part it was written for, on the edges its
author chose. `tools/action_matrix.py` asks what a user asks on every part
they open — does a fillet, a chamfer, a shell, a hole, a sketch, an extrude,
a draft, a thread, a frame, a pattern, a mirror or a split work HERE, on this
face or edge — for every face, edge and body of a fixed corpus, over the
RPC, and holds each answer to a committed baseline.

**The corpus** is `tests/action_matrix/corpus.json`, one row per design
(`kind`, `source`, the `parts` a generator's design contributes), built from
what is committed and never from `scratch/`:

- `reference/<case>`: every [reference part](reference-parts.md), built with
  `reference_parts.build`; a case marked `known_failure` is skipped.
- `fuzz/<seed>`: the seeds of `fuzz_seeds_smoke`, generated as they build
  (the generator's loop without its end checks, which that smoke runs); the
  corpus names the `fuzz_generator` version, and another version is refused
  until the corpus and the baseline follow it.
- `golden/<file>`: every part document in `tests/golden/v*.json`; one that
  does not open with a clean regeneration and a body is in the baseline's
  `skipped` table with the reason, so one that stops opening shows in the
  diff.
- `generator/<name>`: `gear.py`, `gear_train.py`, `slider_crank.py`,
  `toolmakers_clamp.py` and `hinge_box/build.py`, each into a new project and
  a private `--out` directory, visiting only the part tabs that own their
  geometry (not a Derived print plate or a negative control).

The loader refuses a corpus that lacks a reference case, a seed or a golden,
or lists one that is gone. **Entities** come from `list_elements`: each face
and edge is addressed as `{feature_id: the body's created_by, element}` (an
element's `made_by` would name a feature that does not own the body). A face
the element map never named, and an edge `query_geometry` counts but
`list_elements` does not name, cannot be referenced; the baseline's
`unreferenceable` table counts them per design. A body past 400 faces or
1200 edges is not tried. Vertices are not elements, so there is no vertex
row. Bodies that share a `created_by` (a pattern's copies, a split's pieces)
cannot be named apart: they get no body actions, an element name two of them
carry is not tried, and the rest of their elements are, keyed
`<kind>@<body index>:<name>` where a name occurs twice in the part. A
feature naming an element of one of them binds to the body whose map names
it ([refused on the bodies](#refused-on-the-bodies)).

**The actions**, each default derived from what it acts on (`size` the
body's largest bbox extent, `least` its smallest, `a` a face's √area, `r0`
its `inset_mm`); an action whose default would be zero or would not apply
is not tried and not recorded:

| entity | action | feature and default |
|---|---|---|
| edge, not `smooth` | `edge.fillet`, `edge.chamfer` | the edge alone, 10 % of the least of its length, each adjacent face's `a` and its radius if circular |
| circular edge | `edge.frame` | Frame `from_element` |
| planar face | `face.sketch` | Sketch with `face_ref`, a circle at `inside_point` of radius ½ `r0`; it passes when the sketch's plane after regeneration is the face's frame (`CADShape::FacePlane`: the centroid, the outward normal, world Y projected as u, or X near Y) |
| | `face.extrude_add`, `face.extrude_cut` | that sketch, Extrude `add` / blind `cut` (reversed, into the body), 5 % of `size`, in one `batch` |
| | `face.hole` | a point sketch at `inside_point`, Hole `simple`, `through`, diameter `r0` |
| | `face.shell` | Shell opening that face, the Shell tool's default: a tenth of the body's thinnest wall (`list_elements {walls: true}`'s `thinnest_wall_mm`; its least extent where none measures), rounded down to 1, 2 or 5 × 10ⁿ; on a body past 100 faces only the four planar faces whose names hash lowest (a shell offsets the whole body: on the 195-face gear one takes 9-36 s) |
| | `face.draft` | Draft of the adjacent faces square to it (planes, and cylinders along its normal), 2°, neutral plane the world plane square to the face (a face whose normal is not a world axis has none) |
| | `face.frame` | Frame `from_element` |
| cylindrical face | `curved.thread` | cosmetic Thread with the ISO coarse designation whose major diameter is within 5 % of the face's (none: not tried) |
| | `curved.frame` | Frame `from_element` |
| any face | `face.delete` | DeleteFace of that face alone |
| planar or cylindrical face | `face.offset_out`, `face.offset_in` | OffsetFace by ± a tenth of the smaller of `r0` (a cylinder's radius) and `least`, rounded down to 1, 2 or 5 × 10ⁿ as the GUI's default is |
| planar face | `face.replace` | a `through_points` DatumPlane parallel to the face, a twentieth of `size` past the body's farthest corner along the face's normal, and ReplaceFace onto it, in one `batch` |
| a feature's faces | `face.delete_feature`, `face.move` | the face grown by `grow_faces` (Add the feature) to its hole or boss, one row per feature on its first face by name: DeleteFace of the set, and MoveFace of it a tenth of the body's largest extent (rounded) along that axis in the plane of `host_normal`. A face that does not grow plans `grow_faces` alone, which refuses |
| body | `body.pattern_linear` | Pattern of the body's newest feature (the latest that made one of its faces), 2 along X, spacing 1.5 × its X extent |
| | `body.pattern_circular`, `body.mirror` | 3 at 120° about Z; mirror about YZ |
| | `body.split` | Split at the mid-plane square to its largest extent |

The matrix copies no GUI literal (Fillet and Chamfer start at 0.5, Shell at
0.25, in `CreateActions.cpp`). On a body past 100 faces the direct edits,
like the shell, take only the four planar faces whose names hash lowest. A
direct edit passes or is refused, except where it takes away a painted face
(golden `v164-face-appearance`): the document then warns that the face
appearance names no face any more, and a new warning is a fail.

**An outcome.** Each action is one `add_feature`, or one `batch` for a
sketch and its feature, then `undo`:

- **pass**: the regeneration is clean with no warning (a cut that removed
  nothing warns), `check_geometry` is valid, `fuzz_design.check_step` holds
  (finite positive volumes; a cut, Hole, Shell or inward Offset Face never
  adds and stays in the part's box as measured before the action, an add or
  outward Offset Face never removes), and a sketch, Frame, cosmetic Thread,
  Split or Move Face leaves the part's volume as it was, to 1e-5 of it (a split's
  pieces are integrated over new faces); a warning the part already carried
  is not the action's;
- **refuse**: a structured error whose message states a condition
  (`fuzz_design.measured`), and nothing changed: the same revision for one
  call; for a batch, whose rollback is its edits and their undo, so its
  revision moves, the same features and bodies, and the next call is judged
  from the revision the rollback left;
- **fail**: accepted, and the feature is broken, warns or fails a check, or
  a refusal changed the part. A broken feature is never a refusal;
- **finding**: a crash or a hang (60 s an action; the instance is killed by
  its pid, restarted, the design rebuilt, and the run goes on), an
  `internal` error, `nonfinite` numbers, a message naming a signal the
  kernel caught (`OSD_SIGSEGV`, `SIGBUS`...) reported as an ordinary error,
  a failure message that states nothing, or an `undo` that does not give back
  the bodies (volume and bbox to 1e-9) and the feature count from before.

**The ratchet.** `tests/action_matrix/baseline.json` holds, per design (and
part, as `design#part`) and entity, its actions on one sorted line:
`edge.fillet` a pass, `!edge.fillet` a fail, `-edge.fillet` a refusal. A run
fails on a pass that became a fail or a refusal, a refusal that became a
fail, an action that passed and is no longer offered, a design that no
longer builds, and any finding the `known` table does not list — each `known`
row names its unit, entity, action and class, its
[known-bugs](../known-bugs.md) entry and its reproduction in
`tests/action_matrix/open/`; one that no longer reproduces fails the run too,
saying to remove it, its reproduction and its entry. Improvements, new
entities and entities gone are listed, not judged. A name by position can
swap with a sibling between runs, so siblings form a family, judged by how
many pass (`edge.fillet*3 !edge.fillet*1`): a piece whose `ordinal` is all
that tells it from its siblings (`<kind>:<of>[*]`), a `positional` role, and
any name holding a fallback-numbered boundary edge `;U<k>`. That number
follows the order the kernel hands a face's edges back, and so does which of
an edge's two faces names it first, both following shape addresses
([known-bugs](../known-bugs.md#a-blend-faces-edge-names-depend-on-what-the-process-did-before)),
so one edge can carry either face's name from one process to the next. Every
such name of one feature is therefore one family, `edge:<feature>/*;U*`.
The baseline is one OpenCASCADE series' (`occ`); on another the run judges
only crashes and hangs. An action that took more than 5 s is in its `slow`
table with its seconds: the sample and the determinism check skip it, and
the whole sweep gives it ten times those seconds (never under 60) before it
is a hang, so a loaded machine does not turn a 22 s extrude into one.

**Determinism.** `--determinism` takes the sample's designs at one action in
eight, builds each in two fresh instances and runs every action twice in
each (add, undo, add), the second instance in the reverse order: the entity
names, each family's outcomes and each result's bodies (volume and bbox to
1e-9) must agree. The reverse order is for an action whose answer hangs on
what ran on the part before it: a kernel call that writes the p-curves and
tolerances it computes onto the cached body it was handed changes what every
later call on that body builds. A split of random design 33 lost 5.6 mm³ on
a fresh body and none after a fillet had been tried on it, so the full sweep
recorded a pass that a replay could not repeat. Booleans, splits, blends,
drafts and shells therefore build on a copy of the body (topology only) or
non-destructively, never on the cached shape itself.

    tools/action_matrix.py run [--sample] [--shard K/N] [--design PAT]
                               [--actions a,b] [--jobs N] [--out DIR]
                               [--determinism] [--bless] [--profile]
    tools/action_matrix.py bless --from DIR
    tools/action_matrix.py replay CASE.json [--timeout S]
    tools/action_matrix.py self-test

Each worker owns one headless instance (`smoke_app`'s isolation,
`--autosave 0`), replaced every 25 designs; `--jobs` comes from free memory.
A run writes one JSONL per design (entity, action, outcome, class, message,
the calls sent, seconds) and `report.md` (rates by action and by design,
messages by gist) under untracked `scratch/action_matrix/`. Sampling is by a
stable hash of (design, action, family or entity): one in 32 for `--sample`,
which takes the reference parts, the first eight seeds and the gear. Designs
are sharded by a stable hash of their name, except a generator's: its parts
hold half the corpus's actions (the gear train's wheel alone is ten minutes
of them), so every shard builds it and takes the actions whose (part, family
or entity) hashes to it. `replay CASE` builds a reproduction's design in a
fresh instance, sends its one action and judges it (a JSONL row of a run is
a case too).

**Where the time goes.** `--profile` writes `profile.md` (and `.json`) beside
the report: seconds per action, per RPC and per design's build, and the
slowest single actions, so the next slow class is found from a measurement.
An action is its call, the checks on its answer and an `undo`, whose answer
carries the bodies' digest, the feature count and the revision: the check
after the undo is held to that, with no `get_state` (a full one per action
was a large share of the matrix's own time). A design is built in every
shard that holds its units: the builds are about 1 % of a sweep (40 s of
2,994 in the run that measured it), so they are not shared between shards.

**Blessing.** `run --bless` rewrites the baseline from a whole run and
refuses a partial one (`--sample`, `--design`, `--actions`, `--shard`);
`bless --from DIR` merges the outputs of `run --shard K/N --out DIR` for
every K. Both refuse while a finding is not in the `known` table: it wants a
fix, or a bug entry, a reproduction and a `known` row first. The `known`
table and the corpus are edited by hand; everything else in the baseline is
written by `bless`.

The tests (`tests/action_matrix_smoke.py`): `action_matrix_sample` (the
headless suite and `test-fast.sh`), `action_matrix_full_0`..`7` and
`action_matrix_determinism` (labelled `slow;action_matrix`, and registered
only for the ctest configuration `action_matrix`, as the big generators are
for theirs, so that no plain ctest runs some two CPU-hours: `tools/ctest
-C action_matrix -L action_matrix -j4`; `tools/merge-branch.sh --full`
ends with it, and so does the release checklist,
[packaging](../packaging/README.md#releasing)), and `action_matrix_selftest`, which
feeds the classifier and the ratchet planted answers — a pass, a refusal, a
refusal that moved the revision, a broken feature, a caught `OSD_SIGSEGV`, a
silent message, a hole that added, an undo that did not restore (asked
with `get_state`, and as its own digest says), two ordinal
pieces that swapped, a family that lost its pass and an unknown finding — and
must judge each.

## Tests

Headless, one file per feature (`tests/test_extrude.cpp`,
`test_twist_extrude`, `test_revolve`,
`test_datum`, `test_primitives`, `test_boolean`, `test_pattern`, `test_fillet`, `test_shell`,
`test_draft`, `test_split`, `test_refine`, `test_rib`, `test_thread`,
`test_hole`, `test_worm`, `test_curve3d`, `test_sweep`, `test_loft`, `test_pipe`,
`test_derived`, `test_toponaming`, and `test_direct_edit` for the four
[direct edits](#direct-edit), on a 40×30×20 block and on real STEP files in
`tests/fixtures/step/`, each pair equal to the file with the edit made at its
source, with `tests/direct_edit_step_smoke.py` deleting and offsetting every
face of them); the boolean check in
`tests/test_boolean_volume.cpp`, with the intake flange reduced to a block,
two swept bores and the slot, and the same case over the RPC in
`tests/regen_honesty_smoke.py`; regeneration semantics (cache stats,
suppression, rollback) in `tests/test_document.cpp`. The cache is checked
against a from-scratch rebuild in `tests/test_regen_cache.cpp`
(`cached_evaluation_matches_from_scratch`: edits, set backs, undo and redo,
removal and moves over the golden corpus, comparing bodies, element names
and every feature's derived state and error after each step) and over
generator output in `tests/regen_cache_smoke.py`. `tests/test_golden.cpp`
loads the golden corpus (`tests/golden/`), and `tests/test_determinism.cpp`
checks that every golden regenerates to the same volumes and the same element
names. GUI coverage is in `src/app/guitests/GuiTestsModeling.cpp` and the
`defaults/*` suite, which asserts every creation tool either changes the
document or refuses visibly. Random designs (above) check the same
invariants from outside, over designs nobody chose, and the action matrix
(above) every ordinary action on every face, edge and body of a fixed
corpus.

Whole parts are checked by the [reference parts](reference-parts.md)
(`tests/reference_parts.py`, `tools/ctest -L reference`): each built over
the RPC, picked by points, compared tightly against closed-form volume,
area, centroid, box and counts, and again after a STEP round trip, with a
known-failure ratchet. A feature whose result has a closed form lands with a
reference case.

## Not built

- Splitting by a sketch (sketch as knife).
- A body sub-ordinal: split pieces and pattern copies share one `createdBy`,
  and a reference finds its body by the name it carries
  ([refused on the bodies](#refused-on-the-bodies)); a name two of them
  carry is refused, not told apart.
- A thin extrude of an OPEN profile: `thin` walls every closed loop, and an
  open polyline is refused as "sketch has no closed profile", naming its
  [open ends](../sketching/README.md#open-ends).

- A thread runout (DIN 76) or a faded end on a modelled thread, and a
  blind tapped hole's thread trimmed by its drill point: the ends are square.
- Draft angle on a rib (an extrude has one: `taper_deg`); per-side rib
  thickness; multi-profile
  rib networks.
- Up-to-face against a curved face (only planar targets).
- A tapered (conical) helix; more than one loft guide; a scaling
  (`BRepFill_Contact`) guide on a sweep.
- Picking 3D points in the viewport for a fit or control curve (points are
  typed or dragged in the panel).
- Dependency-graph cache invalidation: an edit re-runs everything after it,
  related or not.
- Inserting a feature at the rollback marker.
