# Variables & expressions — plan

> **Status: complete** (V1–V3 + unit-suffixed literals, format v28; project
> scope added in the caster round — see [../dogfooding.md](../dogfooding.md)).
> Dimensional analysis and unit display are the deliberate remainder.

Dimensions as expressions over named parameters (`length = width * 2`),
evaluated during regeneration. Differentiator #2 in the [roadmap](../roadmap.md),
and the roadmap's own assessment holds: *cheap once the document model exists,
nearly impossible before* — and the document model now exists. Do this **early**
(before or alongside modeling M1–M2) so every new feature's params are
expression-driven from day one instead of retrofitted.

> **Status: V1–V3 shipped (July 2026).** Evaluator behind a swappable
> `ExpressionEngine` interface (`src/core/Expression.h`) so a heavier parser
> or a scripting runtime can replace the built-in recursive-descent engine
> without touching the document model. Parameter table + topo-sort/cycle
> evaluation pass on `Document`, `Feature::VisitExprs` threading (extrude
> distance + sketch dimensions, angles in degrees), format v11, undoable
> set/delete/rename commands, docked Variables panel, expression editing in
> Properties + the inline dimension editor, and
> `set/delete/rename/list_variables` RPCs (`add_constraint` takes `expr`,
> `add_feature` takes `distance_expr`).
>
> **Units algebra shipped (C2, July 2026).** A numeric literal may carry a
> unit suffix — `mm`/`cm`/`m`/`in`/`ft` (→ mm) and `deg`/`rad` (→ degrees) —
> converted to base units at parse in the `BuiltinExpressionEngine`
> tokenizer. The unit folds into the number token, so `Dependencies()` and
> `RenameIdentifier` never see it; unit names are reserved parameter names;
> format v28. **No dimensional analysis** (`10mm + 5` = 15, `2in * 3` =
> 152.4) — the recorded deferral, along with per-document unit
> display/switching. **The Variables workstream is complete for v1.**

## Design

### Parameter table
`Document` gains an ordered list of parameters
`{ name, expression, cachedValue, comment }`, serialized (format bump). Names
are identifiers (`width`, `hole_r`); values are unitless doubles interpreted in
the document's units (mm / degrees, as today — real unit algebra is deferred).

### Expressions everywhere a number lives
Every numeric feature parameter becomes an `Expr { std::string text; double
value; }` instead of a raw double: extrude depth, sketch dimensional-constraint
values (Distance/Radius/Angle/…), and every param future features add (revolve
angle, datum offset, pattern count). Migration: on load, a plain JSON number
becomes a literal expression (old files load unchanged); a plain number typed
in the UI stays a literal — expressions are opt-in per field.

### Evaluator
A small hand-rolled recursive-descent evaluator in `src/core/` (no dependency;
exprtk/muparser are overkill and another vendored tree): numbers, identifiers,
`+ - * / ^`, unary minus, parens, a fixed function set (`sin cos tan sqrt abs
min max floor ceil`, degrees for trig to match the UI). Pure and unit-testable.

### Evaluation pass
At the start of `Document::Regenerate()`: topo-sort parameters by identifier
dependency, evaluate in order, **cycle → document-level error** (report, keep
last good values). Then each feature evaluates its `Expr`s against the table
via `RegenContext` before doing geometry. Sketch dimension values are evaluated
before the solve. A parameter edit is a normal `Command` (undoable) and
triggers regeneration — that's the whole "change `width`, the model updates"
feature.

### Rename
Expressions are stored as text; renaming a parameter tokenizes each expression
and rewrites matching identifiers (same approach as the sketcher's id
stability: never leave dangling references silently). Deleting a parameter
that's referenced is refused with the list of referencing fields.

### UI
- A **Variables panel** (docked, like the Model panel): name / expression /
  value table, add/remove/rename, inline error display (parse error, cycle,
  unknown identifier).
- Every numeric input in Properties / feature dialogs accepts an expression:
  typing `=width*2` (or just a non-numeric string) stores an expression;
  the field shows `width*2 = 40.0` (Onshape style). Evaluation errors turn the
  field red with the message, feature keeps its last good value.

### Remote / scripting parity
RPCs: `set_variable`, `delete_variable`, `list_variables`; every RPC that
takes a numeric param accepts an expression string. This is a big LLM win —
"make the spacing `width/count`" becomes one call.

## Milestones

### V1 — Evaluator + parameter table + regeneration pass (~1–1.5 days)
Pure-core: evaluator, `Document` param list, topo-sort/cycle detection,
serialization + migration, `set_variable` command + RPC. Tests: evaluator
cases, cycle detection, param edit regenerates, round-trip.

### V2 — Feature params become expressions (~1–1.5 days)
`Expr` type; extrude depth + sketch dimension values plumbed; Properties-panel
expression editing; error display. Tests: dimension driven by a param follows
a param edit through solve + extrude; literal-number migration.

### V3 — Variables panel + rename/delete safety + remote parity (~1 day)
The panel, rename-rewrites, referenced-delete refusal, expression args across
the RPC surface, `remote_smoke.py` coverage.

## Deferred

- ~~**Units algebra**~~ — **shipped (C2)**; *dimensional analysis* and
  per-document unit display/switching remain deferred.
- **Derived/measured variables** (a variable bound to a measurement — wants
  [inspect/plan.md](../inspect/plan.md)).
- **Per-sketch local variables**; global-only first.
- **Conditionals / string params** (Onshape FeatureScript territory — no).
- **`extract_variables`** — the selected features' literal dimensions into
  named variables driving expressions, in one verb (2026-09-03, from the
  agent-facing follow-ons in [../scripting/plan.md](../scripting/plan.md)).
  The evaluator, the `_expr` fields and the panel are all in place; this is
  the verb over them. Wants a benchmark run, since it changes what the
  benchmark measures.

## Risks

Low. The evaluator is a solved textbook problem; the wide-but-mechanical part
is threading `Expr` through feature params, serialization, and the two UIs —
same shape as the sketcher's Phase B id migration, which went fine.
