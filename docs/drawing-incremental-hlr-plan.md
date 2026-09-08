# Incremental exact drawing projection

Status: implemented, 2026-09-08. See the implementation record for measured
scope and deferred optimizations.

## Objective

Make assembly drawing updates reuse unchanged geometry, self-visibility, and
part-to-part visibility. Preserve exact curves, visible/hidden classification,
and dimension references. Measure cold generation separately from incremental
updates; caches cannot accelerate work that has never been computed.

Scope: CPU exact HLR and its cache architecture. GPU visibility, approximate
spring representations, and background/parallel execution are separate work.
The design should allow isolated worker jobs later without requiring them now.

## Baseline behavior and evidence

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

## Implementation record

Implemented on `feat/incremental-drawing-hlr` in `/tmp/designer-wtdrawing`.
The runtime cache is opt-in at the core API and enabled for drawing actions;
passing no cache to `ProjectView` retains the uncached exact reference path.

- Complete model-space results are cached independently of sheet placement,
  scale, annotation edits, and hidden-line display. Both line classifications
  are retained. Every update gathers current leaf geometry and placements,
  rather than trusting only the root assembly version.
- Self-hidden and directed pair-hidden parameter intervals are cached through
  OCC's public edge-status APIs. No OCC fork or retained mutable algorithm is
  required. Each pair is computed from self-visibility independently of other
  occluders; its hidden intervals are unioned using OCC's tolerance-aware status
  operations. Removal can therefore reveal previously hidden edges.
- Keys retain shape identity, geometry revision, exact placement, view axes,
  projection mode, and deflection. Source IDs are rebound to the current input
  list; reordering/exclusion cannot replay an old positional attribution.
  Local HLR edge signatures guard interval replay after outlines are rebuilt.
- The cache retains at most 16 complete views and 4,096 visibility entries, with
  a 64 MiB payload budget. This accounting excludes the shared OCC geometry
  retained by shape handles; it is not a cap on total process memory. Replacing
  the project clears the cache. Access remains single-threaded.
- `list_drawing_views.cost` now reports `self_ms`, `pair_ms`, `self_calls`,
  `pair_calls`, `self_hits`, `pair_hits`, `projection_hit`, and `cache_bytes`.
  Existing outline/hide times now describe the most recent projection request;
  a complete cache hit reports zero HLR work.

The first engine experiment exposed cache thrashing with a 512-entry prototype:
this saved file produces 32 bodies, 32 self entries and 992 directed pairs.
The corrected limit accommodates those dependencies, and the project benchmark
asserts the expected reuse after a single body's placement changes.

Deliberately not added: transform-normalized reuse between differently placed
instances, persistent on-disk caches, shared prepared OCC surface objects,
background jobs, or another coarse bounding-box filter. The current engine's
outline construction measured roughly 0.4 seconds versus roughly 119 seconds
for a full uncached front view, so retaining additional mutable preparation is
not justified by that profile. OCC continues to do spatial rejection internally.
Cold generation remains expensive; independently computed pair entries may do
more work than cumulative hiding, in exchange for reusable results.

Reproduce the focused checks and opt-in comparisons through nix:

```
nix-shell shell.nix --run 'build/tests/designer_tests "[hlr-cache]"'
nix-shell shell.nix --run 'build/tests/designer_tests "[hlr-cache-benchmark]"'
nix-shell shell.nix --run 'DESIGNER_HLR_PROJECT=/path/to/copied/aircooled.dsgn build/tests/designer_tests "[hlr-project-benchmark]"'
```

The project benchmark reads the first assembly in the copied project, uses its
stored placements, and checks cold/warm projection, moving the last body 5 mm
in X, and removing one body against uncached recomputation. The spring benchmark
constructs a solid four-turn spring, a bolt and a box. Timings are observations
on the shared development machine, not CI thresholds.

Validation: seven focused cache tests passed (4,083 assertions), including a
nested part edit with an unchanged root assembly version; the four-turn
spring comparison passed (8,143 assertions). The headless suite passed 1,129
available tests; the fresh worktree initially lacked the separately built
`femgpu_tests` binary, then all eight of those tests passed after building it.
The drawing GUI test passed. Exact spring and engine sheets were inspected in
the scratch Xvfb renderer; moving/scaling the views performed no HLR work and
preserved the measured bounds. No serialization changes were required.

Final engine benchmark (OCC 7.9.3, 32 bodies, front view):

| Operation | Uncached exact | Cached exact | Actual HLR calls with cache |
| --- | ---: | ---: | --- |
| First projection | 113.0 s | 122.3 s | 32 self, 992 pairs |
| Unchanged view | — | 0.18 ms | none |
| Move last body 5 mm in X | 115.8 s | 16.7 s | 1 self, 62 pairs; 930 pair hits |
| Remove one body after that move | 113.0 s | 0.47 s | none; 31 self and 930 pair hits |

The final run passed 245,395 assertions, including exact-output comparisons and
expected reuse counts. Retained payload was 20.4 MB after the first projection
and 22.8 MB after the removal. Cold population was about 8% slower in this run;
the measured benefit is incremental updates, not faster first-time HLR.
A preceding rerun received SIGTERM before completion; the final run above was
restarted in its own process session and completed successfully.

For the spring/bolt/box workload: uncached/cold were 8.14/8.08 seconds, unchanged
was 0.008 ms, moving the separate box was 17 ms versus 5.95 seconds uncached,
and removing the bolt was 20 ms versus 2.70 seconds uncached. These smaller
workload timings came from the earlier run during worktree validation.
