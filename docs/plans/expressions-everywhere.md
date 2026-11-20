# Expressions everywhere

Every number a person or an agent types into a document should be able to be
an expression of the variables, and stay one. Today only part features can.

## The problem

Variables live in the project (`state.projectParameters`) and in each part
(`Document::m_parameters`). A part feature keeps an expression text beside
each number it declares through `Feature::VisitExprs` (src/core/Feature.h),
and `Document::Regenerate` re-evaluates those texts, so changing a variable
moves the part. Nothing else works that way:

- **Assemblies take no expressions at all.** Mate values and limits
  (`Mate`, `MateRange` in src/core/Occurrence.h), occurrence placements,
  relations (ratio, phase, lift tables, intermittent timings), exploded
  steps, motion studies (driver range, samples), clearance assertions,
  sequences and assembly configurations' mate overrides are plain doubles.
  The assembly solve, RemoteAssembly and RemoteMotion have no expression
  code. "The hinge opens to `lid_open`" cannot be written down.
- **Rules, CAM, drawings, tolerances:** sheet-metal rules (thickness, bend
  radius, K, reliefs), tube rules, every number in a CAM job (tools,
  operations, stock, setups, machine; about 54), a size tolerance's limits,
  a drawing view's scale and position. `DrawingView::scaleExpr` is
  serialized but nothing evaluates or sets it.
- **FEM is half done, its own way.** Seven study numbers take expressions,
  through `EvaluateStudyExprs` (src/core/fem/FemStudy.cpp), not through
  the visitor; about twenty do not (friction, thermal values, gravity,
  nonlinear settings, mode count, keep-outs, thresholds). A variable rename
  does not rewrite study expressions, and deleting a variable a study uses
  is not refused.
- **Feature gaps:** Hole's counterbore/countersink sizes, fit pitch and point
  angle; Thread's pitch, engagement and flank; most of the worm/wheel
  numbers; Curve's degree and bend table; datum points; Cradle's direction;
  sketch pattern/mirror operations; and DatumAxis, RetainingRing, Boolean,
  Derived, Sweep and Loft, which have no `VisitExprs` at all.
- **A configuration's parameter override keys** are not renamed with the
  variable (`Document::RenameParameter`).
- **Only parts recompute.** `ui::SyncProjectScope`
  (src/ui/VariableActions.cpp) regenerates parts and restamps studies; an
  assembly, a job or a drawing never sees a variable change.
- **The mechanism is hand-rolled.** Every bound number is a `double` plus a
  `std::string fooExpr`, written and read by hand as `<field>` and
  `<field>_expr` (about 200 sites in SerializeFeatures.cpp). The RPC has
  three conventions: `<field>_expr` keys through the feature JSON, `expr`
  on a sketch dimension, and "a string where a number goes" in the FEM
  methods. The UI's `ExprDragDouble` can return the text, but only fields
  with somewhere to store it keep it; the rest evaluate once and discard,
  so Ctrl+click `swing/2` on a mate limit stores 12.5 and forgets `swing`.

## The shape of the fix

### 1. One value type

`ExprValue` in `src/core/ExprValue.h`: `{double value; std::string expr;}`,
empty `expr` meaning a literal, with an integer variant (`ExprInt`, a count;
a non-integer result is refused, not rounded) and an angle variant
(`ExprAngle`, stored in radians, written and evaluated in degrees as every
angle at the surface is: docs/conventions.md#units-and-angles). One
serializer pair, `WriteExpr(json, key, v)` / `ReadExpr(json, key, v)`,
writes `<key>` and `<key>_expr` together and refuses an `_expr` key that
does not parse. The existing JSON keys do not change.

### 2. One visitor on everything that holds numbers

`ExprVisitor` moves out of Feature into `src/core/ExprValue.h` and takes the
slot plus a field path (`mates[3].rotation_range.max`) and its quantity
(length, angle, count, ratio, plain), which `Literals` already models
(`Literals::Quantity`). `VisitExprs` is implemented by `Feature` (as now),
`AssemblyDocument`, `JobDocument`, `DrawingDocument`, `fem::Study`,
`SheetMetalRules`, `TubeRules`, `Tolerance` and `Requirement` bounds.
`EvaluateStudyExprs` becomes the Study's visitor. Everything that walks
expressions walks this one hook:

- evaluation (each document against its scope, below);
- rename (`RewriteProjectVarRefs` and the part-local rename), including
  configuration override keys;
- the refusal to delete a variable something uses, naming every user in
  every document;
- `Literals` (`check_literals`, `extract_variables`), which then finds
  literals in assemblies, jobs and studies too.

### 3. Scope

A part resolves part-local, then project, as now. Every other document
resolves against the project's variables. A lone assembly or job outside a
project resolves against an empty table, so an expression there is refused
naming the variable it could not find, as an unknown variable in a part is.

### 4. Recompute

`SyncProjectScope` re-evaluates every open document that holds an
expression that changed, not only parts:

- an assembly re-evaluates and re-solves. A mate value or limit that comes
  out invalid (min above max, a value outside its range, a range excluding
  where the mate stands) is an error on that mate, reported in the
  `set_variable` response's `regen` as a broken feature is
  (CLAUDE.md, "Refuse, don't substitute"), and the mate keeps its last good
  value;
- a CAM job and a study put the evaluated values into their input keys, so
  a changed variable marks a toolpath or a result stale, never silently
  current;
- a drawing re-evaluates its scales and positions.

A failed evaluation keeps the last good value and becomes that object's
error, as a part feature's does today.

### 5. One RPC convention

`<field>_expr` beside `<field>` on every method that sets a number, read by
`ReadExpr`, so it is uniform without each handler parsing it. A sketch
dimension's `expr` and the FEM methods' string-as-number stay accepted as
aliases (existing scripts and the SDK use them) and are documented as such.
Every `describe` row of a method that gains `_expr` keys says so;
`rpc.py`/`rpc-surface.json` are regenerated. A getter returns `<field>_expr`
wherever it returns `<field>`.

### 6. UI

Each field that edits an `ExprValue` passes its `expr` to
`ExprDragDouble`/`ExprInputDouble`'s `exprOut`, so Ctrl+click keeps the
link, and the field shows that it is bound (the feature editor's `ExprDim`
look). Typing a plain number over a bound field unbinds it, as `ExprDim`
does.

### 7. Nothing new arrives unbindable

A convention test probes every document type the way `Literals` probes a
feature (a sentinel through each visited slot, read back from the JSON)
and fails on a serialized number that is neither visited nor on a short
explicit list of numbers that are deliberately not bindable (ids, solver
outputs, derived results, view state). A new field then has to choose.

## Stages

Each stage merges on its own with the suites green.

- **A. The type, the visitor, the RPC reader**, with features and FEM
  studies moved onto them. Behaviour is unchanged except the two bugs
  (study expressions renamed, delete refused; configuration override keys
  renamed). Every golden reads and writes back identical, which is the
  check that the move lost nothing.
- **B. Assemblies:** mates and limits, placements and inner poses,
  relations, exploded steps, motion studies and clearance assertions,
  sequences, assembly configuration overrides; scope, recompute and
  re-solve; the Edit Mate window, occurrence and relation fields.
- **C. Parts' and rules' gaps:** sheet-metal and tube rules, tolerances,
  the feature gaps and the features without a visitor, sketch
  pattern/mirror operations, arc angles.
- **D. CAM and drawings**, including the dead `scaleExpr`.

Stages B to D are format changes (`vNEXT` each, with goldens). Stage A is
not, unless the FEM methods' stored form has to change to fit the type.

## Files

- New `src/core/ExprValue.{h,cpp}`; `src/core/Feature.h`,
  `src/core/Document.{h,cpp}` (evaluation, rename, delete refusal),
  `src/core/Literals.{h,cpp}`.
- `src/core/features/*.h` and `src/core/io/SerializeFeatures.cpp`
  (stage A: onto the type; C: the gaps).
- `src/core/fem/FemStudy.{h,cpp}`, `src/core/io/SerializeStudies.cpp`,
  `src/remote/RemoteSimulation.cpp`.
- `src/core/Occurrence.h`, `src/core/AssemblySolve.cpp`,
  `src/core/MateLimits.cpp`, `src/core/AssemblyRelations.cpp`,
  `src/core/MotionStudy.{h,cpp}`, `src/core/Configuration.h`,
  `src/core/io/SerializeAssembly.cpp`, `SerializeConfigurations.cpp`,
  `src/remote/RemoteAssembly.cpp`, `RemoteMotion.cpp`.
- `src/core/SheetMetal.h`, `src/core/tube/Rules.h`, `src/core/Document.h`
  (`Tolerance`), `src/core/cam/CamJob.h` and its serializer,
  `src/core/DrawingDocument.h`, `src/remote/RemoteCam.cpp`,
  `RemoteDrawings.cpp`.
- `src/ui/VariableActions.cpp` (`SyncProjectScope`), `src/ui/ExprInput.*`,
  `src/ui/OccurrencePanel.cpp`, `src/ui/FeatureEditor.cpp`, the CAM,
  drawing and study panels.
- `tests/test_conventions.cpp` (the probe), goldens per stage.
- `docs/variables/README.md` (the model, the scopes, the one RPC
  convention), `docs/assemblies/README.md` (remove "Mate limits as
  expressions" from Not built), `docs/scripting/rpc.md`,
  `docs/versioning/format.md`.

## Verified by

- Stage A: every golden round-trips identical; the reader fuzz; the rename
  and delete-refusal tests extended to a study and a configuration override.
- The probe test above, per document type.
- Per document kind, through the RPC: bind a value to a variable, change
  the variable, and see the effect where it matters — the mate turns and
  the assembly re-solves; a limit narrowed past the mate's value makes
  `set_variable`'s `regen` report the mate; a tool diameter from a variable
  marks the toolpath stale; a sheet thickness from a variable rebuilds the
  bends; a drawing scale from a variable rescales the view.
- Rename a variable used in a part, an assembly, a job and a study at once:
  every reference follows. Delete it: refused, naming all four.
- A GUI test: Ctrl+click an expression into a mate limit, change the
  variable in the Variables panel, and the limit and the drawn arc follow.
- The headless, GUI and pygui suites per stage; the action matrix after B
  and D, since its corpus covers assemblies and jobs.
