# The job is not part of the part (J1–J9)

> Status: **J1–J6 SHIPPED 2026-09-01/02** (formats v73, v74, v75) — see
> [As built](#as-built), which also lists what did NOT ship, including the
> per-part fixturing frame this plan argued hardest for. Supersedes the implicit
> assumption in [plan.md](plan.md) that a `cam::Setup` belongs to the part it
> cuts. Surveyed against FreeCAD CAM and the two slicers in
> `~/Documents/CAD` on 2026-09-01 — see [What the survey
> changed](#what-the-survey-changed), which added a phase and softened a
> refusal.

## What we have

`cam::Setup` lives in `Document::m_camSetups` (src/core/Document.h:430). That
one line is the whole limitation: a job can only ever be about one part,
because there is only one document in scope and every `Operation::targets`
`ElementRef` resolves against it.

So the most ordinary job a hobby router does — four brackets out of one sheet
of ply — is inexpressible. You can cut each bracket in its own part document,
each with its own stock shrink-wrapped around it, and then own four G-code
files and no sheet.

Two smaller things fall out of the same root:

- **`StockMode::Fixed` is half-orphaned.** W3 built "the sheet you actually
  own" ([stock-and-origin.md](stock-and-origin.md)), and a 1220×2440 sheet
  wrapped around one bracket is a strange object. Nesting is what fixed stock
  is for, and there is nothing to nest.
- **The fixturing frame is on the wrong object.** S1–S7 correctly split
  `frame` (how the part is clamped) from `rotationA` (what the machine does)
  — [setup-frame.md](setup-frame.md). But `frame` sits on the *setup*, so
  every part in a setup would have to be laid on the bed the same way. A
  speaker cabinet's baffle, sides and back are modelled in their assembly
  orientations and cut from one sheet; they need three different frames and
  one stock.

## What is already right

Almost all of it. This is a rehoming, not a rewrite:

- **The generator is already document-independent.** `GenerateAll`,
  `StockBounds`, `PickedFaceRegion`, `RecomputeOrigin`, `DeriveSafeZ` all take
  `const std::vector<Body>&` (src/core/cam/CamToolpath.h). Bodies in, moves
  out. Nothing in the pipeline knows those bodies came from one document.
- **`BodiesInSetupFrame` is the proof of the technique.** It transforms bodies
  into setup space and the whole 2.5D pipeline works unchanged, *and element
  names survive because only the shape is transformed*. Placing a part in a
  sheet is the same operation with a different transform.
- **The doc kind is a solved shape, twice.** `Assembly` references parts and
  places them; `Drawing` references parts by `sourceDocId` and reports itself
  stale when the source's `Version()` moves (`DrawingSourceVersion`,
  src/ui/AppActions.cpp:3607). A job is the third of exactly that shape: it
  owns no geometry and describes an output. A drawing is how the part is
  documented; a job is how it is made.
- **The project file already carries a document list.** `project.json` has
  `documents: [{dir, id, name, type}]` with `type` in part|assembly|drawing,
  and each sub-document's `index.json` carries a `"kind"` discriminator that
  the loaders refuse on mismatch (Serialization.cpp:609, :891, :1508). Adding
  a fourth is a well-trodden path.

**One seam does the work.** Every CAM call site in the UI reads
`state.document->GetBodies()` — 32 of them, across CamPanel.cpp (12),
CamTool.cpp (18) and CamPlayback.cpp (2). Replace that single expression with one that returns the placed
bodies of every part in the job, and the generator, the verifier, the
simulation and the post are untouched.

## What the survey changed

Read on 2026-09-01, by opening them rather than by reputation: FreeCAD's
`Path/Main/Job.py` (933 lines) and `Path/Post/PostList.py`, PrusaSlicer's
`Domain/ModelInstance.hpp` and its arrange libraries, BambuStudio's
`PartPlate.hpp`. plan.md's original survey compared *toolpath algorithms*;
this one compares *the object that owns them*, which is the thing this plan
proposes.

The headline: **FreeCAD already built exactly this object, and it holds three
fields we do not.**

| What they have | Where | What it means here |
|---|---|---|
| `Job.Model` — a *group* of models, stock from `shapeBoundBox` over the whole group | Job.py:336, Stock.py:83 | Confirms J1/J4 unchanged. Same shape, same stock derivation |
| `Job.Fixtures` (`["G54"]`) + `OrderOutputBy` (Fixture/Tool/Operation) + `SplitOutput` | Job.py:203–218 | **A gap.** See J5 below |
| Per-plate print sequence, `ByDefault` falling back to global | BambuStudio PartPlate.hpp:230 | The same conclusion from an unrelated codebase: sequencing is a first-class property of the plate |
| `ModelObject` + N `ModelInstance`, each with its own transform | ModelInstance.hpp | **A gap, and a cheap one.** Four of the same bracket is four instances of one object, not four parts |
| `ModelInstancePVS_{Inside,Partly_Outside,Fully_Outside}`, set by `Print::validate()` | ModelInstance.hpp:16 | **Softens J4.** Three states computed at validate time, not a refusal at drag time |
| `ModelInstance::printable` | ModelInstance.hpp:42 | A placement you keep on the sheet but do not cut this run |
| `Base` is `App::PropertyLinkSubListGlobal` — and Base.py:523 *migrates* from the non-global `PropertyLinkSubList` | Op/Base.py:183–190, 520–531 | Direct precedent for `Target`: they shipped the unqualified link first and had to migrate off it |

Three things worth taking as warnings rather than features:

**FreeCAD's Job lives in the same document as the model and clones it in**
(`createResourceClone`, Job.py:81). That is why its operation links needed the
migration above. Designer's `sourceDocId` + version-compare (drawings) already
does cross-document properly; the clone is what we avoid, not what we copy.

**Arrange is 35 913 lines** across PrusaSlicer's `slic3r-biz-arrange` and
`libseqarrange` — the same order as libarea's 14 349, which plan.md already
declined to reinvent. And PrusaSlicer's own arrange defaults
`allow_rotations{false}` (Settings.hpp:26). It stays in [Not this](#not-this),
now with a number.

**Free in-plane rotation is wrong for the material this feature is for.**
Plywood has grain, and a nest that spins a part to save area can rotate it
across the grain — a strength claim nobody made. Designer cannot currently
even state the constraint: the only grain it has is a *rendering* derivation,
a per-draw axis taken from the asset's own bbox (MeshRenderer.cpp:448), which
for a part cut from a sheet is decorative and unrelated to the sheet's actual
grain. So `rotZdeg` is authored, not solved; and an arrange, if it ever lands,
needs a real grain direction on `Stock` first. That is a prerequisite this
plan does not build and should not pretend it has.

One thing we already have and should not rebuild: FreeCAD's `SetupSheet` is a
job-wide sheet of expression-based defaults (`SafeHeightExpression` and
friends). That is the defaults rule (docs/ui/u10-*) plus `DeriveSafeZ`, which
already derive from the thing they act on rather than from a stored expression.

## The fix

A **job** is a project document: a list of referenced parts, each *placed*,
and the setups that cut them.

```
DocKind { Part, Assembly, Drawing, Job }

JobDocument {
  std::vector<JobPart> parts;      // { id, sourceDocId, name } — like DrawingView
  std::vector<cam::Setup> setups;  // moved off Document verbatim
}

// every machining selection now says which part it is on. It names the PART,
// not the placement: "machine this pocket" is a statement about the bracket,
// and it is cut in every enabled placement of it.
struct Target { uint64_t jobPartId; ElementRef ref; };
cam::Operation::targets: std::vector<ElementRef> -> std::vector<Target>

// SEVERAL placements may name the same jobPartId — that is four of the same
// bracket in one sheet (PrusaSlicer's ModelObject + N ModelInstance).
cam::Placement {
  uint64_t id;
  uint64_t jobPartId;
  glm::dvec2 pos;      // where on the bed, in setup space
  double rotZdeg;      // spin under the spindle — authored, see the survey
  glm::dquat frame;    // WAS Setup::frame — how THIS part is clamped
  bool enabled = true; // on the sheet, not cut this run
};

cam::Setup {
  Stock stock; OriginAt originAt;             // unchanged
  Target topFace;                             // qualified: which part Z0 is on
  double rotationA; bool hasRotaryAxis;       // unchanged — the MACHINE
  std::vector<Placement> placements;          // new
  OrderOutputBy order = Authored;             // new — J5
  bool splitOutput = false;                   // new — one file per group
  std::vector<Tool> tools; std::vector<Operation> operations;
};
```

### Decisions

**Fixturing moves to the placement; the machine stays on the setup.** This
looks like it undoes S1–S7 and does the opposite. S1–S7's finding was that two
different physical claims had been collapsed into one number. `frame` is a
claim about *a part in a fixture* — so it belongs to the part, per part. The
setup keeps `rotationA` and `hasRotaryAxis`, which are claims about the
machine and cannot be per-part. One rotation concept per physical thing, which
is the rule that fixed the `G0 A90` bug.

**Placement is in-plane: position and a spin about setup Z.** Anything
out-of-plane is what `frame` already says. Two ways to tilt a part is the
mistake S1–S7 just finished paying for.

**One `frame` per part, one `stock` per setup.** A part laid differently is
still the same setup if it shares the stock and the touch-off — that is
physically what a sheet job is. A part that needs the *stock* flipped is a
second setup, which S4 already supports.

**A job references parts by stable project-document id and owns no geometry.**
Same contract as `DrawingView::sourceDocId`, same staleness report
(`JobSourceVersion` mirroring `DrawingSourceVersion`). The toponaming
guarantee that [plan.md](plan.md) calls the differentiator is *strengthened*:
the reference now survives a model edit AND says which model.

**An unqualified target is refused, not guessed.** Every `Operation::target`
names its job part. Reading one that does not is an error naming the parts on
offer — never resolved against "the first part", which is plausible and cuts
the wrong bracket.

**`JobBodies(state, job, setup)` is the one seam.** Every placement transform
composes there and nowhere else — the rule that
[setup-frame.md](setup-frame.md) had to learn the hard way when a fourth copy
of the transform turned up in CamPlayback.

## Phases

### J1 — the job document, holding exactly what CAM holds today (~1½ sessions)

`DocKind::Job`, `JobDocument`, a tab, `jobs/<Name>/index.json` with
`"kind": "job"`, a `type: "job"` row in `project.json`. Move `m_camSetups` off
`Document`. The parts list holds one part, placement is identity, `JobBodies`
returns that part's bodies.

Nothing gains a feature. Every existing CAM test passes with its setup rehomed
— that is the phase's gate, and the reason to do the move before the widening.

### J2 — the one seam (~½ session)

Replace `state.document->GetBodies()` at the 32 CAM call sites with
`JobBodies`. `Body` gains a transient `jobPartId`, stamped when the job
assembles its list — the same contract as `createdBy` ("transient like the
shape"), appended last so aggregate inits are unaffected (Feature.h:52).

This is the change that makes multi-part possible without the generator
knowing.

### J3 — many parts, placed (~1½ sessions)

`AddJobPart` / `RemoveJobPart`, placement drag reusing the assembly
occurrence gizmo (A2.5), `frame` migrated from setup to placement, `SolveFaceUp`
("lay this face up", S2) retargeted from the setup to the picked part.
`Operation::targets` widens and the viewport pick resolves face → job part,
the way an assembly pick resolves face → occurrence. `Setup::topFace` — the
face Z zero was taken from — is an `ElementRef` too and needs the same
qualifier; it stays on the setup (there is one touch-off per job) but must name
which part it was picked on.

**Instances come almost free and should land here, not later.** `placements`
is already a vector and nothing stops two entries naming one `jobPartId`; what
is missing is a verb ("duplicate this placement") and the rule that an
operation targets a *part* and is therefore cut at every enabled placement of
it. Getting that rule wrong later — targets pinned to placements — is a
migration; getting it right now is a sentence.

Staleness: a job whose source part has moved on says so, per part.

### J4 — the stock is the sheet (~1 session)

`Relative` over N parts = the bbox of every placed part plus margins, and
`OriginAt::ModelMin` reads that same union rather than one part's box. `Fixed`
finally means something.

Two conditions a single-part job never had. The first draft of this plan made
both **refusals at placement time**; the survey changed that. Both slicers
compute a *state per placement* at validate time and let you drag freely —
`ModelInstancePVS_{Inside, Partly_Outside, Fully_Outside}` set by
`Print::validate()`, and BambuStudio's `is_instance_outside_plate` /
`contain_instance` / `intersect_instance`. So:

- **outside the stock** — three states, not two. Partly-outside is a part
  hanging off the sheet edge; fully-outside is a part that is not on the sheet
  at all, and they are not the same mistake;
- **overlapping another placement** — a state on both.

Neither blocks the drag. Both are drawn in the viewport, and both make
`cam_post` and `cam_toolpath` **refuse by name** — which is where the refusal
belongs, because that is the moment the mistake becomes G-code. Blocking the
drag instead makes the sheet impossible to lay out through the intermediate
states every layout passes through.

### J5 — what order does it cut in (~1 session)

The phase the survey added. With one part, authoring order *is* cutting order
and `move_cam_op` is enough. With four, the natural authoring order is
per-part, and CamPost.cpp:428 emits a tool change on every change of tool
number — so a four-part sheet authored the obvious way changes tools four
times per tool instead of once.

FreeCAD makes this a job property (`OrderOutputBy`: Fixture | Tool | Operation,
plus `SplitOutput`), and `build_postlist_by_tool` says the reason in its own
docstring: "groups operations by tool controller to minimize tool changes".
BambuStudio reaches the same conclusion from the 3D-printing side with a
per-plate print sequence. Two unrelated codebases making sequencing
first-class is enough evidence.

```
enum class OrderOutputBy { Authored, Tool, Part };
```

- **`Authored`** is the default and reproduces today's output byte for byte.
  A single-part job must not change because this field arrived.
- **`Tool`** groups by tool number across every part — one tool change per
  tool.
- **`Part`** finishes each part before starting the next, which is what you
  want when a part may break free and you would rather lose one than four.

`splitOutput` writes one file per group. Unknown enum strings are **refused**
naming the three (docs — "refuse, don't substitute"); a job that silently
posts in a different order than it says is the exact class of bug that rule
exists for, and here it is a broken cutter rather than a wrong drawing.

Gate: a frozen G-code golden per order over one two-part job — the same
technique C1–C5 already uses, and the only honest way to assert an ordering.

### J6 — "Machine this part" (~½ session)

The one-click path from a part tab: create the job, add this part, derive the
stock, open the tab. Without it J1 is a regression for the common case —
today's flow is *open part → add setup → cut*, and it must not become *new job
→ add part → place it → cut*. The `defaults/*` rule applies: it changes the
document or it refuses visibly (docs/ui/u10-defaults-and-refusals.md).

### J7 — RPC and serialization (~1 session)

`new_job`, `add_job_part`, `remove_job_part`, `set_job_part_placement`,
`list_job_parts`; every `*_cam_*` method takes its context from the job tab.

**The part gate is a list kept apart from the handlers**
(RemoteServer.cpp:~777, which documents this exact trap: three handlers were
widened to cover the whole project and all three stayed part-gated). Every
`cam_*` method must join the ungated list in the same change, or CAM RPCs die
on a job tab — the bug the drawings dogfood round already found once.

Format: bump `kVersion`, add `tests/golden/v<N>-cam-job.json` +
`.expect.json`, add the row to docs/versioning/format.md's history. No
migration — no users (CLAUDE.md).

### J8 — the gates (~½ session)

- `rpc_gate_smoke` — the new methods, and the un-gating above.
- `clicking_and_scripting_agree` — placement by drag and by RPC agree.
- `test_conventions.cpp` — the doc-kind lists that must mention `Job`.
- `GuiTestsCam.cpp` — 53 `GetCamSetups` sites migrate; the suite is the
  regression net for J1–J6 and should be moved early, not last.
- **A windowed test, not offscreen.** `--offscreen` has no panels, so the
  viewport is the whole framebuffer and a placement gizmo that is scaled and
  offset in the real app looks perfect there (CLAUDE.md; the case
  `cam/simulation_plays_and_gives_it_back` exists for).

### J9 — the docs this makes wrong (~small)

plan.md, gui.md, stock-and-origin.md and setup-frame.md all say "the part's
setup" or imply one part per job. `frame`'s new home needs stating in
setup-frame.md's own terms, since it reads as a reversal and is not one.

## Not this

- **Auto-nesting / arrange.** **35 913 lines** in PrusaSlicer
  (`slic3r-biz-arrange` + `libseqarrange`) — the same order as libarea, which
  plan.md already declined to reinvent. PrusaSlicer's own arrange defaults
  `allow_rotations{false}`, and for plywood the constraint is stronger than
  that: rotating a part across the grain is a strength claim nobody made — and
  a grain direction on `Stock` is a prerequisite we do not have. Manual
  placement covers the case until it demonstrably does not.
- **Per-part feeds, tools or material overrides.** One tool table per setup,
  as today. (Slicers do have per-object config overrides; a sheet is one
  material, so the case that motivates them does not arise here.)
- **Work offsets — a fixture list (`G54`, `G55`, …).** FreeCAD's `Fixtures`
  is the *machinist's* answer to four copies: don't nest them, repeat the whole
  program at four work offsets (`build_postlist_by_fixture` emits every
  operation once per WCS). It is real, it is cheap, and it is a different
  feature from this one. The constraint it places on this plan is only that
  `Setup::origin` stays a single derived point *that a list could later
  contain* — the same "shaped so it is an addition rather than a redesign"
  rule plan.md applied to the 4th axis. Do not fold it in here.
- **Non-milling job kinds** (sheet laser/plasma, print orientation). The doc
  kind is what would make them possible; this plan does not build them, and no
  field here should be shaped speculatively for them.
- **Continuous 4-axis.** Still deferred, with plan.md's reasons.
- **Back-compat migration.** No users; fix the tests that assumed the old
  shape.

## Cost, honestly

~8½ sessions, up from ~7 before the survey added J5. The mechanical surface is 77 `DocKind::` sites across 12 files
(37 of them in AppActions.cpp), 22 `GetCamSetups` sites in `src/`, and 74 more
across `GuiTestsCam.cpp` + `test_cam.cpp` + `test_golden.cpp`. The 3.5k lines
of CamPanel/CamTool/CamPlayback/SerializeCam/RemoteCam are rehomed, not
rewritten.

The real risk is not any of that. It is J3: cross-document references have a
staleness trap this repo has already hit twice — A5's `PartDocById`-null and
A4's propagation. The drawings answer (`sourceDocId` + a version compare +
"stale" in the report) is the one to copy exactly rather than invent a third
time — and FreeCAD's Op/Base.py:520–531, which migrates its operation links
from a scope-limited form to a global one, is what that mistake costs when it
ships first.

## As built

**J1–J6 SHIPPED 2026-09-01/02** (v73, v74, v75). J7 and J8 landed inside the
phases that needed them rather than as steps of their own; J9 is this
section and the notes in plan.md, stock-and-origin.md and setup-frame.md.

| | what shipped | where |
|---|---|---|
| J1+J2 | `cam::JobDocument`, `DocKind::Job`, the tab, `jobs/<name>/index.json`, and `ui::JobBodies` standing where `document->GetBodies()` stood at 32 call sites | v73 |
| J3 | N parts per job, `cam::Target{jobPartId, ref}`, `Placement{pos, rotZdeg, enabled}`, placement composed in `BodiesInSetupFrame` | v74 |
| J4 | a laid-out fixed sheet anchored at the setup origin; `cam::Fit` in three states + overlap, `check_layout`, refusal at the post | — |
| J5 | `OrderOutputBy{Authored, Tool, Part}`, applied in `GenerateAll` | v75 |
| J6 | `machine_part` and the CAM button; the Parts panel section; a job tab that draws its parts laid out | — |

### What the gates caught, and it was most of the interesting bugs

Every one of these was invisible from the outside and named by a test that
already existed:

- **`rpc_gate_smoke`** — all twelve `cam_*` RPCs were part-gated, and `doc`
  is null on a job tab by definition. Exactly the trap RemoteServer.cpp
  documents: the gate is a list kept apart from the handlers.
- **`rpc_params_gate`** — `set_cam_setup` grew `order` and its describe row
  did not.
- **`test_conventions`** — the placement drag bypassed `ui::ExprInput`, so it
  would have read "10/2" as 10.
- **The GUI suite, shuffled** — twice. A CAM tool window left open with no
  job covered the viewport and ate a later test's clicks; and dropping a
  leftover job tab AFTER resetting the camera let the tab switch re-fit it,
  so a sketch trim two tests later previewed the wrong span, perfectly.

The tool that answers "is this a flake or an order dependence" was itself
broken — the archived `reproduce.sh` wrote a literal `\n` and ran without
xvfb, so it printed the app's help and exited 0, which reads as "the flake
went away". Fixed first, and it paid for itself twice within the hour.

### Design decisions that changed on contact

**The stock had to stop following the part.** J4 was written as "relative
stock spans every placed part", which was already true. The real blocker was
that `stock.place` positions a fixed sheet AROUND the part, so a placement
just dragged the sheet along and nothing could ever be off the edge. A
laid-out sheet is anchored at the setup origin now; Z keeps the `place`
derivation, because thickness is a fact about the board.

**Instances are held but not cut.** The model takes several placements per
part, the way every slicer does. The generator resolves a region to the
first body that answers, so the second copy would be silently skipped —
`cam::PlacementRefusal` says so instead. Wiring the generator to cut repeats
is the obvious next piece.

### Not built

- **Per-part fixturing.** The plan argued hardest for moving `frame` from the
  Setup to the Placement, and it is the one decision that did not ship.
  `frame` is still per-setup, so a job whose parts need to be clamped
  DIFFERENTLY — a speaker cabinet's baffle, sides and back, each modelled in
  its assembly orientation and all cut from one sheet — still needs a setup
  each. The argument stands and the seam is ready (a placement already
  carries the in-plane half); it is a day's work and a version bump.
- **`split_output`**, one file per group. The ordering is what makes it
  meaningful and the ordering is in.
- **Dragging a placement in the viewport.** Numbers in the panel today; the
  assembly occurrence gizmo (A2.5) is the thing to reuse.
- **Staleness per part in the GUI.** `list_job_parts` reports a missing
  source and the panel says so; a source that has merely MOVED ON is not
  reported yet, which is the `DrawingSourceVersion` half of the contract.
- **Auto-arrange**, and everything else in [Not this](#not-this).
