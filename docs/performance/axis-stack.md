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

## What is left in fit_fastener

369 ms a call, and the profile attributes only ~200 of it to
`FitFastenerToHole` itself. The rest is the placement the fit performs —
adding the fastener, mating it to every interface it spans, and the
regeneration that follows — which runs partly on worker threads and so does
not attribute to the calling frame. Forty-six fasteners added one at a time
is forty-six regenerations; `batch` is the lever there, and it belongs to the
generator rather than to this function.

`tests/test_parts.cpp` pins the part that matters: a plate reads identically
with thirty unreachable bodies added, and a body squarely ON the axis past it
is still found — a prune that lost that would silently shorten every bolt.
