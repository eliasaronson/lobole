# Chess set dogfood: what is left

The set is designed and every program is generated
([generators/chess](../../generators/chess/README.md); `run_all.sh PORT`
rebuilds all of it from nothing into `scratch/chess/`). What remains is the
machine, one design change the machining asked for, and the app work the
round found. The two large items -- picking faces by what made them, and a
configuration per occurrence -- are the ones that would have saved most of the
round's scripting; the rest are bugs and CAM gaps. Text is last.

Phases 1 and 4 touch disjoint code (modelling/drawings vs CAM) and can run in
parallel worktrees; phases 2 and 3 both reach element resolution and the SDK,
so they go in order, 2 first.

## 0. At the machine (the user)

1. **The rotary as installed.** Jog A +90 and watch which way a mark on the
   chuck's top moves; read the axis letter and steps/degree from Mach3's
   motor tuning; measure the chuck's diameter, its grip, the chuck-to-centre
   length, and the real rapid feed. They go into `MACHINE` in
   `generators/chess/cam.py`. Until then no rotary program is for the machine.
2. **The pawn blank in cheap wood**, rough and finish only (`-T1`, `-T2`);
   calipers on the first pawn against the drawing.

Phase 1's profile change lands before this, so the pawn needs no rest pass.

## 1. Small fixes and the profile change

- **Profiles a 3 mm ball finishes** (`generators/chess/profiles.py`). Round
  the arc-to-line inside corners too (bead, cove, ball necks), not only the
  line-to-line ones, and widen every groove to at least 3.4 mm, so every
  inside radius is at least 1.6 mm. Verified: each blank's `cam_verify` with
  rough and finish only leaves nothing over the finish's cusp, and the rest
  op is dropped from the jobs.
- **The knight's head can be dimensioned.** Its filleted outline's edges have
  no element names, so `list_drawing_geometry` skips them. Find where the
  fillet leaves edges unnamed (`topo::EdgeBlendNamed`, the `U`/`L` passes in
  `ElementHistory.cpp`) and name them. Test: every edge of a filleted spline
  extrude has a name, and a drawing height dimension to the knight's top
  reads 54.4.
- **The blind slot floor that measured 1 mm across.** Reproduce the
  bishop's first mitre (a slanted blind slot, floor at y = -1) in a test and
  find why `InscribedRadius` saw 0.99 mm where the floor is 3.2 mm wide.
- **Small refs and reads.**
  - `lay_face_up` on a setup of a multi-part job takes `part`, since every
    Derived part's feature is id 1.
  - `list_configurations` takes `part`.
  - Drawing views take a rotation, so a part modelled along X can be drawn
    upright.
  - Wood materials: maple and beech; an epoxy fill.

## 2. Picking faces by what made them

The round's biggest cost was face picking: every operation needed a filter by
normal, centroid and radius, and the wrong ones (a collar step taken for a
bore floor, a silhouette for an edge) each cost a rerun. The information is
already in the element names -- `<cut>/cap1;M<cut>` is that cut's floor,
`<extrude>/g<N>` the side swept from sketch entity N, `...;G<fillet>` a
fillet's face -- but nothing reads it.

- **One decoder** (`src/core/ElementOrigin.{h,cpp}`): a name to
  `{made_by, role: start_cap | end_cap | side(entity) | blend | positional,
  modified_by[], generated_by[], derived_path, pattern_instance}`. It
  handles the Derived prefix (nested), `#i` anywhere in the chain, `.i` on
  both M and G, and the `L` fallback (role unknown, said so). It replaces
  `drawing::MappedNameFeatureIds` and `HoleFeature::RecordTapping`'s prefix
  match, so there is one parser.
- **Roles for the features that rename wholesale where it matters:** the
  Hole names its bore, its bottom and its counterbore instead of
  `<id>/face<i>`, because a drill target is what CAM picks most. Shell,
  draft, loft, sweep and the rest keep positional names and report the role
  as `positional`, naming the feature responsible.
- **`find_elements` RPC** `{part?, body?, made_by, role?, entity?, kind?,
  facing?}`: the faces or edges a feature made in a given role, each with
  its ref, geometry and the reason it matched. An empty answer says why
  ("feature 12 is a Shell, which renames every face; its faces are
  12/face0..n"); an unknown role is refused, naming the roles. Convenience
  roles over it: `floor` (a cut's end-cap pieces), `walls` (its sides),
  `bore` (a hole's).
- **`list_elements`** reports each face's `made_by` and `role`.
- **SDK:** the handle a feature call returns answers for its own faces:
  `cut.floors()`, `cut.walls(entity=)`, `extrude.end_cap()`,
  `fillet.faces()`, `hole.bores()`; `Body.faces(made_by=, role=)` for the
  rest.
- **Dogfood:** rewrite the chess generator's picks with it; every geometry
  filter that can go, goes.

Tests: the decoder over the golden corpus (every named element decodes, the
round-trip `made_by` matches the producing feature), each role on extrude,
revolve, cut, pattern, fillet, hole and a Derived copy, and a smoke over
`find_elements`' refusals.

## 3. A configuration per occurrence and per job part

The set needed twelve Derived parts to place six ranks in two woods, because
a placed part shows its document's active configuration. `Occurrence`
already stores `configurationId` (serialized, used for sub-assemblies); it
widens to parts.

- **One resolver keyed by (part, configuration).** `assembly::PartResolver`
  takes the configuration; `Flatten` fills `FlatInstance.part` from
  `ui::ConfiguredPartDocument` (the cache drawing views and Derived already
  read) and records the leaf's configuration. The ~20 call sites that
  re-fetch `PartDocById(occ->partId)` (selection, mates, measure,
  tolerances, fasteners, view, inspection, simulation) go through one
  `ui::OccurrencePartDoc(state, occ)`, and `test_conventions.cpp` refuses a
  new `PartDocById(` on an occurrence's or instance's `partId`.
- **Freshness includes the configurations.** `SubtreeVersions`, the renderer
  cache, the FEM staleness stamps and `JobBodiesKey` fold in
  `ConfigurationsRevision`, because editing a non-active configuration moves
  no `Version`.
- **Everything keyed by part is keyed by (part, configuration):** renderer
  instancing and tessellation prewarm, in-context ghosts, the BOM (a row per
  part and configuration, the configuration named), motion-study copies,
  STEP export's product dedupe (the sub-assembly pattern at
  `StepExport.cpp:130`), drawings of assemblies.
- **Mates** resolve their faces against the configured copy's element map; a
  face that does not exist in that configuration fails the mate by name.
- **A material per occurrence**, so one part in two woods is one part:
  mass, BOM and rendering read the occurrence's material when it has one.
- **Surfaces:** `add_occurrence {configuration}`,
  `set_occurrence_configuration` for parts (the part refusal goes), the
  occurrence panel's picker, SDK `place(configuration=)`; `machine_part` /
  `add_job_part {configuration}`, `list_job_parts` reports it.
- **Format:** `JobPart.configuration` is new (a kVersion bump, a golden under
  `tests/golden/job/`, a row in `docs/versioning/format.md`); the occurrence
  field already exists and needs a golden of a part occurrence in a
  configuration.
- **Dogfood:** the chess set becomes one `piece` part placed 32 times, each
  occurrence naming its rank's configuration and its wood; the blanks and
  the undersides job take the piece in a configuration directly.

Tests: flatten, solve, BOM rows, mass, interference, STEP and a drawing over
an assembly placing one part in two configurations; a job cutting two
configurations of one part; staleness when a non-active configuration is
edited; the rewritten `nested_assembly_smoke.py` case that used to expect
the refusal.

## 4. CAM

- **CAM calls that do not stall the frame.** `cam_toolpath`, `cam_verify`,
  `cam_post` and `cam_simulate` run on the main thread (the watchdog logged
  60-330 s). The server already parks requests across frames for
  screenshots and UI actions; a CAM request parks the same way: queue the
  passes for every setup it needs (`CamToolpathsQueued`), keep the frame
  running, and answer when the `cam` jobs are done, with verification as the
  `cam-verify` background job the panel already uses. Callers keep
  synchronous semantics; `async: true` stays for pollers.
- **Stock carried everywhere.** The playback (`BeginCamPlayback`) starts
  from what earlier setups left, through the same `SeedFromEarlier` the
  planar check uses. An indexed operation after a turning setup derives
  `top_z` from the turned envelope over its target (the model's largest
  radius along the op's span), not the fresh bar, so the crenel, mitre and
  cross ops need no typed depths.
- **One operation at several rotary indices.** `Operation` gains an index
  list (or count and step); generation runs once per angle with that angle's
  frame and joins the pieces with a retract; the post already writes a new
  A per move and retracts before turning; the verify reads such a file
  through the rotary path. The queen's eight crown setups become one.
- **A spiral rest pass only where the rest is:** one spiral per leftover
  band, retracting between, instead of one winding the whole length at the
  rotary's speed limit; and `holding_clipped_mm` counts a spiral's clipped
  length correctly.

Tests: a parked `cam_verify` answers while `ping` answers in between;
playback of an indexed setup after a turning one starts from the turned
stock; a four-index crenel op posts four A words and verifies clean; the
pawn blank's spiral rest cuts only its bands.

## 5. Text (later)

A text sketch entity (a single-stroke font for engraving, an outline font for
pockets) and an engrave operation: the board's a-h and 1-8.

## Verification

Each phase's tests as above, the headless and GUI suites, the CAM-off build,
and the chess generator rebuilt from nothing after phases 1, 3 and 4 with
every `cam_verify` read. At the machine: calipers on the first pawn.
