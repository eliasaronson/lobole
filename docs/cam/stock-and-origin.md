# Stock, and where zero is (W1–W6)

Two questions that turn out to be one feature. Asked while machining an
imported speaker baffle: *"In Fusion the workpiece is configured — what does
that give, and should we add it? How would I zero the reference for something
I generate?"*

## What we have

```cpp
struct Stock {                      // CamJob.h:145 — this is all of it
  glm::dvec3 margin{2.0, 2.0, 1.0}; // added around the MODEL's bbox
  double bottomMargin = 1.0;
};
glm::dvec3 origin{0.0, 0.0, 0.0};   // …and no emitted coordinate reads it
```

**The stock is always a shrink-wrap.** One mode — "the model's bounding box
plus margins" — which is the right default and follows the defaults rule
(nothing is a literal; everything is derived from the part). What cannot be
said is *"this baffle comes out of a 2440 × 1220 × 18 sheet"*. So a job is
always framed on one part, and nesting, sheet goods and off-cuts are
unexpressible.

> **Half of that outlived W3.** Fixed stock (W3, below) said the size, but
> `stock.place` positioned the sheet AROUND the part, so moving a part
> dragged the sheet with it — a shrink-wrap wearing a size. A job holding
> more than one part is what finally made a sheet a real object: once
> anything is PLACED, fixed stock is anchored at the setup origin and the
> parts are arranged on it ([job-document.md](job-document.md) J4).

**The work origin is a label, not a number.** `setup.origin` takes its Z from
the picked top face and is **never subtracted from an emitted coordinate** —
grep the engine. X and Y are hard `0.0` and nothing can set them. The file is
in setup-frame coordinates about the *model* origin, and the operator has to
set G54 so that setup Z = `origin.z` lands on the material top
(`docs/cam/dogfood-round.md`). For a part you modelled around the origin that
is a workable convention. For a part you *generated* — or imported, where the
origin is wherever the source document put it — there is no answer to "which
corner do I touch off on", which is the question that was actually asked.

Both are the same shape of defect the fixture frame just fixed: a CAD file's
coordinate system standing in for a machining decision.

## The fix

Two things, in this order, because the second is worth much less without the
first.

1. **The origin becomes a number the file respects.** Pick it, and the post
   subtracts it. `origin` then means what the panel has always claimed.
2. **`Stock::mode`** — relative (today), fixed size, or from a solid — so the
   workpiece can be a sheet rather than a shrink-wrap.

### Where the offset is applied — one place

The engine keeps working in setup space. **The post subtracts the origin at
the `Emitter`**, which every coordinate already funnels through
(`CamPost.cpp:40-115`), and nowhere else. Toolpaths, ghosts, the backplot,
stock bounds and derived depths are untouched.

That leaves exactly one consumer to keep honest: the verifier and the
simulator both read the **posted file** (`Inspect`, `ParseGcode`), so they
must add the origin back on parse to return to setup space. One subtraction,
one addition, both named — and `cam sim: playing to the end leaves the
verdict's own map` already fails loudly if the two ever disagree.

### The decision this forces, stated plainly

Making the post subtract `origin` **changes the g-code of every existing job
whose Z zero was picked** — which is all of them, since picking a top face is
the first step the panel asks for. That is not a regression; it is the
correction. Today a file says `Z20.6` for a cut at the stock top and expects
the operator to have zeroed 20.6 mm lower; afterwards it says `Z0.0` and
expects the operator to have zeroed *on the face they clicked*, which is what
"Pick top face — that sets Z zero for the job" has always promised.

No migration (no users), but it must be **loud**: the panel states where zero
is in the file's own terms, `dogfood-round.md`'s pre-flight list is rewritten,
and a golden pins the new numbers.

## Phases

### W1 — the origin is real (~1 session)

`Emitter` subtracts `setup.origin` from every X/Y/Z it writes, including
retracts and arc centres (I/J are incremental, so they are already immune —
assert it rather than assume it). `ParseGcode`'s consumers add it back.

*Gates:* a job posted with `origin = (0,0,0)` is byte-identical to today's, so
the change is provably confined. With a non-zero origin, the picked corner
reads `X0 Y0 Z0` in the file. And the verdict is unchanged either way — same
cut, different words for it.

### W2 — picking it (~1 session)

The missing X and Y, derived rather than typed (u10):

- **a stock-box corner** — the default, and what most operators touch off on;
- **a model-box corner** — for a part cut from oversize material;
- **a picked vertex** — for a datum that is neither.

The Z stays what "Pick top face" already sets. Scripted twin on
`set_cam_setup {origin_at: "stock_min"|"model_min"|…, origin_vertex: {…}}`,
so clicking and scripting agree.

*Gate:* every mode puts `X0 Y0` where the panel says it is; re-picking the
same corner is idempotent, like `lay_face_up`.

### W3 — stock modes (~1–1½ sessions)

`Stock::mode`: `Relative` (today, unchanged and still the default),
`Fixed` (typed dimensions + where the part sits inside them), `FromSolid`
(another body in the document IS the stock — the off-cut case).

`StockBounds` is the one function that answers "what is the workpiece", and
every consumer — safe Z, derived depths, the verifier, the simulator, the
plan view — already goes through it. So this is one function deep, the same
way the fixture frame was.

*Gate:* a fixed 2440 × 1220 sheet gives a stock box of that size whatever the
part is; a job on it verifies; and `Relative` mode re-derives exactly today's
box, so nothing that exists changes.

### W4 — serialization (~small)

`origin` already round-trips. New: `origin_at`, `origin_vertex`, `stock.mode`
and its dimensions — each written only at non-default, so a job that uses none
of them re-saves byte-identically. **kVersion 67 → 68**, a golden at v68, and
a row in `docs/versioning/format.md`.

### W5 — the gates that are not per-phase (~½ session)

- **The simulator and the verifier still agree** with a non-zero origin. They
  consume the posted file, so this is the test that catches an offset applied
  once instead of twice.
- **GUI:** pick a stock corner, post, and read `X0 Y0 Z0` at that corner.
- The fixed-stock case on a real part: the baffle out of a sheet.

### W6 — the docs this makes wrong (~small)

`dogfood-round.md:317` is the operator's pre-flight list and it will be
actively dangerous once W1 lands — it tells them to offset by the model
height. `gui.md`'s Zero section, and `plan.md`'s stock line.

## Not this

**Nesting and multi-part sheets.** W3 makes a sheet expressible; laying
several parts out on one is a separate workstream that wants its own
representation (which parts, where, in what order) and is not a stock mode.

**Work-offset registers (G54…G59).** The file stays single-offset. Choosing
*which* register is a machine convention, and the post can gain a setting for
it when a second setup ever needs a second register.

## As built

All six phases shipped, 2026-08-30, format **v68**.

**W1 landed as designed, and the confinement gate is the useful one.** A setup
whose origin is (0,0,0) posts byte-identically to before, so the change is
provably limited to jobs that have a work origin. `Emitter` subtracts;
`ProgramInSetupSpace` adds back; those are the only two places, and
`cam sim: a job zeroed on its corner still plays its own verdict` fails if
either side is missed — it also asserts that parsing the file RAW disagrees
with the verdict, which is the shape the mistake would actually take.

**Arc centres really are immune**, as predicted: I/J are incremental, so the
offset cancels. The test compares the I/J words of a job posted at zero and
the same job posted from a corner and requires them identical, rather than
trusting the reasoning.

**A tolerance the plan did not anticipate.** An 18 mm part in an 18 mm board
measured as *not fitting*, every time: OCC's bounding box carries a gap (the
same one the CAM round recorded as "mesh bbox grew 4 um"). `kStockFit = 1e-3`
is the allowance, with the same reasoning the verifier's `kGrazeTolerance`
uses — a micron of shortfall is the gap, a millimetre is a part that does not
come out of this material.

**`origin` is now derived, and the file saves the RULE.** `origin_at` — the
corner — is what round-trips; the coordinate is written for a reader and
recomputed on load, so it stays right when the part changes under it. An
unknown corner name is refused rather than read as the default, since every
coordinate in the file is measured from it.

**The default moved, deliberately.** A fresh setup zeroes on the stock's min
corner rather than at (0,0,0). The old default was a hard-coded literal
sitting exactly where the defaults rule wants a derivation, and it put zero at
the *model* origin — which for a generated or imported part is wherever the
source document left it, the question that started this.

### Not built

**A picked vertex as the origin**, which the plan listed. `ElementMap` has no
vertex kind, so it needs element-map plumbing this change does not do. The
three box corners cover "which corner do I touch off on" for box stock, which
is every case the stock modes can express.

**`FromSolid` stock.** A body in the document as the workpiece reads simply
until you notice the verifier samples the MODEL to compare against, so the
stock body would be machined *and* expected. It needs a way to mark a body as
not-part, which is a bigger idea than a stock mode.
