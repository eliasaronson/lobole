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

**Functions:** `sin cos tan sqrt abs floor ceil` (one argument each) and
`min max` (two or more). Trig functions take **degrees**, like the dimension
UI does. There are no inverse trig functions.

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
- `tests/golden/v11-expressions.json` and `v38-entity-exprs.json` pin the
  file format.

## Not built

- Dimensional analysis, and choosing the display unit per document (the
  display units in `test_units.cpp` are a separate, fixed-base system).
- Variables bound to a measurement. `MeasureActions.cpp` can *create* a
  project variable from a measured value, but the variable does not follow
  the measurement afterwards.
- Per-sketch local variables, conditionals, and string values.
- A verb that turns the selected features' literal dimensions into named
  variables in one step (`extract_variables`).
