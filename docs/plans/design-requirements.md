# Persistent design requirements and evidence

## Problem

The app can measure almost anything and check clashes, fits, contact and
motion, but nothing records what the design must satisfy. A generator states
its acceptance criteria in its own Python (OM615's head datums, injector
flats, pump register), and those checks live and die with the script:

- A requirement is not stored with the project, so a later edit cannot be
  checked against it, and a generator can silently rewrite its own criteria.
- Results are not bound to a model revision. A check run before an edit
  looks exactly like one run after it.
- Results do not share a shape. `run_motion_study`, `contact_report`,
  `check_fits` and `verify_assertions` each report in their own vocabulary;
  receipts (`src/core/Receipt.h`) unify the verdict, not the target, the
  expected/observed values or the revision.
- A check that verifies the model implements its input (measured bore ==
  `bore` variable) is indistinguishable from one that verifies the input is
  right (the bore is within a source-backed range).

Four quantities must stay distinct and currently have no place to be kept
apart: manufacturing tolerance (exists, `docs/tolerances/`), requirement
bounds (acceptable outcomes), reference uncertainty (how well a photo or
source pins a dimension), and numerical precision (kernel and solver
tolerances). A production tolerance must never become a larger boolean
epsilon, and a nominal clash check does not establish clearance across
variation.

## Fix

1. **A `Requirement` entity**, persisted per project with a stable id:
   name, rationale, source reference, target scope (stable part, occurrence,
   datum or element ids — never display names or face indices), check kind,
   bounds, severity, and a revision. Kinds, in order: a measured quantity
   within independent bounds; a component count or role; a required contact
   or clearance (generalising `add_assertion`); a fitted-joint condition
   (generalising `check_fits`); a motion quantity with its declared study and
   coverage; and an explicit human review item. A reference that no longer
   resolves makes the requirement `unresolved`, never silently dropped.
2. **One result envelope** for every check: requirement id, target ids, model
   and configuration revision, requirement revision, algorithm and settings,
   status (`pass | fail | not_run | unresolved | unsupported | stale |
   error`), expected and observed values with units, a diagnostic code, and
   coverage. A truncated or sampled result states what it covered and can
   never be a pass. A retained last-good body after a failed regeneration
   never validates as current. Results are cached only while the geometry,
   transforms, configuration, requirement and settings they read match.
3. **Requirement edits are explicit and versioned.** Changing a model
   parameter cannot change an independent bound; editing a requirement
   invalidates its old evidence.
4. **A generic dimensional source record** alongside the catalog's
   `bearing-v1`: a named characteristic, quantity and unit, source location,
   evidence category and optional bounds, reusing the catalog's provenance,
   immutable revisions and reference/embedded storage. Importing a record
   neither creates a requirement nor changes geometry.
5. **RPC, SDK and a requirements panel** with the same capabilities: list,
   add, edit, evaluate, and a view of nominal, bounds, source, observed value
   and evidence status. An aggregate never hides an unchecked mandatory
   requirement behind a green badge.
6. Later: manufacturing checks (wall thickness at a section, draft against a
   pull direction, tool access), each reporting exactly what it evaluated;
   and bounded parameter studies on disposable configurations, labelled as
   exploration rather than worst-case proof.

## Files

`src/core/` (a `Requirement` type beside `Document`/`AssemblyDocument`,
evaluation over the existing `Measure`, `check_fits`, assertion and motion
code), `src/core/io/Serialize*.cpp` (a format bump, golden and history row),
a new `src/remote/RemoteRequirements.cpp` with its `MethodDoc` table,
`tools/designer_sdk.py`, a GUI panel under `src/ui/`, `docs/inspect/README.md`.

## Verification

- Seeded wrong models cannot produce an all-green report: a valid, clash-free
  assembly with a component missing; correct variables with wrong geometry;
  a correct nominal fit with unacceptable limits; a short screw that clears
  everything.
- Editing both a dimension and the variable that drives it still fails an
  independent source-backed requirement.
- A geometry edit, occurrence move, configuration change and reopen each
  invalidate exactly the evidence that read what changed, reported `stale`.
- Requirements round-trip through save/load and undo/redo; a rename keeps
  the id.
- Small fixtures (a shaft and housing, a bracket assembly) for the unit
  tests; OM615 for coverage and cost.
