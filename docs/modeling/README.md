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
- [cam-profiles.md](cam-profiles.md) — the CamProfile feature: a disc cam
  (the mechanism) cut from a follower motion as the follower's exact
  envelope, undercut refused, and the cam relation that reads it.
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
  `CADShape::Volume` and `ComputeMassProps` are cached by TShape identity,
  because every mutating RPC digests every body's volume before and after, and
  an unchanged body — a large Derived snapshot especially — must not be
  re-measured each time. The cache holds the TShape's own, forward figures
  and carries them through a rigid placement; a shape reversed in place reads
  the volume negated, which is what an inside-out solid encloses
  ([the integrator](../inspect/README.md#the-face-by-face-integrator)).

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
| `Import` | STEP/IGES/STL/OBJ/PLY/glTF/BREP bodies; centred on their bbox unless `keep_origin`, stood up by `up_axis` ([interchange](../interchange/README.md#up-axis)) |
| `Sketch` | 2D profile on a base plane, a `DatumPlane`, another `Sketch`'s plane or a `Frame` (`planeRef`), or a body face (`face_ref`); see the sketching area |
| `Extrude` | see [Extrude](#extrude) |
| `Revolve` | profile about an `AxisRef` (`x`, `y`, `z` through the world origin, `sketch_line`, `datum_axis`, `custom {origin, direction}`); `angle_deg`, `symmetric`, `reversed`. The axis must lie in the sketch plane and the profile must not cross it; a joint within 1e-4 mm of it is ON it, so an arc may end on the axis |
| `DatumPlane` | `offset` from a plane, `angle` about an axis, or `through_points`; sketches that reference it move with it |
| `DatumAxis` | two points, a sketch line, or a global axis |
| `Frame` | a named datum frame that mates anchor to (assemblies area) |
| `Boolean` | `union`, `subtract` or `intersect` of two bodies by `createdBy`; both operands consumed into one result, so booleans chain. The result must pass BRepCheck and [the boolean check](#a-boolean-is-checked-before-it-is-kept) |
| `Pattern` | `mirror` (about a base or datum plane), `linear`, `circular` of a source; see [Patterns](#patterns) |
| `Fillet`, `Chamfer` | edges by `ElementRef`; blend faces named from the spine edge. Chamfers that meet three or more at a corner are built in stages (two at a vertex, then one more at a time), so the corner is mitred where the bevels' planes meet; OpenCASCADE's single build cuts it off flat with a triangle of its own |
| `Shell` | hollows a body, `open_faces` by `ElementRef` |
| `Draft` | tapers `faces` about a neutral plane |
| `Split` | a body by a face, a datum or an offset base plane |
| `Refine` | merges coplanar faces and boolean seams |
| `Rib` | see [Rib](#rib) |
| `Hole`, `Thread` | see [holes-and-threads.md](holes-and-threads.md) |
| `Worm`, `WormWheel` | see [worm-gears.md](worm-gears.md) |
| `CamProfile` | see [cam-profiles.md](cam-profiles.md) |
| `Curve3D`, `Sweep`, `Loft`, `Pipe` | see [3D curves, sweep, loft and pipe](#3d-curves-sweep-loft-and-pipe) |
| `Derived` | see [derived.md](derived.md) |
| `Template` | an instance of a feature template; see [templates.md](templates.md) |
| `Fastener` | a generated standard part; only in a part of its own, placed in an assembly (the parts area) |
| `DeleteFace`, `OffsetFace`, `MoveFace`, `ReplaceFace` | change a body's faces where they are; see [Direct edit](#direct-edit) |

Sheet-metal features (`BaseFlange`, `EdgeFlange`, `Hem`, `Jog`, `Fold`,
`Unfold`, `Refold`, `FlatPattern`, `CornerRelief`, `NormalCut`) and `Cope` are
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
  failed (Extrude, Revolve, Loft, Rib, Hole, Normal Cut, Jog, Fold, Base Flange,
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
  where the two touch, splitting a face and renaming it. A cut is judged by
  its faces as well as its volume: one that leaves a face inside the
  material that was there removed something, however little (a groove
  0.001 mm deep along a 60 mm block is 2e-7 of its volume), and is kept.
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
  pattern that worked. A missing source (no body and no tool) is reported
  first, so a pattern opened on a source that failed names the source.
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
  (`ResolveSweepPathWire`), with `BRepOffsetAPI_MakePipeShell`; a sharp
  corner of the path is mitred (`BRepBuilderAPI_RightCorner`: the pieces
  meet on the corner's bisecting plane), for a Loft along a path and a Pipe
  too. A rounded corner is a path with an arc in it (or a `bent_path`).
  `frame`: `corrected_frenet` (default; cannot self-twist
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
   named in that edit's `regen`. Either way the edit is built once: a
   refusal by the copy's own regeneration alone, and an edit that builds is
   not built again when the call commits it, the part taking the copy's
   evaluation into its regen cache (`Document::AdoptEvaluations`: the chain
   hashes are the same whichever document computed them).
   `direct_edit_step_smoke` holds both.
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
   the editor drops the old print. Recording a print changes what the
   edit is keyed on in the regen cache, which re-keys it
   ([architecture](../architecture.md#the-document-model-and-regeneration))
   rather than building it again at the next regeneration. A primitive's
   `<id>/face<i>` is
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
the feature that made them. The check reads each neighbour once and only its
surface's type: describing every neighbour in full across every shared edge
held the main thread a minute on a refined STL's 916-face Delete Face (the
facets carry no p-curves, so each description projected every edge of a
many-sided neighbour), before the heal began. Nothing else before or after
the builder walks the faces per face: `RestoreUntouched` indexes them once.

Every direct-edit builder (Defeaturing, the offset builder, MakerVolume,
and Move Face's booleans and those that join Replace Face's cells to the
body, through `topo::BooleanNamed`'s progress range) also
runs under a deadline, `directedit::BuilderBudgetS` (30 s and 1.5 s a
face), through a `Message_ProgressIndicator` whose `UserBreak` turns true
once it has passed: past it the edit is refused ("gave up after 30 s …").
The builders check it only at their progress points, so a call can run
past the budget before it stops. It is a backstop against a frozen app, far
past any edit that builds, not a timer whose answer could depend on the
machine's load. Move Face separates the feature it moves with booleans of the
body and the body less its faces, two solids whose every face but the moved
ones coincide; on random design 33's chamfered, drilled body the kernel's
edge-face intersections spent 68 s on one bore before they answered, and
the deadline now refuses it at 30.

**Off the frame.** Delete Face is a heavy feature: past an estimate of a
quarter of a second (15 ms a face deleted, 17 faces) it builds as a
`feature` job while the frame keeps drawing, its RPC answering when it
lands, and a cancel rolls the edit back or leaves the feature not built
([heavy features](../ui/long-operations.md#heavy-features)). Over the RPC
such an edit is tried on a copy as a light one is, but in that job, before
anything is committed, so its refusal moves nothing either; inside a
`batch` it is committed and built, and one whose build fails is refused
when it lands, the batch rolled back.

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

All 100 bores of SolidCraft's `plate_100_holes.step` delete in one feature,
nearly all of it Defeaturing, on one thread: 17 s of CPU on a P-core of the
development machine, 33 s on an E-core, 57 s on a low-power E-core, and
93 s there with Ubuntu 24.04's OpenCASCADE 7.6. The test allows 180 s of
CPU, which holds on the slowest core and, unlike the wall clock, does not
grow with what else the machine is running; the edit's own budget is 30 s
and 1.5 s a face, 180 s for these 100.

### Offset Face

`OffsetFace {body_id, faces[], distance_mm (+_expr)}` moves each face along
its own normal, positive outward (adding material); its neighbours follow
along their own surfaces, so a drafted wall stays drafted and a cylinder
changes radius. Each face is put onto its own surface offset by the distance
-- a plane moved, a cylinder of another radius (`Geom_OffsetSurface`, its
canonical surface where it has one) -- by Replace Face's cells
(`directedit::ReplaceOnto`, [below](#replace-face)), one face at a time,
its neighbours extended or trimmed to meet it; a face a later step trims
again keeps one `;M<id>`. They are built from the face's neighbourhood, so
an offset costs about the same on a 291-face import as on a block.

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

Checks: outward never removes volume, inward never adds; a face moved
through Replace Face's cells changes the volume by between a third and
three times its area times the distance (cells that kept only part of the
volume between the face and its target, or filled a dent beside it, pass
every other check); the bbox grows by at most the distance; an inward offset through the body is refused
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
`directedit::ReplaceOnto` builds the change, not the body. It hands
`BOPAlgo_MakerVolume`:

- the replaced face;
- its neighbours extended past the target (`BRepLib::ExtendFace`, across
  only the UV sides the edges they share with the replaced face lie on:
  grown every way, a cradle's end wall ran on through the bottom of the
  part). A plane is grown four times as far as the target lies from the
  face's boundary and no further: grown by the body's diagonal, a chamfer
  beside the face sliced the volume between the face and its target into
  pieces. A curved neighbour is grown by the body's bbox diagonal: a
  sphere's dent grown only that far closes nothing near its poles;
- the target surface bounded to twice that size: a plane as a square
  centred on the body, a cylinder, cone or sphere all the way round, any
  other surface `ExtendFace`d.

Nothing else of the body goes in, so what it costs depends on the face's
neighbourhood and not on the body: built from every face of the body, an
Offset Face on the SO-101 wrist bracket's 291 faces took 25 s, and the
target, as large as the body, ran on through every other wall that crosses
its plane and cut it into cells, so the edit was refused for "separate
pieces" it never made. The volume between the old face and the new one is
the cells that hold pieces of both, read by the pieces' outward normals in
the cell against the face's own in the body: a cell whose face pieces face
the way the face does (it lies on the inner side) and whose target pieces
face back is material to take away, its mirror material to add, and a cell
that holds both and is neither (the rest of a revolved body's inside,
closed by a target that runs all the way round) is left out. No point is
classified, which an imported face's tolerance would blur, and the point
on a piece is the kernel's own (`BOPTools_AlgoTools3D::PointInFace`): a
parameter box misses a shelled box's thin rim. The body is fused with the one set
and cut by the other (`topo::BooleanNamed`, under the edit's one deadline).
A neighbour and the strip the fuse adds to it lie on one surface; they are
merged back into one face across the replaced face's old edges only, and
where a target crossed the face (a tilt) the half the fuse added and the
half the cut uncovered are merged across the line it crossed on
(`ShapeUpgrade_UnifySameDomain` through the boolean's refine hook, every
edge and vertex of a face with no such edge kept, so two coplanar faces an
import carries side by side stay two, and a face moved flush with a plateau
beside it stays its own face). A merge the kernel returns invalid is
dropped and the strips stay, untidy and not wrong (a ball knob's bore).

The new face is `<replaced>;M<id>`, each neighbour `<name>;M<id>`, and the
edge where a neighbour meets the new face `<name>;G<id>`, as a builder's
history names an edge a face generates.
Checks: every untouched face keeps its name (one the change cuts is refused
naming it); the target is there (a target the extended neighbours do not
reach is refused naming them: a floor raised past the top of the wall
beside it); the bbox grows by at most the diagonal. The volume may go either
way, or not move at all (a tilt about the face's middle). The target slot
(`###slot` `ReplaceFaceTarget`) takes a face in the viewport, or a datum
plane's or a sketch's row in the tree; there is no numeric default.

## Cradle

A **Cradle** cuts a holder shaped to a scan out of a block: the block less
the scan grown by a clearance and swept along the direction the object is
lowered in, so it lifts in and out (`src/core/features/CradleFeature.{h,cpp}`,
the pocket in `src/core/mesh/Field.{h,cpp}`). Where a 2.5-D part will do --
a stand, an outline-shaped pocket, a rim -- the scan's outline drawn into a
sketch and offset by the clearance is the parametric route
([a section into a sketch](../sketching/README.md#a-section-into-a-sketch));
to redraw the made part itself, its section traced as the lines and arcs it
is made of ([tracing a section](../sketching/README.md#tracing-a-section)),
with the relations and round sizes the scan supports accepted onto it
([relations the scan supports](../sketching/README.md#relations-the-scan-supports)).

`Cradle {target_id, tool_id, clearance_mm?, direction?, sweep?,
resolution_mm?}`:

- `target_id` is the block (a B-Rep or a closed mesh body), `tool_id` the
  scan: a mesh body, open or closed, or a B-Rep, tessellated at a thousandth
  of its own diagonal or half a cell, whichever is finer.
- `clearance_mm` is the gap a side between the scan and the pocket wall.
  Unset, it is the part's print process's **gap** a side
  ([printed parts](../tolerances/README.md#printed-parts)): what keeps two
  surfaces from closing on each other once printed (PETG 0.3, PLA 0.25).
  With neither it is refused, naming `set_print_process` and
  `clearance_mm`: a literal would be wrong for any real printer.
- `direction` is the way the object moves INTO the cradle, `[0, 0, -1]`
  (world down) unless set; any non-zero vector.
- `sweep: false` keeps the undercuts, for a snap-in grip.
- `resolution_mm` is the grid's cell, half the clearance unless set.

The block becomes a **mesh body** (the same body: its producer, name and
material; its one element `<cradle id>/mesh`); the scan stays. The reply's
`report` gives `clearance_mm` and `clearance_from`, `resolution_mm`, the
`deviation_mm` that implies (half the cell's diagonal, plus a B-Rep scan's
tessellation), the cells and the result's triangles.

**How the pocket is built.** In a distance field, and only the last step
is a mesh boolean (manifold):

1. a grid whose z points against `direction` covers the block the scan can
   reach: the scan's box grown by the clearance and two cells, and with the
   sweep on up through the block's top, cut to the block's box and two
   cells past it, so the caps the level set closes the pocket with lie
   outside the block;
2. at each node, the distance to the scan by `MeshBvh` within a band (the
   clearance and two cells), its sign from the generalised winding number,
   which an open scan (no bottom) still answers. On a closed scan the sign
   carries down a column between two nodes both past the band, which cannot
   have crossed the surface; an open one asks at every node;
3. the sweep is a running minimum up each column, f_swept(p) = min over
   t >= 0 of f(p + t·dir), exact at the nodes because the grid is aligned
   with `direction`;
4. manifold's `LevelSet` contours `clearance - f`, trilinearly sampled, on
   one thread (slabs of it in parallel measured slower, below);
5. manifold subtracts the pocket from the block, which must be closed.

Why a field: the clearance is an iso-level and the sweep a running
minimum, both trivial there, where on a mesh an offset is a Minkowski sum
that on non-convex input "scales with the product of face counts"
(manifold's own source). A field also takes an open or dirty scan, which a
mesh boolean refuses (manifold: `NotManifold`); the subtraction, which must
be exact and watertight, is the one step left to manifold.

The field runs under `parallel::For`; manifold itself is built serial. A
Cradle is a heavy feature
([long operations](../ui/long-operations.md#heavy-features)): it builds as
a `feature` job, reports its progress, and is cancelled from the field's
rows, the level set's samples and between the steps.

### What a cradle costs

Measured on the figure fixture (`tests/test_mesh_cradle.cpp`, hidden
cases `[mesh-cost-cradle-field]` and `[mesh-cost-cradle-feature]`), on the
development machine:

| piece | measured | law (`CradleFeature.cpp`) |
|---|---|---|
| the field (distance, sign, sweep) | 0.68 s wall for 7.5M nodes on 22 threads | 2 µs of CPU a node, shared by the workers |
| `LevelSet`, one thread | 4.6 s at 7.5M nodes, 26 s at 30M | 0.9 µs a node |
| the scan's tree, the subtraction | 1.5 s for a 0.3 mm pocket's 785k triangles | 2.4 µs a triangle of scan, block and pocket (two a cell of the scan's surface) |

The field is cheap because the sign carries down a column on a closed
scan; the level set is nearly all of it. Running the level set in slabs on
every core was measured and dropped: six slabs took 6.1 s against one's
4.5 s at 7.5M nodes, 3.3 s of it the union of the seams.

**The budget** a Cradle is refused past is the patient wait an import is
held to, `interchange::kPatientImportSeconds` (60 s), and 2 GB for the
level set's voxels, which manifold pads to a power of two on each axis
(`mesh::PocketBytes` counts them exactly). On a 100 mm scan the memory
binds first: the figure's stand at PLA's 0.25 mm clearance asks for a
0.125 mm cell, is coarsened to 0.185 mm (30M cells, a deviation of
0.16 mm), and builds in 31 s at 300k triangles and 33 s at 2M, the process
peaking at 2.1 GB. Past the budget:

- a stated `resolution_mm` is refused, naming the coarsest cell that fits;
- the default cell is coarsened in 5% steps until it fits, and refused once
  that would be coarser than the clearance;
- a scan whose triangles alone are past it is refused naming the count to
  decimate to (MeshLab's Quadric Edge Collapse Decimation, Blender's
  Decimate modifier); a smaller block also shrinks the grid.

The cost check (`[slow]`) holds a 300k-triangle stand at its default
resolution to the law's own estimate in CPU time, and fails against a 5 µs
spin planted in the level set's samples.

### After a cradle

Once a body is a mesh, a B-Rep feature on it is refused, naming the feature
that made it one (`Body::meshSince`, read by `MeshBodyRefusal`): "Fillet
needs a B-Rep, and body 'Blank' is a mesh (… triangles) since Cradle
'Holder': move this before it". So a holder is finished in that order: the
B-Rep work first (fillets, chamfers, bosses), then the cradle, then any
[mesh boolean](#mesh-booleans) for screw holes and slots.

### Mesh booleans

A `Boolean` with a mesh operand runs on triangles through manifold
(`BooleanFeature::ExecuteMesh`): both operands must be closed. An open one
is refused naming its boundary loops and the longest, and pointing at
MeshLab, Blender or the scanning app to close it, or at a Cradle, which
takes an open scan. A B-Rep operand is tessellated at a thousandth of its
diagonal, which the reply's `report` states (`target_deflection_mm`,
`tool_deflection_mm`). The result is a new mesh body, as a B-Rep boolean
makes a new body.

How the cradle is checked: `tests/test_mesh_cradle.cpp` on a sphere of
radius 20 in a block, by what it is for -- the pocket wall at r + c within
the reported deviation, the swept opening 2(r + c) wide above the equator
and the unswept one closing over it, the result `Closed()`, and the sphere
lowered in along -Z clashing with nothing only when swept -- and on the
cut figure, an open scan that cradles where a mesh Boolean refuses it by
name; the clearance from the print process and its refusal without one,
each bad input refused naming what is accepted, the budget's refusal
naming the resolution that fits, a mesh Boolean putting a B-Rep screw hole
into a cradle; the goldens `v211-cradle` and `v211-mesh-boolean`;
`refused_calls_smoke.py` (a refused Cradle changes nothing); the GUI tests
`cradle/a_block_cut_to_a_ball_off_the_frame` (the toolbar's defaults, one
undo step, built as a job, undone) and `defaults/cradle`.

## Fitting a primitive to a mesh

`fit_primitive {feature_id, point_mm, kind?, tolerance_mm?, clip_sigma?,
side?, radius_mm?, axis?, axis_tolerance_deg?, within?, part?}`
(`src/core/MeshFit.{h,cpp}`, the region in `src/core/mesh/Grow.cpp`, the
fit in `src/core/mesh/Fit.cpp`) fits a plane, cylinder, cone, sphere or
[torus](#a-torus) to the triangles round a point on the bodies feature `feature_id` made: a
scan's mesh body, or a B-Rep's tessellation (a thousandth of its
diagonal). It changes nothing; it is what the [datums](#datums-on-a-scan)
below refit, and what the Fit tool previews.

1. The point picks the nearest triangle of the nearest body; a point
   farther than a fiftieth of the bodies' diagonal from every triangle is
   refused as a mistake in the coordinates.
2. **The tolerance** a vertex may stand off the surface is the scan's own
   noise unless `tolerance_mm` is given (`mesh::ScanTolerance`): over the
   mesh, how far the far corner of the triangle across each edge stands
   off a triangle's plane, at the median, times 3 × 1.4826 (a robust three
   sigma), leaving out creases (an edge turning past a pentagon's 72°: a
   feature, not noise) and edges flat to four quanta of the file's
   precision (a CAD export's flats, which would put its curved facets'
   bend below the median), never less than those four quanta. On a scan
   with ±0.05 mm of noise that is about 0.18 mm; on a clean CAD export it
   is the facets' own bend.
3. **The region stops at the edge** (`mesh::GrowFrom`). Where a face
   rounds into its neighbour, the first degrees of the round lie within
   the tolerance of the face and face its way, and all of them stand to
   one side of it: a region grown by distance alone leans the fit toward
   them. So:
   - a disk of rings round the seed, grown until a curved kind's
     curvature stands above the noise (the disk's best plane misses a
     vertex by three tolerances), is fitted robustly;
   - **the normal band** is the scan's own: the disk's triangles' median
     tilt from that fit, times 3 × 1.4826, never less than the tilt the
     tolerance gives a triangle of the mesh's median edge
     (atan(tolerance / edge)). A triangle so thin that the file's
     precision cannot fix its normal that closely is allowed what the
     precision gives its least height;
   - the region grows across edges, taking a triangle when every corner
     lies within the tolerance of the surface, its normal is inside the
     band, and it faces the seed's side. It is **regrown from the seed**
     under each new fit, each regrowth capped at three times the
     triangles of the one before, so a model that is wrong early is
     refitted before it can take a neighbour, until two regrowths in a
     row stop short of their cap;
   - **the rim is peeled**: the region's outer ring of triangles is
     removed, ring by ring, while that ring's median tilt from the fit is
     more than twice the region's own (and more than what the file's
     precision tilts a triangle of the median edge, so a clean export's
     flats are not peeled for leaning by nothing), never below half the
     region's area; then it is fitted again. The band is not changed by
     this: it is after the growth.
4. **The fit is robust** (`mesh::FitRobust`, RANSAC): hypotheses are drawn
   from the region's triangles, each its centre and unit normal -- a plane
   from three, a sphere from two normal lines' closest approach, a
   cylinder from two normals (their cross product is the axis), a cone
   from three tangent planes (their meeting point is the apex), a torus as
   [below](#a-torus) -- and
   scored by the vertices within the tolerance; a tie goes to the one the
   triangles' normals agree with (a box's face beats the diagonal plane
   through the same four corners). Only a surface through the seed
   triangle's centre is taken. The best is refitted by least squares over
   its inliers until they stop changing. The samples come from splitmix64
   seeded with the points' own bits, so the same triangles give the same
   surface to the bit on every regeneration and every platform.
5. **The used points are the surface's** (the clip): vertices of the
   region past `clip_sigma` sigma of the residuals are left out and the
   fit made by least squares over the rest (`mesh::FitLeastSquares`),
   until the set stops changing (at most 20 rounds). The sigma is the
   robust one over every vertex of the region (1.4826 × the median
   absolute residual), not the RMS of what the last cut kept: that one
   shrinks with every cut, and a one-sigma clip would eat the region.
   `clip_sigma` is `3` by default and takes `1`, `2`, `3` or `"all"` (no
   clip: the robust fit as it is); anything else is refused, naming
   them. `mesh::FitTriangles` runs the robust fit and this on a set of
   triangles given rather than grown.
6. A curved kind whose region is flat to the tolerance is refused, naming
   the plane: the radius it gave would be the noise's.
7. **What one surface does not explain is said**
   ([two surfaces](#two-surfaces)): a plane or cylinder region that is two
   surfaces is refused naming both, and a plane along the crown of a round
   naming the round.

`kind` is `plane | cylinder | cone | sphere | torus | auto`; any other is
refused, naming them. `auto` grows each and keeps the one covering the
most area, or a simpler one (plane, sphere, cylinder, cone, torus, in that
order) covering at least 95% as much (`meshfit::kAutoShare`) -- a cone
fits a cylinder's triangles as well as the cylinder does, with a half
angle of nearly nothing, and a torus a sphere's with a core of nothing, so
the torus is taken only where nothing simpler covers nearly as much -- and
the reply's `tried` lists each with its area or why it failed.

**The reply**: a plane's `origin_mm`, `normal` (the way the triangles
face) and `extent_mm` (the used points' spans along their two principal
directions, the longer first); a cylinder's `origin_mm` on the axis,
`axis` (its largest component positive: a cylinder's axis has no sense of
its own, and a refit must not flip it), `radius_mm`, `length_mm` and
`arc_deg` (the used points' span along and about the axis); a cone's
`apex_mm`, `axis` (from the apex into the cone) and `half_angle_deg`; a
sphere's `centre_mm`, `radius_mm` and `arc_deg` (the cap the used points
span, seen from the centre); a torus's `centre_mm`, `axis` (largest
component positive, as a cylinder's), `major_radius_mm` (its core
circle's), `minor_radius_mm` (the tube's) and `concave` (the triangles
face the core circle: a round at a boss's foot, not a ring). A cylinder and a sphere also carry
`diameter_min_mm` and `diameter_max_mm`: the fitted size moved to the
lowest and the highest radial residual of the used points, the sizes a
caliper could read. Then two populations: `rms_mm` and `max_mm` over every
vertex of the region, and `sigma_mm` (RMS), `form_mm` (peak to peak) and
`used` (count) over the points the clip kept, with `clip_sigma` -- both,
because a sigma taken over what a cut kept is smaller than the noise.
`warnings` says when a cylinder or sphere covers under 60° of arc
(`meshfit::kPartialArcDeg`): a partial arc is where a radius is least
certain. And `tolerance_mm`, `triangles`, `area_mm2`, `body`,
`seed_distance_mm`, and with hints `seeds_tried`.

### A torus

A round turned about an axis is a torus: a fillet round a boss's foot, the
corner where a round along an edge turns round a rounded corner, a ring.
Every normal of a torus meets its core circle a tube's radius in, and its
axis. So its hypotheses are not drawn from minimal samples
(`mesh::FitRobust`, `TorusHypotheses` in `src/core/mesh/Fit.cpp`):

- **one per tube radius**, on a log scale (steps of 12%) from the
  tolerance to twice the points' span, either sign -- a convex tube and a
  concave round: the region's oriented samples moved along their normals by
  it lie on the core circle when it is right, and a plane and a circle
  through the moved points (twice more through those within three times
  their median distance of it) give the axis, the centre and the core's
  radius; the tube's is then the points' median distance from that circle;
- **the axis the normal lines meet** (`TorusFromNormals`): least squares
  over every normal line, a line's moment eliminated, and the circle the
  points make in the half-plane through it;
- **the osculating fan**, for the first disk a region grows from, too small
  for its third order: the curvatures and their directions, read from how
  the normals turn across it (on a circle a normal's lean is exactly its
  distance along over the radius, where a parabola through the heights
  reads an arc's curvature high), fix a torus up to the tilt of its axis in
  the plane of the normal and the tube's direction; eleven tilts, 15° apart,
  either curvature taken as the tube's;
- **the fit the region was regrown under** (`FitRobust`'s `also`): on a
  fine scan a facet's normal is too noisy for the first two once the region
  is wider than the fan's disk.

Each is scored as the other kinds' are, the six best refined by least
squares over the points they keep and scored again, and the best refined
on its seven numbers (centre, axis, the two radii) as every kind's is.
`concave` is which way the seed's triangle faces the fit.

**The rim of a round.** A round is narrow, so the faces it is tangent to
are a large share of what it first grows over: their first millimetre lies
within the tolerance of it and inside the normal band of a noisy scan, and
the fit leans to them (a 3 mm round between two faces read 3.4 after one
peel). A torus's rim is peeled again under each fit made without the last
rim, until no ring leans, and a ring is peeled also while its triangles
*lag* behind the normal as it turns round the tube away from the region's
middle, on average, by more than an eighth of the region's own tilt: a
tangent face's strip keeps its normal where the round's goes on turning,
where noise lags as often one way as the other.

**Hints**: `side` (`outside` a ring, `inside` a concave round) and `axis`
apply; `radius_mm` is refused (a torus has two radii). A torus region is
not split as [two surfaces](#two-surfaces) are, but it is what two
cylinders side by side are judged against: where the two halves' axes lean
apart by more than a degree (`mesh::kBentDeg`; a step's agree to a tenth),
a torus over the region -- started also from the core those two axes are
tangents of -- is the one smooth surface, and a bent tube read as two
cylinders is not refused as two. A plane on the crown of a round names
whichever round auto would take there: a cylinder, a sphere or a torus.

How it is checked (`tests/test_mesh_fit_scan.cpp`, tilted off the axes, ±0.05
mm of noise along the normal): a scanned ring of core 20 and tube 5 at a
0.5 mm grid: 19.9999 and 4.9996, its axis 0.001° and its centre 0.0004 mm
off, convex, the whole ring grown; `auto` takes it. A concave round of 3
mm at the foot of an R8 boss on a plate (core 11): 10.996 and 2.993, axis
0.01° and centre 0.006 mm off, `concave`, 92% of the round's area and none
of the faces either side past the noise; `auto` takes it, the plate a
plane and the boss its R8 cylinder; a plane asked of the round is refused
naming the torus at the scan's tolerance and at 0.6 mm, never "two
planes". The stepped bore under `auto` at 0.6 mm is still refused as two
cylinders, and the post's side is its cylinder with the torus tried.

### Two surfaces

A tolerance wide enough to take a step, a lip or two faces a few tenths
apart grows one region over both, and one fit over them is neither: a
bore stepping from R4.68 to R5.18 fits R4.93 to 0.25 mm, inside a
0.6 mm tolerance. So the region is judged (`mesh::SplitTwo`,
`src/core/mesh/Split.{h,cpp}`, run by `GrowFrom`; `GrowOptions::judge`,
which `align_import`'s floor turns off):

- **When**: a plane or cylinder whose `rms_mm` over the region is past a
  third of the scan's own noise tolerance (`mesh::kSplitAbove`; that
  tolerance is about six sigma of a vertex's noise, so this is twice the
  noise's sigma) -- the scan's, not a `tolerance_mm` given wider, which
  is what lets the region take both. A region the noise explains costs
  nothing more.
- **The split**: up to 4,096 of the region's vertices are put in two by
  distance to two surfaces of the kind, from several starts (the
  residual's sign, and the region cut across at its quartiles: along the
  axis, or along both of a plane's principal directions), each refitted
  and the points reassigned to the nearer until nothing moves; the start
  with the least squared distance is kept, and each surface refitted over
  its own points within three robust sigma (a shoulder's vertices belong
  to neither).
- **Kept only if** each holds at least 15% of the vertices
  (`kSplitShare`), they stand more than four times the larger one's
  robust sigma apart (`kSplitApart`; the median distance of each's points
  from the other), and one smooth surface does not explain the region
  within 1.5 times as well (`kSplitSmooth`): for planes a quadric of the
  height over the fitted plane, for cylinders a radius varying evenly
  along the axis (a line in the distance along it). A crowned plate and a
  taper pass the first two and fail the third.
- **Then refused, naming both**, the sentence a `from_fit` datum fails
  with too: "two cylinders here: R4.68 mm and R5.18 mm, stepping at 12.4
  mm along the axis [0.2326, 0.9315, -0.2796] from [-3.996, 5.999, 2.506];
  click one, or give radius_mm" (the step where the fewest points of
  either stand on the wrong side of a cut along the axis, measured from
  the region's end, on the axis the two agree on -- the one fit across
  both leans 0.85° on that bore; "side by side" when no such cut exists),
  or "two planes here: 0.30 mm apart and 0.002 deg to each other, through
  [..] and [..]; click one, or give within (a box round the one you
  mean)" (`axis` for two planes more than a degree apart).
- **A hint that meets one of them alone** (`side`, `radius_mm`, `axis`)
  says which was meant: that one's triangles are fitted (`FitTriangles`),
  and the fit is its. `within` needs no split: it keeps the region to one.
- **Under `auto`** a kind refused as two surfaces stops auto taking a
  more general kind over the same triangles (a cone fits a stepped bore
  as well as anything does).

**A plane on a crown.** A plane whose normals turn more than 10° across
its region (`kCrownSpreadDeg`, read from the quadric through its
vertices, so a scan's noisy facets do not count), or that covers under
15% of its own bounding rectangle (`kCrownCover`) while its rms is past
the noise as above, is a strip along the top of a round, the width the
tolerance lets a plane reach across it. It is refused naming what fits
there, grown from the same seed: "the plane fitted here is a strip along
the crown of a round: its normals turn 22.7 deg across it (a flat's stay
within 10); a cylinder fits there: R12 mm, axis [..] through [..] -- fit
kind cylinder" (or a sphere, or neither, with why). Under `auto` the plane
candidate fails with that and the round is taken. The second test needs
the bow as well because a washer's face is a flat ring that covers less
than that of its square.

How it is checked (`tests/test_mesh_fit_scan.cpp`, the fixtures tilted
off the axes): a bore of R4.68 for 12.4 mm and R5.18 after, ±0.05 mm of
noise, the step joined by the row of triangles across it: at the scan's
own tolerance a click fits the cylinder clicked (R4.68); at 0.6 mm it is
refused naming both, the radii within 0.01 mm and the step within 0.2 mm
of the generated ones (apart 13 times their scatter, the even radius 4.5
times the two cylinders' rms), and `radius_mm [5, 5.4]` fits R5.18; a 40
x 30 plate with a 0.3 mm step: refused, 0.297 mm apart, each plane
through its own part (apart 7.8 times, the quadric 2.7); a bore tapering
0.4 mm over 25 and a plate crowned 0.4 mm over 40 (its normals turning
4.6°) are examined and not split -- both pass the share and the
apartness (4.5 and 8 times their scatter) and fail only the smooth
surface, which explains them better than the two do (0.64 and 0.69 of
their rms); a plane
asked of the post's side is refused naming its R12 cylinder, its top
still a plane. `tests/test_datum_from_fit.cpp`: a from_fit DatumAxis on
the stepped bore fails with the sentence. `refused_calls_smoke.py`: a
stepped shaft's two cylinders (as a cylinder and under auto), the 0.3 mm
step on its top and a plane on its side, each refused changing nothing.

### Hints

Near a rim the nearest surface to a point is often the wrong one: the
outside of a cup for its inside, a plating thickness away. All optional:

- `side`: `inside` (a bore, a socket: the triangles face the axis or the
  centre) or `outside` (a boss, a ball), for a curved kind;
- `radius_mm: [min, max]`, for a cylinder or a sphere;
- `axis: [x, y, z]` with `axis_tolerance_deg` (both or neither, the
  tolerance above 0 and at most 90): a plane's normal, a cylinder's or a
  cone's axis, either sense;
- `within: {min_mm, max_mm}`: a box the region stays in (every corner of
  every triangle, the disk's too); the point must be inside it.

A hypothesis outside a hint is not drawn (`FitRobust`'s `admit`); the
least squares that refines the best is not held to it, so a fit that ends
outside one comes back as the surface has it and is refused saying which
("the cylinder fitted here has radius 12 mm, outside radius_mm [20, 30]").
With any hint, the triangles within the pick's own reach (a fiftieth of
the diagonal) of the point are tried as seeds, nearest first, one a
surface -- a triangle turned less than a crease from one already tried, or
in a region already grown, is the same surface -- at most eight
(`meshfit::kMostSeeds`); the reply's `seeds_tried` says how many. Under
`auto`, only the kinds every hint applies to are tried; a hint the given
kind cannot take (`side` on a plane, `radius_mm` on a plane or a cone,
`axis` on a sphere) is refused before anything is read. A datum stores its
hints and `clip_sigma` in its `fit`, so a refit is steered and clipped the
same way.

**How well it fits** (`tests/test_mesh_fit_scan.cpp`; every fixture
tilted off the axes and every number checked against its generator's):

- a turned post scanned at 60k triangles with ±0.05 mm of noise along the
  normal and a 1.5 mm lump on its side: the cylinder's radius 12.002
  against 12, its axis 0.001° and 0.005 mm off the generated one; the
  cone's half angle 33.690° against 33.690°, its apex 0.002 mm off; the
  top all but a few of its own triangles (the smallest round the pole
  lean past the band) and nothing of the cone; a noisy sphere's radius
  39.9999 against 40. Least squares over the side with the lump in it puts
  the axis 0.059 mm off; the robust fit 0.008 mm. Clipped at 3, 2 and 1
  sigma the cylinder keeps 99%, 98% and 73% of its vertices, the radius
  within 0.002 mm each time;
- **the edge**: a block with 0.5 mm rounds on every edge, ±0.05 mm of
  noise, its face clicked 2 mm from an edge: the plane 0.0006° and
  0.002 mm off the generated face; the growth by distance alone, on the
  same mesh and click, 0.006° and 0.007 mm;
- **structured outliers**: the post's side given 30% of the base beside
  it: radius 12.002, axis 0.006 mm off; 140° of a cylinder's arc: radius
  12.001, `arc_deg` 140.1; 12° of an R150 bore: radius 150.04 and the
  partial-arc warning;
- **hints** (`tests/test_datum_from_fit.cpp`): a cup clicked on its rim
  between walls 1.2 mm apart fits the 13.8 mm bore with `side: inside` and
  the 15 mm outside with `outside` (to 0.0002 mm), a radius or axis it
  cannot meet refused naming it.

**What it costs** (one thread): a region is linear in its triangles, the
fits capped at 65,536 points and the hypotheses scored on 4,096. A noisy
sphere grown whole takes 12 ms at 10k triangles, 0.24 s at 300k and 1.3 s
at 2M (hidden `[fit-cost]`); against the growth by distance alone that
came before the band, the peel and the clip, measured in the same run
(`[slow]`, CPU time, held to twice): 1.2 times at 300k and at 2M. Where
the [split](#two-surfaces) runs, a stepped bore of 240k triangles grown
over both its cylinders and refused: 1.3 times. A scanned ring grown
whole as a torus takes 0.11 s at 10k triangles, 0.37 s at 160k and 1.0 s
at 1M, and `auto` there (every kind grown) 0.27, 1.0 and 3.0 s. The
noise tolerance is a stride through at most 200,000 edges, under 20 ms at
any size. A datum refits this on every regeneration that reaches it.

### Datums on a scan

A scan has no faces to sketch on, mate to or measure from, so it takes part
through datums fitted to it. A `DatumPlane` or `DatumAxis` of `kind:
"from_fit"` stores `fit {body_id, point_mm, kind, tolerance_mm?,
clip_sigma?, side?, radius_mm?, axis?, axis_tolerance_deg?, within?}` --
the feature whose body it fits, the seed point, the kind, and the
tolerance, the clip and the [hints](#hints) where they were given (only
those are written) -- and refits on every regeneration, so the datum follows a
re-scan: replace the import's file with a new scan standing in the same
place (aligned the same way) and the datum moves onto it.

- A DatumPlane fits a `plane`: its origin is the seed projected onto it,
  its normal the way the triangles face, its in-plane X the world's X laid
  into it (the world's Y when the normal is near X).
- A DatumAxis fits a `cylinder` (origin the axis point nearest the seed,
  sense with its largest component positive), a `cone` (origin the
  apex, from the apex into the cone) or a `torus` (origin its centre,
  sense as a cylinder's).
- The kind is stored, never `auto`, so a re-scan cannot turn a datum into
  another kind; a kind the datum does not fit is refused on load, naming
  the ones it does, and so is a hint its kind cannot take or a
  `clip_sigma` other than 1, 2, 3 or `"all"`. A sphere makes no datum (it has no plane or axis);
  `fit_primitive` reports it.
- The seed is a world point. A scan moved after its datums were fitted
  (a new `align_import`) leaves the seed where it was; one now off the
  surface fails the datum with the distance, never a fit somewhere else.
  So a re-scan read standing anywhere is laid back by a best fit onto the
  scan it replaces (kept as a second import) or onto the model made from it
  ([laying a scan onto a body](../interchange/README.md#laying-a-scan-onto-a-body)),
  and its datums refit where they were.
- The model built on the datums is checked against the scan it came from
  by `compare_scan` and the Compare to scan tool: a signed distance from
  every scan vertex, the share within the scan's noise, and the places past
  it on the body's faces
  ([how far a body is from a scan](../inspect/README.md#how-far-a-body-is-from-a-scan)).
- A datum that cannot fit -- its body gone, the seed off the surface, a
  curved kind on a flat -- fails with the reason, and its output is the
  default, never the last good one ([the datum
  contract](#regeneration)). `get_state` and the editor show what the
  last regeneration fitted (`FitReport`).
- Datums execute on a baked open too ([baked
  geometry](../versioning/README.md#baked-geometry)), so a from_fit datum
  refits there: one grow and fit over the triangles round its point.

Through them: a **sketch** on a fitted plane (`plane_ref` the datum, as
on any datum plane); a **mate** anchored to one (`add_mate` connector
`{occurrence, frame: <datum id or name>}`, `src/core/DatumFrame.{h,cpp}`:
a DatumPlane's frame is its origin, X and normal as Z; a DatumAxis's its
origin and direction as Z with X the world's X laid square to it -- an
axis fixes no turn about itself, so a mate that needs one takes the
world's); the **`frame`** query, which reports the same frame; and
**`measure`** with a `{datum}` side ([measuring a
datum](../inspect/README.md#datums)). The Fit tool (`###tb_fit_primitive`,
`src/ui/FitTool.{h,cpp}`) picks the point, previews the fit in its window
with its kind (Auto by default; Torus among them) and tolerance (the scan's noise unless
"Own tolerance"), shows what the used points read (sigma, form, the
diameters, a cylinder's length and arc, a plane's spans) and the
partial-arc warning, and Accept adds the datum as one undo step; a sphere
is shown and Accept stays disabled, saying why. Its folded "Hints and
clip" section takes the clip, the side and a radius range (seeded a tenth
either side of the fitted radius), which the datum keeps; the axis and
the box are the RPC's.

How it is checked: `tests/test_datum_from_fit.cpp` through a real
regeneration of an imported scan -- the axis and the top within 0.1° and
0.05 mm of the generated ones, following a re-scan of other triangles and
noise half a millimetre over, the same to the bit after a cold
regeneration, read back as written with every bad kind refused, failing
by name off the scan and on a flat, a sketch standing on the fitted top,
and a part mated to the fitted axis standing on it; the golden
`v215-datum-from-fit` (a shaft's axis and top fitted to its own B-Rep);
hints and a clip kept in the datum's `fit` and refitted by, every bad one
refused on load; the golden `v230-datum-fit-hints`; the GUI tests
`fit/a_plane_picked_on_a_body_is_one_undo_step`,
`fit/a_side_hint_picks_a_cups_wall` and `defaults/fit_primitive`;
`headless_smoke.py` (the new reply keys, and a hinted datum surviving save
and reopen); `refused_calls_smoke.py`.

### Reading a round off a scan

`read_round {feature_id, point_mm}` (`mesh::ReadRound`,
`src/core/mesh/ReadRound.{h,cpp}`) gives an edge of a scan its size. At
the point -- on the edge, or on a model's sharp edge drawn where the scan
is rounded, which stands off it by up to the round's size -- the triangles
about it are split by their normals into two faces, and the edge runs
along both (their normals' cross product). A section square to the edge
(`mesh::Slice`) through the point gives the run of points about it; the
faces in it are lines, each fitted to its end of the run and grown in while
five points in a row do not leave it, and what lies between is read three
ways with the faces held where they are:

- **sharp**: the two lines meeting;
- **a round** of one radius, tangent to both (one parameter);
- **a chamfer** of two distances, one along each face from where they meet
  (two parameters).

Each is scored by its AIC, the residuals over the faces' own noise
(widened by its correlation) squared plus twice its parameters; the lowest
is the `verdict`, and `also_fits` names any within 6 of it -- the readings
the scan does not tell from it. A round's `radius_mm` and `sigma_mm` are
its fit held tangent to both faces with the faces free
(`sketch::FitTangentRound`, as [the pieces
fit](../sketching/README.md#the-pieces-fit) holds a small round), so the
sigma carries where it touches them. The window starts at 40 mesh edges
and doubles while the corner takes more than a third of it, up to a
quarter of the scan's diagonal; the edge's direction is read in the first
window only, where no other face of the part is in it. Refused, naming
why: no mesh within the first window of the point (`no scan`), one face
there (`no edge`), faces too short to read.

Measured by `tests/test_read_round.cpp` on scanned prisms whose one corner
is generated round, chamfered or sharp, 0.02 mm of noise: rounds of 0.5
(sampled every 0.05 mm), 2 (0.1) and 6 mm (0.2) between faces at 90° and
60° read as rounds within three of their sigmas (0.553 ± 0.021 and
0.479 ± 0.008 for the 0.5 mm ones are the furthest, 2.5 sigmas; the
larger within one), the angle within 1.5°; a sharp corner reads sharp and
a 1.5 mm chamfer as one, its distances within 0.1 mm. A 1 mm round every
0.2 mm with 0.03 mm of noise read as a round in five draws of five, and in
three of them the chamfer was within 6 (`also_fits`): the scan carries
six points on it, and says so.

### Reading a thread off a scan

`read_thread {feature_id, point_mm}` (`mesh::ReadThread`,
`src/core/mesh/ReadThread.{h,cpp}`):

- **The cylinder**: a ball of triangles about the point, grown until it
  reaches round at least 150° of the fitted circle and two radii; the axis
  is the principal direction of the triangles' normals whose spread stands
  apart from the other two (a surface of revolution's normals lie on a cone
  about its axis: a thread's flanks tilt them along it, a plain bore's
  not at all), the centre and radius a circle through the triangles round
  it; then a band of four radii along the axis is fitted again.
- **The signal**: each vertex's radius against its axial position, in
  twelve sectors round the axis, binned a mesh edge long.
- **The pitch**: the first strong peak of the sectors' mean
  autocorrelation, then each sector's own in windows of four turns; the
  median of those, and refused as no thread when fewer than half agree
  within 5 % or the peak is under 0.5 (a plain bore). It is then refined to
  the pitch whose fold over every turn and sector leaves the sharpest
  profile; `pitch_sigma_mm` is the windows' spread.
- **The hand**: the shift between neighbouring sectors' signals (a
  right-hand thread advances with the angle about the axis), as a share of
  pitch / sectors (`phase_share`, ±1 for a single start read cleanly).
- **The profile**, folded over every turn: its highest and lowest radius
  are the major and minor diameters, its slope at mid depth the flank
  (`flank_deg`, from the radial plane). `internal` when the triangles face
  the axis.
- **The nearest standard**: ISO trapezoidal (ISO 2904's preferred pitch of
  each size) for a flank under 22.5°, else the metric and unified rows of
  the thread table, by the sum of the diameter's and the pitch's relative
  miss; `LH` for a left hand.

Measured (asserted) on scanned threads with 0.02 mm of noise: an
M8 × 1.25 bolt reads 1.2500 mm, 8.00 mm, right hand, 30° flanks, M8; a
Tr16 × 4 left-hand nut 3.9999 mm, 16.00 mm, left hand, internal, 15°,
`Tr16x4 LH`; a plain 10 mm bore is refused as no thread.

**As a default.** On a model drawn over a scan, the first edge picked into
a fresh Fillet takes the scan's round there (the reading put on the
0.5 mm step when that is within three sigmas), a Chamfer the scan's
chamfer; a cylindrical face picked into the Thread tool takes the thread
the scan carries on it, size, pitch and hand, before the face's own size
(`ui::BlendFromScan`, `ui::ThreadFromScan`;
[defaults](../ui/tool-sessions.md#defaults-and-refusals)). The tool window
says so under the number, "From the scan's round" ("chamfer"; "Size, pitch
and hand from the scan"), while the tool is open and the number is still
the one read (`ui::ScanSeedNote`, `AppState::scanSeed`), as Fold names the
rules its radius came from. Each is clicked through in a GUI test
(`defaults/fillet_takes_the_scans_round`,
`defaults/chamfer_takes_the_scans_size`,
`defaults/thread_takes_the_scans_size`).

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

## The size that fits

A Fillet, Chamfer or Shell that cannot be built at the size asked says the
size that can (a Shell's [wall](#a-shells-wall) below): "fillet radius 10 mm does not fit: face '1/face0' is 15
mm wide between edge '1/face0;U0' and edge '1/face0;U3', so the rounds meet
at 7.5 mm; the largest radius that fits is 7.49 mm", on an 80×15×20 box
with both vertical edges of one end picked. The caller, a person dragging the field or an agent
sending `add_feature` again, would otherwise bisect by hand, one build and
one round trip a guess.

**Measures at the picked edges** (`src/core/MeasureBlend.cpp`, declared in
`Measure.h`; `query_geometry` gives them with `blend_measures: true`): for
each picked edge and each of its two faces, the **dihedral angle** between
the faces at the edge's middle (`DihedralAngleAt`, the solid's interior
angle: over 180° at a concave edge); the face's **width across the edge**,
how far it runs from the edge, in the face and square to it, before its
boundary, the least over five stations along the edge, and the edge it
ends at (`WidthAcrossEdgeAt`; on a plane, or along the ruling of a cylinder
or cone, where that line is straight); and the **least radius of
curvature** of a face that curves toward the blend (`ConcaveRadiusAt`: into
the material at a convex edge, into the air at a concave one).

**Bounds** (`fit::BlendBounds`, `src/core/BlendFit.h`). A round of radius r
where the faces meet at θ takes r / tan(θ/2) of each face; a chamfer takes
its distance. So the setback is at most the face's width, and where the
face's far edge is picked too the two setbacks together are (two rounds on
a 15 mm end face meet at 7.5); r is at most a concave face's least radius
of curvature. Two more are taken from the faces' own geometry rather than
measured at stations ([bounds that refuse](#bounds-that-refuse)): at a
closed circle, the **axis** -- a face of revolution leaning toward it from
the rim reaches it, and no setback passes that (a cylinder's end rounded by
its own radius is a hemisphere) -- and two blends that **meet** across a
planar face, picked or brought in by the builder's tangent propagation.
Each reads as an upper bound -- nothing above it fits, something below it
may still fail -- and the least of them, with the face or edge it is about,
is the blend's bound. The axis and the meet hold in OpenCASCADE: they
refuse a size past them, and every other kind seeds the search.

**The search** (`fit::LargestThatBuilds`). When the build at the asked size
fails -- the builder is not done, a faulty contour, a fault in its signal
scope, or a result `Measure::InvalidReasons` calls invalid; not the edges'
own objections, which no size cures -- the first candidate sits just under
the bound (a thousandth below it, so two rounds that would meet exactly
leave a sliver). Under a bound that refuses, where that builds it is the
answer; under any other the search goes on above it, and where nothing above it builds
the bound is named as the reason. Otherwise eight steps: down by a factor
of 0.4 while nothing has built, so the steps reach a thousandth of the size, then
halving the bracket between the largest that built and the least that
failed by ratio (its geometric mean), so the answer is as close in
proportion whatever its size. Nine builds at most, a fixed count, so the answer does not depend
on how busy the machine is. Every candidate is rounded down to three
significant digits, so the number reported is a number that was built. A
candidate builds when `topo::EdgeBlendUnnamed` returns a solid that
`InvalidReasons` passes: the two things `Execute` requires, without naming
the result. The candidates build on the body with the
splits the blend meets merged ([a blend beside a split](#a-blend-beside-a-split)), as the feature does. `BRepFilletAPI_MakeFillet::Build` ignores its progress range in
OpenCASCADE 7.9, so the search is bounded by its count; it asks
`heavy::Cancelled()` before each candidate, and a stopped search reports
the bracket it has.

A feature that stays broken is regenerated again and again. Its answer is
kept on the feature (`FilletFeature::fitMemo`, restored by
`CopyDerivedState` with the rest of the executed state), keyed on the input
body, the edges and the operation, and reused for any asked size at or
above the least known to fail: a field dragged further past the limit
searches once. The build at the asked size still runs each time, unless it
is past a bound that refuses.

**What it says.** "radius" for a Fillet, "distance" for a Chamfer:

- converged: "fillet radius 25 mm does not fit: the largest radius that
  fits is 3.92 mm — near (40.000, 0.000, 20.000)", the faulty contours'
  places as before; where the bound's candidate built, the bound's fact in
  brackets instead: "chamfer distance 10.5 mm does not fit: the largest
  distance that fits is 9.99 mm (face '2/g2870764257' runs 10 mm from edge
  '2/g2870764257;U1', so a chamfer of 10 mm takes all of it)";
- stopped: "… does not fit: a radius of 5 mm builds and 7.5 mm does not
  (the search was stopped)";
- nothing built: "… does not fit: no radius builds on these edges, down to
  <the least size tried> mm (<what the build at the asked size said>);
  fewer edges in one fillet usually builds".

The answer is also a `FitHint` on the feature (`Feature::GetFitHint`, set
and cleared with the error): `regen.failed[].fits`, the refusal's
`diagnostic` ([rpc](../scripting/rpc.md#structured-errors)) and the tool
window's "Use 7.49 mm" button ([tool
sessions](../ui/tool-sessions.md#the-tool-window)) read it.

`add_feature` and `modify_feature` build a Fillet or Chamfer on a copy
first, as they do a direct edit, and refuse one that does not build with
the document unchanged, unless asked to [reduce](#reducing-on-the-call) it;
a size an upstream edit broke is a broken feature in that edit's `regen`.
`tests/test_blend_fit.cpp` checks each answer by what it is for: the size
reported builds a valid solid of the closed-form volume, and 2 % above it
does not.

### A Shell's wall

`WallObjection` (`ShellFeature.cpp`) refuses a wall that breaks through an
open face before the offset builder runs, and says the wall that fits. A
wall it passes that the builder then refuses -- an inner round smaller than
the wall, a rib thinner than two walls -- is searched the same way, on
`thickness`, in at most three times the CPU the wall asked cost, and not at
all when that took more than 4 s (inside the Shell's whole budget,
`directedit::BuilderBudgetS`, a 200-face gear wheel's refusals took over a
minute each in the action matrix): the first guess half the body's thinnest wall (`ThinnestWall`), then four
steps (`fit::kShellSearchSteps`), since an offset costs seconds where a blend
costs milliseconds. The guess is not a bound: where it builds the search
goes on above it. A candidate builds when `ShellOnce` gives a solid every
one of its checks passes, the same as the wall asked. The builder's reason
keeps its words and gives up its "a thinner wall usually builds" for the
wall that does: a 40×30×10 box whose bottom edges are rounded 1.5, shelled
2 from the top, says "shell failed (wall too thick for the geometry?); the
largest wall that fits is 1.41 mm" (1.41 builds, 1.58 does not: four steps
leave a bracket a blend's eight would close). A candidate the deadline
stops part way is neither (`fit::Stopped`), and the search reports the
bracket it has: "... ; a wall of 0.5 mm builds and 1 mm does not (the
search ran out of time)"; where nothing builds, "... ; no wall builds, down
to 0.00128 mm". The answer is a `FitHint` with `param: "thickness"`, kept
on the feature (`ShellFeature::fitMemo`) like a blend's, so `regen`, the
refusal's diagnostic, `fit: "reduce"` and the tool window's button read it
as they read a blend's. `add_feature` and `modify_feature` build a Shell on
a copy first, as they do a blend, and refuse one that does not build
(`does_not_fit`) with the document unchanged.

### Bounds that refuse

A size past a bound that holds is not built: the builder's own failure far
past a limit is most of what a refusal costs, and nothing past the bound
can build. `fit::Refuses` says which kinds hold, from a sweep
(`tests/test_blend_fit_sweep.cpp`, hidden, `[.fit_sweep]`) that builds
every edge of a directory of bodies, alone and with the edge its face's
width ends at, filleted and chamfered at 1.001, 1.01, 1.1, 1.5, 2 and 4
times its least bound, and at those times the least bound that refuses
where something else measures less. The bodies are the action matrix's corpus
(`tools/action_matrix.py export-breps DIR`) and the kernel cases' bodies
before their first blend (`tools/kernel_cases.py export-breps BINARY DIR`):
over about 32 000 edge sets, OpenCASCADE built past a face's width in one
fillet in six and one chamfer in eleven, past the width between two picked
edges in about one in four, hundreds of them at four times it (a wedge
whose face narrows to a point at one of the five stations, a round that
runs off its face over a cliff), and past a face's curvature in one in
five. Past a circle's axis it built nothing, in about 580 edge sets where
the axis was the least bound and 600 where something else measured less;
nor past two blends that meet by a hundredth, in about 7 600 and 3 600. A
thousandth past where two meet it built twice, on one body: a round
meeting a concave cone's rim across 11 mm built at 1.002 times the bound
and failed at 1.005, the builder running the second blend a little onto
the first. So those two refuse: the axis a thousandth past it
(`fit::kRefuseMargin`: a blend as large as the bound is still the
builder's to answer), two blends that meet a hundredth past
(`fit::kMeetRefuseMargin`). `tests/test_blend_fit_rim.cpp` holds each
against the builder both ways -- the search's first candidate under the
bound builds, the refusal margin past it does not -- on shafts, blind
bores, frustums, tapered floors, drill points, pairs of bores, a bore
beside a side, a prism whose sides lean in and a tangent chain.

**The axis** (`fit::RimBounds`). At a closed circle whose two faces each
have a straight meridian about its axis -- a plane square to it, a coaxial
cylinder or cone -- each face leaves the rim along a direction u in the
meridian half-plane. One that leans toward the axis (u radially negative)
reaches it R / −u_radial from the rim, and no setback runs past that: a
chamfer's distance, a round's r / tan(φ/2), φ the angle between the two
directions. A cylinder's end gives R (rounded by its own radius it is a
hemisphere); a frustum leaning in by α, R tan(45° + α/2); a blind bore's
floor R, concave as it is; a 120° drill point's cone, 2R for a round and
R / sin 60° for a chamfer. It is not where the rolling ball's centre would
cross the axis: on the drill point that is R, and the builder builds 1.5R,
a round whose profile is centred across the axis and whose surface never
reaches it. A bore's rims and a boss's foot lean away from the axis and
have no such bound: one rim of a 1 mm bore through a 20 mm plate rounds at
r5. A disc to its own rim beside a face of another kind keeps the setback
across the disc.

**Two blends that meet** (`fit::MeetBound`). Two edges of the blend --
picked, or brought in by the builder's tangent propagation
(`topo::EdgeBlendContours`: the edges added to the builder and not built,
contour by contour) -- on one planar face, each a line or a circle whose
other face has a straight section (a plane beside a line; a coaxial
cylinder or cone beside a circle), so the setback k per mm is the same all
along it: r tan((π − ψ)/2), ψ the angle between the two faces' directions
into themselves, or a chamfer's 1. Where a segment square to both edges, at
points inside both, lies in the face -- its middle inside it, no boundary
crossing it -- the two setbacks along it add to at most its length g:
past g / (k1 + k2) the blends' edges on the face cross, and the builder
does not intersect two stripes. The candidates are the line of centres of
two circles, the square from a circle's centre to a line, and the middle of
two parallel lines' overlap; they are checked in the face from the least
up, 32 at most. Unlike the width between two picked edges, it takes the
segment square to both, not a line square to one edge at five stations,
and it sees the edges the builder adds. A picked edge the builder takes
into no contour (faces meeting at 179.6° on a sheet-metal relief) counts
for nothing: that is the edges' objection, not a size's. The contours cost
a copy of the body and the edges added (16 ms on the plate below, a tenth
of its build), so the last four inputs' are kept, and a size dragged on
one feature pays it once.

`FilletFeature::Execute` looks for both before it builds
(`fit::RefusingBounds`, under the size asked; a look at analytic geometry,
where measuring every bound costs more than a fitting blend, 0.35 s against
0.2 s on the plate below). Past one it builds only the search's candidates,
seeded at that bound, and the sentence leads with the bound's fact:
"fillet radius 12 mm does not fit: edge '1/face1;U0' is a circle of radius
10 mm; the largest radius that fits is 9.99 mm", `basis: "bound"` in
`fits`, or for a frustum "edge 'E' is a circle of radius 1 mm, and face 'F'
runs 1 mm from it to its axis, so a round of 1.61803 mm takes all of it". The
answer is still a size that built; `add_feature`, `modify_feature` and the
tool window read it as they read any other.

The plate of 100 bores (`tests/fixtures/step/plate_100_holes.step`, R3
bores at a 10 mm pitch in a 100×100×10 plate with R4 corners) with every
rim on its top and the outline's four corner arcs picked is the case this
was built for. Its limit is not the bores' radius: the corner arcs bring
the whole outline in by tangency, and the outer bores' rims meet the
outline's round across the 2 mm between them, at 1 mm (0.999 builds, 1
does not; the rims alone meet each other across 4 mm, at 2). "fillet radius
30 mm does not fit: face '1/b1/face109' is 2 mm wide between edge
'1/b1/face0;U2' (tangent to a picked edge, so blended with it) and edge
'1/b1/face8;U1', so the rounds meet at 1 mm; the largest radius that fits
is 0.999 mm", by one build: 0.26 s of CPU against 0.20 s for a round that
fits, where building 30 first cost 3.4 s against 0.17 s
(`tests/test_blend_fit.cpp` holds it under three times the fitting round).
A size past a bound that does not hold is built and searched as before.

### Reducing on the call

`add_feature` and `modify_feature` take `fit` beside `feature`: `"exact"`,
the default, builds the size asked and refuses one that does not fit;
`"reduce"` builds the size that fits instead and stores it. Any other value
is refused naming the two, and `fit` on a feature that is not a Fillet,
Chamfer or Shell naming the three. The edit is tried on a copy at the size
asked; refused as `does_not_fit` with a size that fits, the feature's
`radius` or `thickness` becomes that size and it is tried again, and that
is what the document holds: the radius is 7.49, and nothing remembers 10.
The response says so beside `regen`, `reduced: {param, asked_mm, built_mm,
basis}`; a size that fits is built as asked and answers no `reduced`.

This is not the substitution "refuse, don't substitute" forbids: the caller asked for it on this call, the
answer names both numbers, and the document holds the number that was
built, so it never loads looking right and being wrong. So it is refused
where it could not keep that promise, before anything changes: a size given
as an expression (`radius_expr`, `thickness_expr`), which a number would
overwrite; inside a `batch`, whose regeneration is deferred and has no
built body to try the size on; while the part is still building; and where
nothing fits, as the same call without `fit` is. It is not stored, so the
format does not move, and regeneration never reduces anything: a later edit
that makes 7.49 too large breaks the feature, which then says the new
size. The GUI's form of it is the "Use" button. `fit_reduce_smoke` checks
it by what it is for: the stored radius is the size built, the body is the
closed-form round of it, a save and reopen gives the same body, and r5
answers no `reduced`; `refused_calls_smoke` that each refusal changes
nothing.

### A blend beside a split

An `add` keeps the faces its two solids share a surface on apart, with an
edge between them, until a Refine: an L joined from two blocks has a back
face in two pieces. OpenCASCADE's blend builder fails, or builds a patch
more, where a blend's end or its run meets that edge, and at every size, so
the search read the builder's failure as the size's ("no radius builds ...
down to 0.00257 mm" where the L from one profile rounds R2.5).

So a Fillet or Chamfer the builder fails on as the body is, or whose
result is invalid, is built again on a copy with the splits it meets merged
(`topo::MergeBesideBlend`, `src/core/ElementHistory.cpp`), and nothing
else. A **split** is an edge between two faces on one surface (a cheap test
of the analytic geometry -- coplanar planes, coaxial cylinders and cones of
one size, concentric spheres and tori -- then `ShapeUpgrade_UnifySameDomain`
decides). It is merged where it shares a vertex with an edge of the blend
(a picked one, or one its tangent chain brings in,
`topo::EdgeBlendContours`), or, between two planes, where one of them is a
face the blend touches: the end of a round on an L's two-piece end face
reaches the edge between the pieces at its setback, away from the round's
own vertex. Every other edge and vertex is kept (`KeepShapes`), so no other
face merges and no other edges join. A split between curved faces is not
merged for being on a face the blend touches: two halves of an earlier
round's torus merged under a later round's chain built an invalid solid.

That result is named from the input through the merge and the blend as
one step (`BRepTools_History::Merge`, as `BooleanNamed` composes a
clean-up): a merged face takes its first piece's name with the blend's
`;M<id>`, what that piece is called after any blend that modifies it, and
the other piece's name ends there, as a face a blend consumes does
([toponaming](toponaming.md#how-each-operation-names-its-output)). The
body as it is comes first because merging renames: a revolve's two
coplanar caps rounded together, merged, left a recorded session's next
feature on the other cap's name with nothing to find
(`tests/fuzz/cases/boolean_failed-779.jsonl`). So a blend that builds as
the body is builds what it always built, names and all, and only one that
did not build moves a name: no golden, and no name the action matrix keys
on, changed. The size that fits measures on the merged body
(`topo::BlendBody`, its faces named as the input's, a merged one by its
first piece), and a candidate builds when either does: a width runs to the
face's own boundary, so R5 on the L's 4 mm wall fits 3.99, where it
measured to the edge between the pieces. A blend that does build as the
body is but builds a patch more where it meets a split keeps it
([known bug](../known-bugs.md#a-blend-that-builds-beside-a-split-builds-a-patch-more)).

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
walking, a fillet beside a bore's seam walked round its vertex for ever
inside `BRepFilletAPI_MakeFillet::Add`, and a very thin shell's arc join
read the sections of an intersection that had failed, which have no edge
(`BRepOffset_Tool::Inter3D`). Each is now an ordinary failed
feature (`tests/test_occ_patches.cpp`, skipped on a kernel without the patch
-- CI's apt OpenCASCADE).

One more never faulted but never returned: a fillet along a closed spline
whose pole a damaged file had thrown 83 m out searched for a point where the
blend's normal is not degenerate by a step that shrank towards the
approximation's 2D tolerance, minutes of probes inside
`ChFi3d_Builder::StoreData`. It now gives up after a thousand, and the
fillet fails on that contour, naming where; the reader
fuzz's damaged copies of the `v217` golden, which fillets that spline, read
inside its CPU allowance again.

## A reshaping feature keeps only a valid solid

A Fillet, Chamfer, Shell, Draft or direct edit
([`directedit::CheckResult`](#direct-edit)) whose builder reports success on
a solid that crosses itself is refused with `Measure::InvalidReasons` ("draft
produced an invalid solid (self-intersecting wire, unorientable shape)"),
and the body is left as it was; check_geometry would have called it invalid.
A Fillet or Chamfer refused so searches for the size that does build
([the size that fits](#the-size-that-fits)). A Shell that reaches past the body's own measured outside — the offset
builder moving a face outward — is refused too: a shell hollows inward.

Stock OpenCASCADE's offset builder answers by where its input lives in
memory, not only by what it is. A drilled box shelled through one bore's
floor (`tests/fuzz/cases/rebuild-139.jsonl`) came back an invalid solid of
several times the volume about one build in seven, in a copy, in another
process or a second time in the same one. `MakeShells` quilts the offset
faces in the order of a map hashed on shape addresses, the split of a cap
that the offset extends (a bore's floor) is oriented against them, and the
quilt keeps the orientation of whichever face comes first; and before
that, `BuildOffsetByArc` registers the offset faces in the order of another
such map, so the same shell of the toolmaker's clamp's M3 screw built on one
copy and not on the next. Every build here (nix, the Flatpak, the AppImage,
the Windows cross build; not the Debian package) carries
`nix/patches/occt-shell-cap-order.patch`, which quilts the caps last, and
`occt-offset-arc-bind-order.patch` (upstream's OCCT#1600, backported to
7.9), which registers the faces in the order they were made; the overlay in
`nix/occt-overlay.nix` applies them, and netgen is built against the same
patched library. With both, each of the screw's eight shells gave one answer
over 100 draws (ten fresh instances, ten copies each), where six had given
up to six -- but the order fixed is a failing one for two shells that used
to build on some copies, so those are now refused every time
([known-bugs](../known-bugs.md#two-shells-of-the-clamps-screw-are-refused-on-every-draw)).

The CI Ubuntu job links apt's OpenCASCADE, which is unpatched, so the Shell
also keeps its own guard (Offset Face, on the same builder, has one like it:
five draws whatever the kernel, under one deadline). Every such answer fails
one of the checks above, so a Shell that fails them is built again on a
`BRepBuilderAPI_Copy` of its input (the same solid at new addresses,
sub-shape for sub-shape, so its index-keyed names are the body's), and is
refused only when every draw failed: five draws on apt's kernel, two on one
with only the cap-order patch, and one on ours, where a copy answers as the
original does (`occpatch::Carries`, `src/cad/OccPatches.h`: the install
lists its patches). An honest refusal (a wall too thick for the geometry)
cost five draws for nothing, a fifth of a full sweep. The offset builder's
Intersection mode (offset faces intersected over the whole solid, not only
between neighbours) was measured as a further try: it built 106 of the
matrix's shells the arc joins refused, but the shell it built on random
design 33 left a body the next cut could not cut, so it is not used. Every
draw shares one budget, the direct edits' (`directedit::BuilderBudgetS` by
the body's faces): the builder stops at its progress checks. One edge/face
intersection of two offset faces asked no progress indicator, and a 0.02 mm
wall on the worm (`golden/v146-worm`) spent minutes there past the budget;
`occt-offset-inter3d-progress.patch` passes the indicator down to it
(nix/patches/upstream/offset-inter3d-progress.md), so that shell now gives
up within about a second of its 46 s budget (the replay answered in 47 s;
it took 96 s on a quiet machine before, and three minutes on a busy one).
`test_shell`'s "A shell builds the same wherever its input lives in memory"
shells 60 copies of that box and requires one solid with one set of names.

`test_occ_patches`' "A shell of the clamp's screw answers alike on every
copy" shells 24 copies of the screw through each of its faces, a different
amount of heap held before each, and requires one answer per face; "A thin
shell of the worm stops when its deadline passes" requires the builder back
within 10 s of a deadline that lands in that intersection.

On a kernel with the cap-order patch alone (or none) a feature after a
Shell sees the same solid with its faces in another order, which mattered
to one check: a Draft that tilted the wall of a sealed cavity through the outer wall passed
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
dimension of it (0.0029 mm for a 290 mm spoke). The box is the tight one:
the conservative box bounds a B-spline face by every pole of its surface,
and a 10 mm face cut from a 2000 mm surface read 2828 mm across, putting the
retry at 0.028 mm, past where the window below starts merging geometry
(`[fuzzy]`). The fraction was measured,
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

A full revolve's cut or add has one more: its seam is its profile, which
lies in the sketch plane, and a sketch drawn on a face of the body puts the
seam IN that face. A d2.5 sphere cut centred on a half revolve's cap beside
a fillet left the cap inside the sphere, exact and fuzzy alike. A solid of a
full turn is the same point set turned about its axis, so a refused one is
tried once more turned a quarter (`RevolveFeature::Execute`), which takes
the seam out of the plane; its names are the solid's. If that is refused
too, the first refusal is what the feature reports.

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

`mass_properties` reads its boxes from the same memos: each body's tight box,
and the total's grown from them (an assembly's occurrences through the
located-box cache), so a repeat call on an unchanged part costs a millisecond
where bounding the total afresh cost five seconds on a worm wheel.
`measure_smoke` holds the repeat to a tenth of a second of the app's CPU.

The per-shape memo lives inside the object: a shape's address-based id is
recycled by the allocator, so an outside cache keyed on the address alone
could hand a freed shape's box to whatever lands at that address next. The
located-box memo beside it holds a handle to each TShape it keys on, as the
volume cache does, so an address it keys on is never reused while the entry
lives; it is a bounded ring (1024), and opening a project empties it.

The conservative box uses `BRepBndLib::Add` with `useTriangulation = false` and
the tight one `AddOptimal`, face by face. `AddOptimal` bounds a face's surface
over its UV rectangle, which on a trimmed sphere holds more of the sphere than
the face keeps (a sphere cut patterned about Z read 4.5 mm past the body), so a
face on a plane, cylinder, cone or sphere is bounded by its edges and vertices
instead: a coordinate has no extreme inside such a face that its boundary does
not reach, except a sphere's six points centre ± r along an axis, which count
when they lie on the face. Any other surface is `AddOptimal`'s. Neither is mesh-derived: a box from the
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
| `src/core/features/CradleFeature.{h,cpp}`, `src/core/mesh/Field.{h,cpp}`, `src/core/MeshOperand.{h,cpp}` | the Cradle, its pocket, a body as triangles for a mesh operation |
| `src/core/MeshFit.{h,cpp}`, `src/core/mesh/Grow.cpp`, `src/core/mesh/Fit.cpp` (`FitRobust`), `src/core/DatumFrame.{h,cpp}`, `src/ui/FitTool.{h,cpp}` | a primitive fitted from a point, the datums that keep it, a datum as a mate's frame, the Fit tool |
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
`project_geometry`, `update_derived`, `set_derived_follow`, `derive_occurrence`
(`src/remote/RemoteModeling.cpp`), and there too `check_literals` and
`extract_variables`
([../variables/README.md](../variables/README.md#literals-that-should-be-expressions));
`list_elements`, `curve_points`,
`tube_schedule`, `grow_faces` ([Delete Face](#delete-face)) and
`fit_primitive` ([fitting a primitive to a mesh](#fitting-a-primitive-to-a-mesh)),
`read_round` and `read_thread` ([reading a round](#reading-a-round-off-a-scan)
and [a thread](#reading-a-thread-off-a-scan) off a scan)
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
addresses, for which the fresh instance was only a luckier draw).
A message's **gist** is its class and first
60 characters less digits, and the report groups seeds by it.

`run --mesh-body` makes every design's second step a closed
[mesh body](../interchange/README.md#mesh-bodies) (a sphere of radius 10,
across, beside or clear of the base), so every feature that reads a B-Rep
meets one it must refuse by name.

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

## Kernel cases

`tests/kernel_cases/` holds small cases whose answers were worked out by
hand, most of them at a coincidence, a tangency or a geometric limit: a cut
tangent to a face along a line, a tool 1e-6 mm into or out of the stock,
holes touching at one point, a round as wide as its face.
`tools/kernel_cases.py` builds each over the RPC and holds it to its answer;
[kernel-cases.md](kernel-cases.md), generated from the same rows, says how
many pass, which fail and under which entry, and what is not translated.

**The cases are copied, never edited.** `solidcraft/<area>/<id>/` is
SolidCraft's kernel rig (`recipe.json`, a command script, and
`expected.json`, its answer: `source: analytic` with the derivation in
`method`, or `source: invariants`), byte for byte, with a graded ladder on
one 60 x 40 x 20 stock. `tools/kernel_cases.py vendor --from CHECKOUT`
copies them and writes `SOURCE` (the repository, the commit, every file's
SHA-256); the runner refuses a tree that differs. `solidcore/<area>/<id>/`
holds the `expected` block of a case solidcore added later, beside a recipe
written here from the entry's description (solidcore's entries are kernel
calls on recorded bodies, not scripts), the body rebuilt from the
description and the recorded vertices; where it needs a command with no
translation (a fit-point spline, a surface loft, a coil of a sketch, a
face split) the recipe still says it in SolidCraft's words and the case is
untranslated. `LICENSES.txt` says what was taken and under which licence.
The answer never comes from a kernel, theirs or ours.

**A recipe is translated at run time** (`tools/kernel_cases_translate.py`)
into the steps [the reference parts](reference-parts.md) run, with its
`base` chain's commands first; nothing translated is committed.

| recipe command | becomes |
|---|---|
| `solid.box`, `solid.cylinder` | a Sketch on a plane `{origin, normal, uAxis}` at the corner or base, and an Extrude |
| `solid.sphere`, `solid.cone`, `solid.torus` | a section Sketch and a Revolve about a `custom` axis |
| `sketch.create` with lines, rectangles, circles, arcs, slots, rounded rectangles, polygons, an ellipse, a control-point spline | one Sketch, sent when a feature first uses it; SolidCraft's named planes are written out as frames (its XZ faces +Y), never left to `Top`/`Front`/`Right`, and a sketch on a face takes SolidCraft's frame for that face |
| `solid.extrude` | Extrude: `distance`, `direction`, `taper` as `taper_deg` with its sign turned, `through_all`, a `profiles` pick by a point as `regions` |
| `operation` `new` / `join` / `cut` | `mode` `new_body` / `add` / `cut`; a target named in a part of several bodies is a new body and a Boolean against it |
| `solid.combine` | a Boolean per tool, chained on the result |
| `solid.fillet`, `solid.chamfer` | Fillet, Chamfer, each edge a `$pick` at its point |
| `solid.shell`, `solid.offset_face`, `face.delete`, `solid.draft` | Shell, OffsetFace, DeleteFace, Draft, faces by `$pick` |
| `solid.hole` | a point Sketch on the face at `position` and a Hole; a `thread` is a tapped fit |
| `solid.revolve`, `solid.loft`, `solid.sweep`, `solid.thread` | Revolve (its axis pointed so a part turn goes toward the sketch's normal, as SolidCraft's does), Loft, Sweep, Thread |
| `solid.pattern.*`, `solid.mirror`, `solid.split_body` | Pattern (a total `angle` divided into the step between copies), Split |
| `parameters.add`, `construct.plane.offset` | nothing sent: an expression is evaluated, a plane kept as a frame |
| `file.insert_step`, `file.export` (STEP), `file.export_step`, `file.new` | `import_step` with `keep_origin` (a STEP or an IGES), `export_step`, `new_project`; `$CASE/` is the case's directory and `$OUT/` the run's scratch, and a body named before a `file.new` is the re-import's when it brings one body |

A command or a parameter with no counterpart is not approximated: the case
is `untranslated`, naming it (a second radius or a setback on a fillet, a
two-distance chamfer, an outward shell, a pattern in two directions, a cut
with several targets, regions that come from crossing curves, every command
outside the table). A `$pick` that finds no element, or more than one, makes
the case `untranslated: ambiguous pick`, never the nearest one; the one
exception is SolidCraft's own rule, that a seam or a tangent join passing
through the point gives way to the real edge there. A body a later command
names that has no `created_by` of its own (a split's piece, a pattern's
copy, a lump) is untranslated too. `inspect.measure` and `document.inspect`
build nothing and are skipped. A mesh's import and conversion (`file.insert_mesh`, `mesh.convert`) and
the exports to other formats are not translated.

**What a case is held to.**

- `body_count` (solids: two lumps one feature left in one body are two,
  unless the case states its `shells`, which is how it says one body has
  several),
  `volume_mm3`, and `area_mm2`, `center_of_mass` and `bbox` where given, from
  `mass_properties`; every body valid and closed by `check_geometry`. A
  volume or an area is held to half a unit in the last digit the case
  states, never tighter than 1e-9 relative, or to the case's own
  `tolerance`; a `[min, max]` is inclusive. A figure whose uncertainty
  (`volume_uncertainty_mm3`, `area_uncertainty_mm2`) is wider than that
  room is a `volume` failure even where the figure itself lands inside it
  ([the integrator](../inspect/README.md#the-face-by-face-integrator)).
  A coordinate is held the same
  way, and a box's side stated as a whole number to 1e-4 mm. SolidCraft's blanket
  1e-3 is not used: the near-tangent ladder's answers differ from the
  stock's by 2e-7 of it.
- `faces` where given, as their own class (`topology`), so a right volume
  on the wrong faces is told apart from a wrong volume. The cases count a
  whole analytic surface once, so faces that share a smooth edge and lie on
  one plane, cylinder or sphere are counted as one (`list_elements`): the
  coplanar faces an `add` leaves unmerged until a Refine do not fail a
  case. `edges` and `vertices` are not graded: SolidCraft counts a chain of
  edges once, and no RPC lists the vertices a chain joins at. `shells` and `watertight` are read off an STL exported at the
  case's `mesh_mm` (0.01 mm), its vertices welded at `weld_mm` (1e-4 mm):
  every edge used by exactly two triangles, and the connected pieces
  counted.
- `outcome: refused`: a call is refused, or adds a feature that fails to
  build, with a message that states a condition (`fuzz_design.measured`),
  and the bodies are as they were. The wording is ours, so `refusal` is not
  matched as text, with one exception: where it states "the largest radius
  (or distance) that fits is N" and our refusal names a size too (`fits`
  on the diagnostic or the failed row: [the size that
  fits](#the-size-that-fits)), the two agree to 2 % (`fits_mismatch`;
  ours is a size that built, rounded down to three digits). A refusal that
  found no size at all is not compared. `solid_or_refused` takes either.
- Never a crash, a hang (120 s a case, or its `timeout_s`; the instance is
  killed by pid), an `internal` error or a non-finite number.

**A failure stays visible, and fails when it stops.**
`tests/kernel_cases/status.json` holds one sorted line per case: `pass`,
`untranslated: <what>`, or `known: <class> (<entry>)`. The classes are
`volume` (a measure or the body count), `topology`, `invalid`, `accepted`
(a case that must be refused and built), `crash`, `hang`, `internal`, each
naming its [known-bugs](../known-bugs.md) entry, one per defect; and
`refused`, a clean refusal of a case that has an answer, which is not a bug
but something [not built](#not-built), and names its line there. A run fails
on a `pass` that no longer passes, a `known` case that passes ("remove the
row and its entry") or fails with another class, an `untranslated` case that
now translates, a failing case with no row and a vendored case missing from
the file. `bless` writes every row but the `known` ones, which are edited by
hand, and refuses while a failing case has none; it also writes `slow.json`,
the cases that took over 3 s.

### Known rows that are not kernel defects

Some rows are `known: volume` or `known: topology` and name this section
instead of a known-bugs entry, because the difference is not a defect that
can be shown:

- **Right to 1e-6, not to the digits stated.** The answer agrees to
  between 1e-9 and 1e-6 relative (a box by a micron), past the floor above.
  Three things are that size and are not told apart here: OpenCASCADE's
  1e-7 mm confusion on a 30 mm part, a swept, lofted or blended surface
  approximated to its own tolerance (a square swept along a Bezier is 2e-6
  off), and the case's own quadrature, whose value is printed to sixteen
  digits whatever it is good to. The modelled M10 thread to a shoulder
  (`06-threads/004`) is one: its groove is SMLib's screw sweep, 6.7e-8 of
  the volume over the ideal one along the threaded length, which lifts the
  centre of mass 6.5e-7 mm, past the last digit the case states; the
  measured centre is the one that volume implies to 2e-9 mm. The top edge
  of a concave R21 wall that runs out tangent, rounded R9.5 (`851`), is
  1.7e-5 mm3 under the case's 9065.809611, 2e-9 of it.
- **The case's formula is approximate.** A rectangle extruded with a taper
  is a prismatoid, V = h (A0 + 4 Am + A1) / 6; two cases state the
  pyramid's h (A0 + A1 + sqrt(A0 A1)) / 3, which is 7e-5 and 7e-4 off, and
  one of them says so.
- **The answer follows SolidCraft's own construction or table.** A loft
  from a square to a circle depends on how the sections are paired, and a
  loft through five or ten circles of an elbow is not the torus (2e-6 to
  1e-3); a 1/4-28 UNF tapped hole takes the tap drill of SolidCraft's
  table. A chamfer on a curved face is placed by SolidCraft where a ball
  touching both faces touches them, ours at an equal chord from the edge
  on each (a half cylinder's root, 0.7127 mm³ where the case's 0.5231 is
  its ball's). solidcore's chamfer along a tangent chain keeps its offsets,
  taken at the wedge of the chain's plane-plane edge, where ours keeps equal
  setbacks: a slot wall's chamfer running into a groove's round removes
  0.274 mm³ where the case's 0.444 is solidcore's, and an independent
  section integration gives 0.273 for equal setbacks. Two of its windows
  are `known: accepted` for the same reason: under equal setbacks the
  wall's chamfer and the window's stay apart (by 0.002 and 0.007 mm), so
  the pair builds where solidcore's offsets overlap and refuse.
- **The case counts pieces of one surface apart.** Two chamfers ending on
  a cylinder's wall (`15505`) state the operand's faces as 14 and 12 where
  the grader, counting a whole surface once, finds 12 and 10 on ours: the
  wall's two pieces at a line, and the revolve's inner face with the
  cylinder it continues, lie on one cylinder. The chamfer adds its one face
  to that, 13 and 11.
- **A face thinner than the confusion.** Two rounded boxes 1e-4 mm apart
  leave a sliver whose ends, where both outlines run tangent into the same
  side, are 1e-4 long and under 1e-9 thick: the case counts each a face,
  and a kernel at 1e-7 mm makes each an edge (8 faces of the case's 10, the
  volume right to 2e-6).

Every other failing row names a [known-bugs](../known-bugs.md) entry or a
line under [Not built](#not-built).

    tools/kernel_cases.py run build/lobole [--ladder [--quick] | --area A] [-k TEXT] [--jobs N] [--write-page]
    tools/kernel_cases.py replay build/lobole CASE [--keep]
    tools/kernel_cases.py bless build/lobole
    tools/kernel_cases.py vendor --from SOLIDCRAFT [--solidcore SOLIDCORE]
    tools/kernel_cases.py self-test

`replay` prints one case's calls, its expected values against what was
measured, and the volume's and area's uncertainty; `--keep` leaves the instance running with the part open.
`kernel_cases_ladder` (headless suite) runs the ladder's tiers less the
slow list on one instance, checks the vendored files against `SOURCE`,
every case for a row, and the generated page against the rows;
`kernel_cases_selftest` feeds the translator a planted recipe per table row
(`tests/kernel_cases/selftest.json`: the calls are written beside it) and
the grader and the ratchet planted answers. The whole corpus is
`tools/ctest -C kernel_cases -L kernel_cases -j4`: run it before a release
and after a change to the kernel layer; `tools/merge-branch.sh` does not.

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

The matrix copies no GUI default: the GUI's are derived from the part
([tool-sessions.md](../ui/tool-sessions.md#defaults-and-refusals)). On a body past 100 faces the direct edits,
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
any name holding a fallback-numbered boundary edge `;U<k>`. The number is
the edge's place in its face's geometric order, but which of an edge's two
faces names it is the first in the result's face order, which a builder may
hand back in shape-address order
([toponaming](toponaming.md#how-each-operation-names-its-output)), so one
edge can carry either face's name from one process to the next. Every such
name of one feature is therefore one family, `edge:<feature>/*;U*`.
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
written by `bless`, in one shape, one line per entity, so a diff names the
entity that moved. `action_matrix_selftest` fails on a baseline in any
other shape (a formatter once reflowed it), and `reformat-baseline`
rewrites it in the tool's own, its content unchanged; `.clang-format`
leaves JSON alone.

The tests (`tests/action_matrix_smoke.py`): `action_matrix_sample` (the
headless suite and `test-fast.sh`), `action_matrix_full_0`..`7` and
`action_matrix_determinism` (labelled `slow;action_matrix`, and registered
only for the ctest configuration `action_matrix`, as the big generators are
for theirs, so that no plain ctest runs some two CPU-hours: `tools/ctest
-C action_matrix -L action_matrix -j4`; `tools/merge-branch.sh --release`
ends with it, `--full` when the branch reaches what it covers
([what a merge runs](../ui/testing.md#what-a-merge-runs)), and so does the
release checklist,
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
`test_hole`, `test_worm`, `test_cam_profile`, `test_curve3d`, `test_sweep`, `test_loft`, `test_pipe`,
`test_derived`, `test_toponaming`, `test_read_round` ([reading a
round](#reading-a-round-off-a-scan) and [a thread](#reading-a-thread-off-a-scan)
off a scan, and the defaults they give), and `test_direct_edit` for the four
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

Small cases at a coincidence, a tangency or a limit, with answers worked
out by hand, are the [kernel cases](#kernel-cases)
(`tools/kernel_cases.py`, `kernel_cases_ladder` in the headless suite and
`tools/ctest -C kernel_cases -L kernel_cases` for all of them).

Whole parts are checked by the [reference parts](reference-parts.md)
(`tests/reference_parts.py`, `tools/ctest -L reference`): each built over
the RPC, picked by points, compared tightly against closed-form volume,
area, centroid, box and counts, and again after a STEP round trip, with a
known-failure ratchet. A feature whose result has a closed form lands with a
reference case.

## Not built

- A datum (or a Frame) as a mate's anchor in the GUI mate tool, which
  picks faces: a scan is mated through its datums by `add_mate` only
  ([datums on a scan](#datums-on-a-scan)).
- A seed that moves with its scan: a from_fit datum's point is a world
  point, so re-aligning the scan after fitting leaves it behind (the datum
  then fails by name).
- Splitting by a sketch (sketch as knife).
- A body sub-ordinal: split pieces and pattern copies share one `createdBy`,
  and a reference finds its body by the name it carries
  ([refused on the bodies](#refused-on-the-bodies)); a name two of them
  carry is refused, not told apart.
- A thin extrude of an OPEN profile: `thin` walls every closed loop, and an
  open polyline is refused as "sketch has no closed profile", naming its
  [open ends](../sketching/README.md#open-ends).

- What the [kernel cases](#kernel-cases) hold as built and Loböle refuses
  cleanly (their rows are `known: refused`):
  - a round or a chamfer at its limit: as wide as the face beside it, as
    tall as its wall or as deep as its pocket, past either, or ending
    against a boss, an opening or another blend ("the blend could not be
    built", "the fillet failed on N contour(s)", or a result the validity
    check refuses);
  - a round whose rail runs on past the rim's end across a neighbour's top
    face (a drum's rim beside a boss flush with its top), one as large as
    the ledge it runs under (a branch tube's joint under a flange 1 mm
    proud, at R1 exactly: 0.999 builds), one that shrinks to nothing where
    two equal tubes touch, a mixed-convexity corner whose concave round
    has to roll over the convex one's end, a joint round whose joint a
    tube's seam vertex cuts 0.37 mm short of a chamfer's crease (with the
    seam elsewhere it builds);
  - a Shell whose wall is thicker than a round inside it, that opens the
    side of a thin rib, or whose floor falls away to a drain, and a Shell
    with no open face (a sealed void);
  - an Offset Face of two adjacent faces at once, and of a face with a
    tangent neighbour without it ([Offset Face](#offset-face));
  - a Sweep along a closed path, a Pattern of a Loft or a Sweep that adds
    to a body, and a Loft to a section the first cannot see whole (a comb);
  - an Import of a lone surface (a trimmed patch, a face with a hole):
    a body is a solid, and a placed shape with no volume is skipped,
    named ([interchange](../interchange/README.md#importing-into-a-part));
  - a thread outside the tables: a metric size with a pitch of its own
    (M44x1), trapezoidal (Tr20x4) and pipe threads (Rp, Rc, NPT), on a
    Thread and on a tapped Hole; a modelled thread in a hole as wide as
    the thread, which SolidCraft narrows to the minor diameter first; and
    a modelled thread whose groove cut the boolean check refuses.

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
- Inserting a feature at the rollback marker. `add_feature` appends at the
  end even with the bar set, so an import cannot be edited ahead of a
  sketch that projects it.
- A direct edit that extends a freeform neighbour: one beside a B-spline is
  refused (`directedit::FreeformNeighbour`), which on an Autodesk STEP is
  most faces ([editing a downloaded part](../interchange/README.md#editing-a-downloaded-part)).
