# Tolerances and fits

A tolerance says how far a measured feature of the model may vary: a bore is
`H7`, a shoulder-to-seat length is `+0.05/−0.02`. With them the app can answer
whether two parts go together and with how much slop — every clash sweep and
interference check otherwise runs at nominal.

## The one rule: a tolerance drives no geometry

A size tolerance leaves nominal alone: `Ø20 H7` is still nominally 20.000, with
a band of 0/+0.021. That is what separates it from a Hole's fit (`M6 normal`
picks a *different* diameter, 6.6 —
[../modeling/holes-and-threads.md](../modeling/holes-and-threads.md#hole)).
So a tolerance is metadata plus a checker:

- Tolerances live on the `Document` **outside the regeneration chain hash**.
  A tolerance-only edit rebuilds zero features; `tests/test_tolerances.cpp`
  asserts it.
- A tolerance attaches to a **measured element**, not to a variable. Its
  nominal is measured from the model, because what the kernel built is what
  gets made, and one variable driving four bores does not make them share a
  tolerance.
- Nothing here touches kernel tolerances (`Precision::Confusion`, boolean
  fuzz). A manufacturing tolerance and a kernel tolerance share a word, not a
  concept.
- All values are millimetres.

## ISO 286 limits and fits

`src/core/FitStandards.{h,cpp}` (namespace `fits`), pure static data with no
I/O, mirroring `ThreadStandards`:

- `fits::Limits(nominalMm, "H7")` → a `Band` of signed deviations (plus the IT
  grade and width); `fits::Fit(nominal, "H7", "g6")` → min/max clearance
  (negative = interference) and the `FitKind` — clearance, transition or
  interference — decided by the bounds, not by the letter.
- Carried: sizes 0 < D ≤ 500 mm (the ISO 286-1 steps), grades IT4–IT13, shaft
  letters `d e f g h js k m n p` and the matching hole letters. Hole K/M/N are
  defined only to IT8 and P to IT7, where the standard's Δ correction exists;
  shaft `k` outside IT4–IT7 is `h`. Everything outside is **refused naming what
  is carried** — dropping Δ would be wrong by exactly one grade step, which
  looks plausible.
- `tests/test_fits.cpp` pins the published tables and textbook fits (Ø20 H7/g6
  = 7..41 µm clearance, Ø50 H7/p6 = 1..42 µm interference, Ø20 N7 = −7/−28,
  Ø20 K7 = +6/−15).

## The stored tolerance

`struct Tolerance` (`src/core/Document.h`), serialized (golden
`v98-tolerances`, and `v99-distance-tolerance` for the second ref):

- `ref` — the face. With `ref2` set it is a tolerance on the **distance**
  between the two faces instead of on `ref`'s size.
- `isoClass` (`"H7"`) — stored as the class, not the numbers it implies, so a
  table correction reaches every document. Empty means `lower`/`upper` (mm,
  signed from nominal) are authoritative.
- A **distance** tolerance takes `lower`/`upper` and refuses a class: a class
  says how a shaft or bore may vary, not how far apart two faces are.
- One tolerance per **subject** — a face's size, or an ordered pair of faces —
  so setting another replaces it, and a bore can carry its own size and any
  number of distances measured from it. `Document::ToleranceFor(ref)` returns
  only the size tolerance, so a fit check never reads a gap as a bore size.
- Only a round face has a size; a size tolerance on anything else is refused.
- A tolerance whose element no longer resolves is kept and listed as
  unresolved: the intent outlives the topology.

## Checks

- **`check_fit`** — `{hole: {class, feature_id?, element?, part?}, shaft: {…},
  size?, intent?}`. A hole alone gives its band at `size`; both give the fit;
  element refs measure the faces (optionally in another part via `part`). Two
  measured diameters that differ, with no explicit `size`, are **refused
  naming both**: an ISO fit is defined on one basic size, and a clearance
  modelled into the geometry *and* asked for as a class is counted twice.
  `intent` (`clearance|transition|interference`) adds `intent_ok`.
- **`check_fits`** (`ui::CheckFits`, `src/ui/ToleranceActions.{h,cpp}`) walks
  the assembly's round mates; a mate already names the two faces of a fit. For
  each whose anchors are both toleranced faces it computes the fit, with the
  **mate type as the intent**: a joint that moves (revolute, slider,
  cylindrical) fails an interference fit and warns on a transition fit; a
  fastened round joint is `reported`, not judged, because a shrunk-on gear and a
  bolt through a clearance hole are both fastened and want opposite fits.
  Anything not fully answerable — an anchor that is a frame, an untoleranced
  side, two basic sizes, a side with explicit deviations (which cannot say which
  side is the bore), both sides the same case — is `unverified` with the
  reason, never a pass. It reaches into sub-assemblies, naming each side by its
  path (`Rig/Housing`). The GUI and the RPC share this one walk. A
  generated bearing's bore or outside is read with the ring's own ISO 492
  band and says which side it is, so the other side may carry a class or
  explicit deviations; such a fit is `reported`, not judged
  ([../parts/README.md](../parts/README.md#fits-and-seats)).
- **`stack_up`** (`tolstack::Sum`, `src/core/ToleranceStack.{h,cpp}`) — worst
  case along a **stated** ordered chain `{terms: [{tolerance, subtract?}]}`:
  the summed nominal and the min/max the bands allow. A subtracted term
  contributes its upper deviation to the minimum. The chain is stated, not
  inferred: a mate names the faces that touch, but a stack alternates a contact
  and a length inside a part, and nothing says which of a part's distances lies
  along the axis. Correlation is not accounted for — a tolerance's nominal is
  measured, so there is no path back to a variable that drives two lengths —
  and the answer says its terms are assumed independent. Fewer than two terms,
  an unknown id, or a term that no longer resolves is refused rather than
  counted as zero.

Worst case only: no statistical stack-up, no form or position tolerances.

## Drawings

- A **diameter** dimension on a toleranced bore reads its tolerance —
  `Ø20 H7` or `Ø20 +0.05/-0.02` (`drawing::FormatToleranceSuffix`,
  `src/core/drawing/DrawingCallout.cpp`) — at every update, since nothing is
  copied. A radius dimension takes no class (an ISO class states a diameter's
  variation, so `R10 H7` would read as twice the band). A circle that matches
  two tolerances gets neither.
- The match (`drawing::FindToleranceForRef`) is name-to-name: the feature ids
  inside the circle's element name narrow the candidates and the radius
  cross-checks them. It cannot match on `ElementRef::featureId`, because that
  is the body's owner (the box a hole was cut into), not the feature that made
  the bore.

## GUI

- **Editor in the Measure tool window** (`ui::DrawTolerancePick`,
  `src/ui/TolerancePanel.cpp`): one picked face tolerances its size, two the
  distance between them — the same picks as `ref`/`ref2`. The class is built
  from a letter picker (uppercase for a bore, lowercase for a shaft, from
  `fits::HoleLetters()`/`ShaftLetters()`) and a grade picker. The letter starts
  empty and the editor refuses until one is chosen: nothing in the geometry
  says whether a cylinder is a bore or a shaft, and there is no honest default
  class. A distance's deviations are not seeded either. The band is previewed
  at this size before it is set, and the distance line says it is the closest
  approach. The editor makes the same refusals as the RPC, in the same words.
- **List** as a section of the Variables panel (`DrawTolerancesSection`):
  wrapped rows with limits recomputed against current geometry, `unresolved`
  for a dangling ref, and a delete button per row.
- **Check fits** (`###bom_check_fits`) beside Check fasteners in the Occurrence
  panel; run on demand, with a summary counting each status apart.

## RPCs

`set_tolerance {feature_id, element, class | lower+upper, b?, comment?}`,
`list_tolerances {part?}`, `clear_tolerance {id}`, `check_fit`, `check_fits`,
`stack_up` — all in `src/remote/RemoteInspection.cpp`. Tolerance edits are
undoable commands (`src/core/Command.h`).

## Tests

`tests/test_fits.cpp` (the tables), `tests/test_tolerances.cpp` (storage,
replace-per-subject, zero rebuilds, undo, round-trip, sizes and distances
coexisting, stack signs and refusals), `tests/tolerances_smoke.py` (RPC:
set/list/clear/undo, `check_fits` off mates including the sub-assembly walk),
`tests/drawing_smoke.py` (callouts), GUI `tolerance/a_measured_bore_gets_a_class`,
`tolerance/a_plane_is_told_what_to_do`, `tolerance/the_panel_lists_and_clears`,
`tolerance/check_fits_from_the_panel`.

## Not built

- An inferred stack-up chain, and correlation between terms (needs tracking
  from measured geometry back to the variables that drove it).
- Statistical stack-up; geometric (form/position) tolerances.
- Thread tolerance classes as anything but display text (`Thread.thread_class`).
- A vendored general-tolerance table (ISO 2768) to seed a distance's band.
