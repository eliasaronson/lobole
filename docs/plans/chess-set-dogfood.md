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

The pawn blank has no rest pass: its profile is finishable by the 3 mm ball.

## 1. Small fixes

- **The rotary finish leaves patches at one angle.** The profiles are
  finishable by the 3 mm ball everywhere (every inside radius is at least
  1.6 mm), yet each blank's verify lists 0.16-0.45 mm regions, each at ONE
  rotary angle rather than a ring -- the finish toolpath misses or lifts
  there, or the verifier samples wrongly. Test: a turned cove finished by a
  ball leaves nothing past its cusp at any A.
- **The mitre's pocket both errors and posts.** The bishop's 3.2 mm notch,
  derived as adaptive for a 3 mm cutter, reports "could not lead into the
  cut" and still produces 9229 moves; the outcome must be one or the other.
- **A pattern of a Hole.** `pattern(source=<hole>)` fails with "source body
  not found": a Hole makes no body, and the pattern looks for one. A hole
  pattern is what a bolt circle is; it should pattern the hole's centres.
- **A cut that removes nothing still splits faces.** A cut extruded away
  from the part warns "removed no material" and leaves the face it touched
  split in two (`cap1;M4.0`, `cap1;M4.1`); with nothing removed the model
  should be unchanged.

## 2. Picking faces by what made them

The round's biggest cost was face picking: every operation needed a filter by
normal, centroid and radius, and the wrong ones (a collar step taken for a
bore floor, a silhouette for an edge) each cost a rerun. What made a face is
already in its name, and nothing reads it. From a real part (a block, a
pocket cut 4 over sketch lines 2-5, a hole 6, a fillet 7):

| face | name | what it says |
|---|---|---|
| pocket floor | `4/cap1` | cut 4's far end |
| pocket walls | `4/g2` .. `4/g5` | swept from sketch entities 2-5 |
| block top | `2/cap1;M4;M6;M7` | extrude 2's end, cut by 4, 6 and 7 |
| fillet | `2/g5;U3;G7` | made by fillet 7 from edge `2/g5;U3` |
| hole bore, bottom | `6/face0`, `6/face1` | positional: the hole renames wholesale |

and in a chess blank, the rook's crenel floor copied in by Derived feature 9
is `9/12/31/cap1#2;M15` -- the third pattern copy of the piece part's cut
31's far end.

### The decoder

`src/core/ElementOrigin.{h,cpp}`: a name to

```
ElementOrigin {
  derived   [ (derived feature id, source created_by) ... ]  outermost first
  madeBy    the feature that MADE it, in the innermost document: the seed's
            tag, or the last G step's tag when there is one (a blend face is
            the fillet's, not the edge's)
  role      start_cap | end_cap | side | blend | pole | seam | positional
            | boundary (the L fallback: made of other faces' edges, no role)
            | bore | bottom | counterbore | counterbore_floor | countersink
  entity    the sketch entity a side face was swept from (0 when none)
  instance  the pattern copy ('#i'), or none; 'm' for a mirror
  modifiedBy the M steps' tags, in order
}
```

It reads every form the grammar has and the ones seen in real documents:
`#i` before later steps, `.i` on M and G, a feature's M on its own seed,
nested Derived prefixes, multi-group caps (`cap0@g<k>`), `U` edge suffixes
(an edge's origin is its face's, plus which boundary edge). An edge decodes
through the face it was named from. `drawing::MappedNameFeatureIds` and
`HoleFeature::RecordTapping`'s prefix match go through it, so there is one
parser. Test: every name in the golden corpus and in the chess project
decodes, and `madeBy` names a feature that exists in that document.

### A hole names its faces by role

`HoleFeature` stops naming its tool positionally. Each hole is built per
centre and its faces named `<hole>/p<entity>/bore`, `/bottom`,
`/counterbore`, `/counterbore_floor`, `/countersink`, keyed by the sketch
entity the centre came from, so adding a hole renames none of the others.
No golden or saved reference names a hole face today. The tapping record
takes the bores by role instead of by radius. Test: a three-hole
counterbored Hole names every face by role, and deleting the middle point
leaves the other two holes' names unchanged.

Shell, draft, split, loft, sweep, pipe, rib, fasteners and imports keep
positional names; they decode as `positional` with `madeBy` still right.

### One RPC: `list_elements` filters and says

`list_elements` gains `made_by` (feature id or name; with `part` on a
Derived-bearing part, `{part, feature}` names a feature of the SOURCE part),
`via` (the Derived feature it came in through), `role`, `entity`, and every
face and edge it lists carries `made_by`, `role` and, when set, `entity`,
`instance`, `derived`. A second RPC would be a second place to learn.

- An unknown role is refused, naming the roles.
- A filter that matches nothing says why: the feature does not exist; it
  made no faces on this part; or it renames wholesale ("feature 12 is a
  Shell; its faces are named by position, 12/face0..n, role positional").
- Roles that read better for a cut are aliases a cut answers to: `floor` =
  `end_cap` of a cut, `walls` = `side`. A through cut has no floor and says
  so.

### The SDK

- Every feature call returns a `Feature` handle -- `hole`, `fillet`,
  `chamfer`, `pattern`, and `cut`, which returns None today because a cut
  makes no body. It still indexes like the response dict (`h["id"]`), so no
  generator breaks. `extrude`/`revolve` keep returning the Body they made;
  one that joins or cuts returns the `Feature`.
- `Feature.faces(role=, entity=)` / `.edges(...)` over the whole part, and
  the words a script means: `cut.floor()`, `cut.walls(entity=)`,
  `extrude.end_cap()`, `extrude.start_cap()`, `fillet.faces()`,
  `hole.bores()`, `hole.bottoms()`.
- `Body.faces(made_by=, role=, via=)` beside the existing geometric filters,
  and `Part.feature("Crenel")` for a feature by name.
- Face and Edge carry `made_by` and `role`; `Element.feature_id` stays the
  body's (it is what a ref needs).

### Dogfood

The chess generator's picks go through it: `cam.py`'s `floors()` (normal,
radius and an x window) becomes the crenel cut's floors via the rook's
Derived feature; the knight's fillet edges are the head extrude's side
edges. What cannot go is said in the round's notes.

Tests: the decoder over both corpora; each role on extrude, revolve, cut,
pattern, mirror, fillet, hole, shell and a nested Derived copy; the RPC's
refusals and its "why nothing"; `sdk_smoke.py` for the handles.

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
