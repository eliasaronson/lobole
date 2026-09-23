# Conventions

The rules every subsystem follows, and the gate that fails when one drifts.
Each exists because a subsystem that decides the question for itself gets it
wrong in a way no local test sees. How the code is organised is in
[architecture.md](architecture.md).

## World axes

**The world is Z-up, the front of a part faces −Y, and right is +X** — the
frame of G-code's spindle, drawing practice, and every CAD and DCC tool the
app exchanges files with.

- `core/Conventions.h` states it once: `conv::kWorldUp`, `kWorldFront`,
  `kWorldRight` (a right-handed frame: right × up = front), their float
  twins, and `kGravity = −9.81 · kWorldUp`.
- **`conv::kNamedViews`** is the one named-view table (front, back, right,
  left, top, bottom, iso): the `view.*` commands, the `view` RPC and
  `screenshot(view=)`, the view cube's labels and clicks, and the drawings'
  `OrientationAxes` all read it. Top and bottom roll so the part's front edge
  sits at the bottom of the screen or sheet, which puts a drawing's top view
  directly above its front view. `FindView` and `ViewForDirection` look it up.
- The studio light rig (`kKeyLightDir`, `kFillLightDir`, `kRimLightDir` and
  their colours) is shared by the raster renderer, the shadow map, the ray
  tracer and the CAM stock renderer.
- Anything that means "up" — a named view, a plane name, a gravity or load
  default, a camera up vector, a light — derives from these constants. A
  literal `(0,1,0)` elsewhere is a claim that the value is *not* world up (a
  local basis, a UV axis, a face normal).
- Base planes are named from the frame: XY is the Top plane, XZ the Front,
  YZ the Right (`cad/SketchPlane.cpp`).
- CAM's setup space is Z-up by machine-tool convention (ISO 841); for a plain
  3-axis setup it is the identity on the world, so CAM needs no compensating
  rotation.
- At the file boundaries: glTF/GLB export converts to glTF's Y-up and metres
  (`RWMesh_CoordinateSystemConverter` in `cad/MeshExport.cpp`); STEP, STL,
  BREP, OBJ and PLY are written in world coordinates. The Onshape importer
  (`tools/onshape_import.py`) keeps Onshape's raw plane geometry, so an
  imported model keeps its source orientation.

**Gate: `tests/test_world_up.cpp`.** It calls the real consumers and checks
them against the constants: the basis is orthonormal and right-handed, the
named views derive from it, `OrientationAxes` *is* the named-view table,
base-plane names follow the frame, gravity and the default FEM load point
down, the measure tool's facing words ("top", "side") follow world up, CAM's
setup frame is the world frame, and the default camera stands upright. A
drawing GUI test pins what the named views show (the front of a plate
80 × 8 × 60 is 80 × 8, its top 80 × 60), and the smokes assert the front
camera sits toward −Y with top on +Z.

## Units and angles

**Storage is millimetres, grams and degrees**, everywhere a value is stored
or crosses the JSON boundary. `core/Units.h` is a presentation layer only: a
document carries its display system (length, mass, angle, each chosen
independently; `set_units`/`get_units`), and a unit change never moves a
vertex. Drawings print in the document's units and say so in the title block.

Expressions accept unit-suffixed literals (`10 mm + 1 in`, `0.5in`,
`30 deg`); a suffix converts to storage units at parse time. There is no
dimensional analysis: after conversion the algebra is plain numbers.

**Angles are radians inside geometry and degrees at the boundary.** The file
format, the RPC surface and the expression language speak degrees; the sketch
solver and mate frames speak radians. Rules:

- The factor is stated once, in `core/Angles.h`: `angles::ToRadians`,
  `angles::ToDegrees`, `kDegPerRad`, `kRadPerDeg`. A bare `180.0 / M_PI`
  elsewhere is a claim that the value is not an angle conversion.
  (`glm::radians`/`glm::degrees` are allowed: they name their direction.)
- A field or key that holds degrees ends in `Deg`/`_deg`.
- Where an RPC parameter has a bare radians form (a mate's `rotation` on
  `add_mate`/`set_mate` and in configuration overrides, an `add_constraint`
  angle `value`, the sketch `pattern`'s per-step `angle`,
  `set_reference_image`'s `rotation`), it has a `_deg` twin. Passing both is
  refused, and a bare radians value over a full turn is refused naming
  degrees as the likely cause (`angles::IsPlausibleSweep`: more than a turn is
  a circle, not a sweep).
- Relations between mates speak radians on purpose: a relation's `phase` is
  in the driven mate's unit (rad or mm) and a rack's ratio is mm per radian,
  so `dof_b = ratio · dof_a + phase` stays coherent.

**RPC parameters name their unit.** A parameter naming a physical quantity
carries a unit suffix (`angle_deg`, `tolerance_mm`) or has its unit named
in its `MethodDoc` row (`add_load`'s `magnitude` is newtons for a force, MPa
for a pressure). A wrong unit is invisible in the answer (a mate pinned at 60
radians, an FEM load that scales every result), so the name is where it has
to be caught.

**Gates:** `tests/rpc_units_gate.py` (a dimensional key without a suffix or a
unit in its row fails), the `test_conventions.cpp` case "no bare
degree/radian conversion outside core/Angles.h", `tests/test_units.cpp`,
`tests/units_smoke.py`, and the twin/refusal cases in
`tests/remote_smoke.py` and `tests/headless_smoke.py`.

A full unit type system is not used: every unit defect sits where a JSON
number becomes a struct field, and a quantity type is assigned there by fiat
just as a double is.

## Defaults derive from what they act on

A default that carries a unit — a length, an angle, a scale, a position — is
derived from the thing it acts on (the bore, the body's bounding box, the
sheet, the face that was picked), never a literal. A literal is right in the
abstract and wrong against any real part.

**Every creation tool either changes the document with its defaults or
refuses visibly.** A tool that runs cleanly and changes nothing says so, and
stays acceptable (Refine on a clean body is a fair answer).

**Gates:** the `defaults/*` GUI suite (`src/app/guitests/GuiTestsModeling.cpp`,
one case per toolbar tool: click it on a fresh document, and the document's
fingerprint must change, or the tool must arm a pick or refuse where it can
be seen; `defaults/no_op_warns_without_blocking`), and headless arithmetic in
`tests/test_actions.cpp` (for example `RecenterSplitOffset`: derived, never
overwriting a user's expression, re-derived per plane). The UI rules are in
[ui/README.md](ui/README.md).

## Refuse, don't substitute

An input the program does not recognise — an enum string, a feature type, a
mate type, a sheet size, a CAM operation or tool type, a material — is
**refused, naming what is accepted**. It is never replaced by a default: the
substitute is always plausible, so the document loads, looks right, and is
wrong (a sheet that loads as A4, `add_mate(type:"revolut")` solving as a rigid
joint with the wrong degrees of freedom, a modal study running as static).

- String-to-enum parsers are named `*FromString` (or `TryXxxFromName`) and
  either throw with the accepted list (`unknown mate type 'revolut'
  (expected one of: fastened, revolute, …)`) or return `bool`/`optional`
  for the caller to refuse on. A `bool` parser is `[[nodiscard]]`.
- `RpcParams` refuses unknown request keys, and `FeatureFromJson` refuses a
  feature type with no `kFeatureTypes` row.
- The directory format is one hand-editable file per item, so a typo in a
  file is a normal-workflow risk, and the loader applies the same rule as
  the RPC surface.

**Gates:** the `test_conventions.cpp` cases "every bool *FromString parser is
[[nodiscard]]" and "an enum-returning *FromString parser has no silent
fallback"; per-parser refusal tests in `test_serialization.cpp`,
`test_drawing.cpp`, `test_fem.cpp` and the CAM tests;
`tests/strict_inputs_smoke.py` (unknown feature-JSON fields refused on the
RPC path); `tests/blindspots/03_unknown_feature_field.py`.

## A mutating RPC reports a broken feature in its own response

A call that can leave a feature broken returns the regeneration summary as
`regen: {ok, failed[], warnings[]}` in **its own** response
(`remote::RegenSummaryJson`, `remote::AttachRegen` in
`src/remote/RemoteHelpers.h`), so a script learns of the failure from the
call that caused it rather than from a `get_state` it would have to know to
poll. That includes undo/redo, configuration switches, variable edits,
`open_project` and checkpoint/history restores. Study edits are excluded:
they cannot break a feature, so a `regen` there would always read `ok`.

**Gate:** `tests/regen_honesty_smoke.py`.

## A verdict says how it knows

Every RPC that answers "is this all right?" (geometry checks, clash and
interference, contact, fastener/drawing/layout checks, CAM verification,
study results, motion studies) attaches a receipt (`core/Receipt.h`): each
claim carries its oracle and version, a verdict of `Pass`, `Fail` or
**`Unverifiable`** (the oracle could not run — never read as clean), and a
basis of `Predicted` (a solver's estimate, such as an FEM factor of safety),
`Verified` (the kernel or a rule ran for real) or `Measured`. A receipt whose
passes rest on predictions rolls up `Provisional`, never `Verified`.

**Gate:** the `test_conventions.cpp` case "every verdict RPC attaches a
receipt".

## A manufacturability check belongs to a declared process

A check that judges whether a part can be made reads a process the document
declares, beside the features that use it: sheet metal's
`SheetMetalRules` (material, gauge, bend radius, K-factor) for
`check_sheet_metal`, `tube::TubeRules` (material, die set, clamp and grip
lengths, thinning limit) for `check_tube`, and a CAM job's tool and machine
for `cam_verify`. A check that would have to guess the process (a cutter
size, a wall thickness for an unknown method) is not built.

## Rules the source is scanned for

`tests/test_conventions.cpp` checks the source itself, because a comment does
not fail the build. Beyond the cases above:

- no numeric field in `src/ui` bypasses `ui::ExprInput` (a raw ImGui numeric
  widget cannot take an expression);
- every feature reference is reported by `VisitFeatureRefs`;
- `CADShape` replaces its shape only through `SetShape`;
- every boolean in `Measure.cpp` is built through `BuildQuery`
  (non-destructive; see [architecture.md](architecture.md#threading));
- a background job is polled by `ui::PollBackgroundJobs`, never by a panel;
- every tool-session sentinel is in the registry, and every scene renderer is
  registered and torn down in `main.cpp`;
- the CAM op-type list in `add_cam_op`'s row matches the table, and the
  adaptive and surfacing cache keys cover every field their passes read;
- a toolbar glyph means one thing, and a command's icon comes from
  `ui/Icons.cpp` alone;
- nothing in `src/` is named `near` or `far` (`windows.h` macros);
- every doc path a file cites exists, down to its heading;
- the test binary is not older than the source it is testing.

A scanning test is deliberately narrow: it keys off a name family
(`*FromString`, `BuildQuery`, …), so a new instance is caught only if it is
named into that family.
