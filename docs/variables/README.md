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
    driven. An `_expr` key the entity does not have (`radius_expr` on a
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
  project variables, saved in `project.json` under `"tables"` (an additive
  key; the envelope stays version 2). A lone part's flat file is the whole
  project, so its envelope carries them (`"tables"`, format v150) and the
  open takes them back into the project scope (`Document::TakeTables`), the
  way the project variables travel. A malformed table refuses the open: an
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

## Rename and delete

- **Rename.** `Document::RenameParameter` rewrites every parameter and
  feature expression with `RenameIdentifier`. The rewrite works on tokens
  and preserves every other byte of the text. Text that cannot be tokenized
  is left as it is.
  - A project-scope rename (`ui::RenameProjectVariable` in
    `src/ui/VariableActions.cpp`) rewrites every part, except parts where a
    local variable of the same name shadows it.
- **Delete** is **refused while the variable is referenced**. The refusal
  lists the references, for example `parameter 'x'`, `feature 'Extrude 1'`,
  or `<Part>: feature '…'` for the project scope. Nothing is left dangling
  silently.

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
| `set_variable` | `name, expression, comment?, scope?` | `scope` is `"project"` (the default) or `"part"`; any other value is refused. Omitting `comment` keeps the existing one. Returns `{value, scope, error?, regen?}`. |
| `list_variables` | none | Project variables first, then the active part's: `{name, expression, value, scope, comment?, error?}`. |
| `rename_variable` | `name, new_name, scope?` | Without `scope`: project if a project variable has that name or no local one does, otherwise part. |
| `delete_variable` | `name, scope?` | Same scope rule. Refused while referenced, and the refusal names the references. |
| `set_table` | `name, columns, rows, comment?` | Adds or replaces a project table. A row is `{key, values: [...]}` or `{key, <column>: value, ...}`; a column a row names that the table does not have is refused naming the columns. Returns `{table, regen?}`. |
| `list_tables` | `name?` | Every table, or the one named (an unknown name is refused naming the tables). |
| `delete_table` | `name` | Refused while any expression reads it (naming them), or when there is no such table. |

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
  - a delete button.

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
- `tests/golden/v11-expressions.json`, `v38-entity-exprs.json` and
  `v150-lookup-table.json` pin the file format.

## Not built

- Dimensional analysis, and choosing the display unit per document (the
  display units in `test_units.cpp` are a separate, fixed-base system).
- Variables bound to a measurement. `MeasureActions.cpp` can *create* a
  project variable from a measured value, but the variable does not follow
  the measurement afterwards.
- Per-sketch local variables, conditionals, and string values (a quoted
  name is a lookup's argument, not a value).
- Table cells holding expressions, or text; a table per part.
- A verb that turns the selected features' literal dimensions into named
  variables in one step (`extract_variables`).
