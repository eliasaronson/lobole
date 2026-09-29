# Chess set dogfood: what is left

The set is designed and every program is generated
([generators/chess](../../generators/chess/README.md); `run_all.sh PORT`
rebuilds all of it from nothing into `scratch/chess/`). What remains is the
machine and the app work the round found. The large item -- a configuration
per occurrence -- is the one that would have saved most of the round's
scripting; the rest are CAM gaps. Text is last.

## 0. At the machine (the user)

1. **The rotary as installed.** Jog A +90 and watch which way a mark on the
   chuck's top moves; read the axis letter and steps/degree from Mach3's
   motor tuning; measure the chuck's diameter, its grip, the chuck-to-centre
   length, and the real rapid feed. They go into `MACHINE` in
   `generators/chess/cam.py`. Until then no rotary program is for the machine.
2. **The pawn blank in cheap wood**, rough and finish only (`-T1`, `-T2`);
   calipers on the first pawn against the drawing.

The pawn blank has no rest pass: its profile is finishable by the 3 mm ball.

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
knights blank's spiral rest cuts only its bands.

## 5. Text (later)

A text sketch entity (a single-stroke font for engraving, an outline font for
pockets) and an engrave operation: the board's a-h and 1-8.

## Verification

Each phase's tests as above, the headless and GUI suites, the CAM-off build,
and the chess generator rebuilt from nothing after phases 3 and 4 with
every `cam_verify` read. At the machine: calipers on the first pawn.
