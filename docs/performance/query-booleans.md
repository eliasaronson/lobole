# A measurement was editing the model, and it corrupted the heap

Found 2026-09-10 chasing `tests/cncest6090_smoke.py`, which failed about three
runs in ten — in isolation, not only under load.

## What it looked like

Three different faces, which is why it took two sessions to name:

1. Usually, `assert result["clean"]` failing because one sample of nine came
   back `UNKNOWN … mount_bolt_0 x … clamp_nut_0: exact contact calculation
   failed`.
2. Sometimes, a hard crash — `malloc(): unaligned tcache chunk detected`,
   SIGABRT, with the top of the stack inside `OSD_ThreadPool::Launcher` in an
   OpenCASCADE boolean that had nothing to do with the previous frame.
3. Occasionally, nothing at all.

Face (1) has an obvious and wrong reading, which the first look took: an exact
boolean between a screw and its nut is interpenetrating helical thread
surfaces, OCCT is fragile there, `pairs_unknown` correctly makes `clean`
false, so the check is right and the ASSERTION is too strong. Every clause of
that is true and the conclusion is wrong. It is one symptom of a memory bug,
and the crash is another.

## The mechanism

`DESIGNER_SWEEP_THREADS=1` passed 15 of 15 where the default failed, so:
concurrency. ThreadSanitizer (`tools/ci-local.sh --tsan`, or a `build-tsan`
tree by hand) then named it in a single run, seven races across three runs:

```
WARNING: ThreadSanitizer: data race
  Write ... BRep_Builder::UpdateEdge(TopoDS_Edge const&, Handle(Geom2d_Curve) const&,
                                     Handle(Geom_Surface) const&, TopLoc_Location const&, double)
  Previous write ... BRep_CurveOnSurface::~BRep_CurveOnSurface()
```

**An OpenCASCADE boolean modifies its arguments.** Computing an intersection
means computing p-curves, and by default OCC stores them on the input edges
(`BRep_Builder::UpdateEdge`) rather than on a copy. For a modelling feature
that is the right default — a boolean consumes its operands. For a
measurement it is a write to the document.

A clash sweep is O(n²) pairs over n bodies, so a single body appears in many
pairs, and the sweeps run those pairs on worker threads. Two threads asking
"does A touch B?" and "does A touch C?" both write to A's edges. The list
being mutated is `NCollection_BaseList`, so one thread's `Standard::Allocate`
raced another's `free` — heap corruption, detected later at an unrelated
`malloc`, several frames from anything of ours.

That also explains the UNKNOWN pairs. A corrupted edge representation makes
the *next* boolean over that shape fail, and `ContactBetween` returning
nullopt is reported honestly as "could not check". The fastener pair was not
special; it was just usually the one that got there second.

## Why the existing rule did not cover it

`core/Parallel.h` and both sweeps already carry a rule for OCC and threads:

> OpenCASCADE fills caches lazily ON the shape … Every caller here warms those
> caches single-threaded first.

That rule is real and it is not this. It addresses **idempotent lazy fills** —
a bounding box, a triangulation, something derivable that any thread would
compute identically. What a boolean writes is **specific to the pair being
intersected**, so there is nothing to pre-compute: no amount of warming makes
`A ∩ B` and `A ∩ C` agree about what belongs on A's edges.

Nor was it visible to the gate built for this: `tests/sweep_parallel_smoke.py`
diffs a one-thread report against an all-thread one byte for byte, on the
premise that "a race in a boolean corrupts an ANSWER rather than crashing".
This one usually corrupts neither — it corrupts the heap, and the answer is
most often still right.

## The fix

`SetNonDestructive(Standard_True)` — OpenCASCADE's own option, documented as
"Safe processing option allows avoiding modification of the input shapes": it
copies a sub-shape rather than updating one in place.

Every boolean in `core/Measure.cpp` now goes through one `BuildQuery` helper
that sets it, because Measure.cpp is the module that answers questions and a
question must not edit its subject — the threading argument is the second
reason, not the first. `BuildQuery` also owns the argument/tool plumbing,
which folds in an older trap: the two-shape constructor `BRepAlgoAPI_Cut
cut(a, b)` **builds immediately**, so anything set afterwards is set too late
and silently ignored. That had already cost one attempt at the contact fuzz.

`tests/test_conventions.cpp` fails if a boolean in that file is built any
other way. It checks the shallow thing — that no `BRepAlgoAPI_X op(a, b)`
form exists — rather than trying to see whether the flag was set, because the
constructor form is exactly the one that cannot be corrected afterwards, and
it is the short one people reach for.

| | before | after |
|---|---|---|
| `cncest6090_smoke`, 25 consecutive runs | ~3 in 10 failed | 25 / 25 |
| TSan races, 3 runs of that smoke | 7 | 0 |
| gripper exact motion sweep, 1131 pairs | 15.2–15.5 s | 15.1–15.9 s |

No measurable cost, on this workload. The copying non-destructive mode does
is proportional to the sub-shapes an operation actually needs to update, which
for a clash query that mostly answers "no" is close to nothing.

## Not done

`core/ElementHistory.cpp` builds booleans too, and feature execution
(`core/features/*.cpp`) deliberately does not use this — a modelling boolean
consumes its operands and should stay destructive. Neither runs on a worker
thread today. The convention is scoped to Measure.cpp for that reason; if a
sweep ever calls something else, the same rule has to travel with it.

`tests/sweep_parallel_smoke.py` still cannot see this bug class. The thing
that can is TSan, which is not scheduled anywhere — it is an occasional audit
(`tools/ci-local.sh --tsan`). This round is the argument for running it when a
test starts failing in a way nobody can reproduce on purpose.
