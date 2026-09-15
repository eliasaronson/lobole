# A slow view names what made it slow

## The problem

An exact view of the 35-occurrence speaker assembly takes 18 s, and 16 of
those are **one imported loudspeaker driver**. Hiding it — `add_drawing_view`
already takes `hidden` — brings the view to 2.16 s, and hiding both drivers
to 0.46 s.

Nothing in the app says so. The caller sees a view that took 18 s and has no
way to learn which of 35 names to put in `hidden`, so the choice is between
waiting and bisecting by hand, which is what it took to find this
(docs/drawing-incremental-hlr-plan.md, "Cold generation, measured").

It is not guessable from the model either. The expensive part is neither the
biggest nor the most detailed: the amp module has 258 faces and 738 edges
against the woofer's 72 and 174, and costs 3.9 s to its 16. What separates
them is **non-analytic geometry** — 93 of the woofer's 174 edges are splines,
where the amp is planes, cylinders, lines and arcs. Exact HLR intersects
curve against curve; a line against a circle is a formula, a spline against a
spline is Newton iteration, and the profile is accordingly 89%
`HLRBRep_Hider::Hide` with `BSplCLib::Bohm`, `PLib::NoDerivativeEvalPolynomial`
and `math_FunctionSetRoot` at the top.

## What to build

**Per-occurrence cost, measured rather than estimated.** The projection loop
already times itself into `ProjectionResult::stats` (`selfMs`, `pairMs`) — it
just adds everything into one number. The same two clocks, attributed to the
shape they were spent on, give the breakdown for free.

**Attribution rule: a pair's cost counts for BOTH of its members.** The
question a reader is asking is "what would leaving this out save", and
leaving out either member of a pair avoids that pair's work. Shares therefore
overlap and can exceed the total; the response says so rather than
normalising and lying.

**Report it where it is read.** `add_drawing_view` returns the cost of the
view and the occupants that carried it, and warns — in `warnings`, beside the
existing ones — when a view took over a second and one occurrence carried
most of it. The warning names the occurrence and the parameter that acts on
it, because a diagnosis nobody can act on is not worth printing.

The breakdown itself is always returned; only the warning is conditional. A
caller that wants the numbers on a fast view should not have to make it slow
first, and a test should not have to build a pathological model to see the
field exist.

## Files

- `src/core/drawing/DrawingProjection.{h,cpp}` — `Stats::bodyMs`, filled by
  the self/pair loop that already holds the clocks.
- `src/remote/RemoteDrawings.cpp` — the response and the warning.
- `docs/drawing-incremental-hlr-plan.md` — the measurement that motivated it
  is already there; link the report from it.

## What the ground truth corrected

The first version charged a pair to both BODIES and summed bodies onto the
occurrence afterwards. That double-charges the pairs *inside* an occurrence —
an imported driver is several solids, and its own cone-against-basket pair is
the dearest call in the view — so the driver came back as **113% of its own
view**, which is a number no reader should ever be shown.

Attribution therefore happens at the group, not the body: `InputBody::groupId`
(the occurrence, set by `DrawingActions`) travels through the cache-key
rewrite untouched, a pair charges both groups only when they differ, and the
costs come back already named. Checked against the measurement that started
this: hiding the woofer saves **15.86 s** measured, and the report attributes
**14.66 s** to it — 8% apart, and the ordering of every occupant matches.

## Verification

- A C++ test over two bodies: the per-body costs are present, non-negative,
  one entry per body that had geometry, and the pair cost appears under both
  members.
- `tests/drawing_smoke.py` (or the nearest drawing smoke): `add_drawing_view`
  carries `cost.by_occurrence`, named by occurrence, on an ordinary two-part
  view — no pathological geometry required.
- The headless suite and the drawing smokes, since the projection loop is
  shared by every view.
