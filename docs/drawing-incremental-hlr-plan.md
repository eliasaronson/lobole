# Incremental exact drawing projection

Status: merged into `main` and integration-tested on 2026-09-08.
Implementation: `a88759aa`; merge: `3301b158`. The feature branch and its
`/tmp/designer-wtdrawing` worktree were removed after integration.

This is the completion record and remaining-work plan. The
[original proposal](drawing-incremental-hlr-plan.draft-backup.md) is archived
separately; its proposed scope is not a claim of completed work.

## Outcome

Drawing updates reuse complete model-space projections, self-visibility, and
visibility between unchanged pairs of bodies. Exact OCC projection remains on
the CPU. Sheet placement and annotation edits no longer force visibility work.
The engine benchmark improved a one-body move from 115.8 seconds to 16.7 seconds
and a subsequent body removal from 113.0 seconds to 0.47 seconds.

First-time projection remains expensive: populating the cache took 122.3 seconds
versus 113.0 seconds uncached in that run. The improvement is in repeated and
incremental updates. No serialization change or format-version bump was needed.

## Status of the original stages

| Stage | Delivered | Deferred or narrowed |
| --- | --- | --- |
| 1. Profile and establish a baseline | Outline time, aggregate self/pair time, call/hit counts, payload accounting, and opt-in spring/engine benchmarks | No detailed per-pair trace, separate extraction/sheet-assembly timers, or new engine top/isometric benchmark matrix |
| 2. Separate projection from layout | Model-space result cache; current leaf geometry and placements gathered on every update; hidden-line display filtering | No separately cached exact curves for resampling; deflection changes invalidate the projection key |
| 3. Reuse part preparation and self-visibility | Self-hidden parameter intervals reused for matching shapes, revisions, placements, and view axes | No cached OCC surface-preparation objects or reuse normalized to part-local coordinates |
| 4. Cache directed pair visibility | Independent pair results, tolerance-aware interval union, bounded caches, and source-ID rebinding | Keys use exact placements rather than relative-placement normalization; cache access is single-threaded |
| 5. Add measured spatial filtering | Existing OCC bounds rejection retained | No additional body-box, depth-range, or face/curve hierarchy filter was implemented |

The old projected-box experiment remains historical evidence: it skipped 72%
of engine pairs but changed 58.7 seconds to 56.8 seconds. It was not rerun as a
new optimization in this change. See [the earlier measurements](agent-scale-followup-plan.md).
The new engine's outline pass took about 0.4–0.5 seconds against roughly two
minutes total; this did not justify adding a retained OCC preparation layer.

## What was built

[DrawingProjection.cpp](../src/core/drawing/DrawingProjection.cpp) owns the cache
implementation and OCC interaction. [DrawingProjection.h](../src/core/drawing/DrawingProjection.h)
exposes `ProjectionCache`, statistics, and an optional `ProjectionOptions::cache`.
Passing no cache retains the uncached reference path; this is not an automatic
retry mechanism for cache errors.

- Complete results retain both visible and hidden curves in model coordinates.
  Position, scale, folded alignment, and stable-reference joining happen in
  [DrawingActions.cpp](../src/ui/DrawingActions.cpp). Annotation edits reuse the
  projection, and hidden-line display is a filter over the cached result.
  The complete-result cache also supports the existing polygonal mode; exact
  self/pair caching does not add dimension references to polygonal output.
- An exact view miss creates a fresh OCC algorithm and rebuilds outlines.
  Self-visibility and directed pair visibility can then be replayed. A missing
  pair starts from the body's self-visibility, independently of earlier
  occluders. Hidden intervals are combined through OCC's tolerance-aware edge
  status operations, allowing removal to reveal edges again.
- Keys retain shape identity (`IsEqual`), revision, exact placement, view axes,
  projection mode, and deflection. Drawing actions supply `Document::Version()`
  as the revision. There is no new persistent occurrence-path identity scheme:
  source IDs are rebound to the current gathered input list and its ElementMaps.
  Reordering or exclusion cannot replay an old positional attribution.
- Local HLR edge signatures check bounds, tolerances, curve type, and projected
  samples before interval replay. Degenerate pole edges are not curve-sampled.
  Shape handles keep identities alive; callers must respect the revision and
  geometry-immutability contract.
- Every drawing update gathers current leaf shapes and placements instead of
  relying only on the root assembly version. Nested part edits are therefore
  detected even when that root version does not change.
- Up to 16 complete views and 4,096 visibility entries are retained under a
  64 MiB payload budget. Each list promotes hits; payload pressure evicts
  visibility entries before view entries. Accounting excludes shared OCC
  geometry held by shape handles and is not a total-process-memory cap.
  Project replacement clears the cache in
  [DocumentSlots.cpp](../src/ui/DocumentSlots.cpp).
- Cancellation does not publish a completed view. Completed self/pair entries
  may remain reusable after a later cancellation. Existing last-good sheet
  geometry is retained on failure. Individual OCC calls remain uninterruptible.

The prototype's 512-entry limit thrashed on the saved engine's 32 bodies:
32 self entries plus 992 directed pairs exceeded it. The delivered limit fits
that workload, and the engine benchmark asserts the expected reuse counts.

[RemoteDrawings.cpp](../src/remote/RemoteDrawings.cpp) exposes these additions in
`list_drawing_views.cost`: `self_ms`, `pair_ms`, `self_calls`, `pair_calls`,
`self_hits`, `pair_hits`, `projection_hit`, and `cache_bytes`. Timing fields
describe the most recent projection request; a complete hit reports zero HLR
work, not the original population cost.

## Measured performance

OCC 7.9.3; a scratch copy of `scratch/aircooled/aircooled.dsgn`; 32 visible drawing
bodies; front view; default model-space deflection 0.05 mm. The benchmark uses
the first assembly by ID and its stored placements, moves the last gathered
body 5 mm in X, then removes the body at the middle index. These are observations
on the shared development machine, not CI timing thresholds or predictions for
other edits and views.

| Operation | Uncached exact | Cached exact | HLR work with cache |
| --- | ---: | ---: | --- |
| First projection | 113.0 s | 122.3 s | 32 self calls, 992 pair calls |
| Unchanged view | — | 0.18 ms | none |
| Move last body 5 mm in X | 115.8 s | 16.7 s | 1 self call, 62 pair calls; 930 pair hits |
| Remove one body after that move | 113.0 s | 0.47 s | none; 31 self hits and 930 pair hits |

Retained payload was 20.4 MB after the first projection and 22.8 MB after removal.
Cold population was about 8% slower; independent pair computation may do more
work than cumulative hiding in exchange for reusable results.

The separate four-turn solid spring/bolt/box benchmark used an isometric view:

| Operation | Uncached exact | Cached exact |
| --- | ---: | ---: |
| First projection | 8.14 s | 8.08 s |
| Unchanged view | — | 0.008 ms |
| Move the separate box | 5.95 s | 17 ms |
| Remove the bolt after that move | 2.70 s | 20 ms |

## Validation completed

- Seven focused cache tests passed: 4,083 assertions. Coverage includes source
  rebinding, hidden-line filtering, revision invalidation, movement, removal,
  reordering, cache clearing, coincident instances, view changes, pre-cancelled
  work, sphere poles, rotated occluders, sheet movement/scaling, annotation
  edits, and a nested part edit without a root assembly version bump.
- The spring benchmark passed 8,143 assertions. The final engine benchmark
  passed 245,395 assertions, including call/hit expectations and comparisons
  of cold, warm, moved, and removed-body results with uncached recomputation.
- `SameProjection` in [test_drawing_hlr.cpp](../tests/test_drawing_hlr.cpp) compares
  curve count/order, kind, visibility, source IDs and edge/face indices, curve
  type, closure, conic metadata, point counts, and sampled coordinates. Numeric
  geometry comparisons use a tolerance of `1e-8`. This validates extracted
  geometry and source references; it is not a direct comparison of every raw
  internal OCC interval or a proof for all possible assemblies.
- [test_actions.cpp](../tests/test_actions.cpp) checks layout reuse and the nested
  invalidation case through drawing actions. Existing drawing/dimension tests
  also ran in the full suite.
- After merge `3301b158`, `designer`, `designer_tests`, and `femgpu_tests` built
  successfully. **All 1,139 headless tests passed**, with no failures, in
  101.86 seconds. This supersedes the initial worktree run that had not yet
  built the separate FEM GPU test binary.
- The merged-main drawing GUI test `build_a_drawing_by_clicking` passed (1/1).
  Exact spring and engine sheets were also inspected in the isolated Xvfb
  renderer during feature validation. Cached sheet movement was checked
  numerically (30 mm for the spring, 5 mm for the engine), and scale edits
  reused projection. Screenshots supplement the geometry assertions.
- Staged-hunk formatting and `git diff --check` passed. The scratch renderer was
  stopped; the human instance on port 8799 was not used to edit documents.

## Reproduce

Run from the current checkout. Build and tests use nix; the GUI helper manages
its own environment. The two benchmarks are opt-in and were run separately
from the full headless suite. Use a scratch project copy for the engine run.

```sh
nix-shell shell.nix --run 'cd build && ninja designer designer_tests femgpu_tests'
nix-shell shell.nix --run 'cd build && ctest -LE gui -j$(nproc) --output-on-failure'
nix-shell shell.nix --run 'build/tests/designer_tests "[hlr-cache]"'
nix-shell shell.nix --run 'build/tests/designer_tests "[hlr-cache-benchmark]"'
nix-shell shell.nix --run 'DESIGNER_HLR_PROJECT=/path/to/copied/aircooled.dsgn build/tests/designer_tests "[hlr-project-benchmark]"'
./tools/gui-tests-xvfb.sh -f 'build_a_drawing_by_clicking'
```

## Cold generation, measured on a real sheet (2026-09-15)

The remaining-work list below asks for "new evidence of savings" before
touching cold generation. Here is the evidence, from the 35-occurrence smart
speaker assembly, exact quality, headless, cold cache each time:

| view | cold |
|---|---|
| front | 8.74 s |
| right | 5.13 s |
| **top** | **17.85 s** |
| iso | 2.22 s |

A four-view sheet is ~34 s. A sampling profile of it
(`tools/profile.sh --app`) puts **89% of a projection inside
`HLRBRep_Hider::Hide`** — curve-curve intersection, B-spline evaluation,
`math_FunctionSetRoot` — and our own `ProjectViewImpl` bookkeeping in the
noise. There is no waste to delete inside the projection.

**The cost is not spread across the assembly; it is two parts.** Hiding
occurrences and re-measuring cold:

| what is left out of the top view | cold |
|---|---|
| nothing (35 parts) | 18.02 s |
| the 14 T-nuts | 17.32 s |
| the 4 port parts (flares, tube, ring brace) | 16.11 s |
| **the 2 loudspeaker drivers** | **0.46 s** |
| drivers + port | 0.33 s |
| everything but the 8-part shell | 0.12 s |

Two imported driver models are **97% of the view**. That is the same shape
the `add_drawing_view` description already records from the engine ("47 s a
view, 0.0 s with its two valve springs left off"): exact HLR is priced by
surface complexity, and one bought part with NURBS surfaces outweighs thirty
extruded panels.

So the useful directions are not "make HLR faster":

- **Say where the time went.** A view that takes seconds should name the
  occurrences that cost it, the way a capture names a frame that is one
  colour. `hidden` already exists; nothing tells the user which two names to
  put in it.
- **Parallel views.** The four views are independent projections, and the
  sheet is the sum of them: ~34 s becomes ~18 s (the longest) at best. The
  prerequisite is the isolated snapshot below — OCC shapes are written to by
  the algorithms that read them (docs/performance/query-booleans.md).
- Bought parts are the ones that dominate and the ones least likely to carry
  a dimension, which is what makes a per-occurrence quality choice worth
  considering rather than a global one.

## Remaining work

These are follow-ups, not completed stages or prerequisites for using the
merged cache:

- Normalize reusable self/pair data to local coordinates or relative placements
  if benchmarks justify sharing across differently placed instances.
- Investigate cold-generation work, prepared surface reuse, or finer spatial
  rejection only with new evidence of savings beyond OCC's existing filters.
- Add isolated snapshots and verified source mapping before background jobs or
  parallel view workers. GPU/hybrid visibility remains a separate project.
- Expand cache-specific testing for eviction under memory pressure, cancellation
  during an update, undo/redo sequences, and more engine view directions.
  The original proposal's entire workload matrix was not newly benchmarked.
- Separate resampling from visibility if scale-dependent sampling is introduced;
  the current drawing path uses fixed model-space deflection.
