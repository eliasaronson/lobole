# D4 — the SO-101 gripper on Derived ghosts (dogfood record, 2026-09-03)

The acceptance test of [derived-plan.md](derived-plan.md): import the arm's
STEP, derive the wrist servo into a new part, design the gripper body and
the moving jaw on its faces, derive the Pi Camera 3 STEP into the jaw and
mount it, mates, interference and contact reports, a drawing. Driven over
the RPC from `scratch/so101/gripper.py` (every step is a function; the
whole thing rebuilds from `setup`) against an `--offscreen` instance on a
scratch port. Every number below is from that run.

Sources: TheRobotStudio/SO-ARM100 `STEP/SO101/SO101 Assembly.step` (20 MB,
336 k entities; the product structure is flat — one PRODUCT is an assembly,
the six servos are loose parts) and the Raspberry Pi
`Camera_module_3_std_model_simple.stp` (30 MB, 584 k entities, **197
bodies**: every pad, silkscreen sheet and SMD part is a body).

## What was built

| part | its frame | on the ghosts of | its own geometry |
|---|---|---|---|
| Gripper body | the gripper servo's case (`ZK_122`) | servo case, its two covers, the wrist-roll horn (four `derive_occurrence` from the in-context edit), the camera (`add_feature Derived` with a placement) | an L-shaped end plate on the servo's bottom face; two cheeks on the covers' outer faces with four M2 clearance holes **on the covers' tapped holes**; the fixed finger; a flange disc and web on the wrist-roll horn's flat face with four M2.5 holes **on its bolt circle**; four standoffs with M2 tapped holes **on the camera's 12.5 × 21 mm hole pattern** |
| Moving jaw | the drive horn | drive horn, passive horn, servo case | hub disc on the horn's face with four M2.5 holes **on its bolt circle**; the arm on the hub, outboard of the body's cheek; the fingertip bent over to the fixed finger's plane |

Second pass (the same day, after "this looks very messy"): the cheeks run
most of the servo's length with chamfered corners and stop 1.6 mm short of
the jaw hub; the fixed finger tapers from the plate to a flat gripping face
at z = −6; the flange disc is sized to the horn; the hub is 24 mm on the
19 mm horn; the arm is a tapered lever on top of the hub with a fingertip
pad whose face opposes the fixed finger's, 13 mm apart at rest. The first
pass of that redesign ran the cheeks to x = −10 and the pairwise check
reported them against the drive horn (174 mm³) and the hub (161 mm³) —
shortened, jaw–body clear by 1.5 mm. Renders `scratch/so101/50-53*.png`,
the drawing `54-drawing.png`.

Every bold hole pattern is external geometry: `project_geometry` of the
ghost's hole edges into an empty sketch, and a Hole at the projected
circles' centres. No hole coordinate was typed. The only numbers carried
by hand are the servo's bounding box (plate and cheek extents) and the
camera's board-back plane.

Assembly: the old `Wrist_Roll_Follower` (which arrived named "SOLID 6"),
the old moving jaw, its nuts and 15 screws removed; the body fastened
origin-to-origin to the servo case (its frame IS the servo's, `add_occurrence`
placed it there); the jaw revolute on its hub's underside snapped to the
hub's rim edge, against the horn's flat face. Both land exactly on their
sources' transforms.

Screenshots: `scratch/so101/10-12*.png` (body), `20-21*.png` (jaw),
`30-31*.png` (the gripper assembly).

## Timings (second pass, after the fixes below)

| step | time |
|---|---|
| import the arm as an assembly (97 parts, 270 occurrences) | 8.2 s |
| import the camera as a part (197 bodies) | 12.7 s |
| `derive_occurrence`, a small ghost, in context | 1.9–2.9 s |
| a feature add in the body with 4 small ghosts | 1.8–2.8 s (a sketch 0.01 s) |
| `add_feature Derived` of the 197-body camera | 6.3 s |
| a feature add in the body after the camera | 4.5 s (**was 12–17 s**) |
| a feature add in the jaw with the 202-body body as a visible neighbour | 3.9–4.5 s |
| pairwise `interference {a, b}` | 0.1–0.5 s |
| `interference {}` (no pair) over 253 occurrences | never returned in 48 min, killed |
| `contact_report` over 253 occurrences | never returned in 23 min, killed |
| `contact_report` over the gripper's 9 occurrences | 9.0 s |
| `save_project` (187 MB) / first `open_project` / second | 1.5 s / 64 s / 1.3 s |
| `list_elements` on the camera part WITHOUT `body` | 172.6 s; with `body: i` instant |

Where the 12–17 s went (`tools/profile.sh --app`, headless: 3.1–3.9 s of
it): `BodyDigestJson` 46.6 % — every mutating RPC digests every body twice,
and the digest is `BRepGProp::VolumeProperties` on 197 unchanged B-spline
bodies. `Document::Regenerate` was the STEP import; `DerivedFeature::Execute`
0.48 % (its decode cache works). The rest, offscreen only, is
`SyncDocumentRenderers` removing and re-adding every mesh, and — for the
jaw — `SyncInContextGhosts` re-tessellating and re-uploading every neighbour
body on every regeneration (it builds its assets fresh instead of reading
`partRenderCache`). That one is still open.

## The checks, at the end

Pairwise `interference` in the gripper's own assembly (body, jaw, servo
case and covers, the three horns), each in 0.1–0.5 s:

| pair | result |
|---|---|
| body – servo case, top cover, wrist-roll horn | touching |
| body – bottom cover | clear by 0.003 mm (the cover's face is 3 µm inside its bbox) |
| jaw – drive horn | touching |
| jaw – body | clear by 3.87 mm (the first arm went through the cheek: 306 mm³) |
| jaw – servo case / top cover / passive horn | clear by 10.8 / 2.1 / 33.9 mm |

`contact_report` on that assembly: 9.0 s, two clashes, both inside the
vendor's servo model (its case interpenetrates its own top cover by
1007 mm³ and bottom cover by 142 mm³) — none involving the parts designed
here. Before the fix below the same report listed nine, seven of them a
part's ghost against the real thing.

Project: `save_project` 1.5 s, 187 MB on disk (the camera snapshot and 97
parts' baked geometry); the first `open_project` 64 s, the second 1.3 s;
after reopening every Derived is `stale: false`, no feature errors, the
body and jaw volumes unchanged, `check_geometry` has no `stale_derived`.

Drawing: `create_drawing` + four auto-placed views of the body + a note,
exported to `scratch/so101/gripper-body-drawing.svg`. On the first pass the
export's own issue list said the four views overlapped on the A3 sheet;
after the placement fix (finding 18) they export as one row at 1:1 with an
empty issue list.

## What broke, in order

1. **`import_step as_assembly` left 12 of 97 parts broken and said nothing.**
   Eleven `Wiring_holder` products and one LED footprint came out of XCAF as
   "no valid shapes" in their staged brep; the RPC response has no
   `regen`/failed list (the regen-honesty rule for a mutating RPC). Product
   names were lost for some nodes — `Wrist_Roll_Follower_SO101` arrived as
   "SOLID 6", `Wrist_Roll_SO101` as "SOLID 5", unnamed nodes as `=>[0:1:1:28]`.
2. **A part holding a 197-body ghost cost 14 s per sketch add** — fixed
   (volume cache), see above.
3. **Sketch external geometry only looked at the FIRST body a feature
   created** and stopped, so every element of a multi-body Derived copy's
   (or an Import's) second body read "no longer exists". Fixed; found by the
   new hole-from-circle test, not by eye.
4. **Hole took only Point entities.** A projected hole edge is a circle (or
   two half-arcs when the source split the wall, as the camera PCB's are),
   so the plan's "hole centres" proof needed every centre copied from
   `list_elements`. Fixed: circles' and arcs' centres count, coincident ones
   merge.
5. **`add_feature Sketch` refuses an `external` key** (accepted: constraints,
   entities, name, plane). Corrected afterwards: `external` has always
   worked — what it refuses is an EMPTY one, and mine was empty because the
   filter above it had found no edges. The strict-key check round-trips the
   built feature, and an empty list serializes back to nothing, so a real
   field read as an unknown one. Fixed (a key the type's documented dialect
   names is accepted when empty), and the Sketch dialect now describes
   `external` and `face_ref` — that omission is the half of this finding
   that was right, and why `project_geometry` was hard to find.
6. **A mate face connector sits at the face centroid.** A plate wider than
   the servo's end face, fastened face-to-face, shifted the body 7 mm.
   Origin-to-origin is the honest mate when a part was placed at its
   source's frame; `snap_edge` on the hub's rim is what a merged underside
   needs.
7. **`add_mate` with `face: "<name>"`** (a string) leaked
   `cannot use at() with string`. Fixed: the refusal names the
   `{feature_id, element}` contract.
8. **`get_state` bodies did not say `reference`.** Fixed.
9. **I drilled the servo's end face because I read a cylinder's `normal`
   as its axis.** Not a product gap: `list_elements` reports `axis` on
   cylindrical, conical and toroidal faces and always has — checked
   afterwards, and this entry corrected. The projection of the real hole
   edges is what showed the tapped holes run along the servo's thickness,
   through the side covers, which is how the SO-101 holds it; the design
   changed to two cheeks. The ghost caught my error before any part was
   printed, which is the feature's whole point.
10. **A circle and a rectangle crossing in one sketch** are refused
    ("profile loops overlap"): the flange disc and its web are two sketches.
11. **`interference {}` and `contact_report` over 253 occurrences do not
    return in bounded time and block every RPC, including `ping`.** Pairwise
    interference is instant. The gripper's own 9-occurrence assembly is
    what the acceptance check runs on.
12. **`contact_report` on an assembly counted a part's derived ghosts as its
    matter.** The gripper body, carrying a copy of the servo it was drawn
    on, reported a 15.4 cm³ clash with that servo — its own ghost — and the
    jaw the same with the body's. The pairwise `interference` already
    skipped ghosts; the report did not. Fixed, and `derived_smoke` now
    asserts a copy placed on its source reports no clash.
13. `add_occurrence {part: <id>}` leaks `type must be string` — `part` is a
    name (the doc says so; the refusal should).
14. `create_drawing {name: "Gripper body"}` is refused because a part tab
    has that name — a good refusal, quoted here because it is the kind the
    others should be.
15. Derived's `bodies` filter is by `created_by`, and an Import's 197 bodies
    are ONE `created_by`: the PCB and lens alone cannot be derived without
    the 194 pads. Fixed in v85 — `body_names` picks them by name (verified
    on a 131-body adapter STEP: two named bodies out of one Import, in
    0.0 s).
16. **A drawing view projected the ghosts.** The top view of the body ran
    the HLR over the 197-body camera copy and took the RPC past its 60 s
    client timeout. A ghost is not the part; fixed (drawing views skip
    reference bodies; `derived_smoke` asserts no ghost curve).
17. `add_drawing_view`'s doc says `{drawing, source, ...}`; its handler
    refuses `drawing` (it works on the active drawing tab). `create_drawing`
    `sheet` is an object, and says so — a good refusal.
18. **Four auto-placed views overlap on the sheet.** The row-and-wrap rule
    clamps a wrapped row back up to the border, and when the views already
    there reach that low the new one lands on them; a taller iso view
    centred on the first row's midline went over the border the same way.
    Fixed: a placement that overlaps a view, the title block or the border
    re-lays the whole sheet out from the top-left, at the current scale and
    then each standard scale down until it fits (one scale per sheet). The
    body's four views now export with an empty issue list;
    `drawing_smoke` asserts it on an 87 × 43 × 60 mm part on A3.
19. **Every copy of an imported part read stale after the first save.**
    Saving adopts the staged import files into the project directory and
    rewrites each Import's `path`; the path was in the source's chain hash,
    so the hash moved and every Derived from an imported part reported
    `stale: true` — `update_derived` then changed nothing (volume delta 0).
    The camera, imported straight from its STEP, did not. Fixed: an Import
    hashes the file's bytes (cached by path, size and mtime), not its path;
    a moved file is the same part, a different file at the path is not.
    Test: `a moved import file is not a changed source; a changed file is`.
20. **An assembly drew every part's derived ghosts.** The build loop that
    tessellates a part's bodies for the assembly had no `reference` check
    (the parallel prewarm beside it did), so a part that was drawn on a
    ghost put a second copy of that ghost into the assembly, at the copy's
    placement, in the part's palette colour rather than the vendor's. The
    gripper therefore showed two servos and two horns inside each other —
    which is what "why are these two circles offset and of different
    materials" turned out to be. The hub and the horn under it are
    concentric (both at x=0, z=0 in the jaw's frame); what looked like an
    offset was the 3.7 mm the hub sits proud of the horn's face, seen
    down the axis. Fixed; `derived/a_ghost_is_not_drawn_in_assemblies`.
21. **A hole centre that drills empty space removed nothing and said
    nothing.** `regen.ok` was true and the feature carried no warning, so a
    flange whose sketch put every bolt hole 7.6 mm off the part read as
    clean through two rounds. (My bug: a sketch's coordinates are measured
    FROM its plane origin, `world = origin + u*U + v*V`, and I passed the
    horn's world position as both the origin AND the UV centre, doubling
    it. The bolt pattern came from the ghost's own edges and was right;
    the material was in the wrong place.) A Hole now warns, naming the
    centres that meet no material — the case where SOME miss, which is the
    one nothing else reports, because the cut still removes material and
    the body-mode "removed nothing" warning stays quiet.
22. **Four of the arm's own screws were orphans.** Removing the old wrist
    follower and jaw left the four M3 that held them floating inside the
    wrist-roll horn, 45° off its own bolt circle, and the new flange sat
    on their heads (4 × 19.6 mm³). They are removed with the parts they
    held; the interference sweep is what named them.
23. The camera's lens/sensor body is named `ATMEL_ATSHA204A_8UDFN` in the
    Raspberry Pi STEP (the crypto chip's footprint name). Not ours.

## Fixed the same day (`edb28a92` and the follow-up)

- `CADShape::Volume` cache keyed by TShape identity (+ location scale).
- `SketchFeature::RegenerateExternal` looks through every body of the feature.
- Hole takes circle and arc centres, merges coincident ones.
- `add_mate` face-connector refusal; `get_state` `reference: true`.
- `contact_report` skips reference bodies on both the part and the assembly tab.
- Drawing views skip reference bodies, and so does the assembly renderer.
- A Hole warns when some of its centres drill empty space.
- Auto-placed views re-lay the sheet out instead of piling up.
- Import features hash their file's content, not its path.
- Tests: `a hole placed on a derived hole edge, no coordinates copied`;
  `derived_smoke` contact_report assertion.

## Still open after D4

- `SyncInContextGhosts` should read `partRenderCache` (the assembly tab's
  per-part asset cache) instead of tessellating every neighbour per regen.
- A Derived `bodies` filter by body NAME (format bump).
- `import_step as_assembly`: report the parts that failed to regenerate in
  its own response; keep the PRODUCT name when the shape label is "SOLID".
- `interference {}` / `contact_report` on hundreds of occurrences: a bbox
  prefilter and the A3 job registry (off the RPC thread, `ping` answers).
