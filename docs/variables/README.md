# Variables and expressions

A number in a document can be an expression over named variables
(`depth = width * 2`). Expressions are evaluated on every regeneration, so
changing a variable updates the model. Humans and agents use the same
machinery: the Variables panel and every numeric field for people, and the
`*_variable` RPCs plus an `expr` / `<field>_expr` on each numeric parameter
for agents.

## Concepts

- **Variable (parameter).** A variable is `{name, expression, comment}`. It
  lives in one of two scopes:
  - **Project** variables are shared by every part in the project. They are
    stored in the project envelope (`project.json`, key `"parameters"`).
  - **Part** (local) variables belong to one part document. They are stored
    under `"parameters"` in the part's JSON, or in its directory `index.json`.

  The struct is `Parameter` in `src/core/Document.h`. Its `value` field holds
  the last successful evaluation. Its `error` field is transient and is not
  serialized.
- **Resolution** looks in the part's local variables first, then the
  project's. A local variable shadows a project variable of the same name.
  Project variables are evaluated without the part scope, so they cannot
  refer to part-local names.
- **Expression field.** A numeric feature parameter keeps its literal value
  and, optionally, an expression text beside it. The feature declares the
  pairs through `Feature::VisitExprs(text, value)` in `src/core/Feature.h`.
  An empty text means a plain literal. In JSON the text is
  `"<field>_expr"` next to the number (for example `distance_expr`), and it
  is omitted when empty. Sketches have two kinds of expression field:
  - a dimension constraint's `valueExpr`, stored as the `"expr"` key;
  - driven entity coordinates in `SketchFeature::entityExprs`: Line
    `ax ay bx by`, Circle and Arc `cx cy r`, Point `x y`, and a spline's
    poles `p<k>x p<k>y` (k from 0), so a spline outline scales with the
    variable that sizes the rest of the part; a text label's anchor
    `x y`, `height`, `angle_deg` (degrees) and `spacing`. They are stored as
    `<field>_expr` keys on the entity. Ellipses and arc angles cannot be
    driven (an ellipse's radius is a distance from its centre to an axis
    end, which can: [../sketching/README.md](../sketching/README.md#ellipses-and-splines)). An `_expr` key the entity does not have (`radius_expr` on a
    circle, whose field is `r`) is refused naming the ones it takes, not
    dropped: dropped, the dimension would silently freeze at its literal.

## The expression language

`BuiltinExpressionEngine` (`src/core/Expression.{h,cpp}`) is a hand-written
recursive-descent evaluator. The document only talks to it through the
`ExpressionEngine` interface (`Compile`, `EvaluateText`, `RenameIdentifier`,
`IsValidParameterName`, `Default()`). Nothing outside that module sees
tokens or syntax trees, so the engine can be replaced without touching
`Document` or the features.

**Syntax**
- Numbers and identifiers (`[A-Za-z_][A-Za-z0-9_]*`).
- The operators `+ - * /`, and `^`. `^` is right-associative and binds
  tighter than unary minus, so `-2^2 = -4`.
- Unary minus and parentheses. There is no unary plus.
- One constant, `pi`.

**Functions:** `sin cos tan sqrt abs floor ceil` (one argument each),
`min max` (two or more) and `lookup("table", key, "column")`, which reads a
project table ([tables](#tables)). Trig functions take **degrees**, like the
dimension UI does. There are no inverse trig functions.

**Quoted names** (`"..."` or `'...'`) exist only as a lookup's arguments: its
table and column must be quoted (a bare name there would be a variable), and
its key is a quoted name or a numeric expression. A quoted name anywhere else
is a compile error saying where it belongs.

**Unit suffixes on literals**

| Suffix | Converts to |
|---|---|
| `mm` | 1 mm |
| `cm` | 10 mm |
| `m` | 1000 mm |
| `in` | 25.4 mm |
| `ft` | 304.8 mm |
| `deg` | 1 degree |
| `rad` | converted to degrees |

The base units are millimetres for length and degrees for angles.

- A suffix binds only directly after a number (`10mm` or `10 mm`) and is
  folded into that number when the text is tokenized. That is why
  `Dependencies()` and rename never see it.
- After an operator, a unit name is an ordinary identifier:
  `4 + m` fails with "unknown variable 'm'".
- Unit names, function names and `pi` are reserved and cannot be variable
  names. Names that merely contain them, such as `mm_gap` or `inner`, are
  allowed.
- There is **no dimensional analysis**. `10mm + 5` is 15, and `2in * 3` is
  152.4.

**Errors never reach the geometry.** An unknown variable or function, the
wrong number of arguments, division by zero, `sqrt` of a negative number,
and a non-finite result are all errors. An error carries
`{message, offset, length}`, so the UI can underline the bad span and the
RPC can report it.

## Tables

A **project table** is a named grid of numbers keyed by row, so bearing and
screw sizes live in the model rather than in a generator's Python:

```
set_table {name: "bearings", columns: ["d", "D", "B"],
           rows: [{key: 6002, values: [15, 32, 9]},
                  {key: 6200, d: 10, D: 30, B: 9}]}
set_variable {name: "od", expression: "lookup(\"bearings\", size, \"D\")"}
```

- **What it is.** `lookuptable::Table` (`src/core/LookupTable.{h,cpp}`):
  `{name, comment, columns, rows[{key, values}]}`. Cells are numbers in base
  units (mm, degrees), like a variable's value. A table name and its column
  names are identifiers; keys are non-empty, unique, without control
  characters; every row has one finite value per column. Anything else is
  refused saying what is wrong (`lookuptable::Validate`), never stored as a
  different table.
- **Keys** are text. A number is stored in its one spelling, the shortest
  text that reads back as the same double (`NumericKey`: `6002`, `6.35`), so
  `lookup(..., 6002, ...)`, `lookup(..., size, ...)` with `size = 6002`, a
  JSON `6002` and the string `"6002"` all find one row. A text key (`"M6"`,
  `"608"`) is matched exactly.
- **Where it lives: the project.** `AppState::projectTables`, beside the
  project variables, saved in `project.json` under `"tables"`. A lone part's flat file is the whole
  project, so its envelope carries them (`"tables"`, format v150) and the
  open takes them back into the project scope (`Document::TakeTables`), the
  way the project variables travel. A malformed table — including one with a
key other than `name`, `comment`, `columns` and `rows` — refuses the open: an
  expression reads it, and a project that lost one would load with every
  lookup in it broken. A `Document` that keeps its own tables (a core test,
  a file read without the app) evaluates its lookups against them.
- **How a lookup evaluates.** The cells reach the evaluator as symbols, one
  per cell, under names no identifier can spell
  (`lookuptable::CellSymbol`: a unit-separator lead and three fields). They
  are seeded as the outermost scope of the project variables
  (`ui::SyncProjectScope`), so a project variable, a part variable and a
  feature field all read them, through the ordinary symbol table and with
  no second channel into the evaluator. Completion skips them.
- **Refusals name what exists.** An unknown table names the tables, an
  unknown key the table's keys (numerically sorted when they are numbers),
  an unknown column the row's columns; a project with no tables says so.
  The error is the variable's or the feature's, like any other.
- **Edits** go through `ui::SetProjectTable` / `ui::RemoveProjectTable`
  (`src/ui/VariableActions.cpp`), on the project variables' cross-tab
  undo: a snapshot holds the tables beside the variables. Every edit
  re-evaluates the project and regenerates every part. Deleting a table is
  refused while any variable or feature field reads it, naming them.

## Evaluation

`Document::Regenerate` calls `Document::EvaluateParameters` first. That
function runs `EvaluateParameterTable` in `src/core/Document.cpp`, which:

1. Seeds the symbol table with the project values, then with each local
   variable's last good value.
2. Compiles each expression.
3. Topologically sorts the variables by the identifiers they use.
4. Evaluates them in that order.

If a variable fails to compile or evaluate, or is part of a cycle
(`"circular reference"`), it keeps its last good value and records the
error. Regeneration continues.

After the variables, every feature expression field is evaluated against
the table, through `VisitExprs`, *before* the regen cache hashes the
feature. This order matters: the evaluated values end up in the feature
JSON the cache keys on, so editing a variable invalidates exactly the
features that depend on it. If a feature expression fails, the field keeps
its last value and the message becomes that **feature's error**. It is not
dropped silently.

Sketch angle dimensions are stored in **radians**, but their expressions
are written in **degrees**. `SketchFeature::VisitExprs` converts between
the two around the evaluation. Driven sketch coordinates are held fixed in
the solver, because the variable table owns their value, not the solver
(see [../sketching/README.md](../sketching/README.md#linked-copies)).

## Literals that should be expressions

A shape can be consistent and still wrong, and nothing else catches it. Four
numbers with an unstated relationship -- a bolt circle, a wall, a relief and
a boss, each a bare literal -- let a boss hang off a flange while every check
passes; had the boss been an expression of the others it could not have
floated, and editing the wall would have carried it. `check_literals` points
at the cause. It is ADVISORY: it reports and never refuses, because equal is
not the same as related, and a lint that fires on every coincidence gets
ignored.

**The field walk** (`literals::Fields`, `src/core/Literals.{h,cpp}`) is
generic. A feature declares its expression pairs through `VisitExprs` and
writes each as `<field>_expr`; the walk puts a sentinel text into every pair
of a CLONE, serializes it, and reads back where each landed. So a new
feature type takes part the day it has a `VisitExprs` and a serializer, with
no table to forget, and a pair its serializer does not write (an extrude's
second distance when it is not two-sided) is no field. A field is named by
its path in the feature's JSON: `distance`, `angle_deg` (the key the number
is under, beside `angle_expr`), `origin/2`, `constraints/3` (a sketch
dimension), `entities/1/r` (a sketch entity's coordinate, which takes an
expression even when it has none yet). Each carries its value in the
expression's units (mm, degrees), its kind -- length, angle or count, from
the field's name and a sketch dimension's kind -- and, when it must not be
driven, why: a coordinate a sketch constraint names (drive the dimension), a
sketch entity a pattern, mirror, copy or projection generates, a hole
diameter its fit sets, an extrude distance that ends through all or up to a
face.

**Literal findings.** A field holding a bare number EQUAL (to 1e-6 mm) to a
variable in scope -- the part's own, then the project's it does not shadow --
is reported with every variable it equals (`-name` when it is the negation).
Zero is never a finding. A literal is only compared with a variable of the
same kind, and a variable's kind is what it is USED as across every part of
the project (a bare reference in a field of that kind, or any reference from
a length field that does not multiply two names), what its name says
(`_deg`, `angle`, `count`, `teeth`), and what its comment's leading unit
says (`mm, ...` a length; `N, ...`, `MPa`, `kg` never a length; a ratio, a
factor or a width "in modules" a count). A variable none of those
classify is compared with lengths only. On the pan/tilt generator's output
the comment rule is what kept `finger_n = 8` (newtons) off an 8 mm hole
depth and `face_k = 6.7` (a width in modules) off a 6.7 mm frame height.

**Placement findings.** A sketch -- a primitive is one
([../modeling/README.md](../modeling/README.md#primitives)) -- or an
explicit `Frame` placed in the WORLD (a baked base plane at any offset, a
`DatumPlane`, another world-placed sketch's plane, an explicit frame or an
offset of one) whose plane a planar face of the part already lies on, over
that face, is reported with the face it should be attached to (`face_ref`
for a sketch, a frame `from_element`): attached to the world it floats, or
is buried, the day the part changes. The faces are the ones that existed
when it ran: the regeneration cache's bodies, or on a part opened from baked
geometry (which carries only the tip) the final bodies with each face's name
cut back to what it was then -- the M steps of later features dropped, a
face a later feature made (a G or S step) left out
([../modeling/toponaming.md](../modeling/toponaming.md#naming-grammar)). Not
reported: a placement whose offset follows a variable (a datum offset or a
frame origin with an expression), a face lying in a base plane through the
origin (the part's own datum, which what made the face measured from too),
and a face in the plane its own feature's sketch was drawn on (an extrude's
start cap over a sketch on the same plane). Reference (ghost) bodies are
skipped.

## Extracting variables

`extract_variables` turns the active part's literal dimensions into named
variables with expressions -- the parameters-first workflow, for a part that
was built from numbers or imported. `literals::PlanExtract` plans it and
changes nothing; `ui::ApplyExtractVariables` (`src/ui/LiteralActions.cpp`)
applies a plan.

- **A dry run first.** Without `apply` it answers the plan: each proposed
  variable `{name, expression, value, quantity, uses[]}` and every literal
  `left_alone` with the reason -- no expression twin, a unitless count, zero,
  a full turn, or the field's own reason above. `feature_ids` and `fields`
  narrow it; an unknown id or path is refused naming what exists.
- **Names** are `<feature>_<field>` (a sketch dimension by its kind, an
  entity coordinate by its field), after `prefix`, made unique against every
  variable of the project and of every part. The expression is the shortest
  text that reads back as exactly the number, so the geometry does not move
  by an ulp.
- **Equal is not related.** Within one feature equal values share one
  variable (and the negation is `-name`); across features only with
  `merge_equal: true`. `reuse: true` binds a literal to the ONE existing
  variable it equals -- the lint's finding -- instead of a new one; a literal
  equal to several is left alone, naming them.
- **One undo step.** In `project` scope (the default) the new variables are
  project variables, and the step is ONE entry on the project-variable
  history that also carries the touched features' JSON
  (`ProjectVarSnapshot::features`), so undo and redo put the features back
  with the table. In `part` scope they are the part's own, and the step is
  `SetVariableCommand`s plus `ModifyFeatureCommand`s coalesced on the part's
  stack. A plan whose name has since been taken, or whose field has gone, is
  refused ("run the dry run again"); an apply that would leave a touched
  feature broken is rolled back and says which.

Save as template ([templates](../modeling/templates.md#saving-one)) reads
the same walk: the literals of the features it saves are offered as the
template's parameters, named as `extract_variables` would name them, and
every variable the features read becomes one.

## Rename and delete

- **Rename.** `Document::RenameParameter` rewrites every parameter and
  feature expression with `RenameIdentifier`. The rewrite works on tokens
  and preserves every other byte of the text. Text that cannot be tokenized
  is left as it is.
  - A project-scope rename (`ui::RenameProjectVariable` in
    `src/ui/VariableActions.cpp`) rewrites every part, except parts where a
    local variable of the same name shadows it.
  - A requirement's expressions -- an `expression` requirement's formula
    and any requirement's expression bounds -- are rewritten with them, in
    the scope the requirement reads (`ui::RenameProjectVariable`, and
    `ui::RenamePartVariable` for a part-local one;
    [requirements](../requirements/README.md#expression)). Undoing a
    project rename puts the requirement back too; a part's undo does not.
- **Delete** is **refused while the variable is referenced**. The refusal
  lists the references, for example `parameter 'x'`, `feature 'Extrude 1'`,
  `<Part>: feature '…'` for the project scope, or `requirement 'wall'`
  (`ui::RequirementReferences`; `ui::RemovePartVariable` for a part-local
  one). Nothing is left dangling silently. A table is refused the same way
  while a variable, a feature or a requirement looks it up.

## Undo

Part-scope edits are Commands in `src/core/Command.h`:
- `SetVariableCommand`
- `DeleteVariableCommand`: undo re-adds the variable at the end of the
  table.
- `RenameVariableCommand`

Project-scope edits go through `ui::SetProjectVariable`,
`RemoveProjectVariable` and `RenameProjectVariable`, and they keep a
separate history (`projectVarHistory`). Global undo routes to whichever of
the two histories changed most recently (see `VariableActions.cpp`).

## RPCs

The RPCs are handled in `src/remote/RemoteProject.cpp`.

| method | params | notes |
|---|---|---|
| `set_variable` | `name, expression, comment?, scope?` | `scope` is `"project"` (the default) or `"part"`; any other value is refused. Omitting `comment` keeps the existing one. Returns `{value, scope, error?, regen?, requirements?}`: `requirements` lists each [expression requirement](../requirements/README.md#expression) whose state the edit changed. |
| `list_variables` | none | Project variables first, then the active part's: `{name, expression, value, scope, comment?, error?}`. |
| `rename_variable` | `name, new_name, scope?` | Without `scope`: project if a project variable has that name or no local one does, otherwise part. Rewrites every expression that reads it, a requirement's included ([above](#rename-and-delete)). |
| `delete_variable` | `name, scope?` | Same scope rule. Refused while referenced (by a feature, a variable or a requirement), and the refusal names the references. |
| `set_table` | `name, columns, rows, comment?` | Adds or replaces a project table. A row is `{key, values: [...]}` or `{key, <column>: value, ...}`; a column a row names that the table does not have is refused naming the columns. Returns `{table, regen?, requirements?}`. |
| `list_tables` | `name?` | Every table, or the one named (an unknown name is refused naming the tables). |
| `delete_table` | `name` | Refused while any expression reads it (a variable's, a feature's or a requirement's, naming them), or when there is no such table. |
| `check_literals` | `part?` | The advisory lint ([above](#literals-that-should-be-expressions)), in `src/remote/RemoteModeling.cpp`: `{part, literals[], placements[], checked}`. Never refuses. |
| `extract_variables` | `feature_ids?, fields?, prefix?, merge_equal?, reuse?, scope?, apply?` | A dry run without `apply` ([above](#extracting-variables)); with it one undo step, `{applied, scope, variables[], left_alone[], regen, digest}`. `scope` other than `project`/`part` is refused. |

Every numeric feature parameter also accepts an expression, as a
`<field>_expr` key in the feature JSON passed to `add_feature` or
`modify_feature`. `add_constraint` takes
`expr`, which is written in base units where **angles are degrees**
(`"45"`, `"45deg"` and `"0.785rad"` are the same angle). Its literal
`value` is in radians for an angle, or you can pass `value_deg` instead
(see [../sketching/README.md](../sketching/README.md#rpcs)).

## GUI

- **The Variables panel** (`src/ui/VariablesPanel.cpp`,
  `DrawVariablesPanel`) is docked behind Model. It has one row per variable
  with these columns:
  - a scope badge, `P` or `L`;
  - the name, where editing commits a rename;
  - the expression, with completion;
  - the value, or the error in red;
  - the comment;
  - "..." (`###var_sweep_<scope>_<name>`), which opens the
    [design sweep](../requirements/README.md#design-sweep) on half to
    twice the variable's value, and a delete button.

  "+ Add variable" creates a **project** variable. The panel has no way to
  create a part-local variable; `set_variable {scope:"part"}` does that.
- **Tables** is a section of the same panel (`src/ui/TablesPanel.cpp`,
  `DrawTablesSection`): "+ Add table", each table as a grid of key and
  columns with every cell, key, column name and the table's name editable
  in place, "+" for a column, "+ Row" (a new row copies the last row's
  values, the next size in a series being nearer its neighbour than zero),
  a delete button per row, and "Delete table". Every edit goes through
  `ui::SetProjectTable`, so the panel refuses what the RPC refuses, in the
  same words. GUI test `variables/a_table_by_clicking`.
- **Numeric fields** (`src/ui/ExprInput.{h,cpp}`) switch to expression
  entry on Ctrl+click, double-click, or Enter on a focused field. Completion
  offers variables, functions and unit suffixes (`src/ui/ExprComplete.h`).
  - A plain numeric field evaluates what you type and stores the *result*.
  - A feature dimension (`ExprDim` in `src/ui/FeatureEditor.cpp`) has an
    **fx** button that stores the expression text and shows its value or
    error. A **123** button turns it back into a literal.
- **Literals** is a section of the same panel (`src/ui/LiteralsPanel.cpp`,
  `DrawLiteralsSection`), on the same actions as the RPCs: **Check** runs
  the lint on the active part, each literal equal to one variable has a
  **Use** that binds it (`###literal_use_<feature id>_<path with dots>`),
  and **Extract variables** plans on the selected features (every feature
  when none is) into project or part scope, listing the proposals until
  **Apply** or **Cancel**. GUI test `variables/literals_by_clicking`.
- The inline sketch dimension editor accepts an expression in the same way.
  It is covered by the GUI tests `dims/inline_editor_expression` and
  `dims/inline_editor_completes_a_variable`.

## Tests

- `tests/test_expression.cpp` covers the language itself: precedence,
  `^` and unary minus, trig in degrees, error spans, dependencies,
  formatting-preserving rename, reserved names, and unit suffixes.
- `tests/test_variables.cpp` covers the table:
  - evaluation in dependency order;
  - refusal of invalid names;
  - an extrude that follows a variable edit;
  - undo and redo of the commands;
  - rename rewriting feature expressions;
  - documents without variables loading unchanged.
- `tests/param_smoke.py` checks that script `Param` values round-trip into
  document variables that drive the geometry. It also checks that nudging
  a variable moves exactly the expected JSON fields.
- `tests/units_smoke.py` checks unit-suffixed variables over RPC, refusal
  of a variable named `mm`, and save/reopen keeping the text.
- `tests/test_lookup_table.cpp` covers tables: lookup by numeric, string
  and computed keys, the refusals naming what exists, quoted names allowed
  only in a lookup, validation and JSON, a document's lookups driving
  geometry through a save, and a project table re-evaluating its readers,
  undoing, and refusing a delete while read.
- `tests/tables_smoke.py` covers the RPCs, both row forms, and the tables
  surviving a flat save and a project.json save.
- `tests/test_literals.cpp` covers the walk and the lint: a literal equal to
  a variable reported, the same field driven by it not, a count never
  against a length, a variable's comment saying what it measures, a sketch
  on a datum plane over a plate's top face reported with that face and not
  once attached to it, an expression-driven offset not reported, a field in
  degrees named by its own key, and a plan that applied drives the same
  geometry.
- `tests/literals_smoke.py` covers the RPCs: both findings, a dry run that
  changes nothing, an apply that moves no geometry and puts an expression on
  every field it proposed, each new variable moving exactly the fields it
  drives, one undo and a redo in either scope, `reuse`, and the refusals.
- `tests/golden/v11-expressions.json`, `v38-entity-exprs.json` and
  `v150-lookup-table.json` pin the file format.

## Not built

- Dimensional analysis: once a suffix has converted, an expression's
  algebra is plain numbers ([units](../conventions.md#units-and-angles)).
- Variables bound to a measurement. `MeasureActions.cpp` can *create* a
  project variable from a measured value, but the variable does not follow
  the measurement afterwards.
- Per-sketch local variables, conditionals, and string values (a quoted
  name is a lookup's argument, not a value).
- Table cells holding expressions, or text; a table per part.
- The lint suggesting `lookup(...)` for a literal equal to a table cell, or
  re-attaching a world-placed sketch to the face it found (a sketch on a
  face takes the face's frame, so its coordinates change too).
- Placement findings for anything but sketches and explicit frames: a
  revolve's custom axis, a 3D curve's points.
