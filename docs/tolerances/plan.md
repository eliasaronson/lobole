# Tolerances and fits — plan

> Status: **T1 in progress**, 2026-09-10. Branch `wttol`.
> Written after a survey of what the app carries today (below). The short
> version: there is a *fit-class* system for clearance holes that drives real
> geometry, a limits check for bearing seats that reads numbers a person types
> in, and nothing else. No dimension, variable, drawing or check anywhere in
> the app carries a permitted variation.

## The gap

Every clash sweep, every `check_fasteners`, every interference volume in this
app runs at **nominal**. A model whose bore and shaft are both modelled Ø20.0
passes every check we have; whether the parts go together, and with how much
slop, is a question nothing can be asked.

That is the whole gap. It is not "we should display ±0.05 somewhere".

## What exists (survey, 2026-09-10)

| thing | where | verdict |
|---|---|---|
| clearance-hole fit class | `parts::Fit{Close,Normal,Loose,Tapped}`, `FastenerTable.h:182` | **real** — records intent, derives the diameter in `Execute`, GUI picker, backward lookup, refuses the unknown |
| bearing-seat limits | `catalog::SeatRequirements`, `Catalog.cpp:446` | **real but detached** — measurements are typed by hand into a `static std::map` (`CatalogPanel.cpp:157`), never read from the model, never persisted |
| `query_geometry` `radius_tol` | `RemoteInspection.cpp:1058` | a *search* band, transient, not design intent |
| variables | `Parameter{name,expression,comment,value,error}`, `Document.h:26` | **nothing** — no tolerance, no unit, no provenance |
| drawing dimensions | `DrawingDim`, `DrawingDocument.h:113` | **nothing** — and dims are driven by design ("the text is computed from the model, never authored") |
| threads | `threads::Standard{designation,majorDia,pitch}` | **nothing** — no 6g/6H |

Everything else the grep finds is kernel epsilon, mesh deflection, HLR chord
deviation or CAM grid resolution — a pure naming collision, and this document
means none of it.

## The one decision everything else follows from

**A size tolerance does not drive geometry.**

This is what separates it from the clearance-hole fit, and the reason it is a
new mechanism rather than another field on `HoleFeature`. `fit: {M6, normal}`
picks a *different number* — 6.6 instead of whatever you typed. An ISO 286
class does not: `Ø20 H7` is still nominally 20.000, plus a band of
+0.021/0. Nominal is untouched.

So a tolerance is metadata plus a checker, and
`docs/inspect/design-intent-plan.md:75` already demands exactly that: *"a
tolerance-only edit rebuilds zero nominal B-Reps."* It costs no regeneration
and cannot make a document look different. That is what makes it cheap.

Two consequences:

- **It attaches to a measured element, not to a variable.** A variable that
  drives four bores does not make those four bores share a tolerance
  (`design-intent-plan.md:66-68`). The nominal comes from *measuring the
  model*, which is the only honest source: whatever the kernel actually built
  is what gets made.
- **It needs no new pairing entity.** A fit is two toleranced faces that mate,
  and the assembly already says which faces mate — that is what a `Revolute` or
  `Cylindrical` `Mate` IS (`Occurrence.h:143`, `Connector::anchor.face`). The
  document already remembers the pairing; we just never asked it.

## Stages

### T1 — the table and the arithmetic (no persistence, no format bump)

`core/FitStandards.{h,cpp}`, mirroring `ThreadStandards` — pure static data,
unit tested, no I/O.

    tol::Limits(nominalMm, "H7")  -> {lower, upper} deviations in mm
    tol::Fit(nominalMm, "H7", "g6") -> {minClearance, maxClearance, kind}

Scope, and it is REFUSED outside it rather than extrapolated:

- **Sizes** 0 < D ≤ 500 mm, the 13 ISO 286-1 steps.
- **Grades** IT4–IT13. Fits live in IT4–IT11; the rest are there so a coarse
  general tolerance has somewhere to land.
- **Letters** shafts `d e f g h js k m n p`, holes `D E F G H JS K M N P`.
  Hole letters up to H mirror the shaft rule (`EI = -es`); K/M/N/P carry the
  standard's Δ correction, which is only defined for K/M/N ≤ IT8 and P ≤ IT7 —
  **outside that we refuse**, because the plausible-looking answer from
  dropping Δ is wrong by exactly one grade step.
- Everything else (`r`, `s`, `u`, sizes over 500) is refused **naming what is
  carried**, per the house rule.

The values are computed nowhere: they are the published IT and fundamental
deviation tables, and `tests/test_fits.cpp` pins them against textbook fits
(Ø20 H7/g6 = 7..41 µm clearance; Ø50 H7/p6 = 1..42 µm interference; Ø20 N7 =
−7/−28; Ø20 K7 = +6/−15).

**RPC `check_fit`**, in `RemoteInspection.cpp` — one method that does both
jobs, in `measure`'s own vocabulary:

    check_fit {hole: {class: "H7", a?: {feature_id, element}, part?, occurrence?},
               shaft: {class: "g6", a?: {...}},
               size?, intent?: clearance|transition|interference}

- `hole` alone → that class's limits at `size`.
- both → the fit: `min_clearance`, `max_clearance` (signed; negative is
  interference), `kind`, and `intent_ok` when an intent was declared.
- with element refs → it **measures the faces** and uses those diameters, so
  the check runs against what was actually built.

The refusal that matters: if the two measured diameters differ and no explicit
`size` was given, **refuse naming both**. An ISO fit is defined on one common
basic size; someone who modelled the clearance into the geometry AND asked for
H7/g6 has double-counted it, and silently picking one of the two numbers is
the substitution this codebase exists to refuse.

T1 already delivers the headline check. Nothing is stored yet.

### T2 — the document remembers (format bump + golden)

`Document` gains `std::vector<Tolerance>`:

    struct Tolerance {
      uint64_t id;
      ElementRef ref;      // the cylindrical face
      std::string cls;     // "H7"; empty = explicit deviations below
      double lower, upper; // mm, signed, from nominal
      std::string comment;
    };

Stored as the class, not the derived numbers — same rule as the hole fit, one
source of truth, and a table correction fixes every document.

- RPCs `set_tolerance` / `list_tolerances` / `clear_tolerance`.
- **`check_fits`** (plural, no args): walk the assembly's round mates, and for
  every one whose two anchors are faces that both carry a tolerance, report the
  fit. A mate anchored on a `Frame` rather than a face, or one where only one
  side is toleranced, reports **`unverified` with the reason** — never a pass.
  (`design-intent-plan.md:113`: "a truncated sweep is incomplete evidence,
  never a pass.")

  **The mate type is the intent, and only some mates have one.** A joint that
  MOVES says so outright — revolute turns, slider slides, cylindrical does
  both — so an interference fit contradicts it and fails, and a transition fit
  warns. A `Fastened` round joint is **reported and not judged**: "nothing
  moves here" is equally true of a shrunk-on gear and of a bracket bolted
  through a clearance hole, and the two want opposite fits, so a verdict either
  way would be invented. `Planar` and `Ball` are not round joints and are not
  fits.

  **It reaches into sub-assemblies**, by the same descent `AssemblySolve`'s
  `ConnectorFrame` makes and minus the transform — a fit is between two SIZES,
  and a size does not care where the unit was placed. Each side is named by its
  path (`Rig/Housing`), because two occurrences of the same bearing are told
  apart by where they sit, not by their part name.
- Format bump, golden at that version, `docs/versioning/format.md` row. A
  tolerance-only edit must rebuild **zero** features — that is the acceptance
  test, not a nicety.

### T3 — the GUI

A tolerance row where a cylindrical face is selected, and a list of the
document's tolerances. `check_fits` reachable as an action, reporting in the
same shape as `check_fasteners`.

### T4 — drawings

Two halves, in this order:

1. **The hole callout says the fit it already knows.** `DrawingCallout.cpp:95`
   prints `Ø4.5 THRU` for a hole whose feature records "M5 normal clearance".
   `docs/modeling/hole-standards.md:69` promised this and it was never built.
   Costs no format change.
2. **A dimension carries limits.** This is the first *authored* field on a
   deliberately *driven* object (`DrawingDocument.h:30`), so it is a decision,
   not a field: the value stays measured, the tolerance is authored beside it.

### T5 — a tolerance on a LENGTH, and the stack that follows

Everything above is diameters, and diameters are the easy half. A part with
three perfectly toleranced bores and forty untoleranced lengths passes
`check_fits` completely and is still not a drawing anybody can make from.

Two steps, and the first is worth having on its own:

**T5a — a tolerance on a measured distance.** `Tolerance` gains a second
`ElementRef`: with one ref it means "this face's size", with two it means "the
distance between these". The nominal is measured the same way — `Measure`
already answers a distance between two elements, which is the whole reason
this is small. It gives a shoulder-to-seat length, a slot width, a boss height
a number nothing else can state.

An ISO class does not apply to a length between two arbitrary faces, so a
two-ref tolerance takes an explicit `lower`/`upper` pair. That asymmetry is
real and should be visible in the API rather than papered over with a class
that silently means something different.

**T5b — worst case along a chain.** With lengths toleranced, the question
that actually sinks assemblies becomes askable: a shaft carries a shoulder, a
spacer, a bearing and a circlip, and every one of them has a band — does the
last one still land in its groove? The arithmetic is the one
`design-intent-plan.md:237-244` specifies, and its guardrails matter more than
the sum:

- Bounds only. Worst case is `sum(lower)` and `sum(upper)`, and that is a
  statement about the extremes, not a distribution. Statistical stackup needs
  correlations we do not have and stays out.
- **Shared inputs are not independent.** Two lengths driven by one variable
  cancel rather than add, and treating them as independent invents variation
  that cannot occur. A chain whose terms are not independent is REFUSED, not
  approximated — the same rule as everything else here.
- The chain is the mates', not a list somebody retypes: the assembly already
  says what stacks against what.

## What to watch

- **`wtunits` is live.** Another worktree is working on units as this is
  written. Limits without units are the `end_angle: 90`-means-radians trap in a
  new place. T1 is mm-only and says so at every boundary; if `wtunits` lands a
  quantity kind, T2 should ride on it rather than inventing a second one.
- **Do not widen a boolean epsilon.** `design-intent-plan.md:41-44`. A
  manufacturing tolerance and a kernel tolerance are different concepts that
  share a word; nothing in T1–T4 may touch `Precision::Confusion`.
- **Worst case is not a distribution.** T1 reports the bounds of independent
  limits and nothing else. Statistical stackup needs correlations we do not
  have (`design-intent-plan.md:245-252`) and is explicitly out of scope.
- **The socket fudge.** `docs/parts/plan.md:323` models fastener socket sizes
  at the middle of the tolerance band as a hardcoded nudge. Once T2 exists that
  is expressible; until then, leave it alone and do not pretend it is a fit.
