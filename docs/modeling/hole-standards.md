# A hole knows what it is for — plan

> Status: **BUILT 2026-09-01** (format v71).
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

> The drawing half of that shipped later than the rest, with
> [the tolerances plan](../tolerances/plan.md) T4a: `FormatHoleCallout` now
> appends the intent to a clearance hole (`Ø3.4 THRU, M3 NORMAL CLEARANCE`)
> and leads a TAPPED one with its thread instead of the tap drill. It was
> promised here when the fit landed and then sat unbuilt while every drawing
> went on calling a hole by its diameter — which the drawing smoke test
> covered up, asserting the callout ended in "THRU" and calling that "reads
> the fit".

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

## What building it corrected

- **Two tables, not three.** 53 of `tap_hole_sizes.csv`'s 100 rows are
  imperial, and their drill columns are fractional and number designations
  ("3/64", "#55") rather than millimetres — the same shape that made the
  clearance table metric-only. Vendoring the metric rows makes
  `drill_sizes.csv` unnecessary: decoding those designations is its entire
  job. The plan budgeted for 298 rows across three files and needed 46 across
  one.
- **The pitch is derived, not asked for.** A tapped hole's key is `M6-1`, and
  the plan noted the trap. What it did not say is where the coarse pitch comes
  from: the vendored standards already carry `P` for every size, so
  `CoarsePitch` reads ISO 4762 rather than tabulating the same numbers
  somewhere they could drift. "M6" on its own means the coarse pitch
  everywhere in engineering, and now it means it here too.
- **The derivation belongs in `Execute`, not at the call site.** A diameter
  computed where the hole is created is a number typed once from a table. In
  `Execute` it survives undo, a reload, a configuration override and an
  expression — everything that rebuilds a feature.
- **The GUI's diameter row goes READ-ONLY under a fit**, rather than staying
  an editable number the next regeneration would silently overwrite. The size
  picker is built from the vendored table's own list, so it cannot offer a
  size the lookup would then refuse.

## What F1 had already built

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
| `tap_hole_sizes.csv` | **done** — metric rows; `drill_sizes.csv` turned out unnecessary (see above) |
| an optional `fit` field on `HoleFeature` | **done** — derived in `Execute`, so it survives undo, reload and configuration overrides |
| format bump + golden | **done** — v71, with the golden's volume computed analytically |
| the hole dialog's size dropdown | **done** — plus a Fit picker, and the diameter row goes read-only under one |

So the part that was always the point — **the hole recording what it is
for** — is what shipped; the lookup that feeds it was already there.

## What it unlocked in F1

`fit_fastener` now ASKS THE HOLE FIRST and falls back to reading the diameter
backwards only when it has nothing to ask. That matters more than it sounds:
the dock round measured the backwards lookup and found 48% of whole
millimetres from 1 to 160 are some size's clearance, so it is an inference
that rarely refuses. A recorded fit is a fact.

**The blind-hole refusal the plan promised now works**, and it could not
before:

    the blind hole in Bracket is a normal CLEARANCE hole for M6, not a tapped
    one — a machine screw has nothing to bite in it. Tap it, or put a nut on
    the far side.

A blind clearance hole and a blind tapped hole are identical to a traversal —
same axis, same depth — so nothing but the hole could tell them apart.

**And the countersunk refusal went too**, later the same day
([../parts/f1-loose-ends.md](../parts/f1-loose-ends.md)). `fit_fastener`
refused `iso10642` outright because "the rim you pick is a plain bore's"; with
a recorded thread, and `FindHoleForRef` already matching a picked circle
against the COUNTERSINK radius as well as the bore, all three questions it was
standing in for became answerable — is the hole countersunk, is this the
countersink's rim or the bore's, and is the countersink deep enough for the
head.
