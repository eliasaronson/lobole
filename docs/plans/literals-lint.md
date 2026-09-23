# An advisory lint for literals that should be expressions

## Problem

A shape can be consistent and still silly, and nothing catches it. Four
numbers with an unstated relationship — a bolt circle radius, a wall radius,
a relief diameter and a boss diameter, each a bare literal — let a boss end
up hanging in mid-air off a flange with daylight behind it, while every check
passes: one solid, a clean motion study, fitted bolts. Had the boss diameter
been an expression of the other three it could not have floated, and editing
the wall would have carried it. `islands()` finds the result after the fact;
nothing points at the cause.

## Fix

A check that names feature dimensions which are bare numbers while a project
or part variable of the SAME value is in scope — the case where the author
almost certainly meant the variable. It ADVISES and never refuses: equal is
not the same as related, and a lint that fires on every coincidence gets
ignored.

- A `check_literals {part?}` read (or a `literals` section in an existing
  model check) walking every feature field that has an `*_expr` twin and no
  expression, reporting `{feature_id, field, value, variables: [names]}`.
- Values compared within the model's length tolerance; unitless counts and
  angles compared only against variables of the same kind.
- A `describe` row with `returns`, and a line in the modeling-workflow
  prompt saying to run it after building a part from literals.

It shares its field walk with `extract_variables`
([extract-variables.md](extract-variables.md)); build whichever lands second
on the other's walker.

## Files

`src/remote/RemoteModeling.cpp` (the read and its `MethodDoc` row), the
feature field walk shared with `extract_variables`, `tools/designer_mcp.py`
(the workflow prompt).

## Verification

A unit or smoke case: a part with a variable `wall_r = 52` and an extrude
whose radius is the literal 52 is reported; the same extrude with
`distance_expr: "wall_r"` is not; a literal 52 with no variable of that value
is not; a count of 52 teeth is not reported against a length variable.
