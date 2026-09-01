# A hole knows what it is for — plan

> Status: **planned, not built — and cheaper now than when this was written.**
> Found 2026-09-01 while surveying open parts libraries for F1
> (capability-review-2026-08-31.md). It was the cheapest item that survey
> turned up, and F1 shipping the same day paid for three quarters of it
> without meaning to: see "What F1 already built" below.
>
> It was written as **independent of F1** — no fastener library has to exist
> for this to be worth doing — and that is still true. What changed is the
> other direction: **F1 now has two refusals waiting on this**, listed at the
> bottom.

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

Small, and smaller since F1: the CSV parser and the clearance table exist
(see below), so what is left is one optional field on `HoleFeature`, the two
tables F1 had no use for, a format bump with a golden, and the GUI's hole
dialog gaining a size dropdown beside the diameter field. The generator work
that made F1 look expensive is entirely absent here.

Two things to get right:

- **The tapped case is not the clearance case.** A tapped hole's drill depends
  on the PITCH as well as the nominal size (M6 is 1.0 coarse, 0.75 fine), so
  the key is `M6-1`, not `M6` — the same key bd_warehouse's fastener tables
  use. Getting this wrong produces a hole that taps badly rather than one that
  is obviously wrong.
- **Attribution travels with the data.** Apache-2.0 requires the NOTICE to be
  preserved; the vendored CSVs need a header saying where they came from, and
  `THIRD_PARTY.md` needs the entry.

## What F1 already built (2026-09-01)

Keeping the two apart was right — the cheap one did not wait behind the
expensive one — but F1 needed the same table for its own reasons and left most
of this sitting there:

| this plan needs | state |
|---|---|
| a CSV parser | **done** — `core/parts/FastenerTable`, multi-table, refuses a malformed file naming the line |
| `clearance_hole_sizes.csv` vendored | **done** — metric rows, provenance header |
| the Apache NOTICE + `THIRD_PARTY.md` entry | **done** |
| the size -> diameter lookup | **done** — `parts::Dimensions("clearance_hole_sizes", "M6")` |
| the diameter -> size lookup | **done** — `parts::ThreadForClearance`, and it refuses a diameter matching nothing rather than snapping to the nearest |
| `tap_hole_sizes.csv`, `drill_sizes.csv` | not vendored — F1 had no use for them |
| an optional `fit` field on `HoleFeature` | not built |
| format bump + golden | not done |
| the hole dialog's size dropdown | not built |

So what is left is the part that was always the point: **the hole recording
what it is for**, rather than the lookup that feeds it.

## What F1 is waiting on from here

Two refusals in the shipped fastener code exist only because a hole does not
say what it is for:

- **A countersunk screw cannot be fitted to a picked rim.** `fit_fastener`
  refuses `iso10642` outright, because the rim you would pick on a plain bore
  is the bore's — the countersink is not modelled, so the head would end up
  inside solid material. A hole that carried its countersink would make this a
  normal case instead of a refusal.
- **A blind hole's thread is assumed, not known.** `fit_fastener` checks there
  is at least one diameter of depth to engage, but nothing can tell a *tapped*
  blind hole from a *clearance* one drilled to the same depth — and a machine
  screw has nothing to bite in the second. The diameters are not close (M6 taps
  at 5.0 against clearances of 6.4/6.6/7.0), so this is decidable the moment
  the hole says which it is.

And one thing that would get sharper rather than newly possible: `fit_fastener`
derives its size by reading the clearance table BACKWARDS from the bore
diameter. That works, and it is an inference — a hole that states `M6 normal`
outright makes it a fact.
