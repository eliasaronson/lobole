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
| `study_probe` | part | `run_study`, `probe_study` | mm (or as `unit` says) | `study`, `case?`, and one face (`face`, `at?`, `quantity?`: translation \| rotation \| displacement_at) or a formula (`readings`, `expression`, `unit?`) |
| `study_result` | part | `run_study`, `study_results` | mm, MPa, or a factor | `study`, `quantity`: max_displacement \| max_von_mises \| factor_of_safety, `case?` |
| `clearance` | assembly | the gap `interference` measures | mm | `a`, `b` (top-level occurrences), `mode?`: mesh \| exact, `motion_study?` |
| `contact` | assembly | `interference` | verdict | `a`, `b`, `expect`: clear \| touching \| not_clashing |
| `mass`, `volume` | part or assembly | `mass_properties` | g, mm3 | |
| `fit_class` | assembly | `check_fits` | verdict | `mate`, `class`: clearance \| transition \| interference |
| `check_fits` | assembly | `check_fits` | verdict | `allow_unverified?` |
| `check_seat` | assembly | `check_seat` | verdict | `occurrences` (the bearings), `allow_unverified?` |
| `check_fasteners` | assembly | `check_fasteners` | verdict | `allow?` [{`part?`, `why?`, `max?`}], `allow_skipped?` |
| `check_literals` | part, parts, or none | `check_literals` | verdict | `documents?` (several parts); neither: every part |
| `motion_clean` | assembly | `run_motion_study` | verdict | `study` |
| `cam_verify` | job | `cam_verify` | verdict | `setup`, `resolution?`, `allow_uncut?` [what prefixes] |

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
  standing clash.
- `cam_verify`: ok and complete, it cuts something, no operation failed,
  every thread judged in its band, every toleranced wall in its band, and
  every hole or thread the job leaves uncut matches an `allow_uncut` prefix.

**A clearance over a motion study** (`motion_study`) is the smallest gap
between the pair over that study's samples: each side's bodies moved from
where they stand now to where the study put them. The study is run, or its
result reused, first; a sample that did not solve makes the measure
unresolved, since the gap there is unknown. Only top-level placements move:
a flexible placement's inner poses are not followed.

**A formula over several faces** (`study_probe` with `readings`): each
reading is a `probe_study` of one face, `{face, at?}`, and the expression
sees `<name>_tx/_ty/_tz` (its translation, mm), `_rx/_ry/_rz` (its rotation,
radians), `_dx/_dy/_dz` (its motion carried to `at`) and `_cx/_cy/_cz` (its
centroid), beside the project's variables. It is how a requirement reads
what a body held by several faces did: the pan/tilt head's lens, carried by
two bearing bores, a shoulder and a worm on a ledge
([generators/pantilt](../../generators/pantilt/README.md#requirements)).
An `at` component may be an expression too.

## Bounds

A number is bounded `at_most {value}`, `at_least {value}` or
`within {min, max}`; a verdict is bounded `pass`. Each value may be an
expression over the project's variables (`"run_gap / 2"`), evaluated when
the requirement is checked; the evidence keeps what it evaluated to.

**Refused, naming what is accepted:** an unknown measure kind, an unknown
key for the kind, a missing required key, an unknown value of a key that
has a vocabulary (`quantity`, `mode`, `expect`, `class`), an unknown bound
kind, a number bounded `pass` or a verdict bounded by a number, `within`
with its min above its max, and a requirement naming a document, study,
occurrence, mate or setup that does not exist. A file holding any of these
does not load: the requirement would load as something it is not.

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
| `motion_clean`, a clearance over a study | the motion study's (`motion::InputKey`) |
| `clearance`, `contact` | the two occurrences as saved and what each places |
| the other assembly measures | the assembly's content through every sub-assembly and every placed part's bodies (`motion::AssemblyInputKey`), with each placed part's tolerances and print process |
| `mass`, `volume` of a part | its bodies, tolerances and print process |
| `check_literals` | the part's features, bodies and variables, and the project's variables |
| `cam_verify` | the job's parts (each part's key) and setups as saved |

and the values of every variable a bound's or a formula's expression reads.
So a heavier camera makes the yoke's deflection requirement stale (its
study's loads read `load_kg`) and leaves the base plate's CAM requirement
current.

## States

A requirement shows one of five states, and only one is a pass:

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

Synchronously it measures in order and answers with every row, the counts
of each state and a receipt (a pass or fail claim per current requirement,
an unverifiable one per other). `async: true` starts a job, kind
`requirements`, unless nothing needs measuring (then it answers at once):

- the job has no worker thread; its `finish` steps on the main thread, one
  requirement a frame, so the app stays responsive;
- a measure that starts a study job (or needs CAM passes cut) waits for it,
  frame by frame, and then measures; a study that fails or is cancelled is
  that requirement's evidence (unresolved), not a reason to start it again;
- it waits while any other study job runs, one run at a time;
- `study_status {job}` follows it (`label` names the requirement being
  measured, `fraction` how far through), `cancel_study {job}` stops it and
  the study it waits on, and a job started over the RPC is cancelled with
  its client unless `outlive_client`.

A measure runs on its document's tab (a study is the active part's), so a
check visits each requirement's tab and comes back to the one it started on,
keeping the camera where it was; a tab switch clears the selection.

## Clearance assertions

The declared clearances (`add_assertion`, v97) are clearance requirements:
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
source, the note, when it was checked and at which revision), the value with
its unit (or "clean" / "findings" for a verdict) and the bound as evaluated.
**Check** starts the job for every requirement that is not current, and
shows its progress while it runs; Shift+Check measures every one again. The
rows come from the same engine `list_requirements` answers with, through a
hook the RPC module installs (`ui::SetRequirementsEngine`), and are cached
on the documents' save stamps. Requirements are stated over the RPC, the
SDK or MCP; the panel shows and checks them.

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

The SDK (`tools/designer_sdk.py`): `App.require(name, measure, at_most= |
at_least= | within=(lo, hi), source=)` states one, or edits the one of that
name, so a generator re-run states the same set; `App.verify_requirements()`
checks as a job, waits, and raises naming every requirement that is not
`pass`. The MCP workflow prompt says to state requirements and check them
before delivering.

## Tests

- `tests/test_requirements.cpp` (`[requirements]`): the refusals and what
  they name, the five states, bounds, the saved form round-tripping with
  evidence and expressions, a lone part's file carrying them, and the v100
  golden's assertions loading as requirements.
- `tests/requirements_smoke.py`: on a bar under load and the slider-crank,
  each measure kind passes and fails, an edit makes stale only what it
  reaches, an edited requirement reads unchecked, a deleted reference
  unresolved, a re-check measures nothing, the async check is a job, both
  project forms carry the evidence current; and on a shaft in a housing,
  check_fits and fit_class pass and then fail on an interference shaft
  (stale before the check: a tolerance is an input), check_seat fails a
  bearing on no seat, check_fasteners over no screw is not a pass.
- `tests/assertion_smoke.py`: the clearance verbs, as requirements.
- GUI `requirements/panel_checks_and_goes_stale`: the panel appears with
  the requirements, Check measures them as a job, and an edit to the model
  makes them stale.
- Golden `v160-requirements` (a lone part's envelope).
- The pan/tilt head (`generators/pantilt`) states its acceptance criteria as
  requirements and stops on any that does not pass.

## Not done

- A study on an assembly cannot be measured: `probe_study` reads part
  studies only, and an assembly study's input hash needs its context.
- A clearance over a motion study does not follow a flexible placement's
  inner poses.
- The panel shows and checks; it does not state or edit a requirement.
- A formula's `unit` is declared, not derived from its expression.
