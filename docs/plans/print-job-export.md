# A print job, exported

## The problem

Every printed dogfood so far reached the printer by hand. For each plate,
the STLs were loaded into Bambu Studio's CLI, the 3MF was unzipped, and its
settings were edited:

- walls 3 for the Strandbeest's standing pins;
- 100 % infill for the clock's pendulum, which also needed a different
  infill pattern, because Bambu refuses grid at 100 %;
- TPU settings, one wall and 15 % gyroid for the jar's gasket;
- no brim and no supports everywhere.

Those settings are part of the design: the pendulum's period was computed
assuming solid plastic, so printing it at the slicer's default 15 % would
silently change it. Nothing in Loböle holds them, and `mass_properties`
assumes every printed part is solid.

## The shape of the fix

### 1. A part's print settings live in the document

A part's print process gains slicer settings:

- the filament (by its process material);
- walls;
- top and bottom layers;
- infill density and pattern;
- supports (default off) and brim (default none).

A body can override the part's settings. The values are checked when they
are set:

- a pattern the slicer cannot print at a density is refused (grid at
  100 %);
- an unknown pattern is refused, naming the patterns that exist.

The print profile's `layer_height_mm` stays the layer. This is a format
change (`vNEXT`).

### 2. Printed mass

For a printed part, `mass_properties` reports a printed mass, centre and
inertia beside the solid ones.

- **The shell** is the body's surface offset inward by the walls
  (horizontally) and the top/bottom layers (vertically, along the print
  direction). It is solid.
- **The core** inside it is weighted by the infill density.
- The answer names the method and that it is an estimate: the slicer's
  real paths differ at small features.

A test: a cube at 15 % against its hand-computed shell plus core. Motion
studies, the clock's pendulum sums and FEM's self-weight use the printed
values when a part is printed. The clock's receipt then states the period
at the infill its pendulum is set to, and refuses a pendulum whose parts
differ in infill unless the period has been computed for them.

### 3. `export_print_job`

A new RPC writes one 3MF per job, ready to open or slice in Bambu Studio.

- **Input:**
  - plates, each a list of bodies with their placement on the bed;
  - per-object settings from section 1;
  - the profile's layer;
  - no brim and no supports unless set.
- **Output:**
  - the 3MF core model;
  - Bambu's per-object metadata (`Metadata/model_settings.config`);
  - one plate per plate.
- **Checks before writing:**
  - every plate passes `check_print`'s bed fit (the bed from the profile);
  - a body that has no print process is refused.
- **Verified by slicing it:** a test with the Bambu Studio CLI, when one is
  installed, slices the exported job and asserts that each object's walls
  and infill reached the G-code. Without the CLI, the test reads the 3MF
  back and checks the metadata, and says the slice was skipped.

`_lib.py` gains `export_print_job(d, plates)`. The clock, the Strandbeest,
the hinge box, the coupon and the jar (whose gasket is TPU, on its own
plate) each write their job beside their STLs.

## Files

- `src/core/PrintAllowance.{h,cpp}` or the part's print process (settings),
  serialization, a golden.
- `src/core/MassProperties*` / `src/core/Measure*.cpp` (printed mass).
- A new `src/core/io/PrintJob3mf.{h,cpp}`; `src/remote/RemoteInspection.cpp`
  or an export module (`export_print_job`, MethodDoc rows).
- The Print process section and the Settings window (the settings).
- `generators/_lib.py`, `generators/{clock,strandbeest,hinge_box,jar}/`,
  `generators/print_coupon.py`.
- Tests, smokes, docs/tolerances/README.md (printed parts), and
  docs/scripting rows.

## Verified by

- **Unit tests:** the settings refused and accepted; printed mass against
  the hand sum; the 3MF's metadata read back.
- **A smoke** that exports a two-plate job, and slices it when the Bambu CLI
  is present.
- **The five generators** write jobs, and the clock's period is stated at
  its infill.
- **The headless, GUI and generators suites.**
