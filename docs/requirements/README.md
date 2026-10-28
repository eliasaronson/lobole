# Requirements

What a design is for, held by the project and checked against it. A
requirement says what to measure, by stable reference, what it must come
to, and why; checking it records evidence of what was measured. A person or
an agent who edits the model afterwards sees which requirements still hold,
which no longer do, and which say nothing about the model as it is now.

The code is `src/core/Requirements.{h,cpp}` (the model and its saved form)
and `src/remote/RemoteRequirements.cpp` (measuring, the RPCs, the check
job).

## The model

A `req::Requirement` is a project-level object, like the project's
variables: it is not inside a part or an assembly, because one design's
requirements reach across them (a study on the yoke part, a clearance in
the assembly, a CAM job's verification).

| field | what |
|---|---|
| `id` | the project's, never reused (`requirements_next_id`) |
| `name` | what it is: "lens deflection at 3 g, level" |
| `source` | why it exists, in a sentence |
| `revision` | starts at 1, bumped by every edit |
| `measure` | `{kind, document, ...}`: what to read, by stable reference |
| `bound` | what the reading must come to |
| `evidence` | what the last check measured (below) |

References are stable ids, never names: a document's slot id, a study's,
an occurrence's, a mate's, a CAM setup's, a face's `{feature_id, element}`.
A renamed occurrence keeps its requirements; a deleted one leaves them
unresolved.

## Measures

Each kind is measured through the RPC that already computes it, called in
process by the check (`Call` in `RemoteRequirements.cpp`), so a requirement
and the call a person would make agree by construction.

| kind | document | through | reads | keys besides `document` |
|---|---|---|---|---|
| `study_probe` | part | `run_study`, `probe_study` | mm, deg, MPa (or as `unit` says) | `study`, `case?`, and one face (`face`, `at?`, `quantity?`: translation \| rotation \| displacement_at \| von_mises_max \| von_mises_mean) or a formula (`readings`, `expression`, `unit?`) |
| `study_result` | part | `run_study`, `study_results` | mm, MPa, or a factor | `study`, `quantity`: max_displacement \| max_von_mises \| factor_of_safety, `case?` |
| `clearance` | assembly | the gap `interference` measures | mm | `a`, `b` (top-level occurrences), `mode?`: mesh \| exact, `motion_study?` |
| `contact` | assembly | `interference` | verdict | `a`, `b`, `expect`: clear \| touching \| not_clashing |
| `seal` | assembly | `check_seal` | mm, the widest closed ring's narrowest trace (0 when none closes) | `a`, `b` (top-level occurrences), `motion_study?` (the worst of its samples) |
| `insertion` | assembly | `check_insertion` | verdict: `a` reaches its seat with no more travel under interference than its own press ([insertion](../inspect/README.md#insertion)) | `a` (the occurrence slid on), `b` (the one it goes on), `entry`: positive \| negative \| either, `mate?` (a mate id; else the one mate joining them), `allowance_mm?`, `samples?`, `rotations?`, `lead_in_mm?` |
| `assembly_sequence` | assembly | `check_assembly_sequence` | verdict: every step of the assembly's sequence goes home against everything placed before it, and none is missed ([sequences](../assemblies/README.md#assembly-sequences)) | |
| `mass`, `volume` | part or assembly | `mass_properties` | g, mm3 | |
| `fit_class` | assembly | `check_fits` | verdict | `mate`, `class`: clearance \| transition \| interference |
| `check_fits` | assembly | `check_fits` | verdict | `allow_unverified?` |
| `check_seat` | assembly | `check_seat` | verdict | `occurrences` (the bearings), `allow_unverified?` |
| `bearing_bore` | assembly | `check_seat` (its `d`) | mm, the smallest bore of the bearings | `occurrences` (the bearings) |
| `section_area` | part | `section` (`area_mm2`, or `enclosed_area_mm2`) | mm², the material a plane cuts or the area it encloses | `of`: material \| enclosed, `base_plane` (xy \| xz \| yz) with `offset` (mm) or `origin` and `normal`, `bodies?` (body names or feature ids) |
| `check_fasteners` | assembly | `check_fasteners` | verdict | `allow?` [{`part?`, `why?`, `max?`}], `allow_skipped?` |
| `check_literals` | part, parts, or none | `check_literals` | verdict | `documents?` (several parts); neither: every part |
| `motion_clean` | assembly | `run_motion_study` | verdict | `study` |
| `follower` | assembly | `run_motion_study`, `check_followers` | rpm, deg, mm (the quantity's) | `relation` (a contact relation), `study`, `quantity`: liftoff_rpm \| drop_rpm \| pressure_angle_deg \| deviation_mm \| deviation_deg \| gap_mm \| gap_deg, `speed_rpm?`, `max_pressure_angle_deg?` ([follower](#follower)) |
| `cam_verify` | job | `cam_verify` | verdict | `setup`, `resolution?`, `allow_uncut?` [what prefixes] |
| `expression` | none, or a part | the evaluator (`req::EvaluateIn`) | as `unit` says (default mm) | `expression`, `unit?`, `document?` (a part), `configuration?` (that part's; 0 = base) ([expression](#expression)) |

A part's `mass`, `volume` and `section_area` go through the functions
`mass_properties` and `section` answer with, on a `const Document&`
(`partmeasure::MassOf`, `partmeasure::SectionAreaMeasure` in
`src/core/PartMeasures.{h,cpp}`), which a [design sweep](#design-sweep)'s
point reads too, so the three agree by construction.

What a verdict needs to pass:

- `contact`: the pair's state is the one expected; `not_clashing` is
  touching or clear, and a press fit the size bands allow is touching
  ([press fits](../tolerances/README.md#press-fits)).
- `fit_class`: the mate's fit is of the class and does not fail or stay
  unverified.
- `check_fits`: no fit fails, none is unverified (unless allowed), and at
  least one fit was judged: "every fit holds" over none says nothing.
- `check_seat`: every listed bearing has both rings on a seat and every
  check passes (an unverified one only when allowed).
- `check_fasteners`: at least one screw examined, none skipped (unless
  allowed), and every finding matches an `allow` entry: its part starts
  with `part` and its reason contains `why`, and the entry has matched
  fewer than its `max` findings before it (an expected finding allowed
  once is not two of them).
- `check_literals`: no literal and no world placement in the part, the
  parts, or every part (a standard part's length can equal a variable by
  coincidence, which is why `documents` exists).
- `motion_clean`: the result is clean: every sample solved, no clash, no
  standing clash, no branch jump. A failure's `detail.findings` names each
  clashing pair (how many samples, the first driver value), the unsolved
  samples and the branch jumps.
- `cam_verify`: ok and complete, it cuts something, no operation failed,
  every thread judged in its band, every toleranced wall in its band, and
  every hole or thread the job leaves uncut matches an `allow_uncut` prefix.

### Follower

A `follower` requirement is one number of `check_followers` over the
study's run ([the follower check](../assemblies/relations.md#the-follower-check)):
`liftoff_rpm`, the study driver's speed above which gravity can no longer
hold the follower on its cam (a follower that never leaves reads the
largest number there is; one that gravity pushes off somewhere reads 0);
`drop_rpm`, the slowest of its cam's drops' landing speeds (refused for a
cam with no drop); `pressure_angle_deg`, the worst over the samples;
`deviation_mm` or `deviation_deg`, the realised motion against the one its
CamProfile was cut for (refused for a cam that is not a CamProfile's); and
`gap_mm` or `gap_deg`, the worst a sample stands off the cam or into it.
A quantity in the other unit than the follower moves in is refused. The
study is run, or its result reused, first; the key is the study's. A design
sweep refuses it, as it refuses the other study kinds.

**A clearance over a motion study** (`motion_study`) is the smallest gap
between the pair over that study's samples: each side's bodies moved from
where they stand now to where the study put them. The study is run, or its
result reused, first; a sample that did not solve makes the measure
unresolved, since the gap there is unknown. Only top-level placements move:
a flexible placement's inner poses are not followed.

**A seal** (`seal`) is a contact that closes all the way round
([seals](../inspect/README.md#seals)): bounded `at_least` the width the
seal needs, it fails on a contact that is open anywhere (a notch reads 0,
and `detail.open` says where) or narrower than that, and its detail is
`check_seal`'s answer. Nothing touching reads 0, with `detail.why`. Over a
motion study it is the worst sample's, like a clearance. The
[jar](../../generators/jar/README.md) states two.

**A bearing against its shaft** (`bearing_bore`): a bearing is a catalogue
row, chosen once, and does not follow a variable; the shaft it carries is
sized from the load. Bounded `at_least` the shaft's own minimum (an
expression: `"shaft_d_min"`), the requirement says the bearings still take
the shaft the design asks for, and a heavier load that regenerates the
minimum past the bore fails it, the bore and the minimum side by side. Each
bearing is read through `check_seat` as the row it places now, so a bearing
swapped for another row is measured as that one; an occurrence that is not
a placed bearing leaves the measure unresolved. The pan/tilt head states one
per shaft ([generators/pantilt](../../generators/pantilt/README.md#requirements)).

**The area inside the walls** (`section_area`, `of: enclosed`): a listing
quotes a building's floor as the area inside its outer walls, and a plan
traced 7 % short reads that floor 14 % small with nothing else saying so. The walls are
separate bodies, so the measure joins their cut faces and reports what they
ring ([enclosed area](../inspect/README.md#enclosed-area)); `bodies` names
the walls, which keeps the slab under them out of the room. The plane is
stated, never "the middle of what is shown", which moves with the model: a
`base_plane` needs its `offset`. A wall renamed or deleted makes the
requirement unresolved rather than a smaller room, and a plane through a
door or window encloses 0 with a note in `detail` saying the ring is open
there.

**The stress on a face** (`study_probe`, `quantity: von_mises_max` or
`von_mises_mean`, MPa): the von Mises stress `probe_study` reads ON the face,
its largest nodal value or its area-weighted mean
([probing a face](../simulation/README.md#probing-a-face)). A study's global
peak often sits in a sharp corner whose value grows with the mesh; a tooth
root or a fillet is the stress a design is held to. The Simulation window's
pinned probes make one ("Add requirement"), bounded at most the material's
yield.

**A formula over several faces** (`study_probe` with `readings`): each
reading is a `probe_study` of one face, `{face, at?}`, and the expression
sees `<name>_tx/_ty/_tz` (its translation, mm), `_rx/_ry/_rz` (its rotation,
radians), `_dx/_dy/_dz` (its motion carried to `at`), `_cx/_cy/_cz` (its
centroid) and `_vmmax/_vmmean` (the stress on it, MPa), beside the
project's variables. It is how a requirement reads
what a body held by several faces did: the pan/tilt head's lens, carried by
two bearing bores, a shoulder and a worm on a ledge
([generators/pantilt](../../generators/pantilt/README.md#requirements)).
An `at` component may be an expression too.

## Expression

A relation between variables is a requirement: `od - bore >= 4`, "a wall of
at least 4 mm round the bearing seat", without a section or a volume
standing in for it. `measure {kind: "expression", expression: "od - bore",
unit?}` is bounded like any number (`at_least {value: 4}`, or an expression
bound). The expression is in base units (mm, degrees), so `"10mm"` and
`"1cm"` read alike, and `unit` is declared, as a formula's is (default
`mm`). It is evaluated by the one evaluator,
`ExpressionEngine::Default().EvaluateText`, in a scope:

- no `document`: the project's variables with the tables' cells seeded
  (`state.projectSymbols`, `ui::SyncProjectScope`), so `lookup(...)` reads
  a table as a variable does. No configuration is seen: a configuration
  belongs to a part.
- `document` (a part): that part's `Document::Symbols()`, as its features
  see them: local variables over the project's, with its active
  configuration's overrides applied.
- `document` with `configuration`: the part's variables in that
  configuration (`Document::SymbolsIn`), whichever is active; `0` is the
  base. Variables only: no regeneration is needed to read them. A
  configuration the part lacks is refused naming the ones it has, and one
  deleted later reads unresolved.

Its bound reads the same scope. **Stated, its names are checked:**
`add_requirement` and `set_requirement` refuse an expression (or an
expression bound) that does not compile (the evaluator's message and
offset), names an identifier its scope does not hold (naming the variables
the scope does), or looks up a table that is not there.

**Names follow the variables.** `rename_variable` rewrites a requirement's
expressions, measure and bound, as it rewrites features' (in the scope the
requirement reads: a part's local variable of the name shadows a project
one, and an undo of a project rename puts the requirement back too);
`delete_variable` and `delete_table` are refused while a requirement reads
the name, naming it. This holds for every requirement's bound expressions,
not only this kind's. A name that goes anyway (a merge, a part deleted)
reads `unresolved`, "variable 'bore' is not in the project", and an
evaluation error (a division by zero) `unresolved` with the evaluator's
message; neither is ever evaluated as 0.

**Evidence**: the value, the unit, the bound as evaluated, and `detail
{scope: project | part, part?, configuration?, inputs {name: value}}`,
every identifier the expression read with its value then, so a failure reads
"od - bore = 3 mm (bore 15, od 18), must be >= 4 mm" (`req::FindingLines`).
The input hash is the names and values the expression and its bound read in
that scope, its text, the configuration it reads, and every cell of a table
a `lookup` names, so a cell edited moves it.

**It is checked where the values move.** One costs microseconds, so it
never waits for Check: every expression requirement whose input hash moved
is measured in place, once a loop iteration (`PollBackgroundJobs` calls
`ui::RefreshRequirementsInPlace`, whichever panel is open), whenever rows
are read (`list_requirements`, the panel), and as it is stated. The edit is
not refused: the call that moved it -- `set_variable`, `set_table`,
`set_active_configuration`, `set_configuration`, an `undo`, anything --
answers `requirements [{id, name, state, value?, unit?, bound, limit?,
note?, finding?}]` for each whose state changed, in its own response, as
`regen` reports a broken feature. The server attaches it after the handler
(`remote::RefreshExpressionRequirements`), to every call whose answer is
untyped or whose `returns` row names it, and not to the requirement verbs,
which answer their rows themselves. So the call that thins the wall
answers `fail`, and the panel shows it without a Check.

## Bounds

A number is bounded `at_most {value}`, `at_least {value}` or
`within {min, max}`; a verdict is bounded `pass`. Each value may be an
expression over the project's variables (`"run_gap / 2"`), evaluated when
the requirement is checked; the evidence keeps what it evaluated to.

**Refused, naming what is accepted:** an unknown measure kind, an unknown
key for the kind (in a saved file, also in the requirement or its evidence),
a missing required key, an unknown value of a key that has a vocabulary (`quantity`, `mode`, `expect`, `class`), an unknown bound
kind, a number bounded `pass` or a verdict bounded by a number, `within`
with its min above its max, and a requirement naming a document, study,
occurrence, mate or setup that does not exist. A key of the wrong type is
refused by its own name and what it takes (`measure.document` is the id of
the part it measures, `measure.allow_unverified` true or false,
`measure.offset` a number), and a `document` written as an open tab's name
also says which id that is: a requirement keeps ids, because a name
repoints silently on a rename. A file holding any of these does not load:
the requirement would load as something it is not.

## Evidence

Checking a requirement records `evidence`: the value (and its unit) or the
verdict, pass or fail, the bound as evaluated, a small `detail` (the
findings behind a failed verdict, the per-bearing rows, the sample a
clearance was smallest at), when, the requirement's `revision` it measured,
and the **input hash** of what it measured, taken after measuring.

The input hash is the one studies key their results on
([studies as jobs](../simulation/README.md#studies-as-jobs)), and covers
exactly what the measure reads:

| measure | input hash |
|---|---|
| `study_probe`, `study_result` | the study's (`fem::StudyInputKey`: the study with its expressions evaluated, the part's B-Rep, names and materials) |
| `motion_clean`, `follower`, a clearance or a seal over a study | the motion study's (`motion::InputKey`) |
| `clearance`, `contact`, `seal` | the two occurrences as saved and what each places |
| the other assembly measures | the assembly's content through every sub-assembly and every placed part's bodies (`motion::AssemblyInputKey`), with each placed part's tolerances and print process |
| `mass`, `volume` of a part | its bodies, tolerances and print process |
| `check_literals` | the part's features, bodies and variables, and the project's variables |
| `cam_verify` | the job's parts (each part's key) and setups as saved |
| `expression` | the names and values its expressions read in its scope, the configuration, and the cells of every table a `lookup` names ([expression](#expression)) |

A body's B-Rep is hashed as a reopened project reads it back
([studies as jobs](../simulation/README.md#studies-as-jobs)), so evidence
saved with the project reads current after the reopen.

Every hash also takes the values of every variable a bound's or a formula's
expression reads. So a heavier camera makes the yoke's deflection requirement stale (its
study's loads read `load_kg`) and leaves the base plate's CAM requirement
current.

## States

A requirement shows one of five states, and only one is a pass (an
`expression` one is measured in place, so it reads `pass`, `fail` or
`unresolved`, never `stale` for long: [expression](#expression)):

| state | when |
|---|---|
| `pass`, `fail` | evidence for the current model (the input hash matches) at the current revision |
| `stale` | the model changed since: the input hash differs |
| `unresolved` | a reference no longer exists (a document, study, occurrence, mate, setup or face), or the measure could not be taken (a study that failed, a gap the kernel would not measure); the note says which |
| `unchecked` | never checked, or edited since its evidence was taken |

`list_requirements` computes the states from hashes and evaluates nothing
else: no study runs, no gap is measured. **Editing a requirement**
(`set_requirement`: any of name, source, measure, bound) bumps its revision
and drops its evidence, which was about the old one.

## Checking is a job

`check_requirements` measures every requirement it is given that is not
current (`pass` or `fail` now), and answers the ones that are from their
evidence, so re-checking an unchanged design computes nothing. A measure
that runs a study asks `run_study` or `run_motion_study`, which answer an
unchanged study from its stored result by input hash, so a requirement made
stale by an edit that left its study's inputs alone (its bound, a variable
only its bound reads) costs no solve either. `recompute: true` measures
every one again and re-runs their studies.

It answers with every row, the counts of each state, `checked` and `reused`,
and a receipt (a pass or fail claim per current requirement, an
unverifiable one per other). Whatever it has to measure, it measures as a
job, kind `requirements`: `async: true` answers at once with the `job`; a
synchronous call starts the same job and waits for it parked
([requests that wait](../architecture.md#requests-that-wait)), so the
frame and other clients' calls carry on, and answers when it is reaped.
Nothing to measure answers at once either way.

- The job's `finish` steps on the main thread, one requirement a frame.
  The main thread only reads the model and writes evidence: a measure whose
  reading is expensive hands it to a **worker** the run owns. A clearance
  and a seal do (one pose or over a motion study's samples): the main
  thread takes both sides' bodies as deep copies, OCC's booleans and
  extrema writing into their arguments
  ([query booleans](../inspect/README.md#query-booleans-are-non-destructive)),
  and every sample's pose, and the worker measures those.
- The evidence a worker lands carries the input hash taken when its copies
  were made. An edit while it measures leaves the requirement `stale`, not
  a pass for a model nobody measured; a requirement edited (its revision
  moved) or removed meanwhile gets nothing.
- A measure that starts a study job (or needs CAM passes cut) waits for it,
  frame by frame, and then measures; a study that fails or is cancelled is
  that requirement's evidence (unresolved), not a reason to start it again.
  With `recompute`, a study is re-run once a check, however many of its
  requirements read it.
- It waits while any other study job runs, one run at a time.
- `study_status {job}` follows it (`label` names the requirement being
  measured, `fraction` how far through, the worker's share included),
  `cancel_study {job}` stops it and the study it waits on, and a job
  started over the RPC, parked or async, is cancelled with its client
  unless `outlive_client`. A worker reads the cancel between samples, and
  the job ends once it has stopped; what was measured before stays.

**An exact clearance over a study is bounded by its meshes first.** At each
sample the tessellations prove the exact gap is at least their distance
less both deflections. The samples are measured exactly in order of that
floor, and the walk stops at the first whose floor is no smaller than the
best exact gap found, so the smallest gap and the sample it is at are the
ones measuring every sample gives. The engine's valve-to-piston clearances
(181 samples) measure 4 or 5 exactly, about 1.5-2 s each instead of 7-9.
`detail.exact_samples` says how many; `samples` counts every pose
accounted for.

A measure runs on its document's tab (a study is the active part's), so a
check visits each requirement's tab and comes back to the one it started on,
keeping the camera where it was; a tab switch clears the selection.

## Clearance assertions

The declared clearances (`add_assertion`, v100) are clearance requirements:
`add_assertion {name, a, b, min_mm, mode?, why?}` adds one on the active
assembly (`at_least min_mm`, `why` as its source); `list_assertions` lists
the active assembly's in the old shape (`standing` holds \| violated \|
unverifiable \| stale \| never); `verify_assertions {id?}` measures them now
and answers with a receipt; `remove_assertion` removes one. An assembly
saved with `assertions/` loads them as requirements
(`ui::AdoptAssertionsAsRequirements`, on open): each becomes a clearance
requirement on that assembly, unchecked (its old verdict was keyed on
another hash), and the next save writes it under `requirements/` and drops
the assembly's `assertions/`.

## Saved form

In a project directory, one file per requirement,
`requirements/<id>-<slug>.json`, beside `project.json`, which carries
`requirements_next_id`; so stating or checking two requirements on two
branches touches two files. A lone part's file carries the list as
`requirements` in its envelope, and reopens with them pointed at it (its
file names no document id); a requirement naming another document keeps the
project in the directory form. Field detail and the version are in
[the format](../versioning/format.md).

Requirements are project state, like catalogs and tab folders: an edit or a
check marks the project dirty (`projectMetaDirty`) and is not on any tab's
undo stack.

## The panel

`src/ui/RequirementsPanel.cpp`, docked with the Model panel, and present only
while the project states a requirement (like the Conflicts panel). Each row
shows the state (coloured), the name (its tooltip: the measure kind, the
source, the note, the finding, when it was checked and at which revision),
the value with its unit (or "clean" / "findings" for a verdict) and the
bound as evaluated. The value and bound columns fit what they say, so a
narrow panel clips the name, which its tooltip gives back, rather than
"findings".

**A requirement that does not pass says why, whole.** Under a failing or
unresolved row, a line in its state's colour runs across the table, wrapped
to it, holding the whole finding: a number against its bound ("measured 8
mm3, must be <= 5 mm3"), a failed verdict's findings one a line (a
bearing's under its designation, a fit's reason, the detail's note), or why
it could not be measured. `req::FindingLines` (`src/core/Requirements.cpp`)
is the one wording: the panel draws it, and every check -- the job, the
synchronous call, a check with nothing to measure -- ends by logging each
requirement it was asked about that does not pass, `requirement '<name>'
<state>: <finding>; <finding>` (`fail` or `unresolved`), so a failing check read from the log says
why.
An `expression` row's tooltip adds the formula and the scope it reads, and
its finding line is the formula with what it read ([expression](#expression)).
When an expression requirement bounds a bare variable, **Sweep the ranges**
opens the [design sweep](#design-sweep) on the ranges the requirements state.
**Check** starts the job for every requirement that is not current, and
shows its progress while it runs on the bar every long operation draws
(`DrawJobProgress`), with Cancel (`###req_cancel`); Shift+Check measures
every one again. The
rows come from the same engine `list_requirements` answers with, through a
hook the RPC module installs (`ui::SetRequirementsEngine`), and are cached
on the documents' save stamps. Requirements are stated over the RPC, the
SDK or MCP; the panel shows and checks them.

## Design sweep

Whether the part rebuilds over the range its author stated is otherwise
known only by typing each value in by hand. `design_sweep` regenerates
**copies** of the model over ranges of its variables and says where it
stops building or meeting its requirements. A design range is not a
tolerance: a [tolerance](../tolerances/README.md#the-one-rule-a-tolerance-drives-no-geometry)
is variation about a nominal the model already has and drives no geometry;
a range is over the variables themselves, and each value in it is another
nominal the model is regenerated at. A sweep's points are configurations
nobody saved, not parts as made. ("Sweep" also names the Sweep feature and
the clash sweep, so this one is `design_sweep` throughout.)

**What it takes.** `design_sweep {variables?, from_requirements?, samples?,
corners?, refine?, documents?, max_points?, async?, outlive_client?}`:

- `variables [{name, min, max, scope?}]`: each end a number or an
  expression in the variable's base units (mm, degrees); `scope` is
  `project`, `part` (the active part's local one) or a part's id, and
  absent the project's, else the active part's.
- `from_requirements: true` takes each range an `expression` requirement
  states on one bare variable (`expression: "t"`, `within {min, max}`); a
  one-sided bound needs its other end in `variables` (`[{name: "t", max:
  12}]`); a range `variables` gives wins.
- `samples` per variable, ends included, evenly spaced (default 5, 2..33),
  each variable varied alone with the others at their values; `corners:
  true` adds the box's 2^n corners (up to 8 variables) and its centre.
- `refine` bisects each bracket found (below), default 0, at most 8 times.
- `documents` narrows the parts rebuilt; by default every part whose
  variables or features read a swept variable, directly or through other
  project variables, and every part an expression requirement reading one
  is on.

**Refused before anything starts** (a refused call changes nothing): an
unknown key (a variable's too); a variable not in scope, naming those that
are; one no part (among `documents`) and no expression requirement reads; one a
rebuilt part's active configuration overrides, naming the configuration (the
sweep would change nothing there, as a held DOF refuses a drag); `min`
above `max` or an end that does not evaluate to a finite number; `samples`,
`refine` or `max_points` out of range; a plan over `max_points` (default
64, at most 256), naming the count it would be and the cap, never cut
short (the count is every sample and corner and `refine` bisections on each
side of each variable at most); `from_requirements` with no requirement
that bounds one variable, naming those it looked at; a `documents` id that
is not a part; a second sweep while one runs, naming its job.

**On copies.** The main thread writes each part it rebuilds to its saved
form (`DocumentIO::Save`, a temporary directory) with a copy of the project
variables and tables; each worker reads them back as `ReadProject`'s
parallel load does ([opening a project](../ui/long-operations.md#opening-a-project)),
so no OCC shape is shared with the open document and `AppState` is not
touched. A swept value overrides the variable's expression on the copy, as
a configuration does, so its dependents follow. A worker walks its chunks
-- one variable's samples and then its bisections, or the corners -- in
order on its one set of copies, the regen cache re-executing only what the
variable reaches, on at most `cad::BooleanWorkers(parallel::Workers())`
sets: every thread on a per-thread error-handler stack, half of them on a
shared one ([performance](../performance/README.md#the-kernels-error-handler-lock)).
`src/core/DesignSweep.{h,cpp}` is the planning, the walk and what a point
records, and knows no `AppState`; the request, the copies and the job are
`RemoteRequirements.cpp`'s.

**What a point records.** Per rebuilt part: `regen` ok or broken, the
broken features `{feature_id, name, error}`, each sketch `{feature_id,
name, ok, dof, conflicting, why?}` that does not solve (a value edit the
regeneration refused, keeping the previous geometry, is one) or whose DOF
differs from the base, and `volume_mm3` and `mass_g`. And every requirement
it can measure on the copies -- `expression`, and `mass`, `volume` and
`section_area` of a rebuilt part -- through the functions the check calls,
as `{id, name, state, value?, unit?, note?}`. The others (studies, an
assembly's, CAM, `check_literals`, and a part's that is not rebuilt) are
listed once in `not_swept` with why: a study per point is minutes, and an
assembly reads every part. A point fails (`ok: false`, with `why`, one
reason a line) when a part is broken, a sketch does not solve, or a
measured requirement fails or is unresolved.

**The answer.** `{points [{values, parts, requirements, ok, why?}], base,
first_failure {name: {below?, above?: {last_ok, failed, why}}}, not_swept,
variables, planned, cap, seconds, cancelled, job}`. `first_failure` is the
failing value nearest the variable's own value on each side, with the last
good one before it (null when its own value fails), so the break is
bracketed and named with the broken feature's own error; `refine` narrows
each bracket by halves. Nothing is saved, the document's version does not
move, and the format does not change; the last eight sweeps stay in memory,
answered by `design_sweep_result {job?}` (the last when no job is given;
`{running, fraction, label}` while it runs).

**A job.** Kind `design_sweep` on the
[registry](../ui/long-operations.md#a-design-sweep): `async` answers with
the `job`, otherwise the call parks
([requests that wait](../architecture.md#requests-that-wait)) and answers
with the sweep. `study_status {job}` follows it (`label` "cam_d = 27.5 (7
of 15)", `fraction`), `cancel_study {job}` stops it between points keeping
what was measured (`cancelled: true`), and it goes with its client unless
`outlive_client`. `design_sweep` is NotInBatch; `design_sweep_result` is
Deferrable. Each point waits `LOBOLE_TEST_SWEEP_MS` first, cancellably
(`remote::SetTestSweepHold`), for the tests that watch one run.

**The window.** `src/ui/DesignSweepPanel.cpp`, a floating Design sweep
window opened from a variable's row in the Variables panel ("..." beside
its delete, `###var_sweep_<scope>_<name>`) and from the Requirements panel
("Sweep the ranges", `###req_sweep`, shown when an expression requirement
bounds a bare variable: `from_requirements`). One row per variable (name,
from, to) and one samples, corners and refine for the sweep; the default
range is half to twice the variable's value, derived from it, and a
variable at zero has no default and says so. A line under the rows gives
the planned count against the cap (or the refusal design_sweep would give)
before Run. Run (`###sweep_run`) shows progress on `DrawJobProgress` with
Cancel (`###sweep_cancel`); the answer is a table, a row a point, failing
cells in the fail colour with the reason in their tooltip, and each
variable's first failure as a line above it. The window plans, starts and
reads through the requirements engine (`ui::RequirementsEngine`'s
`sweepPlan`, `sweepStart`, `sweepResult`), so it and an agent run the same
sweep. It changes nothing in the document.

The SDK's `App.sweep({name: (min, max)} or [...], from_requirements=,
**options)` runs the job, waits and returns `design_sweep_result`'s answer.

## RPCs

In `src/remote/RemoteRequirements.cpp`; the `describe` rows are the
reference for every parameter and key.

| method | what |
|---|---|
| `add_requirement {name, measure, bound, source?}` | states one; refused as above |
| `set_requirement {id, name?, source?, measure?, bound?}` | edits one: revision + 1, evidence dropped |
| `remove_requirement {id}` | |
| `list_requirements` | every row with its state, the counts, `all_pass` |
| `check_requirements {ids?, async?, recompute?, outlive_client?}` | measures (above) |
| `add_assertion`, `list_assertions`, `remove_assertion`, `verify_assertions` | clearance requirements in the old words |
| `design_sweep {variables?, from_requirements?, samples?, corners?, refine?, documents?, max_points?, async?, outlive_client?}` | the model over ranges of its variables, on copies ([design sweep](#design-sweep)) |
| `design_sweep_result {job?}` | a sweep's answer, kept in memory |

The SDK (`python/lobole/sdk.py`): `App.require(name, measure, at_most= |
at_least= | within=(lo, hi), source=)` states one, or edits the one of that
name, so a generator re-run states the same set; `App.verify_requirements()`
checks as a job, waits, and raises naming every requirement that is not
`pass`; `App.sweep(...)` runs a design sweep and waits for its table. The
MCP workflow prompt says to state requirements and check them before
delivering.

## Tests

- `tests/test_requirements.cpp` (`[requirements]`): the refusals and what
  they name, the five states, bounds, the saved form round-tripping with
  evidence and expressions, a lone part's file carrying them, and the v100
  golden's assertions loading as requirements. The `expression` measure:
  project- and part-scope expressions pass and fail as the values move; a
  configuration's override is seen through `document` and not without it,
  and `configuration` reads that one while another is active; a table cell
  edited moves a `lookup`; an unknown name, a malformed expression and a
  missing table are refused naming the scope's; a rename rewrites it (and
  its undo puts it back), a delete is refused naming it (a part-local one
  too), a name gone reads unresolved naming it, a part removed reads
  unresolved, a division by zero reads unresolved; the golden
  `v189-expression-requirement` round-trips with its evidence.
- `tests/test_design_sweep.cpp` (`[design_sweep]`): a block of wall `t`,
  every edge filleted at `r`, swept over `t` breaks between the samples
  either side of `2 r` and names the fillet, and `refine` narrows that
  bracket; a triangle dimensioned `a`, `b`, `c` swept over `c` reads its
  sketch not ok past `a + b`; corners count `2^n + 1`;
  `from_requirements` takes a `within` range and refuses a one-sided one
  until `variables` gives its other end; the user's document (its version,
  bodies and variables) is unchanged after a sweep; and the refusals.
- `tests/design_sweep_smoke.py`: the pan/tilt head's camera
  (`build_tilt.camera`, built alone) swept over `cam_d` from a quarter of
  its value to its value brackets `6 * cam_grip_r` (read from the part) and
  names "Grip edges", `refine` narrowing it; the SDK's `App.sweep`; the
  async form is a job, a second sweep is refused naming it, `cancel_study`
  keeps the points done, a sweep whose client leaves is cancelled, and one
  with `outlive_client` is not.
- `tests/refused_calls_smoke.py`: every refusal of an expression
  requirement and of `design_sweep` (sync and async) leaves the document
  and the job list as they were.
- `tests/requirements_smoke.py`: on a bar under load and the slider-crank,
  each measure kind passes and fails, an edit makes stale only what it
  reaches, an edited requirement reads unchecked, a deleted reference
  unresolved, a re-check measures nothing, the async check is a job, both
  project forms carry the evidence current; and on a shaft in a housing,
  check_fits and fit_class pass and then fail on an interference shaft
  (stale before the check: a tolerance is an input), check_seat fails a
  bearing on no seat, check_fasteners over no screw is not a pass, and a
  bearing's bore passes over a variable minimum, reads stale when the
  variable grows past it and then fails, and reads unresolved on a part that
  is not a bearing; and four wall bodies enclose exactly their floor (the
  slab under them left out by `bodies`), their material is their footprint,
  a plate encloses its Ø10 bore, a bound past the floor fails, and a wall
  deleted reads unresolved. With the measure held
  (`LOBOLE_TEST_REQUIREMENT_MS`): a synchronous check parks while another
  client is answered, then answers as before; `cancel_study` stops an async
  one inside the hold, keeping what was measured; and a check whose client
  leaves is cancelled with it. An expression requirement: the `set_variable`
  that thins a wall answers `requirements` with the row `fail`, the value
  put back answers `pass`, `list_requirements` agrees without a check, and
  an edit that changes no state answers none.
- `tests/assertion_smoke.py`: the clearance verbs, as requirements.
- GUI `requirements/panel_checks_and_goes_stale`: the panel appears with
  the requirements, Check measures them as a job, and an edit to the model
  makes them stale.
- GUI `requirements/a_failure_says_why`: a volume over its bound, a
  `check_fasteners` over an assembly with no screw and a mass on a part
  that is gone each show their whole finding under the row (read back as
  the panel drew it, `ui::RequirementRowDrawnForTest`), wrapped inside the
  table, the Value column fitting "findings", and the name's tooltip up.
  `requirements_smoke.py` reads the log line of a failing clearance.
- GUI `requirements/a_slow_check_keeps_the_frame`: a clearance held 1.5 s
  in its measure (`remote::SetTestRequirementHold`) leaves every frame
  under 250 ms of main-thread CPU; the panel's Cancel ends a 30 s hold
  at once, keeping the evidence; an edit while it measures reads stale; a
  requirement removed meanwhile gets nothing.
- GUI `requirements/an_expression_fails_as_you_type`: a value typed into
  the Variables panel turns the expression requirement's row red with its
  finding, no Check pressed, and the value put back turns it green.
- GUI `requirements/a_sweep_by_clicking`: "..." on a variable's row opens
  the Design sweep on half to twice its value, Run finds the fillet's break
  and says so above the table, the failing row counted; Cancel on a held
  sweep ends it at once, keeping the frame and the document.
- The engine (`generators/engine`) states its valve clearances and both
  studies clean as requirements, and reopens the saved project to see every
  one read `pass` unmeasured (`generator_engine`).
- Golden `v160-requirements` (a lone part's envelope) and
  `v163-bearing-bore-requirement` (the `bearing_bore` measure) and
  `v165-section-area-requirement` (`section_area`, on walls of the building
  materials).
- The pan/tilt head (`generators/pantilt`) states its acceptance criteria as
  requirements and stops on any that does not pass.

## Not done

- A study on an assembly cannot be measured: `probe_study` reads part
  studies only, and an assembly study's input hash needs its context.
- A clearance over a motion study does not follow a flexible placement's
  inner poses.
- The panel shows and checks; it does not state or edit a requirement.
- A formula's `unit` is declared, not derived from its expression, and a
  sweep's progress label shows a variable's value without one (a variable
  carries no unit of its own).
- A part-local variable renamed rewrites the requirements that read it, but
  the part's undo of the rename does not put them back (the requirements are
  project state, not on the part's undo stack); they then read unresolved,
  naming the variable. A project variable's rename undoes whole.
- The Design sweep window takes one `samples` for the sweep, not one per
  variable (design_sweep takes one).
- A sweep does not measure studies, assemblies or CAM jobs at its points.
