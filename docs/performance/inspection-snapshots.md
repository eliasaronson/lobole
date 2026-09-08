# OM615 inspection snapshots — measured 2026-09-08

The typed inspection pilot is described in
[inspection snapshots](../scripting/inspection-snapshots.md). OM615 now captures
one frozen circular-edge measurement set per part during fastener fitting. It
reuses those sets across bolt groups and validates all eight part revisions
before finishing the stage. It does not cache measurements between generator runs.

## Fresh generator comparison

Consecutive fresh-process builds on the same workstation and C++ binary:

| Stage | Previous query path | Explicit snapshots |
|---|---:|---:|
| Bottom end | 5.411 s | 5.531 s |
| Front and rear | 2.920 s | 2.942 s |
| Top end | 30.149 s | 29.728 s |
| Fasteners | 32.378 s | 27.985 s |
| Entire build | 70.872 s | 66.199 s |

The measured build improvement is **6.6%**; fastener fitting improves **13.6%**.
This is one before/after pair, not a stable speedup guarantee. Both runs used
`--profile`. The baseline used `--build-only`; the snapshot run continued through
exact standing and motion validation with `--no-export`. Only their build-stage
timings are compared above.

The baseline's build alone used 773 `frame` calls and 23 `list_elements` calls.
The revised run, including subsequent validation, used 230 and 5 respectively,
plus 8 `geometry_snapshot` and 8 `validate_snapshot` calls. These profiles have
different scopes; they are not a per-method wall-clock comparison. In the revised
run snapshots cost 1.459 s total, freshness checks 0.017 s, and the 46
`fit_fastener` calls still cost 26.368 s. Fitting geometry remains the dominant
cost in that stage.

The revised complete checked run took **282.307 s**, excluding GLB export:

- 78 parts, 250 occurrences; every part passed B-Rep validity, nonempty-body
  and broken-feature checks.
- Exact standing sweep: 806 pairs checked, 30,319 cleared by bounds, no clashes;
  14.141 s.
- Exact motion: 49 poses over 720°, 19,327 pair checks, clean; 198.914 s.
- Four strokes 83.600 mm, squish 3.20 mm, eight valve lifts 8.0 mm, firing order
  1-3-4-2.

Artifacts from this run are in `/tmp/om615-inspection-snapshots/`; the baseline
build and profile are in `/tmp/om615-inspection-baseline/`. These are local
working artifacts, not repository fixtures.

## Isolated inspection comparison

Three alternating-order repetitions on the saved engine, with fresh Body handles
for both paths. Loading is outside the timed region. No competing build or motion
study ran during this measurement. All circular-edge names and centers must agree
before a timing is accepted.

| Part | Circular edges | Live median | Snapshot median | Speedup |
|---|---:|---:|---:|---:|
| Block | 310 | 1.0663 s | 0.2584 s | 4.13× |
| Cylinder head | 549 | 2.5967 s | 1.0117 s | 2.57× |
| Cam cover | 80 | 0.2120 s | 0.0257 s | 8.26× |

The maximum measured center difference was **0.0 mm** for all three parts.
The old path describes faces and edges, then queries each circular edge's frame.
The new path describes edges and returns requested measurements in one call.
Neither path changes geometry. This benchmark requests **all circular edges**;
the generator's radius-filtered bolt queries are a different workload, measured
separately by the build comparison above.

Reproduce against an existing saved engine:

```sh
nix-shell shell.nix --run "python3 tools/benchmark_inspection.py \
  --project /tmp/om615-inspection-snapshots/om615.dsgn \
  --parts block 'cylinder head' 'cam cover' \
  --output /tmp/inspection-benchmark.json"
```

The script owns an isolated headless process and configuration/state directories.
It does not connect to the human's instance or save changes to the source project.

## Regression and iteration gates

The 1,143 headless gates passed across the full run and the targeted freshness
rerun. Static fixtures catch each seeded typing error; integration tests cover
client/raw parity, malformed values, bounded scans, immutable zero-RPC reads,
wrong-server contracts, edited/reopened/replaced documents and batch diagnostics.

Two additional iteration defects surfaced and were fixed: part-variable helpers
previously read or wrote the active tab instead of their owning part, and the
contract generator rewrote identical files, causing unnecessary C++ rebuilds.
No-op contract generation now preserves output timestamps.
