# A hole knows what it is for — plan

> Status: **planned, not built.** Found 2026-09-01 while surveying open parts
> libraries for F1 (capability-review-2026-08-31.md). It is the cheapest item
> that survey turned up and it is **independent of F1** — no fastener library
> has to exist for this to be worth doing.

## The gap

`Hole` takes a raw number:

    hole_kind: simple|counterbore|countersink, sketch_id, diameter (+_expr),
    depth (+_expr), through?, cb_diameter?, cb_depth?, cs_diameter?, cs_depth?

So "a clearance hole for an M6 screw, normal fit" is 6.6 — a number the person
modelling has to know, or look up, and then type. Every time. And once typed it
is 6.6, not "M6 normal", so nothing downstream can tell a clearance hole from a
6.6 mm hole that happens to be the same size:

- the drawing calls it Ø6.6 rather than the fit it is,
- a change from M6 to M8 is a manual edit of an unrelated-looking number,
- and nothing can check that the hole a bolt passes through is bigger than the
  bolt, because nothing knows a bolt was intended.

That last one is the same shape as the fastener gap itself — the model does not
record the intent, so the checks cannot see it.

## The data exists and is small

`bd_warehouse` (Apache-2.0) carries three tables, **298 rows** between them:

| file | rows | what |
|---|---|---|
| `clearance_hole_sizes.csv` | 91 | Close / Normal / Loose, per thread size |
| `tap_hole_sizes.csv` | 100 | the drill for a tapped hole, per size and pitch |
| `drill_sizes.csv` | 107 | number/letter/fractional drills to mm |

    Size,Close,Normal,Loose
    M6,6.4,6.6,7.0

That is the whole thing. No geometry, no new feature type, no library — just a
lookup that turns an intent into the number the kernel already accepts.

## The shape

A `Hole` gains an optional **fit** alongside its diameter, not instead of it:

    hole_kind: simple|counterbore|countersink,
    fit?: {thread: "M6", kind: close|normal|loose|tapped},
    diameter (+_expr), …

- **With `fit`, `diameter` is derived** — the same rule the rest of this
  codebase follows for a defaults-carrying number (docs/ui/u10-defaults-and-refusals.md:
  a default with a unit is derived from what it acts on, never a literal).
- **Without it, nothing changes.** Every existing document, RPC call and test
  keeps working; `fit` is additive and optional, and a document that never uses
  it serializes exactly as it does today.
- **An unknown thread is REFUSED, naming what is accepted** — not rounded to
  the nearest size and not interpolated. A vendored table is a fixed scope by
  definition, and a plausible substitute here is a hole that looks right and
  is not (the house rule, `tests/test_conventions.cpp`).

What it buys beyond saving a lookup: the drawing can call out "M6 clearance
(normal)" instead of Ø6.6; changing the thread re-derives the diameter; and a
future check can ask whether the fastener through a hole matches the fit it was
drilled for.

## Cost, and what to watch

Small: a CSV parser, a lookup, one optional field on `HoleFeature`, a format
bump with a golden, and the GUI's hole dialog gaining a size dropdown beside
the diameter field. The generator work that makes F1 expensive is entirely
absent here.

Two things to get right:

- **The tapped case is not the clearance case.** A tapped hole's drill depends
  on the PITCH as well as the nominal size (M6 is 1.0 coarse, 0.75 fine), so
  the key is `M6-1`, not `M6` — the same key bd_warehouse's fastener tables
  use. Getting this wrong produces a hole that taps badly rather than one that
  is obviously wrong.
- **Attribution travels with the data.** Apache-2.0 requires the NOTICE to be
  preserved; the vendored CSVs need a header saying where they came from, and
  `THIRD_PARTY.md` needs the entry.

## Why this is not part of F1

F1 is a parts library: generated fastener bodies, a placement UI, BOM
integration. This is three CSVs and a lookup on a feature that already ships.
They are related in subject and unrelated in cost, and tying them together
would delay the cheap one behind the expensive one.
