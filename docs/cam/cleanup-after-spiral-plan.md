# CAM cleanup after the spiral

Four tidy-ups the continuous 4-axis round (rotary-continuous-plan.md) left
visible. No format change, and no change to any toolpath, file or number.
Each one either closes a class of silent bug or removes a duplicate.

## 1. Every operation field round-trips

A field on `cam::Operation` touches about seven places: the struct, the
capability row, the serializer's write and read, the RPC's owned-field list,
its apply step and its answer, the panel, and the surface key. The capability
test (`test_cam_capability.cpp`) checks only the GENERATOR side. Nothing
checks that a field survives a save and reload, or `set_cam_op` followed by a
read. Adaptive's `stepover` was already lost on every save once, found by
accident.

- **Save and reload** (C++, `test_cam_capability.cpp`): an op of each type,
  with EVERY field poked, not only the ones the type declares (a file keeps
  what it was given, so a type change and back loses nothing), plus name,
  depths, enabled and a target, goes through `CamSetupToJson` →
  `CamSetupFromJson` and must come back `==`. `Operation` gets a defaulted
  `operator==`, so a new member is compared without anyone listing it. A new
  `OpField` bit without a poke fails the test by name, so a new field cannot
  arrive untested.
- **Through the RPC** (`tests/headless_smoke.py`, CAM builds only): for each
  field an op REPORTS, set a different value with `set_cam_op` and require
  `list_cam_setups` to report it back. The fields come from the op's own
  answer, so the check grows with the table. The other direction comes from
  the refusal for a field the type lacks, which lists what it DOES take:
  every one of those must be reported.

## 2. One definition of a turning cut's feed and time

Four places walk moves and decide "is this a cut that turns, and what F does
it get": the post, `CuttingMinutes`, the timeline and the verdict's cap
warning. They agree because they call the same leaf functions, but each makes
its own choices (does a plunge count, which move is "the one before"). The
rule moves into `CamRotaryFeed`, where a segment gets one answer:

- `PlanFeed(segment, …)`: for a PLANNED move (a toolpath at a tip feed), the
  F to post, whether it was held back, the A rate and the minutes.
- `PostedMinutes(segment, …)`: for a POSTED move whose F is already scaled,
  the minutes the controller takes.

The distinction is the point: a timeline reads a file whose F was already
scaled, so dividing again by the scaled feed would count the scaling twice.

## 3. The finishing generators leave `CamStrategies.cpp`

At 1,609 lines it is the largest file in the CAM core, and the two surfacing
generators (planar and rotary) share nothing with the Clipper-based 2.5D
strategies around them. They move, with their helpers, to
`CamSurfaceStrategies.cpp`. `test_conventions`' surface-key gate reads those
functions by path and moves with them.

## 4. One rotary test part

The necked shaft is built in `test_cam_rotary`, `test_cam_spiral` and
`test_cam_capability`. It moves to `cam_fixtures.h` as `NeckedShaftDoc()`.
The GUI test builds it through `CamPart()` and stays as it is.

## Verified by

The headless, GUI and pygui suites, and the CAM-off build. Beyond that,
posting the handle and the test shaft before and after must give
byte-identical G-code, and both new round-trip checks must fail when one
serializer line is removed.
