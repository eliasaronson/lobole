# `extract_variables`, and reading the selection first

## Problem

Cleaning up a part — pulling hard-coded dimensions out of its features into
named project variables with expressions, and renaming features by what they
do — is the parameters-first workflow the modeling prompt teaches, and it has
no verb. An agent handed an imported or hand-built part does it one
`modify_feature` at a time, re-reading each feature's JSON dialect.

Separately, a request that says "fillet this" or "make the hole here" refers
to what the user selected. `get_state` already reports the selection with
element names (`selection`, `selection_items[]`), but the
`modeling-workflow` prompt never says to read it first, so an agent guesses
which element was meant.

## Fix

1. **`extract_variables {feature_ids?, prefix?, apply?}`** in
   `RemoteModeling.cpp`: walk the named features' (default: all) literal
   dimensional fields that have an `*_expr` twin, propose a variable per
   distinct value (named from the feature and field, e.g.
   `boss_extrude_distance`), and with `apply: true` create the variables and
   set each field's expression, as one undo step through the `batch`
   machinery. Equal literals in different features are proposed as one
   variable only when asked (`merge_equal: true`), since equal is not the
   same as related. The response lists every field changed and every literal
   left alone, with the reason (no expression twin, a unitless count).
   A `describe` row with units named.
2. **One sentence in the `modeling-workflow` prompt**: when a request says
   "this", "here" or "the selected", read `get_state`'s selection before
   acting, and refuse to guess when nothing is selected.

## Files

`src/remote/RemoteModeling.cpp` (handler + `MethodDoc` row),
`tools/designer_mcp.py` (the prompt sentence; `help` picks the row up from
`describe`), `tools/designer_sdk.py` if a composition is wanted,
`tests/` (a new smoke), `docs/scripting/`.

## Verification

- A smoke builds a part from literals, runs `extract_variables` without
  `apply` and asserts nothing changed, then with `apply` and asserts the
  geometry is identical, every proposed field now carries an expression, one
  undo restores the literals, and changing one new variable moves exactly
  the fields it drives.
- `rpc_params_gate` and `rpc_units_gate` pass on the new row.
