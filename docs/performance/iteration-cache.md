# Iteration stages and exact contact reuse

Measured on 2026-09-08 with the real OM615: 78 parts, 250 occurrences, 49 samples
through 720 degrees. Commands and workflow are in
[iteration-workflow.md](../scripting/iteration-workflow.md).

| Operation | Measured result |
|---|---:|
| Standing exact sweep | 806 candidates, zero clashes, 15.31 s |
| First exact motion study | 19,327 checks, zero clashes, 169.64 s |
| Repeated motion, same live model | 19,327 cache hits, **0 new booleans**, 4.93 s |
| Unchanged intake update + preview + save | **3.61 s**, builder skipped |
| Forced intake rebuild + staged apply | **2.20 s**, geometry reused; one datum resolved |

The repeated study was **34.4× faster**. All 49 sample results matched the cold
run after excluding optional pose payloads. Stroke, valve lift, firing order and
reference-dimension gates passed in the full generator run. These are one-run
observations, not percentile budgets. The initial generator build overlapped a
C++ rebuild (165 s), so it is **not** evidence of generation speed improvement
or regression against the previous unloaded 66.2 s measurement.

Local artifacts: `/tmp/om615-iteration/{timings.json,motion-report.json,
warm-motion.json,standing-report.json,validation-report.json}` and
`reuse/{component-update.json,timings.json,validation-report.json}`. These are
reproducible scratch outputs, not checked-in fixtures.

## What is cached and where

`collide::ExactContactCache` lives in the application session, owned through
`AppState` and passed to the core motion runner. It retains up to 50,000 exact
pair results with FIFO eviction. Keys include both geometry identities, internal
placements, and the exact bits of both world matrices. Shape equality verifies
internal placements on a hit, guarding against placement-hash collisions.
Retained shapes prevent pointer recycling from creating false hits.

It caches successful overlap answers, including clashes and their volumes and
centroids. Unknown/failed booleans are not clearance and are not cached. Motion
coverage still counts every candidate; `pairs_cached` and `pairs_executed`
separate reuse from work. Cache hits do not change broad-phase coverage, solve
results, rigid-pair exclusions or sampled-motion scope.

No rounding of poses and no approximate shape matching are involved. A common
rigid movement conservatively misses, as can mathematically equal poses whose
floating-point representations differ. New geometry identities invalidate only
the affected pair entries. Material-only regeneration can retain identities.

The cache is not serialized. Reopening, rebuilding every part, or replacing the
application loses useful geometry identity reuse. GUI background motion still
clones and regenerates Documents, so this benchmark applies to the synchronous
SDK runner. Immutable background geometry snapshots are the next prerequisite
for comparable GUI reuse. The standing sweep is not cached by this change.

## Keep iteration cheap first

Preview skips standing and motion collision checks. Local performs the exact
standing sweep, conservatively over the whole visible assembly. Full runs the
standing sweep plus defined exact motion studies. Missing, skipped, stale or
partial evidence never makes `accepted` true.

Generated component manifests live in part documents. Unchanged fingerprints
skip builder execution; changed builders use a staging part and an undoable
recipe apply. The application reuses the staging B-Rep rather than rerunning its
booleans. Datum resolution still executes against the installed geometry and is
included in `regen.executed`; `geometry_reused` independently verifies shape
identity against staging. Small-box regression tests prove zero executed
features, while parts with datums intentionally have a nonzero count. A forced
intake rebuild on the final binary cached 34 features and resolved its one
mounting datum during apply. All engine occurrence/feature IDs survived, undo
restored ownership, and a subsequent local standing sweep passed. That probe
changed the input fingerprint to exercise a real builder run with identical
output; the small regression fixture separately verifies changed dimensions.
Its report is `/tmp/om615-iteration/component-dogfood.json`.

Further pose scheduling, relative-transform canonicalization, persistent
content-addressed caches and neighbor-scoped standing sweeps should be measured
against this shorter loop. None is needed for every shape-edit preview.
