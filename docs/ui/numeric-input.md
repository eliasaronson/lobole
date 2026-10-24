# Numeric input: expressions everywhere a number is typed

Every numeric field in `src/ui` goes through the `ui::ExprInput` family
(`src/ui/ExprInput.{h,cpp}`), and every one of them accepts an expression —
`width/2`, `sqrt(2)*len`, `25.4 mm`, `30 deg` — evaluated against the
document's and the project's variables when the field commits.

## The widgets

| widget | use |
|---|---|
| `ExprDragDouble`, `ExprDragInt`, `ExprDragFloat2`, `ExprDragFloat3` (both over doubles) | drag fields with text entry |
| `ExprDragFloat` | the same, in float, for view state only (a section's offset and tilt) |
| `ExprInputDouble`, `ExprInputInt`, `ExprInputDouble3` | plain input fields |
| `FeatureEditor`'s `ExprDim` / `ExprCount` | a feature dimension with a persisted `*Expr` slot, shown as an `fx` row when it holds an expression |

**Drag fields own their text.** The drag is drawn with
`ImGuiSliderFlags_NoInput`, and all three gestures ImGui would have turned
into text entry are rebuilt on our own buffer: **Ctrl+click**,
**double-click**, and **Enter** on a keyboard-focused field. Owning the buffer
is what makes expressions work at all: ImGui's own text entry parses with a
bare `sscanf` (a leading identifier leaves the value unchanged, so the field
never registers an edit and the expression is dropped) and truncates at 31
characters. Taking a widget over means taking over every gesture it had.

The stand-in re-seeds ImGui's `InputTextState` with
`ReloadUserBufAndSelectAll()` when it arms, because ImGui keeps one text
state keyed by item id and would otherwise reopen the field holding the
previous expression.

`Drag*N` rows are wrapped in `BeginGroup`/`EndGroup` as ImGui wraps its own,
so the row reports the active component's status and an edit stays undoable
(`properties/vector_row_takes_an_expression` edits and undoes).

**Bounds.** A drag wrapper clamps a typed value to `[vmin, vmax]` only when
`vmin < vmax`; equal bounds mean unbounded, as for `ImGui::DragScalar`.

The `Input*` wrappers keep ImGui's `InputScalar` (a 64-byte text field with
neither the drop nor the truncation) and add only the evaluation.

**The gate.** `tests/test_conventions.cpp`, "no numeric field in src/ui
bypasses ui::ExprInput", fails on any `ImGui::Drag*` / `ImGui::Input*`
(Float, Double, Int, Scalar, and the 2/3/4/N forms) outside `ExprInput.cpp`.
A raw field would accept `10/2` as 10 without complaint — a plausible wrong
value, the GUI form of "refuse, don't substitute". The wrappers have the same
signatures plus `state`, so the fix for anything it catches is the `Expr`
prefix.

## Where the expression text goes

Expressions are opt-in per edit: typing a plain number stores the number and
clears any expression.

- **Fields with somewhere to keep the text keep it.** `ExprDragDouble`,
  `ExprInputDouble` and `ExprInputInt` take an optional `std::string*
  exprOut`, and these fields store it: every feature dimension (`ExprDim`)
  and pattern count (`ExprCount`) — the row switches to its `fx` form, so the
  link is visible; a study's mesh size and topology filter radius; a placed
  force's magnitude. The stored expression is re-evaluated on every
  regeneration, so the value follows the variable.
- **Fields with nowhere to keep it evaluate and store the number**: sketch
  tool-session parameters (fillet radius, pattern step, offset distance), CAM
  parameters, occurrence placement, section planes, a drawing view's scale,
  preferences. Giving CAM operations persisted expression slots would be a
  format change.

## A new variable

The sketch's two dimension fields -- the inline editor and the one typed
while drawing -- take `name = expr`: the PROJECT variable `name` is created
with the expression `expr` and the dimension is driven by `name`, in one
undo step on the project-variable history (which puts the sketch back too:
`PushProjectVarUndoWithFeatures`, the extract-variables recipe). The
expression language has no `=` of its own, so the first one splits.

- The name must be a valid parameter name, else the field stays open saying
  so ("'2w' is not a variable name: a letter or _ first, …").
- **A name already taken** (a project variable or any part's) is refused,
  naming its value and pointing at plain `name` to use it -- never
  silently redefined.
- The dimension's own checks run first: a value the sketch refuses ("Not
  changed: …", [solving](../sketching/README.md#solving)) creates no
  variable.
- An angle's `a = 30` stores 30, degrees, as angle expressions already are.

Over the RPC there is no new call: a `batch` of `set_variable` and
`add_constraint` with `expr` naming it does the same
(`help('add_constraint')`).

## Completion

`src/ui/ExprComplete.{h,cpp}` offers, for the identifier under the caret:
the document's and project's variables with their current values (a
project-scoped one says so), then the builtin functions (`sin cos tan sqrt abs
min max floor ceil`), then the unit suffixes (`mm cm m in ft deg rad`). Tab
accepts; Enter accepts only once a row has been chosen with the arrows
(otherwise Enter commits the field, and an open popup must not swallow it); a
row can be clicked; a function brings its open parenthesis.

It reaches every field that owns its buffer: the drag fields, the `fx`
expression rows, the Variables panel's definition column (not its name or
comment columns — completion belongs in a definition, not in the name being
defined), and both inline sketch-dimension editors (`##v` and the on-create
`##docv`). It does not reach the `ExprInput{Double,Int,Double3}` fields, which
keep ImGui's buffer, or the Configurations panel's override cell, which
refills a stack buffer from the model every frame.

**Ordering, because the popup and the field share one buffer and one frame:**
the popup is drawn *before* the field, from geometry and candidates cached
last frame, because the click that picks a row is also the click that
deactivates the field, and the verdict must be known before the field is
asked whether it committed; the picked text is applied *after* the field,
because a deactivating `InputText` writes its own text on the way out; and it
is applied before the "still active" early-out, because Tab and Enter leave
the field active while a mouse pick does not. The refocus that reloads
ImGui's copy from our buffer survives one frame past the pick.

## Units on screen

The document carries a unit system (`units::System {length, mass, angle}` in
`src/core/Units.h`, stored with the document, set in the Variables panel's
Units section). **Storage is always millimetres, grams and degrees**; the
system is a presentation layer, and changing it never moves a vertex. Every
`units::Format*` (`FormatLength`, `FormatArea`, `FormatVolume`, `FormatMass`,
`FormatAngle`) takes the system explicitly, with no default, so a caller has
to say which; drawing dimensions use `ui::ActiveDrawingUnits`. Numbers are
printed in significant figures with thousands separators (`FormatNumber`), not
fixed decimals: a tessellated volume does not have ten nanolitres of
precision, and a long fixed-decimal string clips in a column. An unknown unit
string is refused, not replaced by a default.

## Tests

GUI, all driving the real field: `properties/dimension_field_takes_an_expression`,
`properties/dimension_expression_is_not_truncated`,
`properties/dimension_field_opens_selected` (opens the field and types with
bare keystrokes — a test that types through `ItemInputValue` clears the field
with Ctrl+A first and so cannot see a field that opens holding the wrong
text), `properties/dimension_field_opens_on_double_click`,
`properties/vector_row_takes_an_expression`,
`properties/completion_row_can_be_clicked`,
`dims/inline_editor_completes_a_variable`,
`dims/name_equals_value_makes_a_variable` (the variable, the binding, one
undo; a taken and a bad name refused with nothing changed),
`dims/refused_name_equals_value_makes_none`,
`dims/typed_dimension_rescales_and_names`,
`configs/an_assembly_configuration_by_clicking` (an unbounded field).

## Not built

A visible affordance that a field takes text (Ctrl+click and double-click are
not advertised in the field itself); completion in the `ExprInput{Double,
Int,Double3}` fields and the Configurations override cell; persisted
expressions for CAM parameters.
