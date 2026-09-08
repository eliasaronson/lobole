# Clamp workflow fixes

Implemented in the following stages. Tests use private application instances;
the human's open document and unrelated `render.png` are untouched.

## 1. Motion diagnostics

Unknown exact-contact checks now retain occurrence IDs/names and a reason in
`sample.unknown_contacts`. The text table prints those pairs and distinguishes
UNVERIFIED, INCOMPLETE, ERROR, KINEMATICS ONLY and actual collision failures.
Unknown pairs contribute to worst-sample selection. Cancelled/error results
cannot advertise `clean: true`. A kernel failure is never retried into a pass.

The current kernel contact API returns an optional result, not an OCC diagnostic
string, so the reason identifies a failed exact calculation; it does not pretend
to know whether topology, coincidence or a kernel bug caused it.

## 2. Drawing and SDK ergonomics

```python
plane = part.datum_plane(base="xz", offset="-width/2")
sketch = part.sketch(plane)  # persistent plane_ref, not a numeric snapshot
pick = ProjectedCurve(curve).pick("start")  # edge/silhouette and endpoint encoding
app.add_drawing_note(text="Bore {{bore}}; pitch {{spacing*2}}", position=[210, 240])
```

Datum offsets follow the plane normal; XZ retains the SDK's +X/+Z axes and -Y
normal. Cross-part references are rejected. World-coordinate conversion from a
live datum is refused: use sketch UV coordinates or native projection. Numeric
`Plane` and `Frame.plane()` remain snapshots, explicitly documented as such.

Notes evaluate `{{expression}}` through the native expression engine. Free notes
use project variables; notes with `view` also use that view's source-part scope.
Source-feature properties are available as `feature_<id>_diameter` and `_depth`
for holes, and `_pitch`, `_major`, `_tap_drill` for known thread designations.
Feature IDs remain stable across edits. Missing variables/features produce an
unresolved-note issue and block export, rather than retaining an obsolete value.
`list_drawing_notes` returns both the template and `resolved_text`/`resolved`.
Renaming a referenced variable currently requires editing its note template;
this is detected as unresolved, not silently left stale.

UI, exported drawings and validation share the resolver. Note anchors are centred
in the UI, matching exports. Validation checks note bounds, title block, views,
dimensions and other notes using the shared conservative label bounds. Automatic
dimension placement also avoids notes. Bounds are estimates, not font-exact
outline intersections, so visual review is still appropriate.

## 3. Threaded interfaces

```python
thread = part.thread(cylindrical_face, "M6", allow_engagement=True,
                     minimum_engagement=12)
rig.screw(cylindrical_mate, thread)  # pitch follows the referenced thread feature
rig.thread_engagement(jaw_occurrence, screw_occurrence)  # requires an actual joint
```

Tapped holes keep **tap-drill geometry**. The cosmetic annotation does not change
STEP, volume, machining geometry or BOM mass. Both mating faces must explicitly
allow engagement. Exact checks relieve only the common cylindrical span of a
matching internal/external thread with aligned axes and sufficient axial overlap.
Matching requires a known designation, the expected major/tap-drill diameters,
axis alignment within the numerical checks, and at least the larger of each
specified minimum and one pitch. An unknown designation, stale face, wrong axis,
wrong diameter, insufficient overlap or mismatched pitch receives no relief.
Other intersections between the same bodies remain collisions.

Named exact interference reports expose `engaged_thread_pairs` and
`thread_engagement_volume`, so accepted contact is inspectable. The SDK engagement
gate additionally fails on a separated pair: being collision-free is not evidence
that a screw is fitted. A whole collision sweep alone does not assert that every
intended joint exists; use explicit engagement gates for those connections.

The exact motion path preserves thread metadata and bypasses the geometry-only
contact cache for annotated pairs. Caching those pairs will need interface
metadata in its cache key; using the old key would return stale acceptance after
an annotation edit. Unannotated engine pairs retain the existing cache path.
Mesh collision remains a geometric approximation and does not apply thread relief.

Screw relations persist `thread_part` and `thread_feature`, deriving their ratio
at each solve. Direct ratio overrides are refused. Deleted, suppressed, invalid
or unsupported thread references report a solve error. This is kinematic/contact
intent, not thread strength, tolerances, handedness support or helical CAD geometry.

## 4. Dogfood and regression gates

The clamp now uses live datum handles, typed projected references, associative
machining notes, real 5 mm M6 / 2.5 mm M3 tap-drill bores and five explicit
thread-engagement gates. Its closed-loop motion, 41 sampled poses, independent
jaw-volume calculation, eight sheets, native save/reopen, STEP round trip and
live variable edits remain covered. A separate M8 fixture verifies that changing
from coarse to fine updates the motion ratio without copying a pitch literal.

Format v95 has a golden file and a version-history entry. Unit tests cover
unknown/cancelled motion diagnostics, note expressions and overlap reports,
thread mismatches, short engagement, missing opt-in and unthreaded intrusion.

### Measured validation (2026-09-08)

The complete headless suite passed **1154/1154** in 146.17 s. The focused drawing
GUI regression also passed, followed by **284/284** tests in the full GUI suite
(126 s across four shards). The updated clamp took **0.844 s to build and
2.790 s end to end**, including five successful explicit thread-engagement
checks, 41 motion samples, eight sheets, STEP, BOM and native project save.
The assembly render and all eight exported sheet layouts were visually reviewed.

The original workflow took 1.71 s end to end; the new engagement checks and
uncached annotated contact calculations add work. This is a correctness tradeoff,
not a claimed performance improvement. A future annotation-aware cache can
recover reuse without accepting stale thread intent.
