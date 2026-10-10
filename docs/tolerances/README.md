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
  nothing.
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

| fdm, pla | size | allowance | source |
|---|---|---|---|
| hole | any | +0.2 | 3dprintbounty.com tolerances guide; PLA shrinks 0.3-0.5 %, less than PETG, so PETG's step at Ø10 is not carried |
| shaft | any | −0.1 | as above: an outside diameter grows by about a tenth |
| internal thread | any | +0.4 | as for PETG (the Fasteners and BOSL2 numbers are not per material) |
| external thread | any | −0.1 | as for PETG |
| gap | any | +0.5 | **a default to calibrate**: sovol3d.com print-in-place guide (0.15-0.35 a side by printer and filament); makerworld.com clearance tests on Bambu printers, 0.25 a side best. One P1S wanted far less ([from a measurement](#from-a-measurement)) |

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
  name and an embedded copy (v168, [below](#print-profiles)) — outside the
  regen chain like a tolerance or the tube shop. A part is made one way, whichever machine
  or tab opens it; a body's material is its appearance and density, and the
  same PETG is printed or moulded with different allowances, so it is not the
  material. It is not a preference, because a part printed on one machine is
  printed on every one that opens it. `set_print_process` is an undoable
  command that rebuilds nothing.
- **Refused, never substituted.** An unknown process names the processes; a
  material the process has no data for names the ones it has. The same
  refusal on load, and in `check_fit`'s `print`.
- **Drives one geometry: a Hole's clearance fit.** A Hole with a fit
  (`M6 normal`, [../modeling/holes-and-threads.md](../modeling/holes-and-threads.md#hole))
  names the size the hole must come out at, so on a printed part it is
  modelled at that size plus the hole allowance: M6 normal is Ø6.6 in steel
  and a Ø6.8 model in PETG. A tapped hole stays at its tap drill (what a
  printed thread wants is the thread report's to say). The regen hash folds
  the process — `printfit::Fingerprint`, the pair, the profile's name and
  every row's range and allowance — into fitted clearance holes only, so
  setting it, or taking a profile whose hole row changed, rebuilds those and
  what follows them, and nothing on a part without one. The requirements
  hash folds the same fingerprint, so a profile edit restales a printed
  fit's verdict. Everywhere else
  the generator applies it where it models the part, and the checks say what
  to model.

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
  major). It notes a cosmetic thread prints nothing, and that printed threads
  are unreliable below M8 (tap a plain bore at the tap drill, or use an
  insert). `add_feature` and `modify_feature` on a Thread return it as
  `print`; `set_print_process` and `get_print_process` list every thread.
- **`print_allowance`** answers one diameter — `{kind: hole|shaft|
  internal_thread|external_thread|gap, size_mm | designation}` against the
  part's process, a stated `{process, material}` or `{profile}` — with
  `model_mm` and what the STL export takes off a round surface: its chords
  sit up to `deflection` inside it, so a hole can lose twice that on the
  diameter (`printfit::ChordLossMm`). Without `stl_deflection_mm` it is the
  deflection `export_stl` meshes this print at ([exporting a printed
  part](#exporting-a-printed-part)). The table came through somebody's
  export: export the parts and the coupon at the same deflection and the
  coupon calibrates both.
- **A profile that is not the store's** says so: `check_fit`'s
  `print_notes` add that the part was modelled with a different copy, or one
  the store does not have, and that the answer is the part's copy.

### Print profiles

A **print profile** (`printfit::Profile {name, table, stlDeflectionMm}`) is a
named, user-owned copy of a table: one printer, one filament, one slicer
preset. The CAM machine store is the pattern
([machine profiles](../cam/setup.md#machine-profiles)).

- **The store** is one file per profile, `~/.config/lobole/print_profiles/
  <slug>.json` (`DocumentIO::LoadPrintProfiles`/`SavePrintProfile`/
  `DeletePrintProfile`, `src/core/io/SerializePrintProfiles.cpp`), mirrored
  in `state.printProfiles` by `ui::ReloadPrintProfiles`
  (`src/ui/PrintProfileActions.cpp`) at startup, headless too, and after
  every save and delete. A broken file is reported and the rest still load.
- **A save refuses** (`printfit::ValidateProfile`) a missing name, two names
  that fold to one file, a base process and material the built-in tables do
  not carry, no rows, a size range that is empty, overlapping or out of order
  within its kind, and an allowance of the wrong sign: a hole or internal
  thread is not negative, a shaft or external thread not positive, and a gap
  is positive (a gap of zero fuses). A refusal writes nothing.
- **The built-in tables stay**, read-only, as what a profile starts from: a
  new profile is the built-in table of its pair, "the defaults, corrected".
- **The document carries its numbers.** A part printed with a profile has
  `print {process, material, profile, conditions, stl_deflection_mm?, rows}`:
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
`python3 generators/print_coupon.py --port PORT --seed` saves both through
`save_print_profile` on the instance at PORT — into whatever config dir that
instance was started with — and leaves any name already stored. The other
rows are the built-in PLA rows.

### Exporting a printed part

A slicer's gap closing shuts what an STL's chords have already narrowed:
on the coupon, `export_stl`'s 0.1 mm took 0.03 mm a side off every gap and
the slicer closed the two narrowest. So `ui::ExportPartStl`
(`src/ui/StlExport.cpp`), the one helper `export_stl` and File › Export
both call, picks the chord deviation:

- **A printed part with no deflection given** meshes at
  `printfit::StlDeflectionMm`: the profile's `stl_deflection_mm`, else a
  tenth of its smallest gap a side (0.025 on the built-in PLA table). An
  unprinted part keeps 0.1 mm.
- **A coarser deflection asked for** is exported as asked, and the reply's
  `warning` says how much of the smallest gap the chords can take. The GUI
  never passes one, so it never warns.
- `CADKernel::SaveSTL` takes the angular deflection explicitly (0.5 rad,
  OCC's own): a chord meets both bounds, which is why the linear one is an
  upper bound on what it takes.
- File › Export says what it did in a viewport pill: "Exported part.stl —
  chords 0.01 mm, 10% of the 0.1 mm gap a side", or the failure, amber.

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

- `--profile NAME` builds against one of your profiles and exports at its
  chord deviation.
- `--record --profile NAME --gap-side MM --bore MM --pin MM --thread-went
  yes|no` keeps the answer: the winners become rows over the profile (or,
  for a new name, the built-in table of `--process`/`--material`), each
  `source` naming the coupon and its date, saved with `save_print_profile`
  at the deflection the coupon was exported at.
- `--seed` installs [the two measured profiles](#from-a-measurement).

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
  part's threads.
- **Preferences › Print profiles**, a library page
  ([library pages](../ui/settings.md#library-pages), id prefix `prof`):
  name (a rename refused when taken), process and material, conditions, the
  STL chord deviation and the rows — kind, from, to (0 = and up), allowance
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
`delete_print_profile {name}` — all in `src/remote/RemoteInspection.cpp`;
`export_stl` in `RemoteProject.cpp`. Tolerance edits are
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
reporting same/differs/missing, and a profile's hole row rebuilding a fitted
clearance hole), golden `v168-print-profile`,
`tests/print_allowance_smoke.py` (check_fit, check_fits and a Thread on a
printed part, and the advised custom thread printing the right major; the
profile store's RPCs and refusals, a disc in a hole exported at the
profile's chords with the gap in the STL within 0.005 mm of the model, a
coarse deflection warned, the match after the store changes, and
`print_coupon --record` and `--seed`),
`tests/test_print_allowance.cpp` "A printed clearance hole is modelled at its
fit plus the allowance", `tests/tolerances_smoke.py` (set_tolerance on a part
that is not the active one), GUI
`tolerance/print_process_from_the_panel`,
`tolerance/print_profiles_are_kept_in_preferences`,
`tolerance/print_profile_picked_and_matched`,
`tolerance/printed_stl_export_says_its_chords`, `tolerance/a_measured_bore_gets_a_class`,
`tolerance/a_plane_is_told_what_to_do`, `tolerance/the_panel_lists_and_clears`,
`tolerance/check_fits_from_the_panel`.

## Not built

- An inferred stack-up chain, and correlation between terms (needs tracking
  from measured geometry back to the variables that drove it).
- Statistical stack-up; geometric (form/position) tolerances.
- A thread tolerance class driving modelled geometry: `Thread.thread_class`
  aims the thread mill and sets the verifier's band
  ([pitch-diameter bands](../modeling/holes-and-threads.md#pitch-diameter-bands)),
  and a modelled groove is the basic profile.
- A vendored general-tolerance table (ISO 2768) to seed a distance's band.
