# Archived proposal: incremental exact drawing projection

Superseded on 2026-09-08 by the [completion record and remaining-work plan](drawing-incremental-hlr-plan.md).
Preserved as the original pre-implementation proposal. The statuses, scope,
and validation requirements below describe that proposal, not what shipped.

---

# Incremental exact drawing projection

Status: proposed, 2026-09-08. Implementation has not started.

## Objective

Make assembly drawing updates reuse unchanged geometry, self-visibility, and
part-to-part visibility. Preserve exact curves, visible/hidden classification,
and dimension references. Measure cold generation separately from incremental
updates; caches cannot accelerate work that has never been computed.

Scope: CPU exact HLR and its cache architecture. GPU visibility, approximate
spring representations, and background/parallel execution are separate work.
The design should allow isolated worker jobs later without requiring them now.

## Current behavior and evidence

- `src/core/drawing/DrawingProjection.cpp` creates one OCC algorithm per view,
  builds outlines, calls `Hide(i)` for self-visibility and `Hide(i,j)` for every
  other shape, and extracts attributed curves.
- OCC performs bounds rejection inside the pair calls. A visited pair does not
  necessarily perform expensive intersections.
- `src/ui/DrawingActions.cpp` caches completed sheet views using drawing/source
  versions. A drawing version change invalidates all views. Projection and sheet
  placement are already separate functions, but only the placed result is cached.
- Prior measurements in `agent-scale-followup-plan.md`: 5.5 seconds self-hiding
  and 41.8 seconds pair work on the engine. A separate projected-box experiment
  skipped 72% of pairs but changed 58.7 seconds to 56.8 seconds. These historical
  numbers are context, not the baseline for the new implementation.
- Source bodies currently use their index in the gathered list as projection
  identity. That is unsuitable as a persistent cache key: exclusions/reordering
  can change indices. Nested assembly attribution also differs from leaf identity.

## 1. Establish a reproducible baseline and inspect OCC costs

Use an isolated instance and a copy of
`scratch/aircooled/aircooled.dsgn`; never mutate the human instance on port 8799.
Record the actual OCC version, build, model, view direction, and tolerances.

Instrument outline/preparation, self-hiding, each directed pair, extraction,
and sheet assembly. Record cache hits, actual HLR calls, retained memory, and
cold/warm wall time. Keep detailed pair tracing opt-in.

Workloads: engine front/top/isometric, bare solid four-turn spring, spring plus
bolt, repeated identical parts, and a sparse box grid. Exercise unchanged update,
view movement, note edit, scale change, hidden-line toggle, component movement,
part geometry edit, occurrence exclusion, and view rotation.

Inspect/profile the expensive pair's OCC internals to separate repeated surface
preparation from necessary curve/surface intersection. Do not infer that all
pair cost is reusable from similar pair timings alone. Investigate batched
occluder processing if setup is repeated, with the existing path as an oracle.

Deliverable: reproducible benchmark command and timing table identifying the
dominant reusable work. No speedup estimate becomes a promise before this gate.

## 2. Separate projection caching from sheet layout

Cache the model-space `ProjectionResult` separately from `SheetView`; apply
position, scale, folded alignment, annotations, and stable-reference joining
without rerunning HLR when projection inputs are unchanged.

Define explicit projection dependencies: geometry revisions for all referenced
leaf parts, occurrence paths and placements, visibility/exclusions, view axes,
projection mode, and applicable numerical settings. Audit nested-part revision
propagation rather than relying on the assembly's own version alone.

Retain both visible and hidden geometry so a hidden-line display toggle can be a
filter. Keep extraction tolerance separate from classification where possible;
if sheet-scale quality needs finer sampling, resample curves rather than repeat
visibility calculations, or conservatively invalidate the affected layer.

Gate: unchanged updates and layout/annotation-only edits make zero HLR calls;
geometry and orientation changes never reuse stale results. Preserve last-good
geometry and existing failure behavior. This stage can ship independently.

## 3. Prototype reusable part preparation and self-visibility

Introduce stable internal body identity distinct from drawing attribution:
source document/body identity plus full occurrence path where needed. Explicitly
remap cached edge/face identities to the current gathered bodies and ElementRefs.
Never persist positional body indices as identity.

Prototype an immutable prepared-part entry keyed by geometry revision, view
direction in part coordinates, and HLR settings. Retain projected candidate
curves, source mapping, self-hidden curve parameter intervals, and any useful
surface preparation that OCC permits safely reusing. Translation reuse is valid
for orthographic projection with explicit coordinate remapping; rotation reuse
requires an equivalent local viewing direction and correct in-plane mapping.

Do not cache mutable shared `HLRBRep_Algo` state or assume copying an OCC handle
isolates it. Determine whether public APIs can import/export the needed state;
if an OCC adapter or upstream change is necessary, document its scope before
building the full cache. A completed 2D part drawing alone is insufficient to
classify other parts hidden behind its surfaces.

Gate: repeated identical parts reuse preparation/self-visibility with identical
geometry and dimension references. Benchmark saved time against memory/copy
cost. If this does not bypass expensive OCC work, keep stage 2 and revise this
prototype before proceeding.

## 4. Cache directed pair visibility and update dependencies

For edges of A hidden by faces of B, cache B's hidden parameter intervals on A's
candidate curves. Key by both geometry revisions, relative placement, relevant
view orientation, and numerical settings. Entries must describe the pair
independently, not visibility accumulated from whichever occluder ran first.

Combine the self-hidden intervals and all applicable pair intervals by union;
their complement gives visible intervals. Preserve interval tolerances, conic
metadata, silhouettes, and source references through extraction. First prove
that this decomposition matches OCC on intersecting/touching and coincident
geometry; do not assume simple polyline concatenation is sufficient.

Moving/changing B invalidates B's affected self data and both directed pair sets
involving B. Reuse unaffected pairs. Removing B must restore edges it previously
hid; the cached final visibility alone cannot support this operation. Determine
the affected set from both old and new placement. Use immutable cache entries,
a bounded LRU/memory budget, and publish only complete successful results.

Gate: cold output and incremental output match full recomputation; changing one
component recomputes only affected dependencies. Report cold overhead, warm
speedup, and memory. Preserve a full-recompute fallback for unsupported cases.

## 5. Add only measured spatial filtering

Evaluate conservative view-space projected bounds and depth intervals, using
tolerance padding. Disjoint projected bounds reject both directions. A body
entirely behind another cannot hide the nearer body's edges, but the reverse
direction may still matter. World-space separation alone is not sufficient.

Bounds are rejection tests, never proof of full occlusion: holes and spring gaps
remain empty. If whole-body bounds duplicate OCC's checks, investigate smaller
face/curve groups or a spatial hierarchy only where profiling supports it.
Charge hierarchy construction to cold timings and reuse it where valid.

Gate: retain a filter only if it saves meaningful wall time on representative
workloads with identical results. The previous 72% rejection result is a warning
against optimizing pair counts instead of expensive work.

## Validation and rollout

Compare against uncached exact projection using source-attributed curve geometry,
visible/hidden intervals, conic information, dimensions, and sheet bounds within
declared tolerances. Curve count or screenshots alone cannot establish equality.
Include spring gaps, bores, complete occlusion, touching/intersecting solids,
coincident edges, reordered/excluded bodies, nested assemblies, repeated parts,
rotated instances, undo/redo, model switching, and cancellation/failure recovery.

Extend `tests/test_drawing_hlr.cpp` and drawing integration coverage with behavior
tests for implemented stages. Keep expensive timing benchmarks opt-in; use call
counts/cache statistics for stable no-recompute assertions, not fragile CI timing
limits. Visually inspect an engine sheet from the isolated renderer and check
real dimension values after cache invalidation operations.

Build through nix: `nix-shell shell.nix --run "cd build && ninja designer designer_tests"`.
Run focused drawing tests during development, the required headless suite before
integration, and relevant GUI tests/screenshots for UI changes. Follow the
repository formatting hook. Runtime caches need no serialization change; any
persisted-format change requires the normal version bump and golden fixtures.

Land reviewable stages in order: instrumentation, model-space projection cache,
part-cache feasibility/prototype, pair cache, then measured spatial acceleration.
Use conventional commit messages. Background workers and parallel views can
follow once cache entries, source mappings, and snapshot ownership are proven.
