# SDK contracts, generator reuse and design validation

Status: implementation plan, 2026-09-08. Documentation only.

This is the next delivery sequence after the OM615 workshop revision. It
refines the order in [design-intent-plan.md](design-intent-plan.md); that plan
remains the definition of quantities, tolerances, requirements and evidence.
Reuse existing regeneration, measurement, inspection and motion machinery.

## What the engine established

The revised engine has 78 parts / 250 occurrences. Its unchanged-edge query
cleanup reduced frame RPCs from 4,073 to 773. A full build still takes about
85 s and complete sampled validation about 312 s. Component builds are useful,
but integrating their changes still requires too much reconstruction.

Failures spanned different layers: malformed oblique kernel geometry, a screw
that could not reach its thread, hidden blind-hole floors, and a dimensionally
wrong head that passed clash checks. No single type system can detect all four.

## Architecture decisions

1. Extend the existing RPC description/registration mechanism with a shared
   machine-readable contract. Generate Python stubs, request/result types,
   agent-facing descriptions and reference documentation from it. Do not
   hand-maintain a parallel set of RPC wrappers or infer contracts solely from
   regex scans of handler code. Keep the current vocabulary gate while migrating.
2. Preserve dynamic discovery for new RPCs. Static tooling uses shipped stubs;
   connection negotiates contract version/hash and capabilities with the actual
   server. Document-format version is a separate concern. An unknown method may
   remain callable dynamically, but must not be advertised as statically checked.
3. Python typing helps authors and agents before execution. Runtime checks still
   run on the server for every caller, including raw RPC clients. Client checks
   provide earlier errors; they do not replace core/domain invariants.
4. Separate structural, reference, quantity and geometric validation. Cheap
   checks precede mutations. Expensive geometry/studies remain explicit and
   budgeted. Never run a clash sweep implicitly on every feature edit.
5. GUI and SDK share domain rules, identities and reports. Existing authorization
   applies; an update preview does not introduce a mandatory confirmation dialog.

## Contract layers

| Layer | Examples | Enforcement |
|---|---|---|
| Request/result structure | Required fields, enum literals, exactly one radius/diameter, tagged feature variants, absent versus null | Shared contract; local preflight and server boundary |
| Semantic handles | Part versus occurrence IDs, face versus edge, owning project/part, ambiguous matches | Static handle types where possible; runtime ownership and resolution |
| Quantities and frames | Length versus angle, expression result dimension, local versus world points, point versus direction | Explicit API semantics, typed helpers and runtime quantity/frame checks |
| Operation invariants | Finite numbers, nonzero direction, valid parameter relationships, expected solid count or volume behavior | Core/domain validation with operation-specific rules |
| Design acceptance | Source-backed head height, screw engagement, required port layout, clearance, tool access | Persistent requirements evaluated against measured geometry |

Do not globally reject negative distances: reversed extrusions are legitimate.
Do not accept booleans as numeric dimensions merely because Python bool is an
int subtype. Reject NaN/infinity explicitly, including at JSON serialization.
Defaults, units and normalization must agree between client and server.

Use standard Python typing: annotations on existing ergonomic classes; generated
`.pyi` signatures; `TypedDict` for wire records; `Literal`/tagged unions for
variants; nominal ID types for raw identifiers; overloads for mutually exclusive
forms. Strong handles must carry actual runtime ownership as well as static tags.
A `NewType` alone does not validate an ID or make a float a physical quantity.
The statically checked facade must not hide unknown methods behind an `Any`
catch-all. Keep an explicit raw `app.call(...)` escape hatch for newly discovered
methods, clearly outside static coverage until its stubs are regenerated.

Use JSON Schema 2020-12 for the expressible wire structure, with a documented
supported subset. Cross-field arithmetic, live-reference checks and B-Rep facts
remain named semantic validators. Keep application-specific metadata for units,
side effects, costs, revision dependencies and diagnostic codes. Do not assume
an agent tool provider supports every schema construct: adapters must disclose
unsupported constraints, and the server must continue enforcing them.

## Delivery phases and acceptance gates

### 1. Contract and diagnostics pilot

Start with the paths the engine uses: sketch/extrusion inputs, feature variants,
geometry queries, fastener fitting and interference results. Inventory current
required/optional fields, result shapes, units and side effects before changing
behavior. Move one domain at a time to the shared contract, rather than blocking
all improvements on typing the entire RPC surface.

Return structured errors with stable code, field path, expected/observed values,
operation/target identifiers and optional report ID. Distinguish invalid input,
stale reference, conflict, kernel failure, failed requirement and incomplete
check. Store complete expensive-check reports so exceptions never discard the
only evidence. Annotate snapshot versus live read, expensive operation, mutation
and context-switch behavior in generated documentation.

Acceptance:
- Wrong enum, missing field, conflicting radius/diameter, bool/NaN dimensions
  and malformed nested requests fail before mutation, without kernel work.
- Positive and negative type-check fixtures verify the generated Python API;
  invalid fixtures must fail for the intended diagnostic, not unrelated errors.
- Raw RPC clients receive equivalent validation. Generate conformance cases
  from the schema, and test valid handler responses against result contracts.
- Defaults/normalization agree. Capability/version mismatches are explicit.
- Optional/partial results cannot become a successful validation verdict.

Suggested commits: `feat: publish typed RPC contracts`,
`feat: generate SDK types and validation diagnostics`.

### 2. Explicit bulk measurement snapshots and reference contracts

Add one explicit inspection operation that accepts a scope and requested fields
and returns immutable measurements from a consistent revision. Include topology
references, owner IDs, configuration, geometry/transform revisions and coordinate
space. Reading a returned center/axis/radius must be a local operation. Offer a
separate explicit refresh/resolve operation for live data.

Reuse current element-query machinery; request only needed fields, paginate or
budget large snapshots, and prevent mixed revisions across pages. Part-local
snapshots survive occurrence movement; world-space snapshots depend on placement.
Define the lifecycle separately for semantic datum references and measured
snapshots. A semantic reference can be re-resolved after regeneration; a snapshot
must not quietly pretend its old coordinates are current. Detect replacement,
missing and ambiguous topology. Include project/session identity to avoid ID reuse
across reopen or reconnect. Resolve and check revisions atomically with mutations.

Avoid immediately changing the meaning of existing `Edge.center`. Introduce the
explicit API, migrate generators, then decide whether to deprecate hidden reads.

Acceptance:
- Matching N hole rims uses a bounded number of bulk requests rather than
  repeated queries inside the matching loop; local property reads issue zero RPCs.
- A geometry edit, occurrence move, configuration change and project reopen each
  invalidate only the appropriate observations, or produce an explicit stale error.
- Duplicate occurrence names remain unambiguous. No silent nearest-face fallback.
- Measure latency, transferred bytes and server work on a bracket and the engine.

Suggested commit: `feat: add revision-bound geometry snapshots`.

### 3. Generator update sessions and stable component identity

Introduce an explicit generator namespace with stable component/datum keys,
owned-output manifest, declared dependencies and input fingerprints. Include
builder implementation/version, units, external files/catalog fingerprints and
relevant configuration. Manual dependency declarations are conservative; undeclared
ambient inputs or nondeterministic builders must opt out of reuse. Builder keys
identify design roles, not a body's current shape hash.

Pilot at component granularity. Build a changed component in staging, validate it,
and replace it while preserving its part/occurrence identity and stable datums.
Reuse the existing feature prefix cache when reconciling retained feature keys.
Resolve affected mates and geometry references before committing the update;
missing datums/topology are explicit conflicts. Defer arbitrary fine-grained DAG
inference until component replacement is reliable.

Provide a dry-run change set (create/update/reuse/retire/conflict), then an undoable
apply. Record ownership and expected revisions; detect human edits before applying.
Unowned objects survive. Retiring formerly owned output is explicit and must
check downstream references. Support cancellation/recovery without exposing half
an assembly as the successful result. Retained last-good geometry carries a failed
attempt status and cannot validate as the new intended revision.

Acceptance:
- Unchanged warm and cold runs execute zero modeling features and create no
  no-op undo entries. Renaming labels does not replace stable identities.
- Changing one manifold rebuilds only its declared dependents; cylinder-spacing
  changes invalidate all affected components. Imported-source changes propagate.
- Human edits conflict visibly, and an intervening edit invalidates an old preview.
- Failure/cancellation rolls back; undo restores the whole update and bindings.
- Compare B-Rep/semantic results, mates and validation verdicts to a clean rebuild.

Suggested commits: `feat: track generator component ownership`,
`feat: apply incremental generator updates`.

### 4. Persistent dimensional requirements and independent sources

Implement the quantity/tolerance/source model in the parent plan, followed by
geometry-bound requirements and revision-bound results. Start with length, angle
and dimensionless quantities. Plain numeric convenience arguments retain documented
method-specific base units. Explicit quantities convert at the boundary. Typed
expressions need real dimensional checking, not string prefixes or a unit label
on an otherwise untyped evaluator. Unknown dimensions must remain unresolved.

Add a general dimensional source-record schema alongside `bearing-v1`: a named
characteristic, quantity/unit, source location, evidence category and optional
independent limits. Missing limits remain unspecified. Reuse catalog provenance,
immutable revisions and reference/embedded storage rules. Importing a record does
not automatically turn it into an acceptance requirement or modify geometry.

Deliver bindings and GUI/SDK views for the OM615 head height and injector flats,
plus a simple shaft/housing fit. Keep input-to-geometry consistency checks distinct
from independent acceptance bounds. Tolerance edits do not rebuild nominal solids,
but do invalidate affected evidence. Use the report statuses from the parent plan;
do not collapse stale, unsupported or partial results into boolean success.

Acceptance includes independent bounds surviving parameter changes, round-trip
and undo, unit mismatch rejection, unresolved sources, and checks refusing failed
regeneration with last-good geometry. Persisted changes require version bumps and
golden fixtures under the repository policy.

Suggested commits: `feat: add sourced quantities and dimensional records`,
`feat: persist geometry-bound requirements and evidence`.

### 5. Functional checks and repeatable visual review

Start with explicit component roles/counts, datum-relative manifold layout,
fastener engagement, selected tool-access envelopes and existing motion quantities.
Add a declared passage network with named openings and required separation between
coolant, oil, gas and exterior. Pilot an explicitly bounded passage check; report
unsupported geometry or incomplete sampling as unverified. Do not claim arbitrary
fluid connectivity from a valid solid or from a few sections.

Add saved review presets: front, both sides, selected sections, injector close-up,
and the failing motion pose. Align comparisons to named datums; preserve camera,
scale, visibility and renderer settings. Images are visual evidence, not dimensional
proof. A visual mismatch creates a review item, never a certified tolerance failure.
Keep original reference files and their usage terms separate from generated exports.

Acceptance: seeded missing parts, wrong-side ports, a short screw, blocked tool
access and an unsupported passage check cannot produce an all-green report. A
review bundle can be opened without rerunning geometry/studies or altering the
design's saved pose. Draft, section wall thickness and process-specific casting
checks follow this first functional slice, with explicit coverage limits.

Suggested commit: `feat: add functional checks and reproducible review bundles`.

## Rollout and measurement

First implementation slice: typed contracts for geometry inspection plus explicit
measurement snapshots and structured errors. Next: component reuse. This supplies
useful agent ergonomics before the larger quantity/requirement system is complete.

Use small fixtures for most regression tests, the shaft/housing and bracket for
integration, and OM615 for realistic performance/coverage. Preserve seeded false-pass
cases. Track unchanged-run executed features, changed-component rebuild scope,
RPC count/bytes, edit-to-preview and edit-to-validated times, diagnostic localization,
false passes and mandatory coverage. Compare repeated warm/cold runs under similar
load; report correctness costs separately from overhead. Set performance budgets
from those baselines rather than promising unmeasured speedup ratios.

Definition of done per slice: core behavior, RPC/SDK parity, applicable GUI exposure,
positive/negative tests, updated examples/docs and reviewable conventional commits.

## Standards references

- [Python typing specification](https://typing.python.org/en/latest/spec/):
  gradual typing, stubs, overloads, nominal identifiers and typed records.
- [TypedDict specification](https://typing.python.org/en/latest/spec/typeddict.html):
  structural typing for request/result dictionaries.
- [JSON Schema composition](https://json-schema.org/understanding-json-schema/reference/combining)
  and [conditional validation](https://json-schema.org/understanding-json-schema/reference/conditionals):
  wire variants, alternatives and structural constraints. These do not establish
  reference validity, physical units or geometric correctness by themselves.

## Implementation progress (2026-09-08)

The first inspection pilot is implemented: canonical request/result schemas for
`geometry_snapshot` and `validate_snapshot`, generated Python types and facade,
client/server validation, structured errors (including nested batch failures),
and immutable bounded part-local edge snapshots. Static positive/negative fixtures,
wire parity and stale-document integration tests accompany it. See
[usage and limits](../scripting/inspection-snapshots.md).

This is the first delivery slice, not completion of phases 1–2 in full. Legacy
inspection/modeling contracts, face/world snapshots, pagination, atomic revision
guards on writes and semantic reference re-resolution are still pending. Component
update sessions, persistent quantities/requirements and functional/review bundles
remain subsequent phases; none is implied by the snapshot revision token.
