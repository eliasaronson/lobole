# Inspection

The "is it right?" toolset: measuring, mass properties, looking inside,
checking for clashes and floating parts, checking the B-Rep, and comparing
against a reference. Every inspection a person does by clicking, an agent can
do over RPC with the same numbers, and every verdict says how it knows.

Core code is `src/core/Measure.{h,cpp}` (measurement, contact, sections,
comparison, geometry checks) and `src/core/MeshCollide.{h,cpp}` (triangle
clash tests). The RPCs are in `src/remote/RemoteInspection.cpp`; the measure
tool's GUI in `src/ui/MeasureActions.cpp` and `src/ui/MeasureOverlay.{h,cpp}`.
`describe {method}` is the authority for any RPC's parameters and response
shape; this page is what they mean.

Two rules run through all of it:

- **A number says what it is of.** `0.000 mm` between two edges that meet at
  a corner is true and useless; "touching — 90° between the face normals" is
  the same geometry, said. Every readout carries its basis, units and
  qualifier.
- **A verdict says whether it was checked.** A sweep that ran out of budget
  is not a clean sweep. Verdict RPCs attach a `receipt`
  ([../scripting/rpc.md](../scripting/rpc.md#what-a-response-carries)) whose
  standing is `unverified` whenever an oracle could not run.

Kernel queries never modify their arguments: every boolean in `Measure.cpp`
goes through `BuildQuery`, which runs it non-destructive, because an
OpenCASCADE boolean writes p-curves onto its arguments' edges and two threads
querying pairs that share a body otherwise corrupt the heap.
`test_conventions.cpp` fails on a boolean built any other way.

## Measure

**RPC.** `measure {a, b?, kind?, distance_kind?, mode?, part?}`. Each side
names one thing: `{feature_id, element}` for a face or edge,
`{feature_id}` for a whole body, or `{occurrence: id|name|"Unit/Part"}` for a
placed part; each side may carry its own `part` or `occurrence`, so one
measurement can span two parts or two occurrences.

- `{a}` alone: that thing's own measure — face area, edge length, the shape
  kind, radius and diameter for a round face or edge; a body reports volume,
  surface area and bounding size.
- `{a, b}`: distance, the closest points, a signed `delta`, and
  `overlap_volume_mm3` / `clash` for two whole shapes.
- `distance_kind`: `minimum` (default), `maximum` (the clearance question),
  `centres` (hole spacing — a circle's or bore's true centre, not a point on
  its rim).
- `kind: angle | both` adds the angle. Between two planar faces it is taken
  between the outward normals (adjacent box faces read 90°, opposite ones
  180°); when either direction's sign is arbitrary, as a line edge's is, it
  is unsigned in 0–90°. `basis` says which. An element with no direction (a
  sphere, a spline) refuses rather than inventing an axis.
- `mode: mesh` takes the minimum distance off the triangles, ± the
  `tolerance_mm` it reports (the two meshes' deflections summed), for pairs
  where the exact extrema search is pathological (every face pair equally
  far, nothing prunes).

**Exact distance between whole shapes** (`ExactDistance`,
`src/core/Measure.cpp`) asks the meshes where the answer is and the B-Rep
what it is. The cached meshes name every face pair whose triangles lie within
two deflections of the nearest; a pair whose triangles are further from the
best exact answer found than the two deflections together cannot beat it and
is skipped; the rest are single face-pair extrema (`Extrema_ExtFlag_MIN`),
run on every core. A list longer than 128 pairs is asked again of meshes ten
times finer, built on copies so the document's triangulation is untouched.
Only a list past 4096 pairs goes to BRepExtrema as two whole shapes. The cap
is fixed, not derived from the thread count, so a one-thread run gives the
same answer bit for bit. Extrema write nothing onto the shapes (unlike a
boolean, [below](#query-booleans-are-non-destructive)); the meshes are built
before the workers start, and a sweep warms every one first
(`collide::Warm`).

### Crossing tessellations are not contact

Meshes that touch or cross say nothing about the solids. A chord of a
concave face (a bore, a ring's tooth space) stands proud of the surface by up
to the deflection, so a shaft 0.01 mm inside a bore meshed at 0.1 mm crosses
it while the solids are apart. `ExactDistance` refines the face pairs within
the slack exactly in that case too, and a pair whose surfaces really cross
answers 0 itself. It used to return 0 the moment the meshes crossed, with no
closest points: a planet pushed toward its ring read as touching 0.04 mm
before it did. When the meshes cross it does not re-mesh finer (seconds on
a gear), and the first pair found whose surfaces cross ends the search, so
two solids that really interpenetrate still answer in about a second;
which crossing pair supplies the (degenerate) points then depends on the
workers. A pair list past the 4096 cap answers 0 without refining: two
shapes that overlap that widely are the overlap pass's question, and
BRepExtrema over two interpenetrating solids takes minutes.
`test_measure.cpp` "A shaft whose mesh crosses its bore's is measured, not
called touching".

Between two whole shapes `measure` also reports `overlap_volume_mm3` / `clash`.
When the distance it has just measured is positive, the surfaces meet
nowhere, so the solids overlap only if one lies wholly inside the other; one
vertex of each solid classified against the other settles that, and the
boolean is not run. Measured on a 70 mm planetary gear set (herringbone teeth
0.1 mm apart): a sun and planet 15-24 s before, 0.6-1.3 s after; a planet in
the ring 29-32 s before, 4-6 s after; the same sun and planet with
twisted-extrude (helicoid) teeth 21.5 s before, 0.5-1 s after. A face or edge pair asks
BRepExtrema for the minimum only, not MINMAX.

**GUI.** The Measure tool (toolbar, `Ctrl+Shift+M`) works on part, assembly
and CAM job tabs. It picks faces, edges, whole bodies, whole occurrences, and
corners.

- It **seeds from the selection**: select two faces, arm it, read the answer.
- **One pick** reads out that element's own measure; **two** their distance
  and angle; **more than two** of one kind their total length or area (a mix
  refuses and says why).
- **The readout leads with the number that is not degenerate.** Touching or
  intersecting elements lead with the angle; parallel ones lead with the
  separation ("parallel, 45.000 mm apart"); otherwise distance then angle.
- **It is drawn on the geometry** on the on-top overlay channel, so a
  thickness through material is visible: a dimension line with arrowheads,
  witness lines, and the value in a pill at its midpoint; an arc for an angle.
  For parallel elements the dimension is placed by projecting one element's
  centroid onto the other, not at the arbitrary point pair the extrema solver
  returns. The picture and the text come from one computation
  (`UpdateMeasureReadout` fills both), so they cannot disagree.
- **Hover** measures the element under the cursor against what is picked
  (or reads its own measure with nothing picked), drawn dimmed; cached on the
  element, the pick and the document version, not recomputed per frame.
- **Pin** keeps a measurement drawn while you take the next; pinned rows
  highlight their dimension on hover and drop themselves when an edit breaks
  a reference. Measurements are session state, never saved.
- The readout uses the document's units. **Copy value**, and **create a
  variable from a measurement** (written in storage units, so it parses as an
  expression).
- Rows name what was picked in words ("Board — top face", "Bracket — Ø25
  bore"); the stable name is the tooltip.
- A **corner** is where two named edges of a face meet. A raw vertex has no
  stable name (the element map names faces and edges), but the pair of edges
  does, so a corner is stored as the two edge refs. One corner reports its
  coordinate; two report the exact distance between them. A pair of edges
  that meets at two vertices (a cylinder's seam) is refused as ambiguous.
- A tab switch, entering sketch mode, or re-arming clears the tool.

`measure_smoke` covers the RPC; a parity phase in `remote_smoke` asserts
that a measurement taken by clicking two occurrence faces and the same one
taken over RPC give the same numbers.

## Mass properties

`mass_properties {body?, part?}` → per body `volume_mm3`, `surface_area_mm2`,
`center_of_mass_mm`, `inertia` (3×3 about the centre of mass), `material`,
`density`, `mass_g`, and a measured `bbox`, each row's `body` its index
among the part's bodies (a Derived ghost is not weighed, and the rows after
it keep their own indices and boxes); plus `total {volume_mm3,
center_of_mass_mm, mass_g}` and `bbox_mm {min, max, size}`, the measured box
of everything it weighed together (on an assembly, every visible leaf where
the solve put it). Density comes from the body's material; a body with
no material has no mass rather than an assumed density, and the BOM names it
in `warnings`. On an assembly tab without `part` it aggregates every visible
occurrence in world space. Its bbox is measured off the surfaces; `get_state`'s
is a cheap conservative bound that can read well over on a swept surface.
A PRINTED part's bodies also carry `printed`, what the slicer's walls,
shells and infill weigh at its slicer settings, and `total.printed` their
sum ([printed mass](../tolerances/README.md#printed-mass)); `{why}` when a
setting is unstated. On an assembly each occurrence says `mass_from`
(printed or solid), and `total.printed` weighs the assembly as built: its
printed parts' occurrences printed, the rest solid, with the inertia about
that centre (`inertia_g_mm2`).

The Properties dock shows the same numbers for the active part (cached by
document version), and a centre-of-gravity marker can be drawn in the
viewport.

## Sections

**The number.** `section {base_plane | origin+normal, offset?, part?,
bodies?, regions?, loops?, enclosed?}` returns the cross-section area (`area_mm2`) where a plane cuts the
bodies — on an assembly tab, every visible occurrence in place. `offset` is
mm along the normal; omitted, the cut goes through the middle of what is
shown and the chosen offset comes back. `regions: true` adds each connected
island `{area_mm2, centroid}` and `region_count`: the total cannot say whether
material is where you think it is, an island can. `loops: true` adds each
island's outline as line/arc/other segments, for recovering the shape of a
part you did not draw. `bodies` (body names or the feature ids that created
them; part tabs only) cuts just those bodies, and an unknown one is refused
naming the bodies there are. An unknown `base_plane` is refused.

### Enclosed area

`enclosed: true` adds `enclosed_area_mm2`: the area the cut RINGS rather
than the area it crosses (`Measure::SectionEnclosedArea`). Each body's cut
face is taken out of one oversized plane in a single boolean, so faces of
separate bodies that meet or overlap act as one; what is left is the open
plane around the part plus one face per region the material closes, and
those regions' areas are the answer. A building's outer walls are four
bodies, and what they enclose is the floor a listing quotes, which no
material area can report; a plate encloses its bore.

- A ring with a gap encloses 0, and `enclosed_note` says so: a door or a
  window at the plane's height opens the walls, so a plan check cuts above
  the openings (the house generator's outer walls enclose nothing at
  1.2 m, where the windows are, so its requirement cuts at 2.34 m;
  [generators/house](../../generators/house/README.md#what-the-round-taught)).
- Material of the chosen bodies standing inside the ring is not enclosed
  area. Leave a body out of `bodies` to count the floor under it, which is
  also how a slab under the walls stays out of the room.
- A requirement states the same thing as `section_area {of: enclosed}`
  ([measures](../requirements/README.md#measures)).

**The view.** The Section tool (toolbar, both part and assembly tabs) keeps a
list of up to four cuts. A fragment survives only on the kept side of every
enabled cut, so two cuts make a quarter-section. Per cut: a source plane —
a base plane, a datum plane of the active part, or a picked flat face — an
offset with a drag arrow in the viewport, two tilt angles (degrees, about the
source plane's in-plane axes), flip, and visibility.

- The tilt pivots about the cut's offset point, so tilting does not also
  slide the cut off the model.
- Datum- and face-sourced cuts resolve live every frame: move the datum and
  the cut follows. A source that stops resolving stops its cut — the row says
  "source missing — this cut is off" and `list_sections` reports
  `source.resolved: false` — rather than cutting along some other plane.
- Cut faces are capped per body in the body's own colour, each cap trimmed
  by the other cuts. The shader clip previews every frame; the caps (an OCC
  half-space boolean per body) rebuild only when a slider or the drag arrow
  is released.
- Picking honours the cut: nothing on the removed side is pickable, and a ray
  that meets the plane inside a solid hits that body's cap (`RayHit::cap`). A
  cap is a body or occurrence pick, never a face — it has no element name —
  so the tools that need a face refuse it.
- Cuts persist in the project envelope (`project.json`, `"sections"`), not
  per document: a cut is a view of the whole project and applies on whichever
  tab is shown.
- The limit of four is the shadow pass's: it carries its planes in push
  constants beside a `mat4`, and four `vec4` planes fill Vulkan's guaranteed
  128 bytes (`kMaxClipPlanes`, `src/render/ClipPlanes.h`).

RPCs: `add_section`, `list_sections`, `update_section`, `remove_section`
(`{all: true}` clears), and `set_section` as the one-cut shorthand. The
default keeps the half opposite the plane's normal (z ≤ offset for `xy`);
`reverse` keeps the other half, and the response says which as `keeps`.

## Clashes, contact and floating parts

`interference`:

- `{a, b}` (bodies or occurrences; a part inside a sub-assembly by path,
  `"Gangway/Board 19"`, or id chain `"2/17"`) →
  `{clash, volume_mm3, distance, state: clash | touching | clear}`. `touch_tol`
  (mm, default 0.001) splits touching from clear, and the contact boolean
  runs with the same 1 µm fuzz, so parts designed to touch report contact,
  not a sliver of clash. Assert every bolted joint `touching` and every
  designed clearance by number; a clash-only check passes exactly when
  nothing touches.
- With no pair, an assembly sweep. It is bounded: pairs whose boxes do not
  overlap cost one box test; the rest are checked closest-first up to
  `max_pairs` (4000) or `deadline_s` (20, checked between pairs — one boolean
  can overrun it). The response says what it checked, box-cleared and left
  unchecked, and a truncated sweep's receipt is `unverified`.
- Each clash carries `centroid_mm` and `bbox_min_mm` / `bbox_max_mm` of the
  overlap in world millimetres — where the solid-in-solid is, not just which
  two parts — in the named-pair check, the sweep and `contact_report` alike,
  from the intersection solid the exact check already builds. A mesh-mode
  sweep knows only that two sides overlap, so its entries carry
  `within_min_mm` / `within_max_mm` instead: the region the two sides' boxes
  share, which holds the overlap somewhere. Motion studies use the same names
  ([assemblies](../assemblies/README.md#motion-studies)).
- An exact named pair waits off the frame ([threads](#threads)).

`contact_report` is the connectivity gate: `connected` (one component = one
structure; anything in its own component is floating), `components`, per-pair
`state`/`distance`, `near_misses` (gap ≤ `near_tol`, default 1 mm — almost
always an arithmetic slip), `clashes`. On an assembly with mates it adds
`unrealized_mates` and `unmated_contacts`. On a part tab it runs over the
part's bodies, which is how a body floating inside one part is found; there
an overlap joins two bodies into one component, like a weld. A pair whose
shapes carry more than `exact_face_budget` faces between them (default 20000
— an imported mesh is one face per triangle) is not started and is listed in
`not_checked`: the deadline cannot preempt a boolean already running, so one
such pair would hang the sweep. Both scopes are bounded the same way
(`max_pairs`, `deadline_s`, the coverage fields and a receipt), run their
pairs in parallel, and say `clash` when `clashes` is not empty, which the
receipt's "no pair interferes" reads.

- **`mode: mesh`** screens each pair on the tessellation first. A pair whose
  triangles cross goes to the exact boolean. One whose triangles do not has
  its exact distance measured (`Measure::ClearanceBetween`), and that settles
  it without a boolean when the distance is positive and no solid of either
  side lies wholly inside the other (one vertex of each solid, classified
  exactly). Anything else goes to the boolean. The answer is the exact one,
  with the same states, distances, clashes and press fits; each pair says
  how it was settled (`checked: distance | boolean`), and `mesh_cleared`
  counts the ones the distance settled. On a 70 mm planetary gear set (five
  lofted herringbone gears, teeth 0.1 mm apart) the print part's body scope
  took 445 s and the assembly 244 s; both now take about 30 s exact and
  24 s in mesh mode, which spares the booleans between meshing teeth.
- **`async: true`** runs the sweep as a `contact` job and answers `{job}`;
  `contact_status {job}` reports `fraction` (pairs checked) while it runs
  and the report once it is done, and `cancel_background_job {kind:
  "contact"}` stops it (a cancelled run keeps no report). One runs at a
  time. Like a study job it is cancelled when the connection that started it
  closes, unless `outlive_client: true`. The gather (the bodies, placed, and
  the mates) runs on the main thread and hands the worker deep copies
  (`CADShape::DetachedUnlocated`), because the frame may mesh the document's
  own shapes while the worker reads them; the copies' cached meshes are
  dropped when the run ends (`collide::Forget`).

### Seals

`check_seal {a, b}` (assembly tab, top-level occurrences by id or name)
answers whether the contact of two placed occurrences closes all the way
round: a lid on a jar, a gasket on its rim. The contact is where the two
overlap (a pressed gasket, the 1 µm-fuzzy boolean `interference` uses) and
where an analytic face of one lies on an analytic face of the other within
1 µm (a skirt flat on a shoulder: the faces' common, a 2-D boolean). The
axis is the one most of the contact's circular edges share, else the
contact's own principal axis that stands apart from the other two.

A half-plane through the axis every half degree cuts the contact into
**traces**: a chain of section edges through faces that lie on each other
(its width the chain's length), or a region of the overlap (its width its
longest extent, a pressed ring's chord, and its depth its area over that).
A **ring** is a path round of traces, each touching the next (their boxes
in the half-plane within 0.02 mm), that comes back to the trace it left;
of the paths from each trace on the first half-plane, the one whose
narrowest trace is widest. A thread's flank contact goes round too but
comes back a pitch along, so it is no ring.

The answer: `sealed`, `least_width_mm` (the widest closed ring's narrowest
trace; 0 when none closes), `rings[]` (`least_width_mm`, `at_deg`,
`least_interference_mm`, 0 where it only touches, `radius_mm`,
`centre_mm`), `open[]` (the arcs, `from_deg`-`to_deg`, with no contact at
all: a notch in a skirt), the axis and what gave it, the contact's area and
overlap volume. Angles run about the axis from world X (world Y for an
axis near X). Two that do not touch are refused. It runs on the frame:
seconds on modelled threads, whose helicoids are left out of the touching
faces but not of the overlap. The requirement kind `seal`
([requirements](../requirements/README.md#measures)) bounds
`least_width_mm`, off the frame and over a motion study if asked.

What it does not judge: whether the ring is pressed enough. It reports the
overlap's depth and leaves the pressure to the material: no study models a
25 % elastomer strain here, and a linear one would not be honest at it.

### Designed overlaps

Some solid in solid is meant: an engaged thread, a retaining ring gripping
its groove, a press fit. `Measure::ContactBetween` — the one boolean the
named pair, the exact sweep, `contact_report`, an exact motion study and an
exact clearance requirement all take — removes each from the overlap before
it is weighed, as an exact annulus, and reports it apart from the clash:

- an engaged thread: `engaged_thread_pairs`, `thread_engagement_volume_mm3`
  ([threads](../modeling/holes-and-threads.md));
- a ring in its groove (`RingGrip`): `ring_grip_pairs`, `ring_grip_volume_mm3`
  ([a ring in its groove](../parts/README.md#a-ring-in-its-groove));
- a press fit, two coaxial round faces whose size bands allow the overlap
  ([press fits](../tolerances/README.md#press-fits)): on the named pair
  `press_fit_pairs`, `press_fits` [{fit, diameter_mm, interference_mm,
  max_interference_mm, engaged_length_mm, volume_mm3}] and
  `press_fit_volume_mm3`; on every `contact_report` pair it applies to, the same
  `press_fits`; and a top-level `press_fits` list [{a, b, fits, volume_mm3}]
  on the exact sweep and on `contact_report`. A round overlap next to a
  banded face that is NOT a fit stays a clash and carries
  `press_fit_refused` [{fit, reason, excess_mm?}] saying why. A
  [printed press](../tolerances/README.md#printed-press-fits) is listed
  the same way, and also where the model shows no interference
  (`volume_mm3` 0).

A relieved pair is in contact, so `contact_report` counts it `touching` and
it joins its component. The ring grip and the press fit are one
designed-overlap step in `ContactBetween` (`RemoveRingGrip`, then
`RelievePressFits`, `src/core/Measure.cpp`), run only when a pair actually
overlaps and a side carries a grip or a band, or both sides carry a
printed press. An exact motion study lists
its press fits once per pair
([motion studies](../assemblies/README.md#motion-studies)). A mesh check has
no threads, grips or bands: its answers carry `relieves` saying so, and an
exact re-run is the one that tells a fit from a clash.

### Mesh mode

`interference {mode: mesh}` (and the motion study's default check) answers
"do these overlap" off triangles with an AABB tree per shape
(`src/core/MeshCollide`). It is about ten times faster than the exact boolean
and is for questions asked over and over.

- It never reports a distance or a volume: those would be the
  tessellation's, not the part's. It returns the verdict and `deflection_mm`;
  a pair closer than the deflection may read either way, so anything that
  must be certain re-runs exact.
- The deflection is 0.2% of each body's bounding diagonal, the renderer's
  own, so a 2 mm screw and a 2 m frame get the same relative fidelity.
  `collide::Mesh::Build` also takes a stated deflection in mm (a motion
  study's `clash_deflection_mm`) and cuts the triangles to it, on a copy of
  the shape: BRepMesh writes its triangulation into the shape, and a finer
  one left there would be what the renderer draws from then on.
- A body wholly inside another is a clash, found by a ray-parity containment
  test that runs only when the two boxes nest. The test starts from a point
  on the inner body's surface, never its box centre: a bearing cap straddling
  a shaft has its box centre inside the shaft.
- The mesh cache keys on placement (`CADShape::PlacementId`), not geometry:
  two occurrences of one part share a shape and sit in different places.
  Each entry holds its shape as well as the mesh, because `PlacementId`
  mixes the TShape's address and a freed address is reused by the next shape
  allocated: an entry holding only the mesh served it for that next shape,
  and the print check read two overlapping boxes as apart. As it doubles,
  the cache drops the entries only it keeps alive, never one still in use,
  which would remesh on a sweep's worker.
- The triangle test uses the separating-axis theorem with the two face
  normals, the nine edge-edge crosses, **and** each triangle's in-plane edge
  normals. Without the last six, coplanar triangles have no separating axis
  and read as intersecting however far apart they are — and a pin whose end
  cap is flush with the face it passes through is coplanar.

#### Touching is not a clash

Two faces seated on each other are coplanar triangles that overlap in their
plane, and the separating-axis test says any two such triangles meet. So
the mesh check used to call every seat a clash where the exact check (a
volume of 0) calls it touching: the clock spaced its wheels an end shake
apart to get a motion study past it. `collide::Mesh::Classify` answers
three ways instead:

- **Overlapping** -- a triangle pierces another, by however little, or one
  body is inside the other.
- **Touching** -- every pair that meets does so on a surface: one of the
  two lies wholly on one side of the other's plane, to the tessellation's
  float noise (a millionth of the part's size) -- two coplanar triangles,
  or one whose edge lies on the other's face. And a point a micron inside
  each solid, at the centroid of a spread of those triangles (24 of them),
  is outside the other solid: that is what tells a seat from two equal
  blocks sunk into each other end to end, where with flush sides no
  triangle pierces another and only the volume differs. NOT within the
  deflection: a nut's flank sunk 0.1 mm into its pillar's, meshed at
  0.26 mm, would read touching, and the clock's thread search took such a
  pose for a clean one.
- **Clear** -- nothing meets.

`interference {mode: mesh}` reports touching as `state: touching` (its sweep
lists clashes only), a motion study lists each sample's touching pairs
under `touching` and does not count them as clashes, and the contact
sweep's mesh screen still sends a touching pair to the exact check that
weighs it. Any overlap is a clash as before (`tests/test_meshcollide.cpp`:
blocks face to face touch, sunk by two deflections they clash, flush sides
included). A stored motion result keeps each sample's `touching` (v180).

`check_geometry`'s floating-bodies warning uses the mesh answer too: it is a
yes/no riding on a cheap check, and an exact contact on concentric bodies (a
spoked wheel) costs hundreds of times the validity check.

## Geometry checks

`check_geometry {body?}` → `{ok, valid, body_count, feature_errors, bodies[]
{valid, bad_faces, bad_wires, bad_edges, reasons}, receipt}`, plus
`connected` / `body_components` on a multi-body part and `stale_derived` for
derived parts whose source moved. Validity is `BRepCheck_Analyzer`; the
reasons are scanned at the face, wire, edge, shell and solid levels, so a
shape whose faces are each fine but whose shell is not ("not connected")
still names why. A solid with a cavity also has every vertex and edge
midpoint of each inner shell classified against its outer skin ("a cavity
reaches outside the body"): `BRepCheck_Analyzer` tests one sample point per
shell, chosen by face order, so its verdict on a void that pokes through the
wall followed an order the offset builder takes from shape addresses. A
boolean or sweep feature runs the same check on its own
result and fails, naming the reasons, rather than handing an invalid solid to
every later measurement — `CADShape::IsValid` is only a null check.

## Finding and naming geometry

- `list_elements {body?, part?, max_faces?, made_by?, role?, entity?, via?}`
  — stable face and edge names per body, the handles `measure`, fillets and
  mates use, each with the feature that made it (`made_by`) and its `role`;
  the filters ask for a feature's faces in a role (a pocket's floor), and an
  empty answer says `why` ([toponaming](../modeling/toponaming.md)).
  `unnamed_faces` counts faces the element map never named; zero is what
  makes the list complete. A piece of a split face or edge carries `piece`
  {`of`, `bounded_by`, `ordinal`}, `ordinal: true` when its name is by
  position ([split pieces](../modeling/toponaming.md#split-pieces)).
- `query_geometry` — declarative filters (`geometry_type`, `normal`/`axis`
  with `angle_tol_deg`, `radius` ± `radius_tol`, area and length bounds,
  `near_point`, `on_point`) and `expected_count`, which turns the query into
  a one-call assertion. Covers unnamed elements too. `near_point` measures
  to a face's centroid or an edge's midpoint; `on_point` `[x, y, z]` (mm)
  to the element ITSELF (`Measure::DistanceToElementAt`, a box test then
  `BRepExtrema_DistShapeShape` against a vertex), within `max_distance`,
  1e-3 mm by default. A full circle's midpoint depends on where the kernel
  put its seam; any point on the circle finds it, so `on_point` with
  `expected_count: 1` picks an element from a fact of the drawing, which is
  how the [reference parts](../modeling/reference-parts.md) pick every edge
  and face. The two point filters read one `max_distance` two ways, so both
  at once are refused.
- `frame` — the origin and axes of a named datum frame, a face, a circular
  edge or an occurrence, in part space or (with `occurrence`) world space:
  exactly the frame a mate connector with the same spec uses.
- `geometry_snapshot` / `validate_snapshot` — frozen, revision-bound edge
  measurements ([../scripting/sdk.md](../scripting/sdk.md#measurement-snapshots)).

## Mutating responses and pictures

Every geometry-mutating RPC already returns a `digest` — per-body volume,
bbox and solid count, and a delta — and a `regen` summary
([../scripting/rpc.md](../scripting/rpc.md#what-a-response-carries)), so the
measure step of execute → measure → render needs no extra call.

`render_sheet {path, views?, focus?}` renders up to six named views (default
iso/front/right/top) into one PNG with a numeric footer — bbox, volume, mass,
and on an assembly the `contact_report` summary — so the numbers sit in the
same image as the pixels. `focus` frames one occurrence in every view. It
needs a renderer (GUI or `--offscreen`).

## Comparing against a reference

`compare_geometry {golden_path}` (a STEP or BREP) against the active part, or
`{a, b}` bodies or occurrences. Each side is fused first, so overlapping
bodies are not counted twice.

- `metrics: "all"` (default) runs three booleans (common and both cuts) for
  `iou`, `dice`, `precision`, `recall` and the diff volumes, plus plain
  verdicts ("Under-generating: 17.8% of the reference is missing").
- `metrics: "volume"` runs no boolean and returns size, placement and
  topology: `volume_ratio`, the volumes and surfaces, `center_offset`,
  `bbox_iou`, and the face and edge census. The overlap fields are absent,
  not zero — a zero IoU would mean "no overlap at all". The verdict states
  the census without judging it (a STEP round trip splits faces), and never
  claims a shape match. On parts carrying toroidal fillets the booleans cost
  seconds; volume mode costs milliseconds. `missing_path`, `extra_path` and
  `materialize` need the booleans and are refused with it.
- Volume is a weak oracle: a frustum faked from stacked discs matches a real
  one on bbox, volume and centre of mass. The census separates them
  (`cone:1 plane:2` against `cylinder:6 plane:7`), which is why volume mode
  still returns it.
- `align: true` moves the candidate's centre of mass onto the reference's
  first, so the answer is about shape rather than position (a correct re-draw
  built about its own origin otherwise scores a low IoU against a reference
  placed elsewhere). `center_offset` still reports the shift.
- `missing_path` / `extra_path` export the diff solids as BREP.
  `materialize: true` draws them over the part — missing red, extra green —
  as a renderer overlay that is never pickable, exported or saved and drops
  itself on any edit or tab switch; `clear_geometry_delta` removes it.

`compare_smoke` and `test_measure.cpp` cover both modes.

## Tolerances and fits

`check_fit`, `set_tolerance`, `list_tolerances`, `stack_up`, `check_fits`
and the print allowance (`set_print_process`, `get_print_process`,
`print_allowance`) are described in [../tolerances/README.md](../tolerances/README.md). Declared clearances
between occurrences (`add_assertion`, `verify_assertions`) are clearance
requirements, and every check here can be one: a requirement holds the
measure and its bound in the project and records evidence of what it
measured ([requirements](../requirements/README.md)).

## Will it print

`check_print` answers for a printed part: on the bed, one piece per solid,
overhangs and bridges against the process, the gap between bodies, the
STL's chords, stacking: every piece on the bed and none printed within
the minimum air over another, and bed_fit: the plate inside the print
profile's bed and, when it states keep-out zones, every piece clear of
them, and slender: no piece standing taller than the limit times the
least width it stands on ([slender](../tolerances/README.md#slender)) —
[../tolerances/README.md#checking-a-print](../tolerances/README.md#checking-a-print).
`export_print_job` then writes the plates as a 3MF the slicer opens with
every piece's settings ([print jobs](../tolerances/README.md#print-jobs)).

## Before delivering a model

[review-checklist.md](review-checklist.md) is the pre-delivery list, one RPC
per box, with a symptom → cause → fix table. The MCP `modeling-workflow`
prompt points agents at it.

## Query booleans are non-destructive

An OpenCASCADE boolean modifies its arguments. Computing an intersection
computes p-curves, and by default OCC stores them on the input edges
(`BRep_Builder::UpdateEdge`) instead of on a copy. For a modelling feature that
is the right default, because a boolean consumes its operands. For a
measurement it is a write to the document.

The clash sweeps run pairs on worker threads, and one body appears in many
pairs, so two threads asking "does A touch B?" and "does A touch C?" both
write A's edges. The result is heap corruption, detected later at an unrelated
`malloc` (`malloc(): unaligned tcache chunk`), or only an UNKNOWN pair,
because a corrupted edge makes the next boolean on that shape fail.

Warming caches single-threaded (the rule in `core/Parallel.h`) does not cover
this. That rule is for idempotent lazy fills such as bounds and
triangulations; a boolean writes data specific to the pair.

So every boolean in `src/core/Measure.cpp` is default-constructed and passed
to `BuildQuery`, which calls `SetNonDestructive(Standard_True)`: OCC copies a
sub-shape instead of updating it in place. The two-shape constructor
`BRepAlgoAPI_Cut op(a, b)` builds immediately, so any option set afterwards is
silently ignored; `tests/test_conventions.cpp` fails on that constructor form
in Measure.cpp.

The convention is scoped to Measure.cpp. `core/ElementHistory.cpp` and feature
execution (`core/features/*.cpp`) build destructive booleans, and neither runs
on a worker thread. The exception is a refused body boolean's fuzzy retry
(docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry), which runs
on operands the document still owns and so goes through
`cad::BuildNonDestructive`. A sweep that calls into anything else has to carry the
rule with it. `AssemblyStudy`'s fuse copies its shapes and runs
`BOPAlgo_Builder` non-destructive for the same reason. Anywhere else in `src/`, a boolean that must not write to its inputs is
built with `cad::BuildNonDestructive` (`src/cad/NonDestructiveBoolean.h`),
which sets the flag before the build; a second convention test fails on the
two-shape constructor followed by `SetNonDestructive`, where the flag comes
too late to do anything.

`tests/sweep_parallel_smoke.py` diffs a one-thread report
(`LOBOLE_SWEEP_THREADS=1`) against an all-thread one byte for byte; it cannot
see this bug class, because the answer is usually still right. ThreadSanitizer
can: `tools/ci-local.sh --tsan`.

## Interference is box-pruned

`interference` and the contact queries reject a pair by bounding box before
any exact boolean: almost every pair in a real assembly is nowhere near its
partner, and an all-pairs sweep is quadratic.

### Threads

A modelled thread is two helicoids of B-spline faces, and an exact
interference of a nut on its pillar cost 1.7 s, the clock's rating nut on
its rod 4.4 s -- held on the main thread, past the watchdog's 3 s. The
profile (`tools/profile.sh`) put two thirds of it in the boolean's
face/face intersections and the rest in the distance pass. Two changes:

- **Surfaces proven apart need no boolean.** Where the caller asks for a
  distance too (the named pair, `contact_report`, `check_print`), the
  meshes bound the pair first: a true distance is at least the
  tessellations' less both deflections, and when that is past the contact
  fuzz the solids overlap only if one is wholly inside the other, which a
  vertex answers. Meshes within their deflections of each other are asked
  again of copies meshed ten times finer, as the distance pass narrows a
  long list. A pair the designed-overlap step reports either way (two
  printed presses, two engaging thread regions, a ring and its groove) is
  always weighed. On the clock's threads: 1.7 s to 0.7 s, 4.4 s to 1.4 s.
- **An exact named pair waits off the frame**
  ([requests that wait](../architecture.md#requests-that-wait)):
  `ParkInspectionRequest` lets the handler resolve the two sides and
  deep-copy their bodies on the main thread, and the boolean and the
  distance run on an `interference` background job; the call answers when
  it lands, and the window and other clients carry on. The sweep (bounded
  by its deadline), mesh mode (milliseconds) and a call inside `batch` or
  with `expect_revision` answer on the frame as before. `contact_smoke`
  watches one run (`LOBOLE_TEST_INTERFERENCE_MS` holds it, tests only).

## Positions are double

The model is double ([../conventions.md](../conventions.md#the-model-is-double)):
a face built at 164.8 sits at the double nearest 164.8, and `Measure`
reports it as that, in doubles, like sketch planes and sketch geometry
([../sketching/README.md](../sketching/README.md#plane)). A filter on a
COMPUTED position still wants a tolerance sized to its magnitude, since
arithmetic rounds: a centroid, an intersection or a solved placement is
exact to rounding, not to the decimal a person would write.

## How it is tested

`test_measure.cpp` (enclosed area: four wall bodies ring exactly the
floor, a gap encloses nothing, a plate encloses its bore level and tilted;
the exact distance over a 96-gon shaft in its bore, concentric and off
centre, against BRepExtrema on the whole shapes, and `ClearanceBetween`
declining a buried box), `test_meshcollide.cpp` (agreement with the exact boolean
on overlapping, clear and enclosed pairs; deflection scaling; the
placement-keyed cache; a pin in a clearance hole reads clear), and the
headless smokes `measure_smoke`, `contact_smoke` (also: `contact_report`
in mesh mode naming the clashes and distances the exact one does, and an
async run reporting progress while the instance answers `ping`, landing the
synchronous report, and ending `cancelled` when cancelled; a test-only
`LOBOLE_TEST_CONTACT_PAIR_MS` holds each async pair so the run is seen
running on any machine), `compare_smoke`,
`query_smoke`, `strict_inputs_smoke`, `papercuts_smoke`,
`sweep_parallel_smoke` (a one-thread sweep and an all-thread sweep give
byte-identical reports). Seals: `test_measure_seal.cpp` (a skirt flat on a
shoulder is a ring as wide as it lands, a notch opens it where it is, a
pressed gasket is a ring with its interference, and two that do not touch
are refused); the jar generator states two and fails a notched skirt. Designed overlaps: `test_thread.cpp`,
`test_retaining_rings.cpp`, `test_press_fits.cpp` and `press_fit_smoke`
([press fits](../tolerances/README.md#tests)). GUI: the `measure/*` and `section/*` cases in the
GUI suite, and the measure parity phase in `remote_smoke`.

## What it does not do yet

- Pick a raw vertex (corners cover the cases people ask for).
- `measure` and `interference` each resolve an occurrence reference (id,
  name or `"Unit/Part"` path) to world-placed bodies with near-identical
  code. They should be one function; interference's version shares a
  prefetched body map with the contact sweep, so merging them is its own
  change with its own test run.
- A face-sourced section on an assembly tab: the face's plane is part-local
  and needs occurrence-aware resolution, so such a cut reports
  `resolved: false` there.
- Exclude components from a section (keep fasteners whole, as drafting
  convention does); the set is derivable — a part whose only feature is one
  Fastener.
- Hatched cut faces (drawings section independently).
- Draft, wall-thickness or curvature analysis. (A printed part's
  overhangs are measured on the B-Rep by `check_print`,
  [../tolerances/README.md#checking-a-print](../tolerances/README.md#checking-a-print).)
