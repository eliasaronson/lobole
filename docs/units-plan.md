# Units — what we have, and what a unit engine would not buy

> **Status: SHIPPED 2026-09-10** (2bf40d64, 136a9df9, 707c15de, 9b212e98,
> f0134d5d).
> No format bump: every twin key is input-only.

## The problem

The app has a display layer (`core/Units.h`), unit-suffixed expression
literals, and a naming convention (`*_deg`, `*_mm`). It has no dimensional
analysis and no quantity types, and the question that started this round was
whether to adopt a full unit engine (mp-units, au, boost::units).

The answer is no, and the reason is where the bugs are. Every unit defect this
project has shipped sits at a BOUNDARY — a value crossing between JSON and a
struct field:

- `set_mate(rotation=60)` pinned a mate to 60 radians and was diagnosed as a
  loop-solver branch bug twice.
- `end_angle: 90` swept 116.6 degrees: a plausible arc and the wrong part.
- `add_load(magnitude=…)` named no unit; every FEM number scales linearly
  with it and a wrong guess is invisible in the answer.
- An A2 sheet loading as A4, placing every view against different paper.

A `quantity<degree>` type does nothing at that boundary: a JSON number becomes
a typed quantity by fiat exactly as it does now. Dimensional analysis checks
arithmetic written in C++, and none of these were arithmetic errors. Against
that, a quantity type would have to unwrap at every one of the ~4300 glm
vector uses and every OCC `gp_*` call, and the `.value()` escape hatch is
where the errors would relocate to. Feature angle fields are additionally
bound to expressions through `visit(angleExpr, angleDeg)`, which takes
`double&`, so typing them cascades into `VisitExprs` and the whole variables
system.

The codebase already solved this exact problem class once. `core/Conventions.h`
replaced four subsystems' independent up-axis assumptions with one header of
named constants plus `tests/test_world_up.cpp`, which fails when a consumer
drifts. Not a type system. Angles are in the pre-`Conventions.h` state, and
get the same treatment.

## What to build

### 1. `core/Angles.h` — the factor, once

Four independent definitions of degrees-per-radian (`Expression.cpp`,
`SerializeFeatures.cpp`, `RemoteAssembly.cpp`, `RemoteMotion.cpp`) and eleven
more sites spelling `180.0 / M_PI` inline. One header states it, and
`ToRadians`/`ToDegrees` say which direction a conversion goes at the call
site — a bare factor reads the same both ways. `IsPlausibleSweep` is the
geometry-based check the arc parser already relies on, shared.

A `test_conventions.cpp` case fails on a new bare literal.
`glm::radians`/`glm::degrees` are deliberately not matched: they name their
direction too.

### 2. The four live bugs

- **`pattern.angle`** — the MCP layer called it "the total sweep in degrees"
  and forwarded it unconverted to a handler reading radians PER STEP. Two
  errors in one number. Gains an `angle_deg` twin; the row and the MCP
  description say per-step.
- **`set_reference_image.rotation`** — radians, no twin, unit named nowhere in
  its `MethodDoc` row. Same shape as `set_mate` before it was fixed.
- **`add_constraint.value`** — DEGREES through this RPC and RADIANS through
  the same field of the same sketch reached via `add_feature`
  (`ConstraintFromJson`), with `get_state` echoing radians back: set 45, read
  0.785, both "right". The bare name becomes radians on both paths, matching
  the model, the file, and the arc/mate precedent; `value_deg` says degrees.
  `expr` is NOT part of the swap — it stays on the expression language's angle
  base, which is degrees, as it is for every other `*_expr`. Both are stated
  in the row and in a new `constraint_value_units` map in `describe`.
- **The copied measurement** — `MeasuredValueForClipboard` converted to the
  reader's DISPLAY unit and emitted a bare number, while a dimension field
  reads a bare number as STORAGE. On an inch document, copying 25.4 mm gave
  "1" and pasting it meant 1 mm. It now carries its unit.

Every twin refuses both spellings at once, and refuses a bare radians value
over a full turn, naming degrees as the likely cause — so a caller who was
passing degrees gets a refusal instead of a plausible wrong part.

### 3. Drawings honour the document's units

Drawing dimension text goes through `core::FormatLength`, a second formatter
that always prints millimetres, and the title block has no UNITS row. An inch
document — which `set_units` fully supports — produces a sheet of bare numbers
that are actually mm and never says so. `DimContext` gains the unit system
(one construction site); the title block gains a row. Export already takes
pre-resolved text, so the fix flows through to SVG and PDF.

The title block on SCREEN needs it too, and did not get it first time: a UNITS
row in the exported PDF alone is the more dangerous half of the disagreement,
because the sheet you check is then the one that does not say. `core::UnitsLabel`
writes the row and `ui::ActiveDrawingUnits` answers which system the sheet is
in, both used by the panel and the two exporters.

### 4. A `describe` unit gate

Forty-two dimensional parameters name no unit anywhere in their row.
`tests/rpc_units_gate.py` requires a dimensional key to carry a unit suffix or
have its unit named in the row, reusing the MethodDoc parsers lifted out of
`rpc_params_gate.py` into `tools/method_doc_scan.py`. This is the part that
generalizes: it catches the next row rather than the current forty-two.

## Verification

`tools/test-fast.sh` per stage; the full `ctest -LE gui`,
`./tools/gui-tests-xvfb.sh` and the python gates before merge. The angle twins
are covered by `tests/remote_smoke.py` (both spellings, both-at-once refused,
a degrees value in the radians field refused); the drawing units by
`tests/test_drawing.cpp`.

## Not in scope

GLB export applies no scale, so parts land 1000x large in a glTF viewer whose
unit is metres; STL import has no scale option. Both real, both unrelated to
the degrees/radians work.
