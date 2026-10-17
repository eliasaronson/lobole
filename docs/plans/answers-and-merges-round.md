# Answers that hide, and merges that collide

## The problem

The showcase builds (clock, Strandbeest, engine) and their merges left a
list of answers that are wrong or unreadable, and of friction in parallel
work.

- **A failing requirement does not say why.** The Requirements panel
  truncates the finding ("finding…") and nothing writes it to the log. The
  user saw "the engine turns: fail" and had no way to learn the reason.
- **`check_print`'s clearance prune depends on meshing.** `SeparatedByOBB`
  builds the box with `BRepBndLib::AddOBB`, which uses the triangulation when
  one exists. The same pair can be pruned or not, and a pruned pair can
  really be inside the limit, by up to the deflection.
- **`mass_properties` misindexes per-body boxes** when reference bodies come
  before real ones (`which` counts the wrong list).
- **A Derived copy loses its source's tolerances**, so a printed press or an
  ISO band on a source part is not on the copy an assembly places.
- **The planetary still does not require stacking.** It was waived while
  the air was an unmeasured 0.6 mm default. The measured K5 air is 0.4 mm,
  under the planetary's 0.525 mm.
- **Merges collide on lists.** Three of the last five branch merges
  conflicted on the same rows: the big-generators table in
  `generators/README.md`, the budget list in `tests/CMakeLists.txt`, the
  `RUNS` table in `tests/generator_runs.py`, and the generated
  `python/lobole/rpc.py`.
- **Agents' instances are invisible to `tools/scratch_port.py`.** An instance
  run with a scratch `XDG_STATE_HOME` announces itself there, not in
  `~/.local/state/lobole/instances`, so a port it holds can be handed out
  again. An instance from a deleted worktree ran for 33 hours unnoticed.
- **Small ones:**
  - `export_stl` takes `deflection` while `check_print` takes
    `stl_deflection_mm`;
  - the `cam` relation's help does not say a lift table must stop short of
    360°;
  - the `background_jobs` row does not list the `contact` kind;
  - extrude has no symmetric option.

## The shape of the fix

1. **A failing requirement's finding is whole wherever it is shown.** The
   Requirements panel shows a failing or unknown row's whole finding (a
   wrapped detail line under the row, and the tooltip), and every check run
   logs each failing requirement's name and finding. A GUI test fails a
   requirement and reads the finding back from the panel.
2. **The clearance prune is exact.** The OBB comes from the B-Rep without
   the triangulation (`AddOBB` with `useTriangulation = false`), so the
   prune is the same meshed or not and never prunes a pair inside the limit.
   A test checks the same pair before and after meshing.
3. **`mass_properties` per-body boxes** are indexed by the body they
   measure. A test puts a reference body first.
4. **A Derived copy carries its source's size tolerances** (ISO and
   printed), mapped by the derived face's source face. A test derives a
   printed press and finds it relieved on the copy.
5. **The planetary requires stacking** under a profile that states the air.
   On the built-in table it is still reported and not required, and its
   README says why.
6. **Merges do not collide on generator lists.**
   - Each big generator declares itself in one file in its own directory,
     `generators/<name>/test.json`, holding its command, its success
     markers, its budget, the instance kind and the README row text.
   - `tests/generator_runs.py`, the CMake budgets and the README table are
     made from those files: the table between markers, by a tool, with a
     `[docs]`-style test that fails when it is stale.
   - `tools/merge-branch.sh` regenerates `python/lobole/rpc.py` (and
     `api/rpc-surface.json`) when a pick conflicts only on them, rather than
     stopping.
7. **Every instance is visible to the port picker.**
   - An instance also writes a one-line claim in a machine-wide directory
     that does not follow `XDG_STATE_HOME`
     (`/tmp/lobole-ports-$UID/<port>`, holding its pid).
   - `scratch_port.py` reads those claims and drops dead pids.
   - `tools/stray-instances.sh` lists every running lobole, its port, its
     cwd (marked if deleted) and its age, so an orphan is one command to
     find.
8. **The small ones:**
   - `export_stl` takes `stl_deflection_mm`, and the old name is refused,
     naming the new one;
   - the cam help states the 360° rule;
   - `background_jobs` documents `contact`;
   - extrude gains `symmetric: true`, which extrudes half the distance each
     way, with a GUI checkbox and a defaults-suite case.

## Files

- `src/ui/` (the Requirements panel), `src/remote/` requirement checks
  (logging).
- `src/core/PrintCheck.cpp` (the OBB), `src/remote/RemoteInspection.cpp`
  (`mass_properties`).
- The Derived feature and `src/core/FitBands.*`.
- `generators/planetary/`, `generators/*/test.json`, `tests/generator_runs.py`,
  `tests/CMakeLists.txt`, `generators/README.md`, `tools/merge-branch.sh`,
  a table tool and its test.
- The instance announcement (main / RemoteServer), `tools/scratch_port.py`,
  `tools/stray-instances.sh`.
- `export_stl`'s handler and MethodDoc row, the cam relation doc, the
  `background_jobs` row, the Extrude feature, its serialization and the
  GUI.
- Docs for each.

## Verified by

- a test for each item that fails before it;
- the headless, GUI and generators suites;
- a dry run of `merge-branch.sh` on a branch that conflicts only on
  `rpc.py`.
