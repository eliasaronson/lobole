# The document format: contract and version history

This page is the **contract**: what a saved document may rely on across
versions, and what changed at each version. The field-by-field truth is the
code — `src/core/io/SerializeFeatures.cpp` (`FeatureToJson` /
`FeatureFromJson`), `SerializeAssembly.cpp`, `SerializeStudies.cpp`,
`SerializeConfigurations.cpp`, `SerializeCam.cpp`, and the envelope and
directory layout in `Serialization.cpp`. How a project is laid out on disk,
baked geometry and autosave are in [README.md](README.md).

## Envelope

```json
{
  "format": "designer-document",
  "version": 135,
  "parameters": [ {"name": "width", "expression": "40", "comment": "…"} ],
  "features":   [ {"type": "Sketch", "id": 1, "name": "Sketch 1", "…": "…"} ]
}
```

- `format` must be exactly `designer-document`; anything else refuses to load.
- `version` newer than the build's `kVersion`
  (`src/core/io/SerializationInternal.h`) refuses to load ("format version
  189 is newer than this build reads (188)"). There is no forward
  compatibility. Older versions load through the loader's migrations. A
  version that is not a whole number, one older than its kind of document,
  and a part holding a feature type newer than the version it claims are
  refused too.
- `kind` discriminates the document: absent or `part`, `assembly`, `drawing`,
  `job`. Each loader refuses a document of another kind, naming it.
- `parameters` precede features: expressions evaluate against them during
  the regeneration that follows the load.
- `features` is ordered: it IS the regeneration history.
- Optional envelope keys, each written only off its default so older
  documents re-save byte-identically: `rollback`, `commit_geometry: false`,
  `generator`, `catalogs`, `sheet_metal`, `tube`, `print`, `units`,
  `tolerances`,
  `active_configuration`,
  `assets` (base64 blobs, single-file form only), and the project's
  `tables`, `requirements` and `templates`, which a lone part's file
  carries for its project.

## Stability guarantees

A file may rely on exactly these:

1. **Feature ids** (`id`, uint64) are unique within a document and never
   reused, including across delete and undo. Cross-feature references
   (`sketch_id`, `target_id`, `source_id`, `parent_id`, `plane_ref`, …) are
   by id, never by index. Files without ids (v1) get them assigned on load,
   in list order. Every id space starts at 1; saving an item whose id is 0
   refuses (`IdForFile` in `Serialization.cpp`), because in the directory
   form the id is the filename and a `0-…` file reads back as "no id".
2. **Sketch entity ids** (uint32) are unique within their sketch and survive
   entity deletion. New entities get a random id
   (`SketchFeature::MintEntityId`), not the next integer, so two people
   editing one sketch on two branches do not mint the same id; older files
   keep the small ids they have. The loader refuses a sketch with two
   entities sharing an id. The datum block (`0xFFFFFF00` origin, `+1` X axis,
   `+2` Y axis) is reserved. Constraint refs are `[entity_id, which]`; pre-v4
   files used indices and are migrated.
3. **Element names** (topological naming) are derived: they are not part of
   the recipe. Features that consume them (`Fillet`/`Chamfer` `edges`, a
   sketch `face_ref`, mates, drawing dimensions) store a
   `{feature_id, element}` pair, and the name re-resolves against the element
   map rebuilt on every regeneration. The baked-geometry cache stores element
   maps beside its bodies, as derived data keyed by the chain hash.
4. **Expressions.** Every dimension field `x` that takes an expression has an
   optional twin `x_expr`. When present it re-evaluates on each regeneration
   and wins over the stored literal, which is kept as the last evaluated value
   so a file loads meaningfully even when a variable is gone. Angles are in
   degrees; bare numbers are in base units (mm). A numeric literal may carry a
   unit suffix (`10mm`, `2cm`, `1m`, `1.5in`, `1ft`, `45deg`, `1rad`),
   converted at parse (`UnitFactor` in `src/core/Expression.cpp`). Unit names
   cannot be parameter names.
5. JSON key order is not significant; writers emit sorted keys (nlohmann's
   default object) so diffs stay readable.
6. **Every key is one a reader knows.** A key no reader of its object
   knows is refused, naming it and the keys that are accepted; so is a key
   written twice in one object. A file is never loaded past what it says,
   because the next save would drop it. So a key a format change adds comes
   with a version bump, which an older build refuses as a whole
   ([what a load refuses](README.md#what-a-load-refuses)).

## Migration policy

Every format change ships, in the same commit:

1. a **loader migration** in the `…FromJson` / `Load` path. There is never a
   save-side compatibility mode: the writer always writes the current version.
2. a **golden file AT the new version**, frozen: `tests/golden/` for a part
   document, `tests/golden/assembly/`, `tests/golden/job/` or
   `tests/golden/drawing/` for those kinds. Each `NAME.json` pairs with
   `NAME.expect.json` (`{features, bodies, volumes}`, volumes to 1e-3
   relative). Goldens are never regenerated by tooling: a failing golden is a
   broken migration, not a file to update.
3. a **row in the version history below**, which is the one place the
   history is written: no comment block beside `kVersion` repeats it.

### A bump takes its number when it lands

Branches are built in parallel, so a branch never chooses the number: two
that both took "the next one" collided on every line that carries it. A
branch writes the placeholder `vNEXT` instead (`vNEXT2`, `vNEXT3` for a
second and third bump on the same branch), and the merge numbers it:

- in `src/core/io/SerializationInternal.h`, raise `kVersionPending` (0 on
  main) by one per bump. `kVersion` is `kVersionLanded + kVersionPending`;
  `kVersionLanded` is main's number and only the merge edits it. A migration
  compares against `kVersionNEXT` (`if (version < kVersionNEXT)`), which is
  `kVersionLanded + 1`.
- name the golden `vNEXT-<name>.json` + `.expect.json`, with `"version"`
  the branch's `kVersion` (so the gates pass on the branch), and name it so
  in the test code that loads it.
- write the history row in [Pending changes](#pending-changes), first cell
  `vNEXT`, its text saying "Golden `vNEXT-<name>`"; write `vNEXT` in any
  code comment or doc that names the version.

`tools/merge-branch.sh` runs `tools/format-version.py land` after the
cherry-pick, in one commit of its own: it gives the k-th pending bump
`kVersionLanded + k`, replaces `vNEXTk` and `kVersionNEXTk` on the lines the
branch added, renames the goldens and sets their `"version"`, moves the
pending rows into the history and resets `kVersionPending` to 0. Because a
branch edits only `kVersionPending` and the pending table, and a landing
returns both to their state on main, a second branch's bump cherry-picks
without a conflict. A branch that edits `kVersionLanded` itself is refused.

The gates:

- `tests/test_golden.cpp`, `Golden corpus loads, regenerates, and
  round-trips`: every golden loads, regenerates to its `.expect.json`, saves
  at the current version and reloads to the same geometry.
- `The golden corpus reaches the current kVersion`: the newest golden across
  all four directories is at `kVersion`.
- `The version history covers every kVersion`: parses this page. A history
  row whose first cell is an integer counts; the highest, plus the pending
  rows, must equal `kVersion`. Keep the first cell a bare version number.
- ctest `format_version_gate` (`tools/format-version.py check`): the header's
  arithmetic, the history reaching `kVersionLanded`, the pending rows being
  exactly `vNEXT`…`vNEXT<kVersionPending>`, every golden's `"version"`
  matching its name, and no placeholder beyond what is pending anywhere in
  the tracked tree (the files that describe this rule are exempt).
- CI runs `tools/format-version.py check --landed` on main: nothing may be
  pending there, so a branch cherry-picked by hand without `land` goes red.

The corpus is a sample of eras, not one file per bump: many early versions
have no golden. Read the table as the record of bumps.

## Version history

Rows marked "…" summarise a range whose individual changes were not recorded
here; the golden corpus and git history cover them.

| v | Change |
|---|--------|
| 1 | First JSON format: Box/Sphere/Cylinder/Import, no ids |
| 2 | Stable feature ids; Sketch + Extrude (by `sketch_id`) |
| 3 | Sketch constraints (PlaneGCS) |
| 4 | Stable sketch entity ids (earlier constraint refs were indices; migrated) |
| 5 | Rectangles become four lines (the rigid `rect` entity is expanded on load) |
| 6 | Construction geometry; more constraint kinds |
| 7 | Arcs; trim |
| 8 | Sketch derived ops (linear/circular pattern, `derived_ops`) |
| 9 | Ellipses |
| 10 | Per-feature `material` slug. An unrecognised non-empty slug is refused on load, naming the accepted set |
| 11 | Document `parameters`; `*_expr` expression twins |
| 12 | `DatumPlane` feature; sketch `plane_ref` |
| 13 | `Revolve`, `DatumAxis`, `AxisRef` |
| 14 | `Boolean` feature (`target_id` / `tool_id`) |
| 15 | 3D `Pattern` feature |
| 16 | Sketch `face_ref` (an ElementRef) |
| 17 | `Fillet` / `Chamfer` on ElementRef edges |
| … | v18–v22: extrude end conditions, suppression, shell, loft/sweep, hole, draft |
| 23 | Assembly documents: `kind: "assembly"` and an `occurrences` list |
| 24 | Assembly `mates` (connector frames + type: fastened / revolute / slider) |
| 25 | Cylindrical, planar and ball mates (optional `dx` / `dy` / `ball_rotation`) |
| 26 | Mate connector `anchor.snap_edge` (a hole-centre frame) |
| 27 | Assembly `exploded` views |
| 28 | Unit-suffixed expression literals. No structural change; the bump makes an older build refuse such a file instead of failing each expression |
| … | v29–v33: FEM studies, drawings, configurations, modal and gravity studies, topology studies |
| 34 | Extrude / Revolve `regions` (profile loops selected by boundary entity-id sets; absent = the whole sketch) |
| … | v35–v37: linked copies, thread and rib, point–line distance dimensions |
| 38 | Sketch entity coordinate expressions (`<field>_expr` on line `ax/ay/bx/by`, circle and arc `cx/cy/r`, point `x/y`) |
| … | v39–v51: split, sketch B-splines, 3D curves, sweep along a curve, pipe, reference images, refine, structured import, patterns and mirror |
| 52 | Extrude `end_mode: "through_all"` (cut only; past every body along ±normal per `reversed`; `distance` ignored) |
| 53 | Drawing dimension kinds `hole_callout` and `pattern_callout` |
| … | v54–v57: sub-assemblies, reference-image changes |
| 58 | CAM setups: stock, work origin, `rotation_a`, tools, operations |
| 59 | CAM operation `type: "adaptive"` |
| 60 | CAM setup `fit_arcs` (default true; false posts curves as line moves) |
| 61 | Expression-driven Box/Sphere/Cylinder dimensions |
| 62 | CAM operation `side` written for clearing operations too |
| 63 | CAM setup remembers the face its Z zero was picked from |
| 64 | Document display `units` (length / mass / angle) |
| 65 | Mate connector `anchor.snap_edge2`: a corner named by the two edges meeting there |
| 66 | Drawing `notes` (text, optional leader, per sheet); CAM tool `tool_type` / `tip_angle`; the `edge_round` operation (`edge_size`) |
| 67 | CAM setup fixture `frame` and `has_rotary_axis`, split out of `rotation_a` |
| 68 | CAM work origin derived from a named corner (`origin_at`) and subtracted by the post; fixed-size stock (`stock.mode` / `size` / `place`) |
| 69 | `Fastener` feature: `standard`, `size`, `length`; every other dimension read from the vendored table; an unstocked size is refused |
| 70 | Seven fastener standards; `drive` defaults to the one its standard uses; drawing dimension `fastener_callout` |
| 71 | `Hole` `fit: {thread, kind: close/normal/loose/tapped, pitch?}`; the diameter is derived from it and still written |
| 72 | Occurrence `attached_to` + placement in that occurrence's frame (a rider re-seated after the solve) |
| 73 | CAM setups move out of the part into a job document (`kind: "job"`, a `parts` list by stable project id, plus `cam`). A part no longer reads or writes `cam`; a v72 part's setups are not migrated. CAM goldens live in `tests/golden/job/` |
| 74 | A job holds several parts: a machining selection names its `part`; a setup's `placements: [{id, part, pos?, rot_z?, enabled?}]`. A part placed twice is refused (`cam::PlacementRefusal`) |
| 75 | Setup `order`: `authored` (default) \| `tool` \| `part`; unknown values refused |
| 76 | A placement may carry its own fixturing `frame` (quaternion) |
| 77 | A setup embeds its machine: `cam[].machine` `{name, travel?, post, fit_arcs?, has_rotary_axis?, rapid_feed?, tools?}`, a copy of a profile under `~/.config/lobole/machines/<slug>.json`. The old per-setup `post` / `fit_arcs` / `has_rotary_axis` are still read. Unknown posts refused |
| 78 | Setup `split_output`: one program per tool, `<project> - <setup>-T<n>.nc`; forces tool order |
| 79 | Drill `peck` depth |
| 80 | Profile / pocket `corner_relief`: `dogbone` \| `tbone` |
| 81 | Profile `lead_arcs` |
| 82 | Profile / pocket `finish_tool` + `finish_stock`: rough and finish from one operation |
| 83 | Assembly `relations`: `gear` / `rack` / `screw` / `linear` couplings between two mates' DOFs, `dof_b = ratio * dof_a + phase`. Unknown kinds or DOFs refused |
| 84 | The `Derived` feature: `source_part`, `source_name`, `bodies?`, `placement`, `reference`, and a `snapshot` whose bodies are content-addressed document assets `derived-<hash>.brep`. Element names on the copy are `<derived id>/<source created_by>/<source name>` |
| 85 | `Derived.body_names?`: pick source bodies by name as well as by `created_by`; a filter that selects nothing is refused |
| 86 | The `Frame` feature (`kind: explicit` \| `from_element` \| `offset`) and the mate anchor `{frame: <id>}` |
| 87 | Assembly `motion` studies: `driver_mate`, `driver_dof`, `from` / `to` in the DOF's unit (radians or mm), `samples`, `check_clash`, `clash_mode`. The result is transient; `motion/` in the directory form |
| 88 | Relation kinds `chain` (ratio keeps its sign) and `cam` (a periodic `lift` table `[[angle_deg, lift_mm], …]`, forward-only) |
| 89 | Drawing view `quality`: `exact` (default) \| `fast` (polygonal HLR, no source refs, cannot be dimensioned). Anything else refused |
| 90 | Drawing view `hidden`: occurrence ids left off that view |
| 91 | CAM setup `reach_offset` (where the bed is on the sheet; `"auto"` over RPC derives it) and `stock_max` as a fourth `origin_at` |
| 92 | CAM tool `chipload_min` / `chipload_max` (mm per tooth); absent falls back to a diameter rule. A half pair or reversed band is refused |
| 93 | Engineering `catalogs` with pinned revision / content fingerprints; reference-only records omit data rows. In `project.json` for a multi-document project |
| 94 | Part-local generated component ownership: namespace, key, input fingerprint, last recipe hash |
| 95 | Bounded thread engagement (`allow_engagement`, `minimum_engagement`); screw relations reference their thread (`thread_part`, `thread_feature`) |
| 96 | CAM profile `drilled_bores`: an inside cut on a level face cuts every opening in it, leaving round openings up to this diameter to the drill. Absent (0) cuts every opening |
| 97 | Import `keep_origin`: keep the file's own coordinates. Absent = centred on the combined bounding box |
| 98 | `tolerances`: `{id, ref: {feature_id, element}, class}` or `{lower, upper}` in mm, plus `comment`. Outside the regen chain hash; one per element. A ref that no longer resolves is kept and reported unresolved |
| 99 | Tolerance `ref2`: the tolerance is on the distance between two elements; takes `lower` / `upper` and refuses an ISO class |
| 100 | Assembly `assertions` (declared clearances): `{name, a, b, min_mm, mode: mesh\|exact, why?}` plus the last `verdict`, `measured_mm`, `checked_key`, `note` |
| 101 | Assembly configurations: `suppressed_mates`, `suppressed_relations`, `mate_overrides` `{mate, rotation? (radians), offset? (mm)}` |
| 102 | A cam relation's follower may turn: `dof_b: rotation`, lift column then in degrees. No new field; the bump makes a v101 build refuse the file |
| 103 | Import `units` (`mm` \| `cm` \| `m` \| `in`) for STL and BREP; refused on STEP; unknown strings refused |
| 104 | Relation kind `freewheel`: `drives` (`positive` \| `negative`, required) and `lead` |
| 105 | Relation kind `click` (a pawl on one mate); freewheel `teeth` and `ride: {amount, mate, dof}` |
| 106 | FEM studies on an assembly: `studies` with `participants [{occurrence, role: meshed\|rigid\|ignored, mesh_size?}]`; loads carry `occurrence`; a fixed load may anchor on `contact_with` |
| 107 | Participant role `bonded` |
| 108 | Sheet-metal rules `sheet_metal` `{material, gauge, thickness, bend_radius?, k_factor?, relief?, relief_width?, relief_depth?}`; part of the regen chain hash |
| 109 | `BaseFlange` and `EdgeFlange` (`position`, `length_mode`, `relief`; each enum refused by name) |
| 110 | `FlatPattern` `{body_id, fixed_face?}` (replaces the body's shape); part configuration `unsuppressed_features` |
| 111 | `Unfold` `{body_id, fixed_face?, bends?}` and `Refold` `{unfold_id}` |
| 112 | `Hem` `{edges, length, kind? closed\|open, gap?, outside?}` and `Jog` `{sketch_id, height, angle_deg?, flip?}`; edge flanges meeting at a corner are mitred |
| 113 | `CornerRelief` `{body_id, style? circular\|rectangular, size?}`; EdgeFlange `corners: open\|closed` |
| 114 | `NormalCut` `{sketch_id, body_id?, regions?}` |
| 115 | Drawing view `configuration`; note `table: none\|bends` |
| 116 | Hem `offset_start` / `offset_end` |
| 117 | `Curve3D` kind `bent_path`: corner `points` and a bend `radius` |
| 118 | CAM op types `surface_parallel` and `surface_waterline`, with `cut_angle_deg`, `sample_step_mm`, `z_step_mm` |
| 119 | `Cope` feature `{member_id, against, gap}`; the part's `tube` shop rules (material, `former_radii`, `min_straight`, `tail_length`, `max_thinning`) |
| 120 | Cope `mitre` list |
| 121 | CAM tool `overall_length` and `collar_diameter` (the holder check) |
| 122 | CAM op type `surface_rotary` (4-axis rotary raster) |
| 123 | Rotary op `rotary_pattern`: `parallel` (default) \| `spiral`; rotary ops read `stepdown` (a v122 file's is dropped on load); machine `a_max_deg_min` |
| 124 | Rotary pattern `helical` and `lead_mm` |
| 125 | `surface_parallel` reads `stepdown` (roughing; a v124 file's is dropped on load); surface ops' `rest_from_tool` |
| 126 | CAM setup `fixtures`: `{id, part \| box, name, pos, rot_z, enabled}`; placement and fixture ids count toward the job's id counter on load |
| 127 | Fixture `seat`: `{face: {feature, name}, edge: -x\|+x\|-y\|+y, along_mm, over_mm, spin_deg}`, re-solved against the stock on every read |
| 128 | AxisRef kind `custom` `{origin, direction}`; helix `start_deg` (+`_expr`); motion study `configuration` |
| 129 | Occurrence `flexible`, `inner_poses` `[{occurrence, position, orientation}]`, `inner_mate_dofs` `[{mate, rotation, offset, dx?, dy?, ball_rotation?}]` (a flexible sub-assembly placement, [assemblies](../assemblies/README.md#flexible-sub-assemblies)); a mate named `{path, mate}`: relation `mate_a_path`/`mate_b_path`, motion study `driver_path` |
| 130 | Explode step `path` `[placement]`: `occurrence` is then a moving part inside that flexible placement ([assemblies](../assemblies/README.md#flexible-sub-assemblies)) |
| 131 | CAM tool `shank_diameter`: how wide the tool is above its flutes; wider than the cutter, a cut deeper than the flutes is refused ([CAM setup](../cam/setup.md#reach)) |
| 132 | Machine `rotary` `{along, chuck_end, word, top_moves}` (how the rotary is installed) and `holding` (chuck and tailstock sizes); setup `held_in`, `grip_mm`, `tailstock`, `chuck` ([4th axis](../cam/rotary.md#how-the-rotary-is-installed)) |
| 133 | Derived `configuration`: the source's configuration the copy is taken in, by id; absent = whichever it has active ([Derived](../modeling/derived.md#configurations)) |
| 134 | Drawing view `rotation_deg`: the view turned on the sheet, counter-clockwise about its own direction; absent = 0 ([drawings](../drawings/README.md#rotation)). A dimension ref's `extreme` (`left`/`right`/`top`/`bottom`): the curve's furthest point that way ([dimensions](../drawings/README.md#dimensions-and-annotation)) |
| 135 | A part occurrence's `configuration_id` names one of the PART's configurations (it was sub-assemblies only); an occurrence's `material` overrides every body's (a slug; absent = the part's own); a job part's `configuration` ([assemblies](../assemblies/README.md#configurations-per-placement)) |
| 136 | CAM operation `index_a_deg`: the rotary indices one operation is cut at, generated once per angle and joined by a retract; absent = the setup's `rotation_a` ([4th axis](../cam/rotary.md#one-operation-at-several-indices)) |
| 137 | Sketch entity `text`: a one-line label (`text`, `font` sans/hershey, CAP `height`, `anchor`, `angle`/`angle_deg`, `halign`, `valign`, `spacing`), its glyphs regenerated from the fields and never saved ([sketching](../sketching/README.md#text)) |
| 138 | CAM operation type `engrave` (sketch curves followed on the curve; a target is `{feature_id: <sketch>, element: "g<entity id>"}`) and its groove, `width_mm` or `depth_mm`; absent = not stated ([engrave](../cam/operations.md#engrave)) |
| 139 | CAM setup `rapid_feed` (mm/min): the setup's own traverse rate, seeded from the machine's and saved with the setup (only the machine's copy was); absent = 0, not stated, and a negative or non-numeric one is refused ([post](../cam/post.md)) |
| 140 | Thread `hand` (`right` or `left`; absent = right, and any other value is refused), and `modeled: true` now cuts the real helical groove instead of erroring; the designation may name a unified thread (`3/8-16 UNC`) or carry a class suffix (`3/8-16 UNC-2B`) ([modelled threads](../modeling/holes-and-threads.md#modelled-threads)) |
| 141 | Feature type `Bearing` {`designation`}: an ISO 15 deep-groove ball bearing generated from its designation (618/619/60/62/63, bores 3 to 30 mm), the whole of a generated standard part ([bearings](../parts/README.md#bearings)) |
| 143 | A part's `print` {`process`, `material`}: how it is printed (`fdm` + `petg` is what the table carries; any other pair is refused on load, naming what is), which `check_fit`, `check_fits` and a Thread's report read for the allowance to model. Drives no geometry; written only when set ([printed parts](../tolerances/README.md#printed-parts)) |
| 145 | CAM tool type `thread_mill` with `tip_flat`, `neck_diameter` and `neck_length` (mm; written only on a thread mill), and operation type `thread_mill` with `passes` (0 or absent = derived); its depths and everything else come from the Thread on its target face ([thread mill](../cam/operations.md#thread-mill)) |
| 146 | Feature types `Worm` {`face`, `module`?, `module_expr`?, `starts`?, `pressure_angle_deg`?, `hand`?, `backlash`?, `length`?} and `WormWheel` {`face`, `teeth`?, `module`?, `module_expr`?, `starts`?, `worm_diameter`?, `pressure_angle_deg`?, `hand`?}: a ZA worm and its hobbed wheel cut from cylindrical blanks, an unknown hand refused; and the relation kind `worm` ([worm gears](../modeling/worm-gears.md), [relations](../assemblies/relations.md#worm)) |
| 147 | Drawings: a view's `section` {`point` [x, y, z] world mm, `label`}, a section view cut square to the view's direction ([section views](../drawings/README.md#section-views)); a note's `style` (`text`, `balloon`, `datum`; absent = text) and note table `bom`, the parts list of an assembly view ([parts lists and balloons](../drawings/README.md#parts-lists-and-balloons)); the title block's `material` and `scale` (blank = derived at export). An unknown style or table is refused on load |
| 148 | A topology result file (`studies/<id>-<slug>.result.json`) carries `input_hash` (`0x` and 16 hex digits; any other form is refused on load): the hash of what produced it. A reopened project reuses the result for a run whose inputs hash the same, and reads it stale when they do not ([studies as jobs](../simulation/README.md#studies-as-jobs)) |
| 149 | Hole `drill_point_deg` (the included angle of the point a blind floor is modelled with, 30 to 180; absent = 118 for a hole with a fit, a jobber drill's, and a flat floor for a plain diameter) and `thread_depth` (+`_expr`, mm from the mouth, a tapped fit only; absent = the depth less three pitches, or the whole bore when through); both refused out of range ([holes](../modeling/holes-and-threads.md#hole)). Golden `v149-hole-point` |
| 150 | Project tables: `tables` [{`name`, `comment`?, `columns`, `rows` [{`key` (a number or a string), `values`}]}] in a lone part's envelope, and in `project.json` (additive there), read by `lookup("table", key, "column")`; a malformed table refuses the load ([tables](../variables/README.md#tables)). A cylinder's or cone's connector frame takes the surface's seam (u = 0) as X instead of a world-axis heuristic, so a stored mate `rotation` on a round face counts from the part's own direction ([connector frames](../assemblies/solver.md#connector-frames)) |
| 151 | Drawings: a view's `detail` {`view`, `center` [x, y, z] world mm, `radius_mm`, `label`}, a detail of another view ([detail views](../drawings/README.md#detail-views)); a note's `style: "fcf"` with `fcf` {`symbol`, `tolerance_mm`, `diameter`?, `modifier`? (`mmc`, `lmc`), `datums`} and `dim`, the dimension a frame stands under ([feature control frames](../drawings/README.md#feature-control-frames)); an unknown symbol or modifier, or a frame ISO 1101 cannot write, is refused on load. Material slugs `aluminum_6061_t6` and `aluminum_6082_t6`. Golden `drawing/v151-frames-details` |
| 152 | Studies: `load_cases` [{`name`, `weight` (+`weight_expr`)}] on a static or topology study, and a force's or pressure's `case` naming one ([load cases](../topopt/README.md#load-cases)); a topology study's `keep_outs` [{`body`} or {`box_min`, `box_max`} mm] ([keep-outs](../topopt/README.md#keep-outs)); a static study's `shape_from` {`study`, `threshold`? or `whole`?}, refused on any other type ([a static study of the optimised shape](../topopt/README.md#a-static-study-of-the-optimised-shape)). A topology result file carries `cases` [{`name`, `weight`, `compliance`}], `keep_outs` [{`what`, `tets`, `volume`}] and `islands` {`threshold`, `components`, `tets`, `volume`, `keep_faces`, `load_path_broken`} ([connectivity](../topopt/README.md#connectivity)). A keep-out that is neither a body nor a box is refused on load. Golden `v152-topology-load-cases` |
| 153 | Every study result is saved beside its study with its `input_hash`, in the directory form only: a static or modal result in a part's or an assembly's `studies/<id>-<slug>.result.json` (`kind` `static`/`modal`/`topology`, `maxima`, each body's mesh with `results` {`displacement`, `von_mises`, maxima, `modes` [{`frequency_hz`, `shape`}]} or one per load case in `cases`, and a shape study's `shape` {`source`, `threshold`, `island_tets`, `volume`, `threshold_lowered_from`}), and a motion result in an assembly's `motion/<id>-<slug>.result.json` (every sample's `worlds`, `leaves`, `clashes`, `unknown_contacts` and `mates`, the pair counts). A reopened project's run of the same inputs is answered from it; one whose inputs moved reads stale ([studies as jobs](../simulation/README.md#studies-as-jobs)). A `kind` that is none of the three, or a malformed hash, is refused on load. Golden `v153-saved-study-results` (the single-file form carries no results; `tests/test_study_inputs.cpp` and `tests/test_motion.cpp` reopen each kind) |
| 154 | A study or motion result is two files beside its study: `<id>-<slug>.result.json` (the metadata, `input_hash`, and `bin_bytes`) and `<id>-<slug>.result.bin` (its numeric arrays, raw little-endian). Each array in the json is a reference {`type`, `offset`, `count`}: `f32` for a field (nodes, displacements, stresses, mode shapes, density), `u16`/`u32` for indices (`i32` where one is -1, a cut triangle on no face) (`tets`, `boundary` six nodes a triangle with `boundary_face` apart, `tet_domain`), `f64` for a motion placement (`worlds` {`occurrences`, `world`}, `leaf_world`). A static or modal body keeps its boundary surface only, with `node_count` and `tet_count` of the mesh it solved ([what a saved result keeps](../simulation/README.md#what-a-saved-result-keeps)). An unknown `type`, a reference outside the bin, or a bin missing or not `bin_bytes` long drops that result ([a lossy load](README.md#a-lossy-load)). A v153 result (arrays inline) still reads. Golden `v154-study-results-binary` (the single-file form carries no results; `tests/test_study_inputs.cpp` reopens each kind) |
| 155 | Sketch constraint refs reach an `ellipse`'s or `ellipse_arc`'s axis ends: `which` 3/4 the major-axis ends, 5/6 the minor-axis ends, so a radius is a `distance` from the centre (0) to one, and `horizontal`/`vertical` on an ellipse levels its major axis. An ellipse or elliptical arc with `r_minor` > `r_major`, or `r_minor` <= 0, is refused on load ([ellipses and splines](../sketching/README.md#ellipses-and-splines)). Golden `v155-ellipse-axis-ends` |
| 156 | Two feature types. `RetainingRing` {`standard`: `din471`\|`din472`, `size` (the nominal shaft or bore diameter, mm)}: a generated retaining ring, every other dimension read from the table. `RetainingRingGroove` {`face`, `from` (a circular edge at one end of that face), `offset` (+`offset_expr`), `standard?`}: its groove cut in a shaft or bore. An unknown standard or size is a feature error naming what is carried ([retaining rings](../parts/README.md#retaining-rings)). Golden `v156-retaining-ring` |
| 157 | An attached occurrence may name the faces it is seated on: `attach_seat` {`face`, `axis`: {`feature_id`, `element`}, `inner_path`?}, the planar face it bears on and the cylinder or cone whose axis centres it (a fastener: its hole's face and bore); with it, `attach_offset`/`attach_rotation` are in the seat's frame, so the piece follows a hole that moves inside its part. Written only with `attached_to`; a seat missing either face is refused on load. A rider without one keeps the part-frame offset, and a fastener is seated in the hole under it on its first solve ([attached, and mated](../parts/README.md#attached-and-mated)). Golden `assembly/v157-attach-seat` |
| 158 | A CAM operation's `cutter_comp`: `off` (the default, not written) or `wear`, on profile, pocket and drill: the finished wall passes post under G41/G42 with a lead-in and a lead-out, the path still the tool centre, so the controller's tool table holds the wear alone. An unknown value is refused ([cutter compensation](../cam/post.md#cutter-compensation)). Golden `job/v158-cam-cutter-comp` |
| 159 | A saved motion result (`motion/<id>-<slug>.result.json`) lists the press fits its exact checks relieved: `press_fits` [{`a`, `b`, `a_name`, `b_name`, `a_inner`?, `b_inner`?, `fit`, `interference_mm`, `max_interference_mm`, `volume_mm3`}], omitted when there are none ([press fits](../tolerances/README.md#press-fits)). The bands that decide them are stamped on bodies from the tolerances, print process and bearings already saved, never saved themselves; they are inputs of a run's hash, so a result stored before a tolerance edit reads stale. Golden `assembly/v159-press-fit` (an exact motion study of a 6005 at its K7 seat's interference; the single-file form carries no results, and `tests/test_press_fits.cpp` round-trips one) |
| 160 | Design requirements ([requirements](../requirements/README.md)), project-level: one file each in `requirements/<id>-<slug>.json` beside `project.json` (which carries `requirements_next_id`), or a lone part's envelope's `requirements` list, each {`id`, `name`, `source`?, `revision`, `measure` {`kind`, `document`?, the kind's keys}, `bound` {`kind`: `at_most`\|`at_least` {`value`} \| `within` {`min`, `max`} \| `pass`; a value a number or an expression}, `evidence`? {`pass`, `value`?, `unit`?, `error`?, `input_hash`, `revision`, `limit`?/`limit_min`?/`limit_max`?, `detail`?, `checked_at`?}}. An unknown measure kind, key or bound kind is refused on load. An assembly's `assertions/` (v100) still loads, as clearance requirements, and is no longer written. Golden `v160-requirements` |
| 161 | Feature templates: a `Template` feature {`template` (the library name it was placed from), `revision`, `on`? (the reference it stands on: `"Top"`\|`"Front"`\|`"Right"`, {`feature_id`, `element`} or {`feature_id`}), `parameters` {name: number (+`name_expr`), string or reference}, `ids` [[local, id]] (the document ids of its expanded features), `definition` (the template it expands, in full)}; and the project's template library, `templates` [{`name`, `revision`, `comment`?, `parameters` [{`name`, `kind`: `length`\|`angle`\|`count`\|`string`\|`reference`, `example`, `comment`?, `choices`?}], `features` [feature JSON with local ids 1..N], `bindings` [{`feature`, `path` (a JSON pointer, or `attach`), `param`}]}] in a lone part's envelope and in `project.json` (additive there). A template that does not validate -- an unknown kind, an expression reading anything but its number parameters, a binding to nothing -- refuses the load ([templates](../modeling/templates.md#the-saved-form)). Golden `v161-feature-template` |
| 162 | A drawing's directory form keeps each view's last projection beside the view's file, as a study keeps its result: `views/<id>-<slug>.projection.json` {`kind`: `drawing-projection`, `revision` (`drawing::kProjectionRevision`), `input_hash`, `mode` `exact`\|`fast`, `view_dir`, `up`, `sectioned`, `ds_edges`, `unresolved`, `curves` {`count`, `kind`, `flags` (1 visible, 2 closed), `type`, `points_per`, `source`, `edge`, `face`: index arrays; `points`: `f32`; `conics`: `f64`, centre and radius per curve}, `cuts`? [{`body`, `edge_origin`, `edge_from_face`, `face_origin`, `loop_sizes`, `loop_points` `f32`}], `bin_bytes`} and `.projection.bin`, the arrays in the v154 reference form. Derived: written only when its bytes differ, never by an autosave (the single-file form carries none), kept out of the project's history with the results; a reopened view whose inputs hash the same is drawn from it with no hidden-line removal, and one that does not read is dropped with a warning, never refused ([saved projections](../drawings/README.md#saved-projections)). Golden `drawing/v162-stored-projection` (the single file) beside the directory form `drawing/v162-stored-projection/`, a projection with a foreign hash among it |
| 163 | A requirement's measure kind `bearing_bore` {`document` (an assembly), `occurrences` [bearing occurrence ids]}: the smallest bore of those bearings, mm, each read off its catalogue row, bounded `at_least` the shaft's own minimum ([measures](../requirements/README.md#measures)). A build that does not know the kind refuses the file. Golden `v163-bearing-bore-requirement` |
| 164 | A face appearance override: on a body's creating feature, `face_appearance` [{`element` (a face's element name on that feature's body, which may be a face a later feature made), `color`? (linear RGB), `roughness`?, `metallic`?}] beside `appearance`, each field inheriting from the body's look when unset. An entry with no element or no field is refused on load. Stripped from the regen hash like `appearance`; a name that no longer resolves is reported, never dropped ([a face of its own](../rendering/materials.md#a-face-of-its-own)). Golden `v164-face-appearance` |
| 165 | A requirement's measure kind `section_area` {`document` (a part), `of`: `material`\|`enclosed`, `base_plane` `xy`\|`xz`\|`yz` with `offset` (mm) or `origin` and `normal`, `bodies`? [body names or creating-feature ids]}: mm², the material a plane cuts or the area that material rings ([measures](../requirements/README.md#measures)); and six building materials, `brick`, `roof_tile`, `plaster`, `roofing_felt`, `render`, `soil` ([materials](../rendering/materials.md#building-materials)). A build that knows neither refuses the file. Golden `v165-section-area-requirement` |
| 166 | A reference image's `show_always` (true; omitted when off) in a sketch's `images`: drawn whenever its sketch is shown and in quick stills, not only while the sketch is edited ([underlays](../sketching/README.md#reference-images-underlays)). Underlays are stripped from the regen hash already. Golden `v166-underlay-show-always` |
| 167 | A motion study's `clash_deflection_mm` (mm; omitted when the mesh check derives its deflection per body, 0.2% of each body's bounding diagonal): the tessellation its mesh clash check is cut to. Refused on load unless positive, and on an `exact` study, which checks the B-Rep ([motion studies](../assemblies/README.md#motion-studies)). Golden `assembly/v167-clash-deflection` |
| 168 | An Extrude's `twist_deg` (+ `twist_expr`) and `herringbone` (true): the profile turns that many degrees about the sketch normal through the sketch origin over the length, right-handed about the direction of travel; a herringbone climbs the first half at that rate and back over the second. Omitted at zero twist and when off, so an untwisted extrude writes what it did. Blind and symmetric only, never thin: the other combinations fail the feature, naming what is accepted ([twisted extrudes](../modeling/README.md#twisted-extrudes)). Golden `v168-twist-extrude` |
| 169 | A part's `print` may name one of the user's print profiles, `profile`, and then embeds its copy beside `process` and `material`: `conditions`, `stl_deflection_mm` (omitted when not stated) and `rows` [{`kind`, `from_mm`, `to_mm` (absent = and up), `allowance_mm`, `source`?}]. The copy is what the part reads, whichever store opens it; rows without a profile name, or a copy the profile rules refuse, are refused on load. The regen and requirements hashes fold in the rows' content ([print profiles](../tolerances/README.md#print-profiles)). Golden `v169-print-profile` |
| 170 | Two load types: `torque` (`magnitude` N·mm, right-handed about the axis; `axis_origin` + `axis_dir` in mm, omitted for a cylindrical face, which lends its own) and `remote` (`magnitude` N along `direction`, acting at `point` mm and carried by the face). A torque on a face that is not cylindrical without an axis, and a remote load without a point, are refused when the study runs; a remote load without `point` is refused on load. A build that knows neither type refuses the file ([torque and remote loads](../simulation/README.md#torque-and-remote-loads)). Golden `v170-torque-remote-loads` |
| 171 | Relations over several mates: kinds `linear_n`, `epicyclic` and `planet`, each written as `terms` [{`mate`, `path`?, `dof`, `coefficient`, `teeth`?}] and a `constant` (Σ coefficient · dof = constant) in place of `mate_a`/`mate_b`/`dof_a`/`dof_b`/`ratio`/`phase`; an epicyclic's terms are sun, ring, carrier and a planet's sun, carrier, planet. A gear's or chain's `teeth_a`/`teeth_b` when its ratio came from them, and a gear's `internal` (true: +teeth_a/teeth_b); `internal` on another kind is refused. An older build refuses the new kinds by name ([relations over several mates](../assemblies/relations.md#relations-over-several-mates)). Golden `assembly/v171-epicyclic-relation` |
| 172 | A static study result (the `studies/*.result.json` and its `.bin`) keeps each node's stress tensor, `stress`: six floats a node, Voigt order [xx, yy, zz, xy, yz, zx], MPa, per load case, beside `von_mises`. What the largest principal stress and the stress across a printed part's layers are read from; a result without it reads, and is refused only for those ([printed layers](../simulation/README.md#printed-layers)). Golden `v172-printed-layers` (a PLA bar's study; results are not in the single-file corpus, so their round trip is `tests/test_study_inputs.cpp`'s) |
| 173 | A Thread's `flank_angle_deg` (degrees, half the thread angle from the plane normal to the axis; omitted at the ISO/UTS 30): a custom thread's groove, overlay depth and printed major follow it; refused on a table designation and outside 15-60 at regeneration, and on load when it is not a number ([flank angle](../modeling/holes-and-threads.md#flank-angle)). And the material `tpu`, TPU 95A as printed. A build that knows neither reads a 45-degree thread as 30 or refuses the slug. Golden `v173-thread-flank` |
| 174 | Relation kind `intermittent` (a carry, a Geneva): b steps once per `period_deg` of a, during the window [`start_deg`, `start_deg` + `duration_deg`), and stands still the rest of the period — `step` (the follower's unit, degrees for a turn), `period_deg`, `start_deg`, `duration_deg`, `motion` (`uniform` or `geneva`) and, for a Geneva, `slots` (3 or more; its step and window are −360/slots and 180 − 360/slots). An unknown motion, a Geneva under three slots and a relation missing any of them are refused on load; an older build refuses the kind by name ([intermittent](../assemblies/relations.md#intermittent)). Golden `assembly/v174-intermittent-relation` |
| 175 | A print profile's `layer_height_mm` (the slicer preset's layer) and `z_gap_mm` (whole layers of `layer_height_mm`; first defined as the gap between two parts printed one over the other, since read, with no format change, as the vertical air a face needs over another part), each omitted when not stated — in the profile store and in a printed part's embedded copy. A Z gap without a layer height, or not whole layers of it, is refused, naming the two nearest that are; a value that is not a number is refused on load. An older build reads the copy without them, so a print-in-place generator falls back on its rule ([print profiles](../tolerances/README.md#print-profiles)). Golden `v175-print-z-gap` |
| 176 | A piece of a face or edge one operation split is named by the faces only it touches, `<name>;M<tag>[a\|b]` (an ordinal `.<i>` only where nothing tells pieces apart), not by its position `<name>;M<tag>.<i>`; every reference saved after this holds such names. No migration: a saved `.<i>` name resolves to the piece it named, in result order, and the regeneration warns that it is by position ([split pieces](../modeling/toponaming.md#split-pieces)). Golden `v176-split-piece-names` |
| 177 | A sketch constraint's `driven` (true: a driven, or reference, dimension; omitted when false). It adds no equation to the solve and has no input: its `value` is written as what the geometry measures and is not read back as an input. `driven` on a kind that is not a dimension (`distance`, `distance_x`, `distance_y`, `distance_pl`, `radius`, `diameter`, `angle`), on one with an `expr`, or that is not a boolean is refused on load. An older build reads a driven dimension as driving, an equation the sketch may not meet ([constraints](../sketching/README.md#constraints)). Golden `v177-driven-dimension` |
| 178 | A print profile row's `kind` may be `press`: a printed cap pressed onto a printed pin, its `allowance_mm` the cap's bore less the pin on the diameter, either sign ([press allowance](../tolerances/README.md#press-allowance)). No new field; an older build refuses such a part by its version rather than by the row. A profile without a press row reads the built-in default of its pair. Golden `v178-press-allowance` |
| 179 | A size tolerance's `printed: "press"` (no `class`, no `lower`/`upper`): a printed press, its band read from the part's print profile, not stored ([printed press fits](../tolerances/README.md#printed-press-fits)); any other `printed` is refused. A print profile's `bed_mm` [x, y, z], the printer's build volume, omitted when not stated -- in the profile store and in a printed part's embedded copy; not three positive numbers is refused. A motion study's `max_refine` (halvings of a step, 0..12), omitted at its default of 6; a stored motion result's per-sample `refined` and `branch_jump` and its `refined_samples` and `branch_jumps` ([branch jumps](../assemblies/README.md#branch-jumps)). An older build reads the tolerance as explicit 0/0 deviations and the profile without its bed. Golden `v179-printed-press` |
| 180 | A stored motion result's per-sample `touching` [{a, b, a_name, b_name, a_inner?, b_inner?}]: the pairs a mesh check found meeting only on a surface, which are not clashes ([touching is not a clash](../inspect/README.md#touching-is-not-a-clash)); omitted when empty. The run's input hash moves to `motion/3` (no format change of its own), so a result stored before -- whose seats were clashes, and whose cam followers could carry a branch jump -- is run again rather than reused. An older build reads the result without the list. Golden `v180-touching-sample` |
| 181 | A part's `print_settings` {part?, bodies?: {"<created_by>": ...}}, each {walls?, top_layers?, bottom_layers?, infill_pct?, infill_pattern?, supports?, brim?, direction?}, only what is stated, omitted when there are none ([slicer settings](../tolerances/README.md#slicer-settings)); an unknown key, pattern or brim, a count out of range, or 100 % infill with a pattern that cannot print solid is refused on load. A print profile's `bed_exclude_mm` [[x0, y0, x1, y1], ...] (the bed's keep-out zones; needs `bed_mm`) and `line_width_mm`, omitted when not stated, in the store and in a part's embedded copy. An older build reads the part without its settings and the profile without its zones. Golden `v181-print-settings` |
| 182 | A Derived feature's snapshot body may carry `fit_bands` [{`face` (the SOURCE's face name), `lower`, `upper` (mm on the diameter), `side`: `hole`\|`shaft`\|`either`, `label`, `allowance`? (mm, omitted at 0), `printed_press`? (true)}]: the size bands the source stamped on that body, as it read them (its tolerances under its print process, a bearing's rings), stamped on the copy under its own face names ([size bands on a copy](../modeling/derived.md#size-bands-on-a-copy)). An unknown `side` is refused on load. An older build reads the copy without its bands, so a press on the source is a clash on the copy. Golden `v182-derived-fit-bands` |
| 183 | A Hole's `printed` (`round` \| `teardrop` \| `teardrop_flat`, absent = round): its roof pointed up the part's print direction; an Extrude's `taper_deg` (+`taper_expr`, absent = 0); a study load's `region` (`{normal, from_mm, to_mm}` or `{center_mm, radius_mm}`, absent = the whole face). Additive: an older file reads as before. Golden `v183-printed-features` |
| 184 | An intermittent relation's `motion` may be `table`, with `table`: the law over the window as `[driver fraction, step fraction]` points from `[0, 0]` to `[1, 1]`, the driver's rising and the step's never falling, linear between them. A table that is not such a law, a `table` motion without one and a `table` under another motion are refused on load; an older build refuses the motion by name ([intermittent](../assemblies/relations.md#intermittent)). Golden `assembly/v184-intermittent-table` |
| 185 | A print profile's `slender_ratio` (the tallest a piece stands over the least width it stands on: check_print's [slender](../tolerances/README.md#slender) limit) and `slender_ratio_source` (where it was read), both or neither, omitted when not stated, in the store and in a part's embedded copy; a ratio without its source, a source without its ratio or a ratio not above 0 is refused on load. An older build reads the profile without them, and judges by the built-in, unmeasured 8. Golden `v185-print-slender` |
| 186 | A requirement's measure kind `insertion` {`document` (an assembly), `a` (the occurrence slid on), `b` (the one it goes on), `entry`: `positive`\|`negative`\|`either`, `mate`? (the mate id whose frame's Z is the axis), `allowance_mm`?, `samples`?, `rotations`?, `lead_in_mm`?}: passes when `a` reaches its seat with no more travel under interference than its own press ([insertion](../inspect/README.md#insertion)). An unknown `entry` is refused on load; a build that does not know the kind refuses the file. Golden `v186-insertion-requirement` |
| 187 | An assembly's **sequence** ([assembly sequences](../assemblies/README.md#assembly-sequences)): `sequence` {`steps`: [{`occurrence`, `direction?`, `origin_mm?`, `mate?`, `distance_mm?`, `turn_deg?`, `thread?`, `note?`}], `allowance_mm?`} in the assembly's index (and in a single-file assembly), absent when it has no steps. A step key the loader does not know is refused. Golden `v187-assembly-sequence`. |
| 188 | An assembly sequence's step names a **group** (`occurrences`: two or more ids, moved as one) or is a **pose** (`pose`: [{`mate`, `rotation_deg` \| `offset_mm`}], placing nothing) in place of its one `occurrence` ([assembly sequences](../assemblies/README.md#assembly-sequences)); a step states exactly one of the three. Golden `v188-assembly-sequence-groups`. |
| 189 | A requirement's measure kind `expression` {`expression` (a formula over the variables), `unit`?, `document`? (a part: read in its scope), `configuration`? (that part's configuration, 0 = base)} ([expression](../requirements/README.md#expression)); its evidence's `detail` carries `scope`, `part`?, `configuration`? and `inputs` {name: value}. A `configuration` without `document` is refused on load; a build that does not know the kind refuses the file. Golden `v189-expression-requirement` |
| 190 | An Import's `up_axis` (which axis OF THE FILE is up: `auto`, `none`, `+x`, `-x`, `+y`, `-y`, `-z`; absent = `auto`) and `turn_deg` (a turn about world Z after it; absent = 0) ([up axis](../interchange/README.md#up-axis)). An unknown `up_axis` is refused on load. An Import saved before v190 loads as `none`, so no saved part moves. Golden `v190-import-up-axis` |

## Pending changes

A branch's format changes, written here with first cell `vNEXT` and moved
into the history above, numbered, when the branch lands
([how](#a-bump-takes-its-number-when-it-lands)). Empty on main.

| v | Change |
|---|--------|
| vNEXT | Two sketch constraint kinds: `curvature` (G2 between two curves joined end to end, at least one a spline: tangent there with equal curvature) and `arc_length` (a dimension, mm: a circular arc's radius x sweep; driven like any dimension). An older build refuses the file on the unknown kind ([curvature continuity](../sketching/README.md#curvature-continuity), [arc length](../sketching/README.md#arc-length)). Golden `vNEXT-g2-arc-length` |

## Paths in a project

A shared project is someone else's input, so no string in one is a path
until `core/io/ProjectPaths.h` says so:

- a document's folder is its name only when that is one plain file name
  (no `/`, `\`, `:`, `.`, `..` or control character; `IsPlainComponent`),
  otherwise `document-<id>`; a name from a STEP file or a script ("Bracket
  1/2") stays a name;
- a document's `dir` in `project.json` must resolve inside the project, even
  through a symlink, or the project is refused (`InsideProject`);
- an asset name, and a baked body's `brep` file, must be a plain file name,
  or the document is refused (`CheckedAssetName`) or its baked geometry is
  ignored and reported as a derived drop
  ([a lossy load](README.md#a-lossy-load));
- an Import path is adopted from staging only as `<subfolder>/<file>`.

## The directory form

A part saved to a `.dsgn` directory (the default for a new save) is:

```
part.dsgn/
  index.json                 envelope + the ordered feature-id list
  features/<id>-<type>.json  one file per feature (the type suffix is cosmetic)
  studies/  configurations/  assets/   per-kind files, absent when empty
  studies/<id>-<slug>.result.json + .result.bin   a study's saved result
  geometry/                  baked bodies (derived; see README.md)
  .gitattributes             geometry/*.brep binary -merge -diff
```

A drawing's directory is `index.json` (the sheet, the view and dimension
ids, the notes), `views/<id>-<slug>.json` and `dims/<id>-<kind>.json`, and
beside each view its saved projection, `views/<id>-<slug>.projection.json`
+ `.projection.bin` (v162).

The id alone is identity: renaming a feature never renames its file. Files
are written temp + rename, identical content is not rewritten, deleted
items' files are removed, and `index.json` is written last so a torn save
leaves a stale but loadable index. The same feature JSON, version number and
migrations apply as in the single file.

## Assemblies

An assembly uses the same `designer-document` format and version number,
with `"kind": "assembly"`. Its body is an ordered `occurrences` list; each
occurrence is `{id, name, part_id, part_version, transform: {position,
orientation}, visible, fixed}` plus optional `ref_kind` (a sub-assembly),
`configuration_id` (a part's or a sub-assembly's configuration; v135 for a
part), `attached_to` (with `attach_offset` / `attach_rotation`, and v157's
`attach_seat`, the faces it is seated on), `material`
(v135: what this placement of a part is made of) and `flexible` (v129, with
the `inner_poses` and `inner_mate_dofs` it keeps). `orientation` is a
quaternion
`[x, y, z, w]`. Occurrences reference parts by **stable id**, never by name,
so renaming a part cannot break an assembly. The stored `transform` is always
the last solved placement, so a file opens at sane positions even with its
mates stripped; mates re-solve over non-`fixed` occurrences on load.

A mate is `{id, name, mate_type, a, b, flipped, rotation, offset}` (the RPCs
report the same kind as `type`) plus
`dx` / `dy` / `ball_rotation` where its type uses them; `a` and `b` are
connectors `{occurrence, anchor}`. An exploded view is `{id, name, steps:
[{occurrence, offset}]}` and is view state only: the applied view and factor
are never serialized. Mates, exploded views, relations, motion studies and
FEM studies share the assembly's one never-reused id counter.

Directory form: `index.json` (kind + id lists) plus `occurrences/`, `mates/`,
`exploded/`, `relations/`, `motion/` and `studies/`, one file per item, each
directory absent when it would be empty. An `assertions/` directory (v100)
is read, as the project's clearance requirements, and not written again. A motion or FEM
study's result sits beside it as `<id>-<slug>.result.json` (v153), with the
input hash that produced it, and its arrays in `<id>-<slug>.result.bin`
(v154).

## The project envelope

A multi-document project is a directory with `project.json`:

```json
{"format": "designer-project", "version": 2, "active": 0,
 "documents": [{"id": 3, "name": "Stand", "dir": "parts/Stand", "type": "part"}]}
```

`type` is `part`, `assembly`, `drawing` or `job`, stored under `parts/`,
`assemblies/`, `drawings/` or `jobs/`; `id` is the stable per-document id and
`folder` an optional tab-bar folder. Optional keys: project-scope
`parameters`, `folders`, `sections`, `catalogs`, `tables`,
`requirements_next_id`, `templates`; the project's requirements are files under
`requirements/` beside it (v160). The envelope is at version 2: the keys
added since were additive, and older builds ignored them. This build refuses
a key it does not know, as a document's reader does, and a v2 document
entry without a `type`, so a key added to it raises the project
version.
