# Inspection

The "is it right?" toolset: measuring, mass properties, looking inside,
checking for clashes and floating parts, checking the B-Rep, and comparing
against a reference. Every inspection a person does by clicking, an agent can
do over RPC with the same numbers, and every verdict says how it knows.

Core code is `src/core/Measure.{h,cpp}` (measurement, contact, sections,
comparison, geometry checks) and `src/core/MeshCollide.{h,cpp}` (triangle
clash tests). A part's mass and the area a plane cuts through it are
`src/core/PartMeasures.{h,cpp}` (`partmeasure::MassOf`,
`partmeasure::SectionOf`, `partmeasure::ResolveBodies`), on a
`const Document&`: `mass_properties` on a part and `section` answer with
them, and a [requirement](../requirements/README.md#measures) and a
[design sweep](../requirements/README.md#design-sweep)'s point read them,
so the three agree. The RPCs are in `src/remote/RemoteInspection.cpp`; the measure
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

## Mesh bodies

A [mesh body](../interchange/README.md#mesh-bodies)'s triangles are its
geometry, so what is measured on it is exact to the mesh
(`src/core/MeshMeasure.{h,cpp}`).

- **On its own**, `measure {a: {feature_id}}` (or its one element) gives
  `kind: mesh`, the triangles, components, `closed`, `surface_area_mm2`,
  `size` and `boundary_loops`, and `volume_mm3` when it closes. When it is
  open the volume is null and `volume_refused` names the loops and where to
  close them (MeshLab, Blender, the scanning app). `list_elements` lists its
  one element with the same statistics and the median edge length, and
  never trips `max_faces`.
- **A point on a body** is `{feature_id, point_mm: [x, y, z]}` on either
  side of `measure`: snapped to the nearest surface point (a mesh's by its
  tree, a B-Rep's exactly), and the reply's `snapped_a` / `snapped_b` give
  the snapped point and `moved_mm`. Two points measure point to point.
- **Between a mesh body and anything else** the minimum distance comes from
  `mesh::Bvh`; a B-Rep side is tessellated on a copy at a thousandth of its
  size, and that deflection is the reply's `tolerance_mm` (0 between two
  meshes). Only the minimum and `kind: distance` are answered; anything
  else is refused, naming them.
- **`mass_properties`** uses the divergence theorem over the triangles
  (`mesh::ComputeMassProperties`: volume, area, centroid, inertia) for a
  closed mesh, and refuses an open one naming its loops.
- **`interference`** between two bodies of a part, one a mesh, intersects
  their triangles with manifold (the B-Rep side tessellated as above), so
  the volume is exact to the mesh; `mode: mesh` and `tolerance_mm` say so.
  An open mesh is refused.
- **The Measure tool** keeps the point clicked on a mesh body (a `Point`
  measurable, like a corner), and measures point to point or point to
  anything.
- **`check_print`** reads a part of mesh bodies off its triangles
  (`printcheck::CheckMeshes`): the bed by the lowest vertex, the shells by
  the closed components, the overhang by each triangle's normal (a ceiling
  counts as 90°), the clearance between bodies by their trees, the bed fit
  by the vertices. Bridges, stacking and slender read a B-Rep's surfaces and
  are skipped saying so; chords do not apply. An open mesh body is refused
  for printing, naming its holes and where to close them. A part mixing
  mesh and B-Rep bodies is refused: check either set with `bodies`.

## Datums

A scan is measured through the datums fitted to it
([datums on a scan](../modeling/README.md#datums-on-a-scan)): a side of
`measure` may be `{datum: <id or name>}`, a DatumPlane or DatumAxis of the
part (on its tab, or with `part`), measured exactly as the plane or line it
is (`MeasureDatums`, `src/remote/RemoteInspection.cpp`).

- **On its own**: its `kind` (plane or axis), `origin_mm` and `normal` or
  `direction`.
- **Against a bare point** `{point_mm: [x, y, z]}` (a point in space, not
  snapped to a body): the distance to the plane or line, the foot of the
  perpendicular as the closest point, and for a plane `signed_mm` (positive
  on the normal's side). `kind: distance` only.
- **Against another datum**: `angle_deg` -- two planes between their
  normals (0-180), two lines unsigned in 0-90 (an axis's sense is
  arbitrary), a line against a plane 0-90 -- and `basis` saying which;
  the minimum distance (0 where they meet or cross, the offset when
  parallel) with the closest points; and `parallel`.

A face, edge or body against a datum is refused naming the sides that are
accepted, as is a datum with an `occurrence` (in an assembly, `frame`
`{occurrence, frame: <datum>}` gives it placed in the world).

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
(and every mutating reply's `digest`) is a cheap conservative bound that can
read well over on a swept surface, and several times over on a STEP whose
faces are B-splines ([known bugs](../known-bugs.md#get_state-and-the-digest-report-a-steps-box-several-times-its-size)).
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

A [mesh body](#mesh-bodies) is cut by its slice, so a scan's cross-section
area and outline are numbers too; an open one's cut does not close, adds no
area, and is named in `open_meshes`.

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
  the openings (a house's outer walls enclose nothing at the height of its
  windows, so its floor-area requirement cuts above them).
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
- **To sketch** (part tabs) starts a new sketch on the active cut's plane
  with the cut drawn in it, lines, lines and arcs, or splines as the combo
  beside it says, and opens it for editing: the same section
  `project_section` adds ([a section into a sketch](../sketching/README.md#a-section-into-a-sketch)).
  A cut that crosses no body is refused in the window and makes nothing.
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
  128 bytes (`kMaxClipPlanes`, `src/render/ClipPlanes.h`). A `project.json`
  listing more is refused on open: the app never writes one.

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

### Insertion

A part must be able to reach where it is mated. The motion studies start
with every part already seated and the clash checks look at it there, so a
wheel whose press-D bore could only reach its D by being forced over a
round pivot -- the printed clock's escape wheel, which bound halfway
through its hub -- passed every check. `check_insertion {a, b, entry}`
(assembly tab, top-level occurrences by id or name) slides `a` along an
axis from clear of `b` to where it is placed, and fails the slide that
forces something through on the way.

- **The axis** is the Z of a mate's frame: `mate` (an id or name; a
  revolute, cylindrical, slider or fastened mate), else the one mate
  joining `a` and `b`, else an explicit `axis {origin_mm, direction}`.
  `entry` is the side `a` goes on from: `positive` (from the side the
  axis points to), `negative`, or `either` (both tried, the better
  reported). An unknown entry, a mate with no axis, a pair no mate joins
  with no axis given, or both `mate` and `axis` is refused.
- **By sections.** Each side is cut into **bands** along the axis over
  which its cross-section does not change (core/MeasureSections.h): an
  extrusion, a bore, a straight tooth band is one band whatever its
  length, its outline the B-rep's own planar section. Where a face is not
  along the axis -- a chamfer, a fillet, a helical flank -- the span is
  sliced `slice_mm` thick (the travel over `samples`, 64 by default, and
  no thinner than 20 µm) on a grid anchored at the seat: only the grid
  cuts such a span, and each slice is sectioned at its grid cell's middle
  (or as near it as a span's end slice reaches), so the two sides compare
  level sections where the part ends up. A slice's reading holds within
  half a slice of where it lines up with the other side's, not over the
  whole overlap of their spans. Where the faces are free-form (a twisted
  flank, a modelled thread) the slices are cut from one triangulation of
  the solid, 0.5 µm or coarser as the free-form area grows -- a boolean
  section of a twisted flank is seconds a slice -- else from the B-rep's
  own exact sections. Two bands
  interfere or not whatever the slide: one 2D polygon question (Clipper2)
  -- an overlap thicker than 1 µm, or, with `allowance_mm`, outlines
  nearer than how much they close up as made. A printed bore prints under
  its model (print_allowance's hole allowance, a side), so a light press
  modelled 0.05 a side oversize is a press as printed, and a bore at the
  running gap is not; `allowance_mm` 0 reads the model as modelled. The
  allowance is a polygon offset **in the section's plane** -- how a
  printer's XY closing acts on a part printed along its axis -- so a
  helical or conical face's gap is its transverse one, wider than the
  normal gap by 1/cos of its lean: the planetary's planet reads a 0.097
  normal clearance to its ring as a transverse one at the 0.1 allowance.
- **The slides** at which two bands' spans overlap are one open interval
  from their ends, so the travel under interference is a union of
  intervals: exact, with no step. Faces that meet end to end -- a hub on
  its arbor's shoulder -- have no length between them: a **contact, not a
  press**, however the allowance closes them up.
- **The press** is the axial length over which the seated pair
  interferes. A part pressed on is pushed under interference exactly that
  far; the check **fails** a slide whose travel under interference is
  longer than its press plus `lead_in_mm` (a step, the travel over
  `samples`, by default). A round pivot passed through a D bore, a press
  band that runs on past the seat, or a bore too small for the whole shaft
  all fail; a press band that ends where the part goes on, or a pivot at
  the running gap under the bore, passes. Travel spent interfering
  **deeper** than the seat itself does (by more than half the allowance)
  fails past the lead-in too: the part is forced harder than the press it
  ends in, however long that press reads (`deeper_mm`).
- **Rotation** about the axis is free while sliding: when the placed
  orientation fails, `rotations` (12 by default) others are tried and the
  best reported, as `rotation_deg` from the placed one (a keyway past a
  lug). A turn is the outlines turned: no new section. A turn either side
  is symmetric under is skipped, and one that does not seat -- it
  interferes at the seat more than the placed part does, a D turned off
  its flat -- is reported (`seats: false`) and never chosen. A try that
  can no longer beat the best is abandoned.
- **One exact boolean confirms it**: at the reported try's worst offset
  (the seat when nothing interferes on the way), the clash check's 1 µm
  fuzzy, non-destructive boolean says whether there is matter in matter,
  and where the sections read only a gap under the allowance the exact
  least distance says so too (`confirm` {`at_mm`, `expected_overlap`,
  `expected_gap`, `overlaps`, `volume_mm3`, `gap_mm`, `agrees`}). A pass
  the boolean contradicts -- matter where the sections read none -- is
  not a pass; the sections reading more than the boolean finds (a slice
  standing for a chamfer's whole span) is the safe side, said in `agrees`
  and the log but not failed.
- **Bounded.** The mesh a solid is sliced from is refused past 6 M
  triangles, a solid cut past 20 000 bands, and a check that would read
  past 4 M band pairs -- each naming the ceiling -- and only the seat's
  orientation and one other are kept at a time; a sequence's kept
  sections (below) stop at 40 M points of outline, the least recently used
  dropped first. The engine's 68-step sequence peaks at 0.9 GB for the
  whole instance. Without these a whole-solid mesh at 0.5 µm and every
  orientation's outlines kept took an instance to 22 GB.
- **A screw turns by its helix.** A part going on by a modelled thread
  turns a turn a pitch of its offset from the seat, the other way about
  the axis when it comes from the other side; its steps are one length on
  both sides. (Spread over the longer side's travel and read along the
  shorter, the jar's lid read 7.3 mm forced through the neck's flanks.)
  A turned path has its one orientation and tries no other.
- **A threaded pair is refused**: when both sides carry modelled threads
  on the path the part goes on by turning along the helix, which a
  straight slide reads as the flanks forced through each other the whole
  way (the jar's lid). A thread elsewhere on either side -- a pillar's
  beside an arbor's path -- does not count; a modelled thread, whose
  marked face the helix replaced, is found by its helical faces.
  An [assembly sequence](../assemblies/README.md#assembly-sequences)'s
  step can say a part goes on by its thread, which judges the pair by the
  thread's own fit instead.
- **A side with nothing to slide is refused by name**: a hidden
  occurrence, or one whose part has no bodies -- a frames-only part like
  the planetary's kinematic carrier, which still gives its frames to mate
  on when the project is opened again. The RPC and the requirement both
  say which side and why, rather than "no visible matter".
- **Off the frame.** The call is read and refused on the frame, then runs
  as an `insertion` job (`background_jobs`, cancellable, with a fraction)
  while the call waits for it; the frame never waits on it, and the
  instance answers other clients meanwhile (`insertion_smoke`).

What it costs, before (an exact boolean a sample, every orientation) and
after (sections), on this machine -- `seconds` {`sections`, `pairs`,
`confirm`} in the answer says where it went:

| pair | before | after |
|---|---|---|
| the counter's wheel 1 in its housing (allowance 0.1) | 33 s | 1.1 s |
| a planetary planet in its herringbone ring (0.1) | 728 s | 4.2 s |
| the clock's stage 2: its 21 insertion requirements | 23 s | 4.6 s |

Most of the planet's 4.2 s is the one confirming boolean of two
herringbone solids.

**An assembly sequence keeps its sections** (`MeasureSections.h`,
`BandCache`): every step slides one part past mostly the same placed
parts, along a handful of directions, and cutting them again for each
step was nearly all of its time. A solid is cut once per direction and
slice thickness, in that direction's own frame -- the axis through the
origin, the reverse direction sharing the cut, mirrored -- and a later
step moves its bands to that step's axis and reach instead (a 2D shift
and turn, a clip to the reach). A part moved since by a pose is moved
with them when it only turned about the path and, if any band is a
slice, moved along it a whole number of slices; otherwise it is cut
again. Steps along one direction share one slice thickness where their
travels allow (from a ladder of 20 µm doubled, never thicker than the
step's own). A step whose part sweeps a short stretch of a long solid
cuts that stretch -- its own span and the travel the way it comes, a
slice to spare -- and a later step reuses it when its own stretch lies
within. A solid outside it meets no moving band at any slide a try reads,
so cutting more changes no answer, only the cost: the clock's bob step
cut three times its stretch and took 14.5 s, against 1.3 s now. The
same sequences, before and after, on this machine:

| sequence | before | after |
|---|---|---|
| the engine's, as built (68 steps, a group a cylinder, two poses) | 559 s | 74 s |
| the engine's first draft (78 steps) | 480 s | 97 s |
| the clock's stage 2 (33 steps, one group) | 212 s (36 steps) | 66 s |
| the Strandbeest's leg pair put together (37 steps, one pose) | 114 s | 19 s |

**A check that keeps nothing still cuts only what it can reach.** A
turned or screwed path cuts its slices a step thick, its own, so its bands
are not kept; it, and a check given no cache (`check_insertion`), cuts the
fixed side only over the stretch of the axis the moving side can come near
-- its own span, the longest travel either way it may come from, two
slices to spare -- as the kept path does. A fixed band outside that meets
no moving band at any slide a try reads, so the answer is the one a whole
cut gives. The clock's rating nut screws onto the foot of the pendulum
rod, whose line runs up through the movement: cutting every plate and
wheel along it was 93 of the step's 101 s, and the step is now 12 s
(sections under a second, 262 fixed bands for 3058), its stage's
sequence 163 s to 84 s, every step answering as before.

**A path whose reach finds faces but no matter is still a check.** The
travel comes from the fixed side's span over the faces whose box comes
within reach; the bands keep only the matter that does. When none does
-- the clock's centre spacer, going onto the back plate's centre beside
the third wheel's rim, whose face's box reaches the path and whose teeth
are 10 mm from it -- no band survives, and the check still runs: its
tries read no pair and pass at that travel, and the boolean confirms the
seat. Answered as clear instead (travel 0, no confirm), the answer would
turn on whether a solid the part never reaches was cut: the plate 0.4 mm
under the spacer's seat. Only a side with no face within reach at all is
clear without a try.

The answer: `pass`, the try reported (`entry`, `rotation_deg`,
`travel_mm`, `interfering_mm`, `press_mm`, `excess_mm`, `deeper_mm`,
`from_mm` -- where
the interference begins, from the seat -- `worst_depth_mm`, the most the
faces must give with the allowance counted, and `worst_volume_mm3`), the
step, slice, lead-in and allowance, the bands each side was cut into and
the pairs tested, the confirmation, the axis and what gave it, and every
try. The requirement kind `insertion`
([requirements](../requirements/README.md#measures)) states a pair once.

What it does not judge: a screw (above), the order parts go on in (each
pair is slid alone, against the one part it goes on -- an [assembly
sequence](../assemblies/README.md#assembly-sequences) puts them together in
order, each against everything before it), or the force a press takes.

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
boolean, sweep or direct-edit feature runs the same check on its own
result and fails, naming the reasons, rather than handing an invalid solid to
every later measurement — `CADShape::IsValid` is only a null check.

## Finding and naming geometry

- `list_elements {body?, part?, max_faces?, made_by?, role?, entity?, via?, walls?}`
  (`walls: true` adds each body's `thinnest_wall_mm`)
  — stable face and edge names per body, the handles `measure`, fillets and
  mates use, each with the feature that made it (`made_by`) and its `role`;
  the filters ask for a feature's faces in a role (a pocket's floor), and an
  empty answer says `why` ([toponaming](../modeling/toponaming.md)).
  `unnamed_faces` counts faces the element map never named; zero is what
  makes the list complete. A piece of a split face or edge carries `piece`
  {`of`, `bounded_by`, `ordinal`}, `ordinal: true` when its name is by
  position ([split pieces](../modeling/toponaming.md#split-pieces)).
  An edge is `smooth` when there is nothing to blend there: its two faces'
  outward normals agree at five points along it (G1, to 1e-4 rad; two planes
  at one), or it is
  a seam or degenerated (`Measure::EdgeIsSmooth`). A planar face carries
  `inside_point`, a point ON it, and `inset_mm`, that point's exact distance
  to the face's boundary (`Measure::FaceInsetOf`: an 8 x 8 sample of its UV
  box, 32 x 32 when that finds nothing inside, refined three times about the
  best, keeping the sample furthest from a
  polyline of the boundary that the face's classifier puts inside; then the
  exact distance to each boundary curve). The centroid is not such a point:
  an annulus's lies in its hole, an L's off its corner. Other faces carry
  neither: the sampling on a thread's spline faces cost more than the rest of
  the listing.
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
- `grow_faces {faces}` — the faces of the one hole, boss or pocket the
  given faces belong to, with the names it `added` and the `host_normal` of
  the face it sits in; refused, saying why, when no inner loop bounds them.
  Changes nothing ([Delete Face](../modeling/README.md#delete-face)).
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
(`BRep_Builder::UpdateEdge`) instead of on a copy. A modelling feature builds on
a copy of its cached operands
([modeling](../modeling/README.md#kernel-calls-build-on-a-copy-of-the-cached-body)).
For a measurement it is a write to the document.

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
are refused); the jar generator states two and fails a notched skirt.
Insertion: `test_measure_insertion.cpp` (a D bore forced over a round
pivot to a D that stops short of the end fails at every orientation, a D
run out through the end the part goes on from passes pressed its own
length, a pivot at the running gap under the bore slides through it, a
plain press of its own length passes and the same bore mid-shaft fails by
the shaft it is forced along, a keyway passes a lug only turned half round
and says so, a path whose reach finds a rim's box but not its matter
answers the same kept or not, with a plate under its seat or without) and `insertion_smoke` (the RPC's axis from the joining mate,
a named mate and an explicit axis, the requirement passing and failing,
and the refusals); the clock generator states one on every pressed or
slid pair and fails its first stage-1 escape arbor. Designed overlaps: `test_thread.cpp`,
`test_retaining_rings.cpp`, `test_press_fits.cpp` and `press_fit_smoke`
([press fits](../tolerances/README.md#tests)). GUI: the `measure/*` and `section/*` cases in the
GUI suite, and the measure parity phase in `remote_smoke`.

## What it does not do yet

- Measure a face, edge or body against a datum, or a datum of an
  occurrence in world space (`frame` places one; `measure` does not yet).
- Pick a raw vertex (corners cover the cases people ask for).
- Say where a cylinder face's axis is: `list_elements` and `measure` give
  its direction and radius, and its centre takes a `distance_kind:
  centres` measure against something else.
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
