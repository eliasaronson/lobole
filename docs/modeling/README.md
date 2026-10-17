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
| `Import` | STEP/STL/BREP bodies; centred on their bbox unless `keep_origin` |
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
  ... is gone"), a face that no longer resolves. An Extrude or Revolve whose
  sketch failed fails too, rather than building from the sketch's last plane.

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
  (`BRepFill_ContactOnBorder`) — `ThruSections` has no guide support.
- **`Pipe`** is a circle of `outer_diameter` (hollow when `wall_thickness` >
  0) swept along a path.

All four share `SweepSectionsAlongSpine` (`SweepFeature.h`). A path or guide
that gives no spine is reported by `SweepPathError`: when the path feature is
itself broken (a bent path edited into one that cannot be bent) the consumer
names it and defers to its error, instead of claiming the path is the wrong
kind of thing.

## Sizes past the model space are refused

`kModelSpaceMm` (`src/core/ModelSpace.h`) is a kilometre: OpenCASCADE's
confusion tolerance is 1e-7 mm and a double carries about 16 digits, so past
about 1e9 mm a coordinate cannot tell two points apart and an algorithm loops
or crashes instead of failing. A Hole's diameter and depth, a linear
pattern's reach (spacing × (count − 1)) and a Shell's thickness are refused
past it, by name. A Shell is also refused at a wall of half the body's
thinnest extent or more, which leaves no cavity.

## A reshaping feature keeps only a valid solid

A Fillet, Chamfer, Shell or Draft whose builder reports success on a solid
that crosses itself is refused with `Measure::InvalidReasons` ("draft
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
(nix, the Flatpak, the Windows cross build) carries
`nix/patches/occt-shell-cap-order.patch`, which quilts the caps last; the
overlay in `nix/occt-overlay.nix` applies it, and netgen is built against
the same patched library.

The CI Ubuntu job links apt's OpenCASCADE, which is unpatched, so the Shell
also keeps its own guard. Every such answer fails one of the checks above,
so a Shell that fails them is built again on a `BRepBuilderAPI_Copy` of its
input (the same solid at new addresses, sub-shape for sub-shape, so its
index-keyed names are the body's), up to five draws, and is refused only
when every draw failed. On the patched kernel the first draw is the answer.
An honest refusal (a wall too thick for the geometry) costs the five draws.
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
`OCC_CONVERT_SIGNALS`, which is all of ours, so the blend, shell and draft
builders run inside `occsig::RunCatchingOccSignals` (`src/cad/OccSignals.h`),
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
`tube_schedule` (`src/remote/RemoteInspection.cpp`). `get_state` lists each
feature with its error, warning, `suppressed` and, for a Derived, `stale`.

## GUI

The part toolbar (`DrawModelToolbar`, `src/ui/Toolbar.cpp`) has one
`###tb_*` button per creation tool; each opens a preview with a tool window
and Accept. The feature tree's context menu has Edit, Rename,
Suppress/Unsuppress, Rollback to here / Roll to end, and, where they apply,
Reattach face… (a face-attached sketch) and Pick edges… (a fillet or chamfer
with a dangling edge). An errored feature is red with its message as the
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

## Tests

Headless, one file per feature (`tests/test_extrude.cpp`,
`test_twist_extrude`, `test_revolve`,
`test_datum`, `test_primitives`, `test_boolean`, `test_pattern`, `test_fillet`, `test_shell`,
`test_draft`, `test_split`, `test_refine`, `test_rib`, `test_thread`,
`test_hole`, `test_worm`, `test_curve3d`, `test_sweep`, `test_loft`, `test_pipe`,
`test_derived`, `test_toponaming`); the boolean check in
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
invariants from outside, over designs nobody chose.

Whole parts are checked by the [reference parts](reference-parts.md)
(`tests/reference_parts.py`, `tools/ctest -L reference`): each built over
the RPC, picked by points, compared tightly against closed-form volume,
area, centroid, box and counts, and again after a STEP round trip, with a
known-failure ratchet. A feature whose result has a closed form lands with a
reference case.

## Not built

- Splitting by a sketch (sketch as knife). (That split pieces share one
  `createdBy`, so a feature naming the second binds to the first, is a
  [known bug](../known-bugs.md#a-fillet-on-the-second-piece-of-a-split-does-not-build).)
- A thin extrude of an OPEN profile: `thin` walls every closed loop, and an
  open polyline is refused as "sketch has no closed profile".

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
