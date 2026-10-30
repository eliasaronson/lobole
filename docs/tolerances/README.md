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
  letters `d e f g h j js k m n p` and the matching hole letters. Hole K/M/N are
  defined only to IT8 and P to IT7, where the standard's Δ correction exists;
  shaft `k` outside IT4–IT7 is `h`. Everything outside is **refused naming what
  is carried** — dropping Δ would be wrong by exactly one grade step, which
  looks plausible.
- **J and j** are neither a deviation applied at every grade nor symmetric:
  each grade has its own tabulated deviation, carried per grade from ISO
  286-1:2010 Tables 2 and 3 and checked cell for cell against ISO 286-2:2010
  Tables 8 and 24. Hole J (ES tabulated, EI = ES − IT) is defined for **J6,
  J7 and J8 at every size to 500 mm** (J7 at 3..6 mm is printed ±6,
  "identical with JS7" — the same band); from IT9 the standard makes J
  symmetric, so J9 and up are refused naming JS9. Shaft j (ei tabulated,
  es = ei + IT) is defined for **j5, j6 and j7 at every size, and j8 only up
  to 3 mm**; anything else is refused naming those and the js class of that
  grade. EI = ES − IT is 286-2's own rule (its Figure 1); one cell of the
  copy of Table 8 checked here reads otherwise — J6 at 250..315 as +25/−5,
  a band of 30 where IT6 is 32 — and the rule's −7 is what is carried.
- `tests/test_fits.cpp` pins the published tables and textbook fits (Ø20 H7/g6
  = 7..41 µm clearance, Ø50 H7/p6 = 1..42 µm interference, Ø20 N7 = −7/−28,
  Ø20 K7 = +6/−15, Ø20 J7 = +12/−9, Ø20 j6 = +9/−4, a 6002's Ø32 J7/h6 a
  transition fit).

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
  A mate on a flat face is not a fit and is not listed (the interface a bolt
  clamps, a part sat on a face). Anything not fully answerable — an anchor
  that is a frame, an untoleranced
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

## Press fits

A dowel pin drawn 0.02 mm over its bore, a bearing modelled at its seat's
interference: an overlap the tolerances allow is not a clash, and the
contact checks know it from the bands the model already carries. No flag
and no mate is needed, and nothing new is saved.

- **The bands** (`FitBand`, `core/Feature.h`): every round face with a size
  tolerance — its class's band at the size the face is made at, or its
  explicit deviations — and a generated bearing's bore and outside with
  their ISO 492 normal bands. On a printed part the face is made at its
  modelled diameter less the process allowance
  ([printed parts](#printed-parts)), and its band is read there; a printed
  face still needs a tolerance to have a band. `fitband::Stamp`
  (`src/core/FitBands.{h,cpp}`) puts them on the part's bodies after every
  regeneration and every tolerance or print-process edit, so every copy of a
  body that reaches a contact check carries them, as a ring carries its
  grip. Tolerances stay outside the regeneration chain: the stamp is the
  Document's, like a body's appearance, and a tolerance edit still rebuilds
  nothing. A Derived copy carries the bands its source stamped, as the
  source read them, under its own face names
  ([size bands on a copy](../modeling/derived.md#size-bands-on-a-copy)).
- **A press fit** is a bore of one placed body and a shaft of another (read
  off each face's outward normal), coaxial, engaged along the axis, the shaft
  larger, both banded, whose bands allow the interference the model shows:
  as made (a printed side's allowance taken off), the shaft's diameter less
  the bore's is at most the shaft's upper deviation less the bore's lower.
  `Measure::ContactBetween` relieves exactly the annulus between the two
  cylinders over the length they engage (and a hair past each end, ten times
  the radial interference, which takes a chamfer's run through the
  annulus). Whatever else the pair overlaps is still a clash.
- **Not a press fit, still a clash**, with the reason in `press_fit_refused`
  {fit, reason, interference_mm?, max_interference_mm?, excess_mm?}: past
  the band (`excess_mm`, the interference beyond what the fit allows, and
  nothing relieved), a fit that allows no interference (a clearance fit), a
  band on the wrong side (a hole class on the shaft), one side with no band
  ("the shaft (Ø8.024 modelled) carries no size band"), or two faces whose
  axes are more than 0.1 µm apart over the engaged length. Two faces neither
  of which is banded are plain geometry, and nothing is said. A refusal
  explains a clash, so it is said only when an overlap is left once every
  fit of the pair is relieved: a bearing's bore and the round outside of
  the plate its outer ring is pressed into share an axis and an engaged
  length without being a fit, and beside the relieved outer-ring fit they
  have no clash to explain.
- **What is reported**: the fit as `"Ø<basic> <hole band> / <shaft band>"`
  ("Ø47 K7 / 6005 outside (ISO 492 normal)", "Ø8 H7 / p6"), the basic being
  the roundest size both made diameters lie within their bands about; the
  interference as made and the most the bands allow, on the diameter; the
  engaged length; and the relieved volume. Where each check puts it is in
  [../inspect/README.md](../inspect/README.md#designed-overlaps).
- **Pairing is geometric.** Two coaxial banded faces of placed parts are
  found as a ring's grip is, so a fit needs no mate and holds on a part tab's
  bodies too. A mate that names the two faces (the pairing `check_fits`
  reads) places them exactly coaxial, so it is the same pair.

`check_fits` is a different question and unchanged: it reads a fit off a
mate at ONE basic size, and two faces modelled at different diameters are
`unverified` there. A model drawn at the interference is what this section
is for.

### Printed press fits

A printed cap pressed on a printed pin is a press, but no ISO band says so:
p6 at Ø3 allows about 0.01 mm, and a printed press is whatever the
profile's [press allowance](#press-allowance) is. So a size tolerance has a
**printed press** class besides an ISO class and explicit deviations
(`Tolerance::printedPress`, v179; `set_tolerance {printed: "press"}`, the
one value accepted). It is set on both faces of the press, the cap's bore
and the pin.

- **Its band is not stored.** `printfit::PrintedPress` reads it off the
  part's print profile, as an ISO class is read off its table, so a
  corrected profile reaches every press. Both faces are modelled at what the
  press needs, so the band is about the pin's modelled diameter with no
  process allowance on top: the bore's is the profile's `press` allowance,
  the pin's zero, each give or take a tenth of the profile's gap at that
  size (`kPressToleranceShare`, the tenth the clearance item allows a gap).
  A profile with no press row reads its pair's built-in one, and the reply
  says `press_from`.
- **Refused when it is set**, naming why: a part with no print process, a
  process that is not FDM, a face that is not a cylinder, a size the
  profile's press or gap rows do not cover. `fitband::PrintedPressBand` is
  the one check, and `list_tolerances` reports a band that stopped
  resolving as `error`.
- **The relief** is the [press fit](#press-fits) one, unchanged:
  `fitband::Stamp` stamps the band (`FitBand::printedPress`), and
  `Measure::ContactBetween` relieves the annulus. Two differences. A
  printed press is reported even where the model shows no interference
  (a press allowance of 0 still holds, because FDM prints the bore small and
  the pin large), with a relieved volume of 0; it is skipped only when the
  model is looser than the band. And it pairs only with another printed
  press: against an ISO band it is refused, saying so.
- **Reported** as `"Ø<pin> printed press <interference> (<profile>)"`
  ("Ø3.22 printed press 0 ('K5 PLA on P1S' (fdm pla))"); past the band it is
  a clash with `press_fit_refused` and its `excess_mm`, as an ISO fit is.
- Elsewhere: `check_fits` reports a printed pair (`reported`, it is the
  contact check's) and refuses one paired with an ISO class; `stack_up`
  adds its band; a drawing callout says "printed press"; a CAM job mills
  nothing to it.

## CAM

A CAM job cuts a toleranced round face, and a floor or shoulder whose
distance from a level reference face is toleranced, to the middle of its
band, and judges the posted cut against the band, not nominal; the
program's header states what it aimed at
([../cam/operations.md](../cam/operations.md#tolerance-bands)). The job
reads the part's size and distance tolerances as it assembles its bodies,
so a tolerance edit regenerates its toolpaths and still rebuilds no
feature.

## Printed parts

An FDM printer makes a hole smaller than its model and a shaft larger, so a
fit on a printed part is **modelled** at its size plus a process allowance.
`src/core/PrintAllowance.{h,cpp}` (namespace `printfit`) holds the
**built-in tables**, static data like `FitStandards`: per process and
material, a signed allowance **on the diameter**, model = basic + allowance.
They are the defaults. What one printer, filament and slicer preset really
want is a [print profile](#print-profiles), the user's own copy of a table,
corrected from a coupon.

| fdm, petg | size (the printed diameter) | allowance | source |
|---|---|---|---|
| hole | under Ø10 | +0.2 | 3dprintbounty.com tolerances guide (FDM holes 0.1-0.4 under, model 0.2 over); PETG holes 0.2-0.3 under on Ø10, part of it faceting |
| hole | from Ø10 | +0.3 | as above |
| shaft | any | −0.1 | sovol3d.com clearance guide (PETG sliding fit 0.2-0.3 per side, most of it the hole's) |
| internal thread | any | +0.4 | FreeCAD Fasteners 3D-print nut 1.03·D + 0.10 (M8 → +0.34); BOSL2 4·`$slop` at 0.1 |
| external thread | any | −0.1 | FreeCAD Fasteners 3D-print screw 0.99·d − 0.05 (M8 → −0.13) |
| gap | any | +0.6 | sovol3d.com print-in-place guide (0.15-0.35 a side by printer and filament); PETG bonds to itself more than PLA, so 0.3 a side |
| press | any | 0 | **a default to confirm with the coupon's press row** ([press allowance](#press-allowance)) |

| fdm, pla | size | allowance | source |
|---|---|---|---|
| hole | any | +0.2 | 3dprintbounty.com tolerances guide; PLA shrinks 0.3-0.5 %, less than PETG, so PETG's step at Ø10 is not carried |
| shaft | any | −0.1 | as above: an outside diameter grows by about a tenth |
| internal thread | any | +0.4 | as for PETG (the Fasteners and BOSL2 numbers are not per material) |
| external thread | any | −0.1 | as for PETG |
| gap | any | +0.5 | **a default to calibrate**: sovol3d.com print-in-place guide (0.15-0.35 a side by printer and filament); makerworld.com clearance tests on Bambu printers, 0.25 a side best. One P1S wanted far less ([from a measurement](#from-a-measurement)) |
| press | any | 0 | **a default to confirm with the coupon's press row**: a printed bore pressed onto a printed pin is modelled at the pin's size, or up to 0.1 under ([press allowance](#press-allowance)) |

A **gap** is two surfaces printed together that must come apart — a
print-in-place joint or gear. Its size is the inner surface's diameter, its
model the outer's, and the allowance the whole diametral clearance; a
generator that needs it per side (a gear's backlash, a tip clearance) halves
it.

It assumes a 0.4 mm nozzle and the slicers' XY and hole compensation at their
default of 0 (Bambu Studio and PrusaSlicer ship no per-material values, so the
allowance belongs in the model). Every number is community practice and CAD
tool defaults, **to confirm with a test coupon** — `generators/print_coupon.py`
prints the table's sizes and ± steps around them, every report on a built-in
table says so, and the answer is kept in a print profile.
The PLA rows are the generic FDM numbers less PETG's extra shrinkage; that
PLA's and PETG's differ by less than the sources disagree with each other is
the coupon's to settle.

Where the numbers come from, beyond the table's column:

- **Slicers** compensate but carry no per-material data. Bambu Studio's
  `xy_hole_compensation` and `xy_contour_compensation` default to 0 (a few
  vendor profiles set 0.02-0.025); PrusaSlicer's `xy_size_compensation` is
  0 and its `filament_shrinkage_compensation_xy` 0 %.
- **The Fasteners workbench**'s 3D-printer mode models a screw at
  0.99·d − 0.05 and a nut at 1.03·D + 0.10: 0.47 mm diametral between them
  on an M8.
- **BOSL2** enlarges an internal thread by 4·`$slop` on the diameter; its
  default is 0, its examples 0.05-0.15, typically 0.1.
- **Community practice** for PETG is 0.1-0.2 mm radial clearance a side;
  PETG shrinks 0.5-0.8 %, a little more than PLA.
- **Faceting.** A hole exported as n chords loses the sagitta,
  r·(1 − cos(π/n)), on the radius (nophead's polyhole models it at
  r / cos(π/n) instead), so the STL export's tolerance is part of the
  allowance; `print_allowance` reports it, below.

- **Where it lives: on the part.** `Document::PrintProcess()`, serialized as
  the envelope's `print {process, material}` (v143) — with a profile, its
  name and an embedded copy (v169, [below](#print-profiles)) — outside the
  regen chain like a tolerance or the tube shop. A part is made one way, whichever machine
  or tab opens it; a body's material is its appearance and density, and the
  same PETG is printed or moulded with different allowances, so it is not the
  material. It is not a preference, because a part printed on one machine is
  printed on every one that opens it. `set_print_process` is an undoable
  command that rebuilds only the holes modelled from it (below).
- **Refused, never substituted.** An unknown process names the processes; a
  material the process has no data for names the ones it has. The same
  refusal on load, and in `check_fit`'s `print`.
- **Drives a Hole's geometry: its clearance fit, and a printed
  teardrop.** A Hole with a fit
  (`M6 normal`, [../modeling/holes-and-threads.md](../modeling/holes-and-threads.md#hole))
  names the size the hole must come out at, so on a printed part it is
  modelled at that size plus the hole allowance: M6 normal is Ø6.6 in steel
  and a Ø6.8 model in PETG. A tapped hole stays at its tap drill (what a
  printed thread wants is the thread report's to say). A Hole printed as a
  teardrop (`printed`,
  [../modeling/holes-and-threads.md](../modeling/holes-and-threads.md#printed-holes))
  is refused without a process, and its flat is the process's bridge. The
  regen hash folds the process — `printfit::Fingerprint`, the pair, the
  profile's name and every row's range and allowance — into fitted
  clearance holes and printed holes only, so
  setting it, or taking a profile whose hole row changed, rebuilds those and
  what follows them, and nothing on a part without one. The requirements
  hash folds the same fingerprint, so a profile edit restales a printed
  fit's verdict. Everywhere else
  the generator applies it where it models the part, and the checks say what
  to model.

### TPU

| fdm, tpu | size | allowance | source |
|---|---|---|---|
| hole | any | +0.2 | **to confirm with a coupon**: the FDM row (3dprintbounty.com tolerances guide); nothing published measures TPU 95A apart from it |
| shaft | any | −0.1 | **to confirm**: the FDM row; TPU 95A's own is not published |
| gap | any | +1.0 | **to confirm**: 0.5 a side to slide (blog.uavmodel.com print-in-place clearance guides, 2026: 0.5 sliding, 0.3 for a hinge pin that deforms to turn; another table, 0.5 and up). TPU sags into a gap as it prints and bonds to itself; the guides disagree, and none measured a Bambu printer |
| press | any | 0 | **to confirm**: a TPU sleeve fits at 0, or 0.1 mm under on the radius (forum.prusa3d.com, "question about designing for TPU", on a 95A) |

TPU 95A (Bambu Lab TPU 95A HF) is a built-in table so that a printed TPU part
— a belt, a spring, a gasket — is a printed part like any other:
`check_print` judges it, its allowances come from `print_allowance`, and a
TPU profile on the printer fits its plate to the bed. Every row says
**TO CONFIRM WITH A COUPON** in its source; none is measured here. It
carries **no thread rows**: a printed thread in a 95A flexes off its flank
and nothing published says what allowance holds one, so a TPU thread is
refused naming the table rather than read from PLA's. Its limits:

- **overhang 45°**, the FDM rule. Bambu's TPU 95A HF data sheet (Technical
  Data Sheet V1.0) gives about 55° as the most its tuned profile prints; a
  flexible layer sags first, so the rule is kept.
- **bridge 5 mm**, half the FDM rule: Bambu lists bridging among TPU 95A
  HF's weaknesses (bambulab.com), and its data sheet's 20 mm is the longest
  its profile spans, not a clean ceiling. `printfit::MaxBridgeMm` and
  `MaxBridgeSource` are per material for that reason.
- **its own chords, 0.02 mm** (`printfit::BuiltinStlDeflectionMm`): a
  tenth of its 0.5 gap a side would be 0.05, a quarter of the 0.2 its holes
  are opened by, so the chords are held to a tenth of the hole's. A profile
  that states `stl_deflection_mm` keeps its own; `check_print`'s chords
  item says `deflection_from: material`.

**The bed is the printer's, not the material's**: a TPU plate fits the
same P1S a PLA one does. `print_coupon.py --seed` installs `TPU 95A on P1S`,
the built-in TPU rows on the P1S's bed, keep-out zone, line width and 0.2 mm
layer, so `bed_fit` runs on a TPU plate and a print job takes it. The
engine's springs and belt and the jar's gasket print with it.

### What the checks say

- **`check_fit`**: a measured face on a printed part is at the size it
  PRINTS — the modelled diameter less the allowance (`size_from: printed`) —
  so a PETG bore modelled Ø8.2 meets a steel Ø8 pin at one basic size, and
  one modelled Ø8 is refused as two sizes, the reason saying "model the hole
  8.2". `print.{hole,shaft}` gives each printed side's `basic`,
  `allowance_mm`, `model`, `modelled`/`prints`/`compensated` when measured,
  the row's `source` and a `band_note` when the class is finer than the
  process holds (about ±0.2 mm on desktop FDM): "H7 on a printed Ø8 bore
  (fdm petg): model Ø8.2". A side with no face may state `print: {process,
  material}`, to ask before there is a model.
- **`check_fits`** (`ui::CheckFits`) reads the printed side the same way,
  bearing seats included, and a side's `print` carries the same fields. A
  printed side modelled at the bare size is `unverified` with what to model.
- **Threads.** `ui::PrintedThreadFor` reports a Thread on a printed part: the
  modelled major (the table's, or a custom thread's from its face), what it
  prints, and for a table thread what to model — as a custom thread on the
  face it names (a boss's face is a custom thread's major, a bore's wall its
  basic minor: M8 internal in PETG → a Ø7.047 bore, which prints the Ø8
  major). A custom thread's major from a bore is the wall plus twice its
  basic depth at its own [flank
  angle](../modeling/holes-and-threads.md#flank-angle). It notes a cosmetic thread prints nothing, a modelled one whose
  groove was not cut prints nothing either (with the feature's error), and
  that printed threads are unreliable below M8 (tap a plain bore at the tap
  drill, or use an insert). A thread that did not build has no size to
  report: it carries `failed` (why) and its sentence says "not built",
  never a Ø0 that reads like a measurement -- and add_feature refuses a
  designation the tables refuse, so only a file can hold one.
  `add_feature` and `modify_feature` on a Thread return it as
  `print`; `set_print_process` and `get_print_process` list every thread.
- **`print_allowance`** answers one diameter — `{kind: hole|shaft|
  internal_thread|external_thread|gap|press, size_mm | designation}` against the
  part's process, a stated `{process, material}` or `{profile}` — with
  `model_mm` and what the STL export takes off a round surface: its chords
  sit up to `deflection` inside it, so a hole can lose twice that on the
  diameter (`printfit::ChordLossMm`). Without `stl_deflection_mm` it is the
  deflection `export_stl` meshes this print at ([exporting a printed
  part](#exporting-a-printed-part)). The table came through somebody's
  export: export the parts and the coupon at the same deflection and the
  coupon calibrates both.
- **The confirm note** a report on one row ends with is that row's: a
  built-in row (or a profile row still carrying a built-in source, copied
  and never measured) asks for the coupon -- "the built-in tables are
  community practice ..." -- and a profile's own measured row names its
  source instead ("measured: MEASURED, print_coupon on a P1S ...", and to
  print the coupon again when the printer, filament or preset changes;
  `printfit::ConfirmNote(source)`). `print_allowance`, a printed side of a
  fit and a printed thread all say it; `get_print_process`'s top-level
  `confirm` is about the built-in tables it lists.
- **A profile that is not the store's** says so: `check_fit`'s
  `print_notes` add that the part was modelled with a different copy, or one
  the store does not have, and that the answer is the part's copy.

### One part printed over another

A flat face over another part fails at any number of layers. One empty
layer under it, and the slicer prints it onto the part below, bonded
across its whole area; more, and a part with nothing of its own to bridge
from is printed into the air, its first layer drooping into strands that
weld to whatever they land on. The 2026-10-15 K5 PLA print on a P1S showed
both: the [counter](../../generators/counter/README.md)'s wheels, each one
layer over the next, came off fused solid (the ceiling's skin stayed on
the units wheel when the lid was torn off), and the coupon's old Z-gap
rings, started a few layers over the plate, welded to their posts. A part
printed in place follows two rules instead:

1. **every piece starts on the bed;**
2. **where one part prints over another, the face above is
   self-supporting** — a 45° flank, or a bridge between the part's own
   walls — **and leaves enough vertical air** for the strands not to reach
   the part below.

How much air is the profile's `z_gap_mm` ([below](#print-profiles)),
measured by the coupon's [air row](#the-coupon); `check_print`'s
[stacking item](#checking-a-print) checks both rules;
[`generators/hinge_box/`](../../generators/hinge_box/README.md) is the
smallest part built to them, and the
[counter](../../generators/counter/README.md#what-changed-from-the-print-that-fused),
rebuilt, the largest: every wheel on a tube from the bed, every transfer
on its own column, every face over another piece a 45° cone the air over
it.

### Print profiles

A **print profile** (`printfit::Profile {name, table, stlDeflectionMm,
layerHeightMm, zGapMm, bedMm, bedExcludeMm, lineWidthMm}`) is a named,
user-owned copy of a table: one
printer, one filament, one slicer preset. The CAM machine store is the pattern
([machine profiles](../cam/setup.md#machine-profiles)).

- **The store** is one file per profile, `~/.config/lobole/print_profiles/
  <slug>.json` (`DocumentIO::LoadPrintProfiles`/`SavePrintProfile`/
  `DeletePrintProfile`, `src/core/io/SerializePrintProfiles.cpp`), mirrored
  in `state.printProfiles` by `ui::ReloadPrintProfiles`
  (`src/ui/PrintProfileActions.cpp`) at startup, headless too, and after
  every save and delete. A broken file — one that does not parse, writes a
  key twice, or holds a key a profile or a row does not have — is reported
  by name and the rest still load.
- **A save refuses** (`printfit::ValidateProfile`) a missing name, two names
  that fold to one file, a base process and material the built-in tables do
  not carry, no rows, a size range that is empty, overlapping or out of order
  within its kind, and an allowance of the wrong sign: a hole or internal
  thread is not negative, a shaft or external thread not positive, and a gap
  is positive (a gap of zero fuses); a press may be either. A refusal
  writes nothing.
- **The air over another part** (v175). The `gap` rows are sideways: two
  walls printed beside each other. How much vertical air a face needs
  over [another part printed under it](#one-part-printed-over-another)
  belongs to the printer and the preset, not a diameter, so it is not a row:
  `layer_height_mm` (the preset's layer) and `z_gap_mm`, the vertical air
  a face needs over another part to come away from it, in whole layers of
  it (the slicer ends air at a layer); each 0 / absent until stated. A
  `z_gap_mm` without a layer height, or not whole layers of it, is refused
  naming the two nearest that are (0.3 at 0.2 mm layers: 0.2 or 0.4) —
  never rounded. Neither drives geometry in the app, so neither is in
  `printfit::Fingerprint`. `check_print`'s
  [stacking item](#checking-a-print) judges by it through
  `printfit::MinAirMm`: the profile's `z_gap_mm`, else three layers of its
  `layer_height_mm`, else three layers of FDM's common 0.2 mm (source
  "process", to confirm with the coupon); a process that is not FDM has
  none. The coupon measures it, and a print-in-place generator reads it.
- **The bed** (v179): `bed_mm` [x, y, z], the printer's build volume,
  absent until stated; three positive numbers or refused. It drives no
  geometry. `check_print`'s [bed_fit](#checking-a-print) fits a plate into
  it, and a generator that packs plates (the Strandbeest, the hinge box)
  reads it. Only a profile states one: the built-in tables are a process,
  not a printer, and nothing guesses a printer. `print_coupon --seed`'s P1S
  profiles carry 256 x 256 x 250 (the height is Bambu Studio's P1S
  `printable_height`, the slicer's limit, under the published 256); a
  profile saved before has none until it is set in Settings, Print profiles
  (`###prof_bed_x`/`_y`/`_z`) or with `save_print_profile`.
- **The bed's keep-out zones** (v181): `bed_exclude_mm`, rectangles
  [x0, y0, x1, y1] in the bed's own coordinates (from its front-left
  corner, as the slicer's printable area), absent until stated; each with
  x1 > x0 and y1 > y0, and only with a `bed_mm`, or refused. A P1S keeps
  [0, 0, 18, 28] clear for its nozzle wiper (Bambu's `bed_exclude_area`),
  and Bambu refuses a part there at every rotation, so with zones bed_fit
  becomes [positional](#checking-a-print). `--seed` writes the P1S's.
- **The wall line width** (v181): `line_width_mm`, the slicer preset's,
  absent until stated: what the [printed mass](#printed-mass) takes a wall
  loop as. `--seed` writes 0.45, the inner wall line width of Bambu
  Studio's 0.20 mm Standard on a 0.4 mm nozzle; and to a profile of a
  seeded name that lacks the bed, the zones or the line width, those, the
  rest left as it is.
- **The slender ratio** (v185): `slender_ratio` with its
  `slender_ratio_source`, the tallest a piece may stand over the least
  width it stands on, absent until a tower test states it: check_print's
  [slender](#slender) item, which without it judges by an unmeasured 8.
  `--seed` writes none. The Settings window edits both (`###prof_slender`,
  `###prof_slender_source`).
- **The built-in tables stay**, read-only, as what a profile starts from: a
  new profile is the built-in table of its pair, "the defaults, corrected".
- **The document carries its numbers.** A part printed with a profile has
  `print {process, material, profile, conditions, stl_deflection_mm?,
  layer_height_mm?, z_gap_mm?, bed_mm?, bed_exclude_mm?, line_width_mm?,
  slender_ratio?, slender_ratio_source?, rows}`:
  the name and an embedded copy, the way a CAM setup embeds its machine. The
  copy is what every lookup reads (`printfit::Find` resolves it before the
  built-in table), so a part opens anywhere with what it was modelled with —
  nothing is substituted and nothing refuses to open.
- **Its state against the store** (`printfit::MatchProfile`): `same`;
  `differs`, the user's profile of that name now says something else; or
  `missing`. `get_print_process` reports it as `match`, the Print process
  section says it with *Use mine* (take the store's copy — an undoable
  command on the part) and *Save as mine* / *Save it* (write the part's copy
  to the store), and `check_fit` notes it.

### From a measurement

The first two profiles are the measurements made on one Bambu Lab P1S
(Bambu Studio 0.20 mm, X-Y hole and contour compensation 0, the STL at 0.01
mm chord deviation), with `print_coupon --gap-only`:

| profile | gap a side | what was seen |
|---|---|---|
| K5 PLA on P1S | 0.1 | 0.075 freed only under force and rubbed; 0.1 slid freely; a gearbox modelled at 0.25 was loose |
| PLA on P1S | 0.15 | an ordinary PLA slid at 0.15 |

They live in the user's config, not in the build:
`python3 generators/print_coupon.py --port PORT --seed` saves both, and
`TPU 95A on P1S` ([TPU](#tpu)), through
`save_print_profile` on the instance at PORT — into whatever config dir that
instance was started with — with the P1S's 256 x 256 x 250 mm bed, its
keep-out zone and its wall line width, and leaves any name already stored,
except that one which lacks any of those three is given the P1S's and
nothing else. The other rows are the built-in PLA rows.

### Press allowance

A **press** is the other way round from a gap: two parts printed APART and
pressed together to stay, such as a printed cap on a printed pin
(`printfit::Printed::Press`, v178). Its size is the pin's modelled
diameter, its model the cap's bore, and the allowance the bore less the
pin on the diameter. It is quoted a side as the INTERFERENCE, minus half
the allowance. Either sign is a press, because FDM already prints a bore
small and a pin large (the hole and shaft rows). That is where the hold
comes from with the bore modelled at the pin's size, and why a bore
modelled much under the pin splits a thin cap.

- **The built-in row is a default** of 0 for both materials: the bore at
  the pin's size, the middle of community practice (the pin's size, or up
  to 0.1 under for a tight fit). Its source says it is to confirm with the
  coupon's press row.
- **A profile with no press row**, one saved before the kind existed or
  whose press row has not been printed yet, reads its pair's built-in row.
  The lookup refuses nothing here, unlike a missing hole row:
  `printfit::ForBasic` marks the answer `builtinFallback`, and
  `print_allowance` says `from: builtin` with a note naming the profile and
  the coupon. That is how an unmeasured `z_gap_mm` falls back to whole
  layers, and it is the only kind that falls back. A profile states the
  others from the start.
- **The coupon's press row** (`print_coupon.py --press-only --size D`): pins
  of Ø`D` on shoulders on a plate, and a loose cap for each on the bed in
  front of it, its bore stepped `--press-step` on the diameter either side
  of the table's press, two caps at every step. Pins are numbered on the
  plate, 1 the tightest, and each cap carries as many notches in its rim.
  Press each cap down its pin onto the shoulder. The smallest step whose
  two caps both hold by hand and do not split is the answer. A step is at
  least 0.1 mm (`PRESS_MIN_STEP`, a smaller one refused): the scatter
  between two printed caps is larger than 0.05 mm, a Strandbeest row at
  that step read out of order, and the pair at each step shows the
  scatter. The scatter is the printer's, about as large in mm at any size,
  so a larger reference pin would not shrink it, and the press a part
  needs is the one at its own size: the row stays at the size it is for. `--record --press-side MM` keeps it (the interference a side, as
  modelled) as the profile's `press` row from Ø0 up. A generator that
  presses caps prints the row beside its first parts, at its own pin and
  cap (`press_row(d, proc, size, steps, step, name, cap_d, cap_t)`), as the
  Strandbeest's first plate does (generators/strandbeest/README.md).
- **Seen by the clash checks** once both faces carry a
  [printed press](#printed-press-fits): a pressed cap and its pin are two
  occurrences on a fastened mate, and the exact check relieves and lists
  the press. The coupon still says what the press is.

### Exporting a printed part

A slicer's gap closing shuts what an STL's chords have already narrowed:
on the coupon, `export_stl`'s 0.1 mm took 0.03 mm a side off every gap and
the slicer closed the two narrowest. So `ui::ExportPartStl`
(`src/ui/StlExport.cpp`), the one helper `export_stl` and File › Export
both call, picks the chord deviation:

- **A printed part with no deflection given** meshes at
  `printfit::StlDeflectionMm`: the profile's `stl_deflection_mm`, else its
  material's own (TPU's 0.02, [TPU](#tpu)), else a tenth of its smallest
  gap a side (0.025 on the built-in PLA table). An
  unprinted part keeps 0.1 mm.
- **A coarser deflection asked for** is exported as asked, and the reply's
  `warning` says how much of the smallest gap the chords can take. The GUI
  never passes one, so it never warns.
- `CADKernel::SaveSTL` takes the angular deflection explicitly (0.5 rad,
  OCC's own): a chord meets both bounds, which is why the linear one is an
  upper bound on what it takes.
- **Several bodies are one shape** through `CADShape::UnionOf`, as the
  BREP export's are: bodies whose boxes are apart are gathered as a
  compound, which is what a fuse leaves them, and each group whose boxes
  meet is fused in one boolean. A print plate is a hundred pieces that
  touch nothing, and fused one `Union` at a time it re-processed the
  growing whole for each: 42 s of a 123-piece plate's export, 0.7 s now.
- File › Export says what it did in a viewport pill: "Exported part.stl —
  chords 0.01 mm, 10% of the 0.1 mm gap a side", or the failure, amber.

### Checking a print

`check_print` (`src/core/PrintCheck.{h,cpp}`, namespace `printcheck`) is
the one printability check: the RPC, the Print process section's *Check
print*, and every print-in-place generator through `generators/_lib.py`'s
`check_print`. `ui::RunPrintCheck` (`src/ui/PrintCheckActions.cpp`) derives
the limits from the part's print, so the button and the RPC cannot disagree;
the core takes bodies and limits and no document. It answers over the
part's non-reference bodies (or `bodies`) as one print along `direction`
(default +Z; the bed is the plane below it through the origin). Nine
items, each `{status: pass | fail | skipped, measured, limit, note}`:

| item | measured | limit | how |
|---|---|---|---|
| `bed` | the lowest point along the direction | within 1 µm of 0 | exact: each body rotated so the direction is +Z, boxed by `BRepBndLib::AddOptimal` without triangulation; plus the area lying on the bed |
| `shells` | the pieces the print comes off the bed as | one per solid | solids within 1 µm of each other are one piece, from the clearance item's distances; no boolean. A fused print-in-place pair fails; an extrude of loose discs is one body of several solids and several pieces |
| `overhang` | area of downward faces past the limit, the worst angle, and where (body, face, element, point) | the process's (`printfit::MaxOverhangDeg`, 45° for FDM), FROM VERTICAL, and an area allowance (default 0) | the B-Rep's surface normals: a plane is one exact normal; a curved face is sampled at every node and triangle centroid of a tessellation (on a copy, 0.2% of the body's size) by its UV, then the worst sample is climbed on the face's own UV domain. A designed 45° flank reads 45.000, not a facet's 47-50, so no slack is needed. The AREA is the tessellation's triangles each classified by the exact normal at its centroid: its error is the triangles that straddle the limit |
| `bridges` | the widest span of the ceilings, and each one's area and height | the process's (`printfit::MaxBridgeMm`, 10 mm for FDM, 5 for [TPU](#tpu)) | a ceiling is a downward face within 1° of horizontal everywhere, off the bed: a plane by its one normal, any other surface (a derived or lofted face is often a B-spline) by every sample. Its span is its inscribed diameter, as if anchored along its whole edge: twice the largest distance from a point of the face to its edges (polylines at a quarter of the tessellation's chord), on a grid over its UV and then climbed |
| `clearance` | the minimum distance between any two solids, with the closest pairs | the gap a side (`printfit::SmallestGapSideMm` of the print), passing down to a tenth under it | `Measure::ClearanceBetween` per pair (BRepExtrema, the distance `measure` gives), `ContactBetween` when it cannot prove them apart; a pair whose oriented boxes are four gaps apart is clear without measuring, each solid's box built once rather than once a pair; the boxes are the B-Rep's, never its triangulation's, so the prune is the same meshed or not and a box never stops inside a curved surface by the chord deviation |
| `chords` | the chord deviation `export_stl` meshes this print at | a fifth of the gap a side | `printfit::StlDeflectionMm`, as [the export](#exporting-a-printed-part) takes it, or `stl_deflection_mm` |
| `stacking` | each piece's lowest point (`lifted`: the pieces that start above the print's lowest point, with their height and lowest face); per piece over ANOTHER piece, the area of its faces under the limit, the least vertical air and where (`pairs`) | on the bed within 1 µm; the minimum air (`printfit::MinAirMm`, or `min_air_mm`), passing down to a tenth under it | the pieces are the shells item's (solids within 1 µm); each lowest point exact, as the bed's. The samples are points of every downward face's own surface, off the bed, at UVs the overhang item's tessellation places and inside the face (a triangle's UV past a curved edge is the surface beyond the face, not a sample): a triangle is split while it is wider than four limits, or while its centroid and corners disagree and it is wider than a sixteenth of the limit. From each, a ray goes down along the direction to the nearest material within the limit: only a surface that faces up can be met first from outside, so the upward triangles of every solid's tessellation are binned on a grid across the direction, and the triangle a ray meets names the face and where on it; the air is then taken off that face's own surface, closed-form on a plane and by Newton from the triangle's UV on anything else, and a triangle whose surface puts the material outside the limit passes the ray on (a hollow's chord lies above its surface). A triangle on its face's boundary hands the ray to that face and every face across its edges, each intersected exactly (`IntCurvesFace`) and kept only where the ray meets it on the face, the least air of those: past a top's polygon the ray meets the flank's triangle, whose surface runs on above the top. So the air is exact up to a face's edge: the hinge box's cone pins read their modelled 0.4 mm on K5 to a micron. The exact intersections run on boundary triangles alone, which keeps a plate of thousands of faces to seconds. A face, or triangle, with no other piece's box within the limit under it is never sampled. Material of the sample's own piece first is the bridges item's; another piece's is air. The area is the leaves whose centroid is under the limit |
| `bed_fit` | the plate's box: `extent_mm`, x along the bed's x (the world's x, or y when the direction is x), z up the direction; with keep-out zones, every piece where it is (`intrusions`) | the profile's `bed_mm`, with `shortfall_mm` per axis; and its `bed_exclude_mm` (`exclude_mm`) | `BRepBndLib::AddOptimal` of every body in the printer's frame. Without zones the extent, not a position: a slicer places the plate, so it fits when its box does. With zones (v181, `printcheck::FitOnBed`) the part's x and y ARE the bed's, printed up +Z: a piece whose footprint box leaves the bed, or which itself (`BRepExtrema` against the zone's prism, after the boxes meet) enters a zone, is an intrusion naming the zone. A piece laid along the bed's diagonal has a box over the whole bed and misses the corner. Another print direction with zones fails, saying the frame is not the bed's |
| `slender` | per piece, its height over the least width of its first layer (`ratio`), and its worst section narrower than that (`neck`: the height above it over its width); `pieces` worst first, `over` how many are past the limit | `slender_ratio` of the profile, else the built-in 8, unmeasured ([slender](#slender)) | the pieces are the shells item's. The footprint is the convex hull of the piece's downward planar faces on its lowest point, its least width the narrowest extent across one of the hull's edges; a brim (the piece's [settings](#slicer-settings), `outer` or `both`) adds its width each side. The sections are cut through the overhang item's tessellation at 256 heights up the piece, each the least width of its convex hull; a section within a fiftieth of the footprint is the footprint, so a uniform pin has no neck |

- **bed_fit is the printer's**, so it runs only on a part printed with a
  profile that states `bed_mm` (v179) and is skipped otherwise, its note
  saying which: the built-in table is no printer's, and a profile without
  a bed has not said one. A skipped bed_fit makes no claim in the receipt:
  without a printer there is nothing to claim. `generators/_lib.py`'s
  `PRINT_ITEMS` carries it, and `check_print` there demands it pass on a
  part with a profile; on the built-in table it is printed as skipped and
  not demanded.
- **A tenth under the gap passes**: an arc-approximated involute (gear.py)
  comes out microns short of its gap, and FDM holds about ±0.2 mm; a
  clearance modelled within 10% of the profile's prints the same.
- **Faces are judged from the part's own lowest point**, so a part off the
  bed fails the bed item alone, not every face on its underside. Stacking
  measures from the same point: the bed item asks that the PRINT is on the
  bed, stacking that every PIECE is.
- **A flat face over another piece fails at any number of layers**
  ([why](#one-part-printed-over-another)). What passes is a piece that
  starts on the bed and reaches over another with a face that holds itself
  up, the minimum air above it: wheels stacked one layer over each other
  fail (the counter's run builds that layout as a negative control); a
  ring round a post
  under a 45° head with three layers passes.
- **Every limit can be stated** (`max_overhang_deg`,
  `overhang_allowance_mm2`, `max_bridge_mm`, `min_gap_mm`,
  `stl_deflection_mm`, `min_air_mm`, `max_slender_ratio`); each item says
  where its limit came
  from (`limit_from` / `required_from` / `deflection_from`: `asked`,
  `process`, `profile`, `gap`, `material` (TPU's own chords), for the
  air `layers`, and for slender `default`), the process's
  with its `source`. A process that is not FDM knows no air, so stacking
  is skipped, saying so. An **unprinted** part has no limit to take, so
  overhang, bridges, clearance, chords, stacking and slender are `skipped`, the note naming
  `set_print_process` or the parameter, and the receipt is `unverified` —
  never judged against a number nobody chose. `ok` is every item that ran
  passing.
- **Refused**: a direction that is not three numbers or has no length, a
  limit out of its range, a body or part that does not exist.
- **A part placed for a job**: with zones a part checked where it was
  modelled (a coupon at the origin, in the keep-out corner) fails bed_fit;
  a generator checks the part's own shape without it and bed_fit on the
  plate it prints on.
- **The Print process section** has *Check print* (`###print_check`) under
  the part's rows and threads: one line per item, measured against limit, a
  failure amber, a skipped item greyed with its reason, and "the part has
  changed since" once it has. The overhang, bridge and stacking lines have
  *Show* (`###print_check_overhang`, `###print_check_bridge`,
  `###print_check_stacking`), which selects the worst face so the viewport
  points at it: for stacking, the face with the least air over another
  piece, else the lowest face of the highest piece off the bed. The last
  line but one, *fits the bed*, gives the plate's extent in the bed and,
  failing, what it is over by; the last, *slender*, the worst piece's
  height on its footprint (and brim), its neck, and the ratio against the
  limit.

#### Slender

A piece stood on end comes off the bed, or sways under the nozzle, once it
is tall against what it stands on: the clock's four pillars (123 mm on a
Ø5.0 end, 24.5:1) and two arbors (79 and 95 mm on Ø4.8) let go part-way up
on the P1S, and the nozzle strung the rest of their layers into the air,
while every other item passed the plate. The slender item fails a piece
whose height over the least width of its first layer, or whose height
above any narrower section over that section's width, is past the limit:
a bottle on a wide foot is as weak at its neck as a pin at the bed.

- **The limit is not measured.** A profile states `slender_ratio` with its
  `slender_ratio_source` (v185; one without the other is refused), from a
  tower test of pins of rising height. Without one the item judges by the
  built-in `printcheck::kSlenderRatio`, 8, and its `source` says so: FDM
  guides put a free-standing pin's safe height at a few to ten times its
  width. `limit_from` is `profile`, `default` or `asked`.
- **A brim counts at the bed and nowhere else.** An `outer` or `both` brim
  adds `printset::kBrimWidthMm` (5 mm, Bambu Studio's default) each side to
  the footprint; `inner` adds nothing outside it, and `ears` grow only at
  sharp corners, which a round foot has none of. A print job writes
  `brim_width` on every object with a brim, so the slicer prints the width
  the item counted. A piece past the limit on its own footprint and
  within it with the brim says `brim_holds`: the brim is what keeps it
  down. A neck is never widened by one.
- **What passes is a shaft lying down.** A round lying on the bed has a
  line for a first layer and its underside past 45°: the clock and the
  engine lie theirs on a flat cut along the whole length, whole layers
  deep and as deep as leaves the round meeting it at 45° or steeper
  (`clock_spec.lie_depth`, the engine's `d_shape`), so the overhang item
  passes the same piece.

### Slicer settings

A printed part's slicer settings are part of the design: a pendulum's
period is computed for the plastic it is printed with, and printed at the
slicer's 15 % it swings at another. `printset::Settings`
(`src/core/PrintSettings.{h,cpp}`) holds them beside the print process:

- `walls` (wall loops, at least 1), `top_layers` and `bottom_layers` (0 or
  more), `infill_pct` (0-100) and `infill_pattern` (rectilinear,
  aligned_rectilinear, grid, gyroid, honeycomb, cubic, triangles, line,
  concentric, lightning, adaptive_cubic, tri_hexagon, 3d_honeycomb,
  cross_hatch, hilbert_curve), `supports` (default off) and `brim` (none,
  outer, inner, both, ears; default none). Each is optional: unset is the
  slicer preset's, and the job leaves it to the slicer -- except supports
  and brim, which a job writes off and none unless set.
- `direction`, the print direction in the part's frame, unit (default +Z):
  the [printed mass](#printed-mass) reads it, for a part printed lying
  down or on its end, and a printed hole's teardrop points up the part's
  ([printed holes](../modeling/holes-and-threads.md#printed-holes)). A plate's piece never carries its source's: the
  plate's frame is the bed's.
- **Refused** (`printset::Validate`, every parser and the GUI through it):
  walls under 1, a layer count under 0, an infill outside 0-100, a
  direction that is not a finite non-zero vector, an unknown pattern or
  brim (naming the accepted), and 100 % with a pattern that cannot print
  solid -- Bambu Studio refuses grid there and asks to switch to
  rectilinear; the solid ones are its top-surface patterns, rectilinear,
  aligned_rectilinear, concentric and hilbert_curve -- or with none, since
  the preset's may be grid.
- **The part and its bodies** (`printset::PartSettings`): the part's, and
  per body an override keyed by the creating feature's id
  (`Body::createdBy`, the key `set_material` uses), merged field by field
  over the part's and validated merged. A plate's piece is a Derived copy,
  and its settings are its SOURCE's, between the plate part's own and the
  plate's override for that piece: plate part < source part < source body
  (`Body::derivedFrom`, the source's createdBy, stamped by the Derived
  feature) < plate body (`ui::PrintOf`, `src/ui/PrintJobActions.cpp`).
- **Stored** (v181) as the document's `print_settings {part?, bodies?:
  {"<created_by>": {...}}}`, only what is stated, omitted when there are
  none (`src/core/io/SerializePrintSettings.cpp`); an undoable
  `SetPrintSettingsCommand`; no regeneration but the printed holes', when
  the part's `direction` changes. Golden `v181-print-settings`.
- **The filament is the body's material** when that is one (pla, petg,
  tpu), else the process's: what the mass weighs must be what the slicer is
  loaded with, so there is no separate filament setting, and a body of a
  material that is no filament is refused by a job. A TPU part (the jar's
  gasket, the engine's belt and springs) is printed in the TPU process, on
  the TPU profile of the same printer ([TPU](#tpu)).

### Printed mass

For a printed part, `mass_properties` reports `printed` beside every
body's solid mass, and `total.printed`: what the slicer's walls, shells and
infill weigh (`printmass::EstimatePrinted`, `src/core/PrintedMass.{h,cpp}`).

- **The method is the slicer's, on a raster.** The body is meshed (a chord
  of a tenth of the line width) and cut at the profile's layers up the
  print direction, each layer at its middle; each section is filled
  even-odd on a grid of a quarter of the line width (coarsened to at most
  2 million pixels a layer and 200 million a body, said in `pixel_mm`). A
  pixel is SHELL within the walls' thickness (walls x `line_width_mm`) of
  the outline, by an exact Euclidean distance transform, or where any of
  the `top_layers` above it or the `bottom_layers` below it is not
  material; otherwise CORE, weighted by the infill. Mass, centre and
  inertia (about the centre, world axes, OCC's matrix of inertia, as the
  solid's) are the weighted sums, each pixel a slab with its own moment.
- **Printed solid, it is the solid**: at 100 % infill the answer is the
  body's own mass properties off the B-Rep, exactly (`estimate: false`, no
  raster), so a pendulum's period does not move with a pixel.
- **Otherwise it is an estimate and says so** (`estimate: true`, `method`): the
  slicer's paths differ at small features, and a wall is taken as walls x
  line width. A cube is exact to the pixel: 20 mm at 0.5 mm lines and 0.2
  mm layers is its shell plus its core by hand (`[print][mass]`).
- **Its inputs are stated, never guessed**: the profile's `layer_height_mm`
  and `line_width_mm`, and walls, both shells and the infill. Any unstated
  gives `printed: {why}` naming them; a part not printed gives `null`. A
  body with no material weighs as its filament, the process's
  (`density_from`).
- **Remembered** per shape, placement and inputs (the shape held, so its
  address cannot be reused), so a generator that weighs the same pendulum
  many times pays once.
- **An assembly weighs as built.** From an assembly tab each occurrence
  says `mass_from`: `printed` when its part is printed (its `printed`
  {mass_g, center_of_mass_mm}, the sum of its bodies' estimates carried
  through its placement), `solid` otherwise. `total.printed` then weighs
  the whole as it is made: each printed occurrence's printed mass, centre
  and inertia, each other one's solid, with the inertia about the common
  centre in world axes (`inertia_g_mm2`, the parallel-axis sum,
  `assembly::InertiaAbout`) and the occurrences named each way (`printed`,
  `solid`); a printed body with no estimate makes it `{why}`. `total.mass_g`
  stays the solid sum, as a part's does (`print_job_smoke`).
- **What reads it**: the bill of materials and a drawing's parts list
  ([the BOM](../assemblies/README.md#bill-of-materials)), the clock's pendulum sums (its period is the printed
  pendulum's, the receipt and card saying at what), FEM's self-weight and
  modal mass (the printed share of the solid, spread evenly; the stiffness
  stays the solid material's, `src/core/fem/FemStudy.cpp`), and the study
  panel's self-weight seed (`ui::SimulationWeightN`). Motion studies are
  kinematic and weigh nothing.

### Print jobs

`export_print_job {path, plates: [{part, name?}], slicer_config?, name?}`
writes ONE 3MF Bambu Studio opens and slices (`src/core/io/
PrintJob3mf.{h,cpp}`, `ui::BuildPrintJob`, `src/remote/RemotePrint.cpp`).
A plate is a part whose bodies are the pieces where they lie on the bed --
x and y from its front-left corner, as a generator places them.

- **What it contains**: the 3MF core model with every PIECE its own object
  (a welded mesh at the print's chord deviation, never one merged mesh: a
  merged plate is one object whose extent reaches the keep-out corner, and
  Bambu then arranges it off the bed); Bambu's per-object settings in
  `Metadata/model_settings.config` (`printset::BambuObjectSettings`: the
  profile's `layer_height`, `wall_loops`, `top_shell_layers` and
  `bottom_shell_layers` with their thickness floors zeroed so the count is
  what prints -- the preset's 1 mm `top_shell_thickness` would raise two
  layers to five -- `sparse_infill_density`, `sparse_infill_pattern`,
  `enable_support` 0 and `brim_type` no_brim unless set, and with a brim
  its `brim_width`, `printset::kBrimWidthMm`, the width the
  [slender](#slender) item counts); one `<plate>`
  per plate, its objects placed at Bambu's plate grid (columns
  sqrt(n), one more when it rounds down, each plate a bed and a fifth
  on); and `Metadata/lobole_job.json`, the job as the answer gives it.
- **A project, or the model form.** Measured with Bambu's CLI (02.08):
  per-object settings are read from any 3MF, but PLATES are kept only in a
  Bambu project (the Application metadata and a
  `Metadata/project_settings.config`), and Bambu applies a project's
  config over its built-in defaults -- a partial one slices on a 200 mm bed
  with a generic start G-code, and one without the printer, process and
  filament ids crashes the CLI. Loböle has no slicer presets of its own, so
  `slicer_config` (a path to a JSON config, or the object) makes the job a
  project, written whole; `tools/bambu-config.py --printer ... --process
  ... --filament ... -o pla.json` makes one from your Bambu Studio's
  system presets (flattened through `inherits` and `include`, then a
  one-cube project built by the CLI). Without one a job is one plate, in
  the model form, which Bambu opens with your own presets.
- **One job is one spool.** Every piece's filament must be the same: a
  TPU plate is a job of its own (`jar_tpu_job.3mf`). On a P1S the spool is
  swapped between them anyway, and the CLI crashes slicing a second
  filament of a project, its own included.
- **Checked before anything is written** (a refusal writes nothing):
  every plate printed with a profile that states `bed_mm` and
  `layer_height_mm`; one printer and layer for the job; every piece on the
  bed and clear of the profile's keep-out zones (`printcheck::FitOnBed`,
  positional; on a bed without zones, every piece's box on the bed); every
  piece's merged settings valid and its filament resolved; more than one
  plate without `slicer_config`; and a config that lacks a key a project
  needs, is for another bed (`printable_area`), is lower than the profile
  (`printable_height`: bed_fit would pass what the slicer refuses), keeps
  out a zone the profile does not cover, or is for another filament.
- **The answer** names the `form` (project | model), the filament, the
  profile and layer, and per plate every object with its settings
  (`described`: "walls 3, top/bottom 5/5, 100 % rectilinear, no supports,
  no brim"), its `slicer` keys, its source part, and the plate's bed_fit.
- **Verified by slicing**: `print_job_smoke` exports a two-plate PLA job
  and a TPU job and, when Bambu Studio is installed
  (`$LOBOLE_BAMBU_STUDIO`, `bambu-studio` on PATH, or
  `~/.local/state/nix-roots/bambu-studio`), slices them with the CLI and
  reads every object's walls (the extrusion runs of its outer and inner
  walls at mid-height, under its G-code label), its infill (the sparse
  infill's E against its neighbour's) and its shells (the layers before
  sparse infill starts) back out of the G-code, with no brim and no
  support anywhere. Without the CLI it reads the 3MF back and says the
  slice was skipped.
- `generators/_lib.py`'s `export_print_job(d, plates, path, filament)`
  takes the config from `$LOBOLE_SLICER_CONFIGS/<filament>.json` and, for a
  job of several plates without one, writes nothing and says why in the
  receipt. The clock, the Strandbeest, the hinge box, the coupon, the jar,
  the engine, the counter and the planetary write their jobs beside their
  STLs.

### The coupon

`generators/print_coupon.py --port PORT [--size 8 --steps 2 --step 0.05]`
builds a printed part with bores and pins at the table's model sizes and
`steps` either side, an internal and an external custom thread of the size's
metric coarse pitch at the printed allowance, and a row of loose Ø`size`
discs standing in holes at the table's print-in-place `gap` and `steps` of
twice `step` either side (the same step on each side of the disc, labelled
as the gap a side). Each is engraved with its modelled size. It exports STL
(refused unless the discs come out as shells of their own), the project and
a JSON list into `scratch/print_coupon/`. Every size derives from `--size`,
and every number comes from `print_allowance`, so the coupon tests the
table the build carries. Print it, find the bore a Ø8 pin slides in, the pin
that slides in a Ø8 reamed hole and the tightest disc that pushes out by
hand. The material defaults to `pla`. When every disc comes free, or none
does, `--gap-only --gap-center <mm a side> --step <mm>` prints a second
coupon of the gap row alone, centred where the first one pointed.

Two rows of rings measure what a print-in-place design leans on. Every
ring stands **on the bed**, in a pocket through the plate, round a post of
the plate (Ø half the size); a rail a little wider
than the post bridges the pocket and ties the post to the plate, and a
45-degree head on the post flares out over the ring's inner edge. That head
is the one face over another part: self-supporting, as a design's should
be. Sized from `--size`: a ring Ø1.6 size and 0.25 size thick, its pocket
0.15 size clear of it all round, the head overlapping the ring's hole by
0.15 size; the part of the plate under the rows a quarter of the size
thick, so a fingertip reaches the rings.

- The **air row** puts N whole layers of vertical air under the head at the
  ring's inner edge (`--z-layers`, default 1, 2, 3 and 4, or four round the
  profile's `z_gap_mm` when it has one), the ring clear of its post by
  twice the profile's gap a side so only the air varies. The layers are the
  profile's `layer_height_mm`, else `--layer`; with neither the coupon
  refuses rather than assume one. Turn each ring: the fewest layers that
  turn cleanly are the air, which `--record --z-gap-layers N` keeps as
  `z_gap_mm`.
- The **small-gap row** is the same ring and post with the head at the air
  row's largest air, and the bore's gap a side stepped round the profile's
  (one `--step` under it, three over: −0.05 … +0.15 at the default). It
  measures the sideways gap at the post's size on the first layers, which
  a Ø`size` disc does not. `--record --post-gap-side MM` keeps the
  tightest that turned as a `gap` row from Ø0 to Ø`size`, on top of the
  profile, whose own gap carries on from Ø`size` (the disc row's size).

Each ring is labelled with its air or its gap a side in mm. The full coupon
carries both rows (a new printer needs every number); `--z-gap-only` prints
them alone on a plate a quarter of `size` thick — 100 × 40 mm at Ø8 — for a
profile whose other numbers are already measured:

    python3 generators/print_coupon.py --port PORT --z-gap-only \
        --profile "K5 PLA on P1S" --layer 0.2
    python3 generators/print_coupon.py --port PORT --record \
        --profile "K5 PLA on P1S" --layer 0.2 --z-gap-layers 3 \
        --post-gap-side 0.1

The coupon passes [`check_print`](#checking-a-print) itself, every item,
with `min_gap_mm` and `min_air_mm` set to the least the coupon holds on
purpose (its tightest disc and ring, its one-layer ring; a head comes
within its air/√2 of its ring's edge): every piece starts on the bed and
nothing is closer than it means to be. With a thread its overhang item is
reported, not required — a metric thread's flanks overhang past 45°.

- `--profile NAME` builds against one of your profiles and exports at its
  chord deviation; with a profile that states a bed and a layer it also
  writes `<name>_job.3mf`, the coupon's [print job](#print-jobs), from a
  plate part with a copy of the coupon in the middle of the bed (the coupon
  itself stands at the origin, in a P1S's keep-out corner, so its own
  check leaves bed_fit to the plate).
- `--record --profile NAME --gap-side MM --bore MM --pin MM --thread-went
  yes|no --z-gap-layers N --post-gap-side MM` keeps the answer: the
  winners become rows over the profile (or, for a new name, the built-in
  table of `--process`/`--material`), each `source` naming the coupon and
  its date, the air its `z_gap_mm` (N layers of the profile's layer height,
  or `--layer`, which it then states too) and the post's gap a `gap` row
  under Ø`size`, saved with `save_print_profile` at the deflection the
  coupon was exported at.
- `--seed` installs [the two measured profiles](#from-a-measurement) and
  `TPU 95A on P1S` (the built-in TPU rows), at
  the preset's 0.2 mm layers and with no Z gap measured, with the P1S's
  bed (256 x 256 x 250), its keep-out zone and the preset's 0.45 mm wall
  line width -- given, too, to a stored profile of either name that lacks
  them.

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
- **Print process** as a section of the Variables panel
  (`DrawPrintProcessSection`, `###print_process`): one combo — "Not
  printed", the user's profiles (`###print_prof_<name>`), then the built-in
  tables labelled "(default)" (`###print_fdm_petg`). Under it the match
  block, copied from the CAM setup's machine block: nothing when the same;
  amber "Modelled with a different '<name>' than yours." with `Use mine`
  (`###print_use_mine`) and `Save as mine` when it differs; "'<name>' is not
  one of your profiles." with `Save it` (`###print_save_mine`) when missing.
  With no profiles yet, a `Preferences › Print profiles` button
  (`###print_prefs`). Then the rows of the part's own copy with their sources
  as tooltips, the confirm-with-a-coupon note on a built-in table, and the
  part's threads. Then *Slicer settings*: walls, top and bottom layers and
  infill each stated (`###slicer_on_<field>`) or left to the preset, the
  pattern (`###slicer_pattern`) and brim (`###slicer_brim`) from the lists
  the RPC accepts, supports (`###slicer_supports`); a refused combination
  is said in red and not stored, and the bodies' overrides are counted.
- **Preferences › Print profiles**, a library page
  ([library pages](../ui/settings.md#library-pages), id prefix `prof`):
  name (a rename refused when taken), process and material, conditions, the
  STL chord deviation, the layer height and the air over another part
  (`z_gap_mm`), the bed, its keep-out zones (`###prof_zone_x0` ...,
  `###prof_zone_add`, `###prof_zone_remove`) and the wall line width
  (`###prof_line_width`), and the rows — kind, from, to (0 = and up), allowance
  and source — each written on every edit. A typed allowance is a
  measurement: its source becomes "coupon <date>". The tolerance editor
  says what a printed face prints at and what to model once a class is
  chosen, and the Thread editor shows the printed-thread report.

## RPCs

`set_tolerance {feature_id, element, class | lower+upper, b?, comment?,
part?}`, `list_tolerances {part?}`, `clear_tolerance {id, part?}`,
`check_fit`, `check_fits`, `stack_up`, `set_print_process {process, material
| profile | clear, part?}`, `get_print_process {part?}`, `print_allowance`,
`list_print_profiles`, `save_print_profile {profile | from_part, part?}`,
`delete_print_profile {name}`, `check_print {part?, bodies?, direction?,
max_overhang_deg?, overhang_allowance_mm2?, max_bridge_mm?, min_gap_mm?,
stl_deflection_mm?, min_air_mm?}` ([checking a print](#checking-a-print)) — all in `src/remote/RemoteInspection.cpp`;
`set_print_settings {part?, body?, clear?, walls?, top_layers?,
bottom_layers?, infill_pct?, infill_pattern?, supports?, brim?,
direction?}` (each key set, `null` unsetting it), `get_print_settings
{part?}` and `export_print_job {path, plates, slicer_config?, name?}` in
`src/remote/RemotePrint.cpp` ([slicer settings](#slicer-settings),
[print jobs](#print-jobs)); `export_stl` in `RemoteProject.cpp`. Tolerance edits are
undoable commands (`src/core/Command.h`). The edits take `part` (a name,
stable id or slot index) to act on **any** part, not only the active one —
a generator or an assembly tab records intent on the parts it placed without
switching tabs — and the edit lands in THAT part's undo history (the stashed
slot's own stack), so it undoes on its own tab like an edit made there.

## Tests

`tests/test_fits.cpp` (the tables, J and j cell for cell and their
refusals), `tests/test_tolerances.cpp` (storage,
replace-per-subject, zero rebuilds, undo, round-trip, sizes and distances
coexisting, stack signs and refusals), `tests/test_press_fits.cpp` (a dowel
pin at the top of H7/p6 relieved by exactly its annulus, 0.01 mm past it a
clash naming the excess, no band, one band, a clearance fit, a band on the
wrong side and a micrometre off-axis each a clash saying so, a collar's
overlap beside a fit still a clash, a printed bore read at what it prints, a
6005 at its K7 seat's interference, the same in a round plate with no
refusal from the plate's outside, a motion result keeping its press fits,
and golden `assembly/v159-press-fit` run as an exact motion study),
`tests/press_fit_smoke.py` (the K7 bearing a press fit in the named pair,
the exact sweep, `contact_report` and an exact motion study, the mesh modes
saying they relieve nothing, and an H7 seat a clash in each, the stored
motion run invalidated by the tolerance edit), `tests/tolerances_smoke.py` (RPC:
set/list/clear/undo, `check_fits` off mates including the sub-assembly walk),
`tests/drawing_smoke.py` (callouts), `tests/test_print_allowance.cpp` (the
table, its inverse, refusals, storage, the fit and thread sentences; a
profile through its file, each refusal, a part reading its embedded copy and
reporting same/differs/missing, a profile's hole row rebuilding a fitted
clearance hole, and the layer height and Z gap round-tripped and refused
off whole layers; "A press is a cap's bore less its pin", the built-in
default, a profile without a press row reading it and saying so, a
measured one), goldens `v169-print-profile`, `v175-print-z-gap` and
`v178-press-allowance`,
`tests/print_allowance_smoke.py` (check_fit, check_fits and a Thread on a
printed part, and the advised custom thread printing the right major; the
profile store's RPCs and refusals, a disc in a hole exported at the
profile's chords with the gap in the STL within 0.005 mm of the model, a
coarse deflection warned, the match after the store changes, a Z gap off
whole layers refused, `print_coupon --record` (the Z gap too) and
`--seed`, `check_print`'s minimum air taken from a profile's `z_gap_mm`
and else from three of its layers, and the press: the built-in default, a
profile without a row answering `from: builtin`, `--record --press-side`
and the `--press-only` coupon printing every item),
`tests/test_print_allowance.cpp` "A printed clearance hole is modelled at its
fit plus the allowance", `tests/test_print_check.cpp` (a box on and off the
bed, a 45° funnel reading 45 and passing and a 60° one failing with its
area, a slot's ceiling spanning its inscribed 10 mm, two boxes keeping the
gap, too close, touching and overlapping, one body of loose solids, chords
past a fifth of the gap, another print direction; and, tagged
`[printcheck]`, stacking: a block one layer over another failing as a piece
off the bed and as 100 mm² at 0.2 mm of air, side by side passing; a ring
round a post under a 45° head passing at three layers and failing at one
with the cone's area over the ring; a body's own bridge over its own floor
not counted; no minimum air skipping the item and one not over 0 refused;
the limit from a profile's `z_gap_mm`, its layers, FDM's default, and none
for a process that is not FDM; and at a face's boundary, a cone pin's air
read exact up to its top edge, the air over a hollow at the limit read as
the limit and not its chord, and a ray past a top's polygon reading the
top and not the flank beyond),
`tests/test_print_job.cpp` (`[print]`: the settings refused where the
slicer refuses them and an override over its part, the slicer's keys, a
printed cube against its shell and core by hand -- centre, the solid
inertia, the same cube printed on its side -- a printed mass naming what it
lacks, bed_fit's keep-out zones, and the 3MF's entries, plate grid and
zip), golden `v181-print-settings`, `tests/print_job_smoke.py` (the
settings' refusals changing nothing, the printed masses, a two-plate job
read back, the refusals writing nothing, and the Bambu CLI slice, above),
`tests/print_check_smoke.py` (the same negative controls over RPC — a 60°
overhang, a fused pair, a near pair, a part off the bed, a block one layer
over another failing stacking off the bed and under the air, still lifted
under a stated `min_air_mm` below its air — the limits from the print, an
unprinted part's skipped items stacking among them, and the refusals,
`min_air_mm` among them), `tests/tolerances_smoke.py` (set_tolerance on a part
that is not the active one), GUI
`tolerance/print_process_from_the_panel`,
`tolerance/print_profiles_are_kept_in_preferences`,
`tolerance/print_profile_picked_and_matched`,
`tolerance/printed_stl_export_says_its_chords`,
`tolerance/print_check_from_the_panel`, `tolerance/a_measured_bore_gets_a_class`,
`tolerance/a_plane_is_told_what_to_do`, `tolerance/the_panel_lists_and_clears`,
`tolerance/check_fits_from_the_panel`. The generators run as tests
([the big generators as tests](../../generators/README.md#the-big-generators-as-tests)) keep the checks
honest on real parts: `generator_hinge_box` passes every `check_print`
item, stacking included, swings the lid 0-180° clean onto the rim, and
fails stacking on both negative controls (half the cone's air, the lid a
layer off the bed); `generator_counter` and `generator_planetary` pass
every item too, on K5 with its measured air.

## Not built

- In `check_print`: a ceiling over another piece also reads as a bridge
  (stacking is what fails it); a bridge anchored on two edges only reads
  its inscribed diameter, not its length; no support or island detection
  within a piece (a piece that starts off the bed is stacking's); no wall
  thickness; no overhang shading in the viewport.
- `check_print`'s bridges, stacking and slender on a mesh body
  ([measuring one](../inspect/README.md#mesh-bodies)): each reads the
  B-Rep's surfaces (bridges a UV grid classified against the face, stacking
  rays refined on the exact surface, slender a footprint of the downward
  planar faces), so each needs a version that reads triangles, and is
  skipped saying so until then.
- In a print job: a second filament in one project (the CLI cannot slice
  it), supports' own parameters (only on and off) and a brim's other than
  its width (which is the slender item's 5 mm), the
  slicer's per-object modifiers (a region of a part at another infill).
- An inferred stack-up chain, and correlation between terms (needs tracking
  from measured geometry back to the variables that drove it).
- Statistical stack-up; geometric (form/position) tolerances.
- A thread tolerance class driving modelled geometry: `Thread.thread_class`
  aims the thread mill and sets the verifier's band
  ([pitch-diameter bands](../modeling/holes-and-threads.md#pitch-diameter-bands)),
  and a modelled groove is the basic profile.
- A vendored general-tolerance table (ISO 2768) to seed a distance's band.
