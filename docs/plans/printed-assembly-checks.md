# Checks for assembled printed parts

## The problem

The Strandbeest was the first print dogfood built from parts assembled
after printing: pins, pressed caps, a keyed crankshaft, four plates. It
passed every check only after working around four things the app could
not say.

- **A printed press fit reads as a clash.** The exact contact check
  already relieves an intended interference between a bore and a shaft,
  but only when both faces carry a size band (an ISO class or explicit
  deviations). A printed cap pressed on a printed pin carries no band, and
  an ISO band is the wrong size for it anyway: p6 at Ø3 allows about
  0.01 mm, while a printed press is the profile's measured `press`
  allowance. So the walker built every pin and cap as one occurrence.
- **A motion study can jump to the mirror branch without a word.** Each
  sample seeds from the last. At 45° a step, the Jansen leg converged on its
  mirror pose with zero residual, so the study reported it as solved. Only
  the generator's own knee-sign check saw it.
- **Nothing knows the bed.** A print profile has no bed size, `check_print`
  has no item for a plate that does not fit, and the walker brought its own
  packer and a P1S constant.
- **Two small gaps:**
  - `mass_properties` gives each body's box but no box for the whole
    selection;
  - a `batch` call's result does not carry the id a call inside it created
    (an `add_mate`), so the walker looked its mates up again with
    `list_mates`.

## The shape of the fix

### 1. A printed press fit has a band

A size tolerance gains a printed press class. It is set on the two faces of
a printed press, the bore and the shaft, as an ISO class is set today. Its
band comes from the part's print profile:

- the bore's band is the profile's `press` allowance (on the diameter),
  give or take a tenth of the profile's gap, the same tenth the clearance
  item allows;
- the shaft's is zero, give or take the same tenth.

So `fitband::Stamp` stamps printed faces from the profile, and the relief
in `Measure::ContactBetween` works unchanged. It reports the fit as
`"Ø<d> printed press <interference> (<profile>)"`. A printed press class on
a part with no print process, or on a process that is not FDM, is refused
when it is set, naming why. An interference past the band is still a clash,
with `press_fit_refused` saying by how much, as for an ISO fit.

The Strandbeest goes back to a pin and a cap as separate occurrences on a
fastened mate. Its clash study runs exact, and it reports the pressed pairs
as `press_fits`.

### 2. A motion study does not jump branches

`MotionStudy` watches every free joint coordinate between samples. When
one moves more than a set multiple of its largest move over the previous
samples, while the driver moved one step, the study re-solves that interval
at half the step, seeded from the earlier sample, and keeps halving.

- **The flip goes away:** it was a step too large for the seed. The
  sample's result is the continued pose. The run counts the subdivisions
  (`refined_samples`).
- **The jump stays at the smallest step:** the linkage really does pass
  through a singular pose. The sample is flagged `branch_jump`, naming the
  mate and the jump. A study with a jump does not pass, as an unsolved
  sample does not.

`max_refine` (default from the driver's step: down to a 64th of it) bounds
the cost. The Jansen leg at 45° a step must come back on its branch. A
four-bar driven through its dead point must report `branch_jump`.

### 3. The bed is part of the print profile

- A print profile gains `bed_mm` (x, y, z): the printable volume.
  `--seed`'s P1S profiles carry 256 × 256 × 256.
- `check_print` gains an item, `bed_fit`: every body of the plate lies
  inside it, with the shortfall per axis.
- A profile without `bed_mm` skips the item and says so. Nothing guesses a
  printer.
- `_lib.py`'s `PRINT_ITEMS` gains it.
- The walker and the hinge box read the bed from the profile instead of a
  constant. The walker keeps its packer, which now packs into the
  profile's bed.

This is a format change for the profile (`vNEXT`).

### 4. The two small ones

- `mass_properties` gains `bbox_mm` for everything it measured, beside the
  per-body boxes.
- A `batch` result carries each inner call's created id at the top level of
  that call's result, as the same call made alone does.

## Files

- `src/core/FitBands.{h,cpp}`, the tolerance model (`core/Feature.h`'s
  size tolerance and its serialization), `src/core/PrintAllowance.{h,cpp}`,
  `src/core/Measure*.cpp` (the report string only).
- `src/core/MotionStudy.{h,cpp}`, `src/remote/RemoteMotion.cpp`.
- `src/core/PrintCheck.{h,cpp}`, `src/ui/PrintCheckActions.cpp` and the
  Print process section, the Settings window (bed), the RPC rows.
- `src/remote/RemoteInspection.cpp` (`mass_properties`), the batch
  dispatcher.
- `generators/_lib.py`, `generators/strandbeest/`,
  `generators/hinge_box/build.py`, `generators/print_coupon.py`
  (`--seed`).
- Tests: `test_fit_bands*`/`test_contact*`, `test_motion.cpp`,
  `test_print_check.cpp`, `test_print_allowance.cpp`, the smokes for each
  RPC, a golden for the format change.
- Docs: docs/tolerances/README.md (press fits, print profiles, checking a
  print), docs/assemblies/README.md (motion studies), docs/assemblies/solver.md
  (branches), the RPC rows.

## Verified by

- **Unit tests:**
  - a printed cap on a printed pin at the profile's press is relieved and
    reported, and past the band it is refused with `excess_mm`;
  - a printed press on a part with no process is refused when it is set;
  - the Jansen leg at 45° a step stays on its branch, with
    `refined_samples` > 0;
  - a four-bar through its dead point reports `branch_jump`;
  - a plate over the bed fails `bed_fit` with its shortfall, and a profile
    without a bed skips it;
  - `mass_properties`'s `bbox_mm`;
  - a batch's created ids.
- **Generators:** the Strandbeest with separate pins and caps passes the
  exact clash study and reports its press fits; the hinge box reads its bed
  from the profile.
- **Suites:** headless, GUI, and the generators suite.
