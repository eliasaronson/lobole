# Fitting a fastener ray-cast every body in the assembly

Measured 2026-09-15 while profiling the OM615 generator, whose build is 75-100
seconds for 302 occurrences from 104 parts.

## The finding

The generator's own RPC profile (`--profile`, `rpc-timings.json`) put
**`fit_fastener` at 46 calls and 34.4 s — 747 ms each, a quarter of the run.**
A sampling profile of the same build put `ui::FitFastenerToHole` at **21% of
the process**, and inside it `parts::TraverseAxis` at 22.9%, whose own biggest
callee was `BRepClass3d_SolidExplorer::InitShape` at 11.5%.

Reading the code said why:

- `TraverseAxis` is handed **every body of every visible leaf** — the caller
  gathers them all, because the rim is on one of them and the stack is made of
  the rest. On the engine that is 300-odd bodies.
- For each, it casts **four rays** (the bundle offset around the bore) and
  constructs a `BRepClass3d_SolidClassifier` **per ray**, each of which
  explores the whole solid.

A bolt's axis passes through three or four bodies. The other 296 were being
ray-cast and classified to discover that.

## What changed

- **A slab test first**, against the body's own memoized bounds, with the ray
  already in the body's local frame — the same prune, for the same reason, as
  the hover pick (docs/performance/hover-pick.md).
- **One classifier per body, not per ray.** The four rays classify against the
  same shape; constructing it four times was three constructions wasted on
  every body the bundle reached.

## The measurement

One binary, an environment variable choosing the arm, prune first so a warm
cache would favour the other:

| | engine build | `fit_fastener` |
|---|---|---|
| **with the prune** | **74.9 s** | **16.96 s** (369 ms/call) |
| without | 82.1 s | 24.28 s (528 ms/call) |

**−9% on the build, −30% on the call.** The profiled share moved with it:
`TraverseAxis` 22.9% → 11.2%.

## What is left in fit_fastener — measured, not guessed

The first answer here said the remainder was the placement and the
regeneration behind it. **It is not.** A phase timer inside the function,
over the engine's 46 fits:

| phase | share | per fit |
|---|---|---|
| flatten the assembly | 0.0% | 0.1 ms |
| **derive** (rim search + axis traversal) | **93.8%** | 433.7 ms |
| place the hardware | 4.1% | 19.1 ms |
| mate the joint | 2.0% | 9.3 ms |

Placing three occurrences and mating every interface they span costs 28 ms
together. Everything else is finding out what the bolt goes through.

So the rim search got the same treatment as the traversal. It described the
geometry of **every leaf in the assembly** — 302 of them — to find which
carried the picked rim, when an `ElementRef` names a feature and a part
without that feature cannot carry it. That is `Measure::Describe`'s own first
test, done for free by comparing ids:

| | derive per fit | all 46 fits |
|---|---|---|
| before | 433.7 ms | 21.26 s |
| after | **370.7 ms** | **18.15 s** |

What remains is the ray casting itself — plus, since 2026-09-18, one pass
over the placed bodies for their THREAD spans (`parts::ThreadEndAlong`, so a
screw into a tapped boss ends where the tapping does), whose inner loop is
empty on a body carrying no threads — against the bodies the axis really
does cross — and on this engine those are the block and the head, where an
OCC ray-shape intersection is not cheap. Going further would mean a mesh
first pass (`collide::Mesh` has the BVH for it), and that trades exactness
for speed in the one number a fastener must get right: its LENGTH. Not worth
it without a reason.

The phase line survives at DEBUG, so the next "why is fitting slow" is read
rather than re-profiled — the same reason `SyncAssemblyRenderers` logs its
phases.

`tests/test_parts.cpp` pins the part that matters: a plate reads identically
with thirty unreachable bodies added, and a body squarely ON the axis past it
is still found — a prune that lost that would silently shorten every bolt.
