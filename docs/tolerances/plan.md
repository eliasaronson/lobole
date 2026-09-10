# Tolerances and fits — plan

> Status: **T1-T5b BUILT**, 2026-09-10. Branch `wttol`, format v98+v99.
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
| `query_geometry` `radius_tol` | `RemoteInspection.cpp` (`query_geometry`) | a *search* band, transient, not design intent |
| variables | `Parameter{name,expression,comment,value,error}`, `Document.h` (`struct Parameter`) | **nothing** — no tolerance, no unit, no provenance |
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

### T1 — the table and the arithmetic. BUILT (no persistence, no format bump)

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

    check_fit {hole: {class: "H7", feature_id?, element?, part?},
               shaft: {class: "g6", feature_id?, element?, part?},
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

### T2 — the document remembers. BUILT (v98, golden)

`Document` gains `std::vector<Tolerance>`:

    struct Tolerance {
      uint64_t id;
      ElementRef ref;        // the face
      ElementRef ref2;       // set (v99) = a DISTANCE, not a size
      std::string isoClass;  // "H7"; empty = explicit deviations below
      double lower, upper;   // mm, signed, from nominal
      std::string comment;
    };

Stored as the class, not the derived numbers — same rule as the hole fit, one
source of truth, and a table correction fixes every document.

- RPCs `set_tolerance` / `list_tolerances` / `clear_tolerance`.
- **`check_fits`** (plural, no args): walk the assembly's round mates, and for
  every one whose two anchors are faces that both carry a tolerance, report the
  fit. A mate anchored on a `Frame` rather than a face, or one where only one
  side is toleranced, reports **`unverified` with the reason** — never a pass.
  (`design-intent-plan.md:110`: "a truncated sweep is incomplete evidence,
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

### T3 — the GUI. BUILT.

**The editor lives in the Measure tool window**, not a panel of its own, and
that is the point rather than a saving: measuring a face and saying what it
may vary by are the same gesture on the same pick. One picked element is that
face's SIZE, two are the DISTANCE between them — which is exactly a
`Tolerance`'s `ref` and `ref2`, so the tool's existing pick list needed no
parallel notion of a "tolerance selection".

- The class is assembled from a letter picker and a grade picker built from
  `fits::HoleLetters()`/`ShaftLetters()`, so what is offered IS what
  `fits::Limits` accepts — except the letter/grade pairs the standard leaves
  undefined, which refuse in the window rather than being hidden.
- **Uppercase is a bore, lowercase a shaft, and it is stated rather than
  inferred.** Nothing in the geometry says which a cylinder is — the
  concavity that would tell us is not something `Measure` reports — and
  guessing would put an H7 on a shaft.
- The letter starts EMPTY. There is no honest default class for a face nobody
  has called a bore or a shaft, so the editor refuses by name until one is
  chosen: U10's `RefusesUntilPicked`, applied to something that is not a
  feature. A distance's deviations are likewise not seeded — there is no
  general-tolerance table vendored to derive one from, and a band nobody chose
  is the plausible-and-wrong default this project refuses.
- The band the pickers imply is previewed live at THIS size, so a class that
  cannot apply says so before it is committed.
- Every refusal the RPC makes is made here, in the same words. A GUI that
  accepts what the script API refuses is the substitution this workstream
  removed.
- The distance line names **which** distance ("closest approach"): the Measure
  tool can be showing the furthest separation or centre-to-centre while a
  tolerance is always on the minimum, and a number that silently disagrees
  with the readout two lines above it is worse than a longer label.

**The list is a section of the Variables panel**, its neighbours there being
the other authored numbers that sit outside the regen chain. It is wrapped
ROWS, not a table: the panel is 218 px wide by design and ImGui cells clip
rather than elide, so a limits column reading "19.98 mm" when the value is
"19.98 .. 19.993 mm" looks like one number — the same defect that once made
385169 g read as 385. Limits are recomputed against current geometry rather
than stored, and a ref that no longer resolves reads `unresolved` instead of
vanishing.

**`check_fits` is a button beside "Check fasteners"** in the Occurrence panel,
for the same reasons that one is there: it is a question about what the
assembly is MADE of, and it walks the whole thing, so it is asked for rather
than run every frame. The walk itself moved to `ui::CheckFits` so a person and
a script get the same verdict from the same code — the arrangement
`CheckFasteners` already uses — and the RPC is now only its wire form. The
summary counts each kind apart; an aggregate that hid unknowns behind a green
badge is the one thing this check exists not to do.

### T4 — drawings

Two halves, in this order:

1. **The hole callout says the fit it already knows. BUILT.**
   `FormatHoleCallout` printed `Ø4.5 THRU` for a hole whose feature recorded
   "M5 normal clearance"; `docs/modeling/hole-standards.md:69` promised
   otherwise when the fit landed and this was the half that never got built.
   A clearance hole now appends its intent (`Ø3.4 THRU, M3 NORMAL CLEARANCE`)
   and a TAPPED one leads with the thread rather than the tap drill
   (`M3 THRU`) — printing Ø2.5 for an M3 tapped hole is not unhelpful, it is
   the wrong instruction. The fit is named even when it is the normal one,
   because "M3 clearance" conventionally IMPLIES normal and an implication is
   what recording the fit did away with. A hole with no fit formats exactly as
   before, so an old drawing reads as it always did. No format change.
2. **A dimension carries limits. BUILT — and it needed no authored field.**
   The decision the plan flagged (the first *authored* field on a deliberately
   *driven* object) turned out not to arise: the model already records the
   band, so a DIAMETER dim reads it the same way a hole callout reads its fit.
   `Ø20 H7` for a class, `Ø20 +0.05/-0.02` for an explicit band. No format
   change, nothing to keep in sync, and a tolerance edit shows on the sheet at
   the next update because nothing was copied.

   Two things it refuses. A RADIUS dim takes no class: an ISO class states how
   a DIAMETER varies, so "R10 H7" would be a band twice the one written. And a
   circle matching two tolerances is labelled with neither — one bore wearing
   another bore's band is worse than an unlabelled one.

   The provenance match is name-to-name, not against `ElementRef::featureId`:
   that field is the id of the feature whose BODY owns the name (the box a
   hole was cut into), while the name encodes the features that MADE the face.
   Matching on the former finds nothing on any bore that was cut rather than
   created — which is every interesting one, and it is what the first version
   did.

### T5 — a tolerance on a LENGTH, and the stack that follows

Everything above is diameters, and diameters are the easy half. A part with
three perfectly toleranced bores and forty untoleranced lengths passes
`check_fits` completely and is still not a drawing anybody can make from.

Two steps, and the first is worth having on its own:

**T5a — a tolerance on a measured distance. BUILT (v99).** `Tolerance` gained a
second `ElementRef`: with one ref it means "this face's size", with two it
means "the distance between these". The nominal is measured the same way —
`Measure::DistanceDetailed` already answers a distance between two elements,
which is the whole reason this was small. It gives a shoulder-to-seat length,
a slot width, a boss height a number nothing else could state.

An ISO class does not apply to a length between two arbitrary faces, so a
two-ref tolerance takes an explicit `lower`/`upper` pair and REFUSES a class.
That asymmetry is real, and it is visible in the API rather than papered over
with a class that would silently mean something different.

The uniqueness key widened with it: one tolerance per **subject**, which is a
face for a size and an ordered PAIR of faces for a distance. So a bore carries
its own size and any number of distances measured from it, and the size lookup
`check_fits` runs never hands back a statement about a gap when it asked how
big a bore is.

**T5b — a summed band over a stated set. NOT the inferred chain this plan
first described.**

The first draft of this section said the chain could come off the mates, the
way `check_fits`' pairing does, and that a chain whose terms were not
independent would be refused. Neither survives inspection, and the second one
is the load-bearing half:

- **The mates do not give the chain.** T2's pairing worked because a revolute
  mate *literally names both faces of the fit*. A stack alternates — a contact,
  then a length INSIDE a part to its far face, then another contact. The
  contacts come free; the internal contributions are T5a distances, and
  nothing says which of a part's distances lies along this axis. Deriving it
  works only when someone happened to tolerance exactly the pair of faces the
  mates land on.
- **The independence refusal is not implementable as written.** It would need
  to know that two lengths are driven by one variable. But a tolerance's
  nominal is MEASURED, and there is no path from a measured distance back to
  the expression that produced it — `Parameter` carries no provenance to build
  one from. Refusing correlated chains needs geometry-to-variable dependency
  tracking, which is its own workstream.

That matters because the independence handling is the ONLY thing a stack-up
check offers over arithmetic. Once T5a records the bands, `sum(lower)` and
`sum(upper)` is two minutes in a spreadsheet. Shipping the easy half and
calling it a stack-up check would produce confident answers that are wrong in
exactly the case people build stack-up tools for.

So T5b is the small honest version, and that is what shipped: `stack_up`
takes an **explicitly stated, ordered set** of tolerances — each added or
subtracted, because a gap is `A - B - C` and not a sum of everything — and
reports the summed nominal with the worst case the bands allow. The person
chose the chain, the tool did the addition, and the answer says outright that
its terms are assumed independent.

The signs are the whole of the arithmetic: a term that SUBTRACTS contributes
its UPPER deviation to the minimum. Getting that backwards produces a stack
wrong by twice the band and entirely reasonable-looking, so it is pinned by
tests rather than trusted. Fewer than two terms is refused — one tolerance is
a tolerance, and summing it would dress a lookup up as an analysis — and so is
a term whose geometry no longer resolves, rather than counting it as zero and
silently shortening the chain.

The inferred chain waits on dependency tracking from geometry back to
variables. It is not queued behind T5b; it is a different problem.

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
  have (`design-intent-plan.md:206-222`) and is explicitly out of scope.
- **The socket fudge.** `docs/parts/plan.md:323` models fastener socket sizes
  at the middle of the tolerance band as a hardcoded nudge. Once T2 exists that
  is expressible; until then, leave it alone and do not pretend it is a fit.
