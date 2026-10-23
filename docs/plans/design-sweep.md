# Design sweep

## The problem

A generator states its intent in variables and comments ("a wall of at
least 4 mm round the bearing seat", "the plate takes 3 to 8 mm"), and
nothing holds the model to it:

- a relation between variables cannot be a requirement: every measure
  reads geometry through an RPC, so `od - bore >= 4` can only be faked
  with a `section_area` or a `volume` standing in for it;
- `set_variable` to a value the design never allowed answers `regen: ok`
  whenever the kernel still builds;
- whether the part rebuilds over its stated range is unknown until each
  value is typed in by hand. A fillet that cannot round a wall thinner
  than twice its radius, a sketch that loses its solution: each sits at
  some value in or near the range, found only by editing the user's
  document and undoing.

## A design range is not a tolerance

A tolerance is manufacturing variation about a nominal the model already
has: it drives no geometry, lives outside the regeneration hash, attaches
to a measured element and not to a variable
([tolerances](../tolerances/README.md#the-one-rule-a-tolerance-drives-no-geometry)).
A design range is the opposite: it is over the variables themselves, each
value in it is a different nominal, and the model is regenerated at every
one. Nothing here touches tolerances, fits or stack-ups; a sweep's points
are configurations nobody saved, not parts as made.

"Sweep" also names the Sweep feature and the clash sweep, so this one is
`design_sweep` throughout.

## Stage 1 — the `expression` measure

1. **The kind.** `expression` in `req::kMeasureKinds`, numeric: its reading
   is an expression over the project's variables and tables,
   `measure {kind: "expression", expression: "od - bore", unit?}`, bounded
   like any number (`at_least {value: 4}`, or an expression bound). `unit`
   is declared, as a formula's is, default `mm`; the expression is in base
   units (mm, degrees), so `"10mm"` and `"1cm"` read alike.
2. **Where it is evaluated.** By the one evaluator,
   `ExpressionEngine::Default().EvaluateText`, as a bound is. The scope:
   - no `document`: `state.projectSymbols`, the project variables with the
     tables' cells seeded (`ui::SyncProjectScope`), so `lookup(...)` reads
     a table as a variable does;
   - `document` (a part): that part's `Document::Symbols()`, as its
     features see them: local variables over the project's, with the
     active configuration's overrides `EvaluateParameters` applied;
   - `document` with `configuration`: the part as regenerated in that
     configuration (`ui::ConfiguredPartDocument`), whatever is active, the
     way a placement or a drawing view asks for one. A configuration the
     part lacks is refused naming the part's configurations.
   Without `document` no configuration is seen: a configuration belongs
   to a part.
3. **Unknown names.** `add_requirement` / `set_requirement` compile the
   expression and refuse one that does not compile (the evaluator's
   message and offset) or names an identifier its scope does not hold,
   naming the variables that scope does. `rename_variable` rewrites a
   requirement's expressions (measure and bound) as it rewrites features;
   `delete_variable` and `delete_table` are refused while a requirement
   reads the name, naming it. A name that goes anyway (a part deleted, a
   merge) reads `unresolved`, "variable 'bore' is not in 'Housing'", and an
   evaluation error (division by zero) `unresolved` with the evaluator's
   message. Neither is ever evaluated as 0.
4. **Evidence.** The value, the unit, the bound as evaluated, and
   `detail {scope, configuration?, inputs {name: value}}`: every identifier
   the expression read with its value then, so a failure reads
   "od - bore = 3 mm (od 18, bore 15), must be >= 4 mm"
   (`req::FindingLines`). The input hash is the names and values the
   expression and bound read in that scope (`ExpressionNames` and
   `CurrentHash`, which today read only `projectSymbols`), plus the
   configuration id, plus the content of every table a `lookup` names, so
   a cell edited makes it stale.
5. **It is checked where the values move.** One costs microseconds, so it
   does not wait for Check: wherever the project scope is synced or a part
   regenerates, each `expression` requirement whose input hash moved is
   measured in place. It does not refuse the edit: `set_variable`,
   `set_table` and the configuration RPCs answer `requirements [{id, name,
   state, value, bound}]` for each whose state changed, in their own
   response, as `regen` reports a broken feature; so the call that thins
   the wall answers `fail`, and the panel shows it without a Check.
6. **The panel.** `RequirementsPanel.cpp` draws the row as any number's;
   its tooltip adds the expression and its scope, and the finding line
   under a failing or unresolved row is the one above.
7. **Saved form.** The JSON shape is the existing one, but a new kind is a
   format change, as `section_area` and `insertion` were: a build that
   does not know it refuses the file. `vNEXT`: `kVersionPending` raised,
   golden `tests/golden/vNEXT-expression-requirement.json` + `.expect.json`
   (a project-scope and a configured part-scope one, with evidence), a
   row in format.md's *Pending changes*.

## Stage 2 — the design sweep

1. **The RPC.** `design_sweep {variables?, from_requirements?, samples?,
   corners?, refine?, documents?, max_points?, async?, outlive_client?}`:
   - `variables [{name, min, max, scope?}]`, each end a number or an
     expression in the variable's base units (mm, degrees; the describe
     row says so, for `tests/rpc_units_gate.py`);
   - `from_requirements: true` takes each range an `expression`
     requirement puts on one bare variable (`within {min, max}`); a
     one-sided bound needs its other end in `variables`;
   - `samples` per variable, ends included, evenly spaced (default 5, at
     most 33), each variable varied alone with the others at their values;
     `corners: true` adds the box's corners (up to 8 variables) and centre;
   - `refine` bisects each bracket below (default 0, at most 8 times);
   - `documents` narrows the parts rebuilt; by default every part whose
     parameters or features read a swept variable, directly or through
     other variables.
2. **Refused before anything starts** (a refused call changes nothing):
   an unknown key; a variable not in scope, naming those that are; one no
   part reads; one the active configuration of a rebuilt part overrides,
   naming the configuration (the sweep would change nothing there, as a
   held DOF refuses a drag); `min` above `max` or a non-finite end;
   `samples` or `refine` out of range; a plan over `max_points` (default
   64, ceiling 256), naming the count it would be and the cap, never cut
   short; `from_requirements` with no requirement that bounds one
   variable, naming those it looked at; a `documents` id that is not a
   part; a second sweep while one runs, naming its job.
3. **On a copy.** The main thread writes each part it rebuilds to its saved
   form with a copy of the project variables and tables; the worker reads
   them back as `ReadProject`'s parallel load does
   ([opening a project](../ui/long-operations.md#opening-a-project)), so
   no OCC shape is shared with the open document and `AppState` is not
   touched. A swept value overrides the variable's expression on the copy,
   as a configuration does, so its dependents follow. A worker walks its
   points in order on one copy, the regen cache re-executing only what the
   variable reaches, on at most `parallel::Workers()` copies.
4. **What each point records.** For each rebuilt part: `regen` ok or
   broken, the broken features `{feature_id, name, error}`, each sketch's
   solve state `{feature_id, ok, dof, conflicting}` where it is not ok or
   its `dof` differs from the base, `volume_mm3` and `mass_g`
   (`MassOfBodies`). And every requirement it can measure on the copy:
   `expression`, `mass`, `volume` and `section_area` of a rebuilt part,
   through the same functions the check calls (the RPC bodies factored
   onto a `const Document&` so the two agree by construction). The others
   (studies, assemblies, CAM) are listed once in `not_swept` with why: a
   study per point is minutes, and an assembly reads every part. A point
   fails when a part is broken, a sketch does not solve, or a measured
   requirement fails or is unresolved.
5. **The answer.** `{points [{values, parts, requirements, ok}], base,
   first_failure {name: {below?: {last_ok, failed, why}, above?: ...}},
   not_swept, planned, cap, seconds}`. `first_failure` is the failing
   value nearest the variable's own value on each side, with the last
   good one before it, so the break is bracketed and named with the
   broken feature's own error. Nothing is saved and the format does not
   move; the last sweep stays in memory, answered by
   `design_sweep_result {job?}`.
6. **A job and its rows.** Kind `design_sweep` on the registry
   ([long operations](../ui/long-operations.md#the-registry)): `async`
   answers with the `job`, otherwise the call parks
   ([requests that wait](../architecture.md#requests-that-wait)).
   `study_status {job}` follows it (`label` "cam_d = 27.5 mm (7 of 15)",
   `fraction`), `cancel_study {job}` stops it between points keeping what
   was measured, and it goes with its client unless `outlive_client`.
   `design_sweep` is NotInBatch (it parks on a job, as
   `check_requirements` does); `design_sweep_result` is Deferrable.
7. **The GUI.** A Design sweep window (`src/ui/DesignSweepPanel.cpp`),
   opened from a variable's row in the Variables panel ("Sweep...") and
   from the Requirements panel ("Sweep the ranges", `from_requirements`).
   One row per variable (name, from, to, samples); the default range is
   half to twice the variable's value, derived from it, and a variable at
   zero has no default and says so. A line under the rows gives the
   planned count against the cap before Run. Run (`###sweep_run`) shows
   progress on `DrawJobProgress` with Cancel (`###sweep_cancel`); the
   result is a table, a row a point, failing cells in the fail colour with
   the reason in their tooltip, and each variable's `first_failure` as a
   line above it. The window changes nothing in the document. The SDK's
   `App.sweep(...)` runs the job, waits and returns the table.

## Files

- Stage 1: `src/core/Requirements.{h,cpp}` (the kind, its keys,
  `FindingLines`); `src/remote/RemoteRequirements.cpp` (the measure, the
  scope in `CurrentHash`, the in-place check, describe rows);
  `src/remote/RemoteProject.cpp`, `RemoteConfigurations.cpp` (the
  `requirements` in their responses); `src/ui/VariableActions.cpp` (rename
  rewrites, delete refusals); `src/ui/RequirementsPanel.cpp`;
  `src/core/io/SerializationInternal.h`, `tests/golden/vNEXT-*`.
- Stage 2: `src/core/DesignSweep.{h,cpp}` (planning, the point walk, what
  a point records; no `AppState`); the `design_sweep` RPCs and job in
  `src/remote/RemoteRequirements.cpp`; the part-only measures factored
  out of `RemoteInspection.cpp`; `src/ui/DesignSweepPanel.cpp`, an entry
  in `VariablesPanel.cpp` and `RequirementsPanel.cpp`;
  `python/lobole/sdk.py`, `rpc.py` and `rpc-surface.json` remade.
- Docs: `docs/requirements/README.md` (the measure, the sweep),
  `docs/variables/README.md` (rename and delete see requirements),
  `docs/ui/long-operations.md` (the job), `docs/versioning/format.md`.

## Verified by

- Stage 1, `tests/test_requirements.cpp`: project- and part-scope
  expressions pass and fail; a configuration's override is seen through
  `document` and not without it, and `configuration` reads that one while
  another is active; a table cell edited makes a `lookup` stale; an
  unknown name or a malformed expression is refused naming the scope's
  variables; a rename rewrites it, a delete is refused naming it, a part
  removed reads unresolved naming the variable, a division by zero reads
  unresolved; the golden round-trips with its evidence.
- Stage 1, `requirements_smoke.py`: a `set_variable` that thins the wall
  answers `requirements` with the row `fail`, the value put back answers
  `pass`, and `list_requirements` agrees without a check. GUI
  `requirements/an_expression_fails_as_you_type`: a value edited in the
  Variables panel turns the row red with its finding, no Check pressed.
- Stage 2, `tests/test_design_sweep.cpp`: a block of wall `t`, every edge
  filleted at `r`, swept over `t` breaks between the samples either side
  of `2 r` and names the fillet, and `refine` narrows that bracket; a
  triangle dimensioned `a`, `b`, `c` swept over `c` reads its sketch not
  ok past `a + b`; corners count `2^n + 1`; `from_requirements` takes a
  `within` range and refuses a one-sided one; the user's document (its
  version, bodies and variables) is unchanged after a sweep.
- Stage 2, `tests/design_sweep_smoke.py`, on a generator part: the pan/tilt
  head's camera (`build_tilt.camera`, built alone) swept over `cam_d` from
  a quarter of its value to its value. The grip is `cam_d / 3` thick and
  its edges are filleted at `cam_grip_r`, which does not follow `cam_d`,
  so `first_failure.cam_d.below` brackets `6 * cam_grip_r` (read from the
  part, not written in the test) and names "Grip edges". The async form is
  a job, `cancel_study` keeps the points done, and a sweep whose client
  leaves is cancelled.
- `refused_calls_smoke.py`: every refusal in stage 1 item 3 and stage 2
  item 2, each leaving the document and the job list as they were.
- GUI `requirements/a_sweep_by_clicking`: Sweep from a variable's row, Run,
  the failing row in the fail colour with the first-failure line, and
  Cancel on a held sweep ending it at once with the frame kept.
- `tests/rpc_units_gate.py`; the headless, GUI and pygui suites.
