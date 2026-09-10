# Design intent, validation and agent iteration

Status: proposed implementation plan, 2026-09-08. No implementation in this
change. Derived from the OM615 iteration; applies equally to human and agent
modeling. The target is a reviewable connection between requirements, model
inputs, measured geometry and validation evidence.

The next implementation order is refined in
[SDK contracts and iteration](sdk-iteration-plan.md), including generated typing,
bulk snapshots, incremental component updates and their acceptance gates.

## Existing foundations and actual gaps

- `src/core/Document.h::Parameter` stores name, expression, comment, evaluated
  double and transient error. Both project and part scopes exist. Unit suffixes
  are converted to base values; `Expression.h::SymbolTable` is double-valued,
  with no dimensional analysis. See `docs/variables/plan.md`.
- Regeneration already has a prefix cache and executed/cached counters.
  Generator reuse across runs is a separate problem; do not replace that cache.
  See `docs/modeling/regeneration.md`.
- Mutation responses already carry geometry digests. Motion results already
  include occurrence IDs, exact overlap volumes, centroids and optional poses.
  Geometry comparison, contact reports and multi-view rendering also exist.
  See `docs/inspect/agent-validation-plan.md` and `RemoteMotion.cpp`.
- The missing layer is consistent evidence delivery, persistence/invalidation,
  reusable generator identity and explicit acceptance requirements. Avoid
  implementing a second measurement or motion engine.

## Core distinction

Variables describe inputs to the nominal design. Tolerances describe permitted
variation of specified quantities. Requirements describe what must be true.
Measurements and validation results are evidence about a particular revision.
These should be linked, but should not all be mutable variables.

Four concepts must remain distinct:

1. Manufacturing tolerance: permitted variation of a produced characteristic.
2. Requirement bounds: acceptable outcomes, including functional ranges.
3. Reference uncertainty: how well a source establishes a target dimension.
4. Numerical precision: representation, solver residuals and kernel tolerances.

A photograph estimate is not a manufacturing tolerance. A production tolerance
must not become a larger boolean epsilon. Passing a nominal clash check does
not establish clearance across manufacturing variation.

## Proposed data model

All new persisted entities have stable IDs; names are editable labels.

### Parameter extensions

Retain the existing nominal expression and evaluated value. Add optional:

- Quantity kind and display unit; first support length, angle and dimensionless
  values. Define base-unit semantics explicitly at every API boundary.
- Bilateral/asymmetric size tolerance as signed lower and upper deviations
  from nominal. Validate ordering, units and finite bounds. Untoleranced does
  not mean zero manufacturing variation.
- Source reference, source location and provenance category: supplied,
  sourced, estimated, derived or unspecified. Preserve comments.

Do not put arbitrary requirements, geometric tolerances or stochastic
probability distributions into a pair of deviation fields. Parameter-only
metadata is insufficient for local dimensions and measured characteristics:
allow the same size-tolerance definition on an explicit dimension/measurement
binding. Sharing a variable does not force every manufactured feature it drives
to have the same tolerance.

Keep nominal expression evaluation deterministic. Introduce quantity checking
with a documented policy for existing bare literals and legacy parameters.
Do not imply that adding a `unit` string makes the current evaluator unit-safe.
Reject known incompatible quantities; report unresolved types explicitly.
Dependency-aware dimensional inference and tolerance propagation are separate
steps. Tolerance-only edits do not rebuild unchanged nominal B-Reps.

### Requirement

Proposed fields: ID, name, rationale, source references, target scope, check
kind, target/bounds, severity, evidence policy and revision. Checks refer to
stable part/occurrence/datum/measurement IDs, not display names or arbitrary
face array positions. A reference that cannot resolve becomes unresolved.

Initial kinds:

- Measured dimension or quantity within independently specified bounds.
- Required component count, role or spatial relationship.
- Required contact/clearance and fitted-joint conditions.
- Motion quantity or clash condition with declared study and coverage.
- Explicit human review item for evidence that cannot yet be automated.

Geometry-binding checks and independent acceptance checks have different
purposes. `measured bore == bore variable` verifies implementation of the
input. A source-backed range verifies that the chosen input is appropriate.
Both are useful; the first must not be presented as the second.

Changing a model parameter cannot silently change an independent acceptance
bound. Requirement edits are explicit, versioned and invalidate old evidence.
Show requirement changes separately in a change preview; follow existing user
and project authorization rules rather than introducing blanket approval gates.

### Validation result

Use one report envelope across checks: requirement/check ID, target IDs,
model/configuration revision, requirement revision, algorithm/settings,
status, expected/observed values with units, diagnostic code, evidence and
coverage. Include source revision when source content is versioned locally.

Statuses distinguish pass, fail, not-run, unresolved, unsupported, stale and
execution-error. A truncated sweep is incomplete evidence, never a pass.
A sampled motion result states its sample count and domain. Retained last-good
geometry after regeneration errors must not validate as a healthy current model.

Cache results only when the relevant geometry, transforms, configuration,
requirements and check settings match. Presentation changes need not invalidate
pure geometry checks. Aggregate status must show mandatory coverage and unknowns;
do not hide unchecked requirements behind a green "valid" badge.

## Delivery sequence and acceptance gates

### 1. Make failures directly inspectable

Normalize existing digests and inspection/motion results into the shared report
shape. Preserve complete reports behind report IDs; SDK exceptions include the
report reference, diagnostic code and a concise summary. Add a GUI action and
RPC to isolate targets and show the failing pose/overlap using existing view and
comparison machinery. Viewing evidence should not change the saved design pose.

Promote suspicious boolean outcomes into structured diagnostics: material loss
on an ordinary additive operation, removed volume exceeding a known cut-tool
volume, unexpected disconnected bodies and no-op operations. Scope these rules
to operations for which the invariant is valid; transformations/replacements
need their own contracts. Expensive validation should be explicit and budgeted.

Audit the sketch-to-kernel precision boundary exposed by the arc-path bug.
Add fractional-coordinate arc/hole cases and distinguish numerical failure from
physical interference. Keep renderer precision concerns separate.

Acceptance: a bad manifold operation and a motion clash each produce a complete
saved report and an inspectable view without rerunning the check; duplicate
occurrence names remain unambiguous; partial results cannot pass. Measure
report overhead on the full engine, not only primitive shapes.

### 2. Reuse generator work across runs

Add an explicit generator namespace, stable keys for owned parts/features and a
manifest of inputs, dependencies, generator version and produced entities.
Initially use declared dependency tracking with conservative invalidation.
Reuse the existing regeneration cache within each retained part.

Preview the update before applying it: create/update/reuse/conflict. Preserve
unowned human edits and detect edits to owned output. Never infer permission to
delete an unrelated part because it is missing from a generator manifest. Apply
updates as undoable transactions; retain the last successful result and expose
failed attempts. Support interrupted-run recovery and explicit ownership transfer.

Acceptance: rerunning an unchanged generator executes zero modeling features;
editing one manifold rebuilds only its declared dependents; changing cylinder
spacing invalidates every affected dependent; a manual edit produces a visible
conflict, not silent overwrite. Compare semantic/B-Rep results and validation
verdicts against a clean full build. Test both warm reuse and cold restart.

### 3. Add toleranced quantities and executable dimensional requirements

Implement parameter/dimension metadata, source records and unit semantics, then
a minimal requirement evaluator over existing measurement queries. Keep measured
outputs read-only in the validation layer; feeding them back into design inputs
requires an explicit separate solve operation to avoid dependency cycles.

Expose identical capabilities in Variables/Properties, a requirements panel and
RPC/SDK. Show nominal, limits, source, observed value and evidence status. Give
existing parameters an explicit unspecified metadata state.

Acceptance: asymmetric limits round-trip; rename preserves identity; incompatible
units are rejected or explicitly unresolved; a tolerance-only edit rebuilds zero
nominal geometry but invalidates affected evidence. Deliberately modifying both
a dimension and its driving variable still fails an independent source-backed
requirement. Cover expression-error/last-good-value and configuration cases.

Persisted changes require the normal format-version bump, golden fixture,
round-trip and undo/redo checks; follow the project's current no-migration policy.

### 4. Extend requirements to function, architecture and manufacturing

Add a small, explicit catalog of checks, each with prerequisites and evidence
limits. Start with component counts/roles, datum-relative layout, required
connections and clearances, fastener engagement and simple tool-access envelopes.
Reuse motion studies for lift, stroke, ratios and sampled collision checks.

Add manufacturing checks incrementally: selected-section wall thickness, selected
faces' draft against a declared pull direction, and declared machining/tool access.
Each reports what was actually evaluated. Draft alone does not prove mold release;
minimum wall thickness alone does not prove castability. Report unsupported core
withdrawal or process assumptions as review items. Datum-based GD&T belongs to
measured geometry and datum systems, not scalar variable tolerances.

Use reference images as review evidence, with camera/scale/alignment uncertainty.
Existing geometric comparison can verify against trusted CAD references. A visual
model may flag mismatches, but a plausible render must not certify dimensions or
manufacturability. Keep automated measurements and human/visual judgments distinct.

Acceptance: a valid, clash-free assembly with the wrong component layout fails a
layout requirement; a correctly sized but inaccessible bolt fails the declared
access test; unimplemented checks remain visibly unverified.

### 5. Evaluate variation and robustness

Begin with explicitly supported analytical relationships and conservative bounds.
For independent bilateral hole/shaft size limits, diametral clearance bounds are
`hole_min - shaft_max` and `hole_max - shaft_min`. State whether clearance is
radial or diametral. This does not by itself account for position/form errors.

Preserve dependencies and shared-variable correlation; derived dimensions are not
independent random variables. Arbitrary nonlinear expressions cannot be certified
by evaluating only all-minimum/all-maximum configurations. Add bounded parameter
studies on disposable configurations, with deterministic sampling, budgets and
failure locations. Label sampled studies as exploration, not worst-case proof.
Statistical yield estimates require explicit distributions/correlations and are
out of the initial scope.

Acceptance: nominal clearance can pass while the supported worst-case fit fails;
shared-variable cancellation does not create false variation; parameter studies
leave the nominal model unchanged and always report coverage/unsupported bounds.

## How to establish that the right thing was built

Before generating geometry, capture a small requirement set from the user's
brief and references. Record what is known and what is assumed. Bind each
requirement to measurable characteristics or an explicit review item. Use those
requirements to select inspections, then report achieved coverage after edits.
Do not let the generator silently rewrite its own acceptance criteria.

Use OM615 as a regression fixture, not the only fixture. Pair it with a simple
shaft/housing joint and a small bracket assembly so behavior is understandable
without waiting for the engine. Include deliberately wrong models: valid but
missing a component, correct variables but incorrect geometry, correct nominal
fit but unacceptable limits, and an attractive shape with an unsupported source.

Success metrics: time to locate and inspect a failure; unchanged-run executed
features; edit-to-validated-preview time; false-pass rate on seeded defects;
mandatory requirement coverage. Avoid a single opaque "design confidence" score.

## Licensed engineering catalogs — first implementation

Implement user-imported bearing catalogs before connected vendor APIs or PDF
extraction. CSV/JSON inputs use an explicit column mapping and mm/in units.
Preview validates required dimensions, optional mating limits and provenance;
installation is a separate undoable operation. No geometry changes on import.

Catalog versions are immutable and identified by namespace, supplied revision
and a SHA-256 fingerprint of normalized records. Unknown fields remain reported
as unsupported. Missing fit limits remain unverified, not inferred from nominal
bearing dimensions. Queries return pinned record handles. A bearing-seat preview
compares explicitly supplied measured shaft/housing/fillet dimensions to supplied
catalog limits, with independent statuses for missing measurements or limits.

Storage is explicit: reference-only by default (no records or local filesystem
paths in saved projects), or embedded when the user chooses to include permitted
data. Reference-only catalogs reopen unresolved and can be resolved by explicitly
reimporting matching content. Existing versions never update silently. Dataset
exports obey the same choice. STEP links are metadata only in this slice; they
are not fetched or bundled automatically. Provider APIs, tool/holder catalogs,
PDF extraction and automatic geometry application remain later milestones.

Deliver core parsing/validation with tests; project persistence and undo plus
RPC/SDK; then GUI preview/install/query and live integration checks. Give each
milestone a conventional commit. Seed fixtures are synthetic and carry explicit
provenance, never copied vendor data. Include malformed CSV, unit conversion,
duplicate IDs, invalid dimensions, changed-content refusal, reference-only
round trips, missing requirements, and no-regeneration-on-import checks.

### First-slice delivery status

Implemented local bearing CSV/JSON import, column mapping, explicit units and
provenance, immutable content-pinned revisions, project persistence, cross-tab
undo/redo, reference/embedded exports, RPC/SDK handles and a Variables-panel
preview/install/browser. Exported catalog packages can be imported again.
See [the import guide](../parts/catalog-imports.md) and
[synthetic example](../../examples/catalogs/synthetic-bearings.csv).

Seat checks currently consume caller-supplied measurements and explicitly supplied
limits. They do not persist geometry bindings or requirements. The broader phases
above remain planned: typed quantities/tolerances, persistent requirements,
geometry-bound evidence, manufacturing/access checks and variation studies.
Connected SKF/Sandvik adapters and tool/holder schemas remain future work; no
vendor dataset or redistribution permission is assumed by this implementation.

Validation for this slice: all 1,131 headless tests pass; the rendered GUI suite
passes 284/284; the live remote GUI smoke passes with a new panel-focus regression.
A manual rendered walkthrough covers form entry, preview, install, browsing and
seat observations. That walkthrough exposed and fixed test-engine focus restoration
between RPCs and clipped labels in the default narrow sidebar.

## Follow-up evidence from the revised OM615

The workshop revision in `generators/om615/om615.py` now checks actual head datums,
27 mm injector wrench flats and the presence/size of a modeled pump register.
Its report distinguishes sourced limits from estimated interfaces. This is
**generator-local evidence**, not completion of the persistent Requirement
model above. The currently implemented `bearing-v1` catalog schema cannot
represent these head/injector dimensions. A future generic dimensional schema
should support a named characteristic, units, independent bounds and a source
page, rather than treating an engine component as a bearing.

Two further examples reinforce the fitted-joint requirement: a screw can have
zero clashes while ending before its tapped part, and an odd ray-crossing count
can hide a blind floor. The standard-parts fitter now checks selected screw
engagement and classifies ambiguous ray intervals. Engine validation continues
to require the standing sweep as well as motion; neither substitutes for
joint engagement or fluid/machining requirements.
