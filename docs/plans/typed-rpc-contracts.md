# Typed RPC contracts

## Problem

Two methods (`geometry_snapshot`, `validate_snapshot`) have machine-readable
request schemas. Every other method is a `MethodDoc` row whose `params`
column is a list of key NAMES: the dispatcher refuses an unknown key before
the handler runs, but a missing key, a string where a number goes, or an
enum typo is found inside the handler — after it may have started work — and
comes back as prose (`missing required parameter 'x'`, a rewritten
`json::exception`). Nothing tells a client which keys are required or what
type each is, so the SDK checks key names and nothing else, and a script has
no static types at all. And a write built from a geometry snapshot has no way
to say which geometry it was built from, so a reference that went stale
between the read and the write is applied to whatever is there now.

## The shape

### One source: the `params` column, typed

A contract is declared IN the row's `params` column, which already is the
key list every gate and the dispatcher read. A contracted row types every
key; an uncontracted row stays a bare name list and behaves exactly as
today. There is no second table to fall out of step with the row.

```
"feature_id?:int>=1, index?:int>=0, feature:object"
"name:string, expression:string|number, scope?:'project'|'part', comment?:string"
"type?:@mate_types, a:object, b:object, rotation_deg?:number, path?:int[]|null"
```

- `name` is required, `name?` optional.
- Types: `int`, `number`, `bool`, `string`, `object`, `null`, `any`; an array
  is `T[]` or `T[N]` (fixed length).
- `|` is a union; `'lit'` a literal (an enum is a union of literals).
- `@vocab` is one of `describe`'s published vocabularies (`mate_types`,
  `views`, `material_slugs`, …), resolved from the same table the parser
  refuses against — so the contract cannot list a value the handler rejects.
- A range follows a numeric type: `int>=0`, `number>0`, `number>=0<=1`.

A row is contracted when any key carries a `:`; then every key must.
`expect_revision?:revision` is the one key the dispatcher, not the handler,
consumes (below).

`RpcContract.{h,cpp}` (in `designer_core`, so it is unit-tested) parses a
column into a contract and renders it as the JSON Schema subset the
inspection contracts already use, extended with `anyOf`, type arrays and
`exclusiveMinimum`/`exclusiveMaximum`. Vocabulary references resolve through
a callback the dispatcher supplies. Validation is the existing
`ValidateInspectionValue`, so both families refuse through one validator.

### Where it runs

- `ProcessPending` validates a contracted request before parking, the part
  gate, the handler, a UI action or a capture — the same place the
  unknown-key check runs. `HandleSemantic` validates again for a batch's
  calls after `$ref` resolution; the batch pre-check validates every call
  that carries no `$ref` before the first call runs.
- `describe` publishes `request_schema` for every contracted method (as it
  does for the two inspection methods), the `params` list stays plain names,
  and `contract: true` marks the row. The SDK already validates any method
  with a `request_schema` before sending.

### Structured errors

A contract refusal is a `ContractError`: the readable sentence in `error`
and `diagnostic {operation, code, path, param, expected, observed,
accepted?}`. `code` is `missing_argument`, `unknown_argument`,
`invalid_argument` (the inspection codes) or `stale_revision`; `param` is the
top-level key; `accepted` lists the values an enum or vocabulary takes. The
sentence names the method, the key, what it must be and what it got
("add_mate: 'type' must be one of fastened, revolute, …; got \"revolut\"").
Two uncontracted refusals gain the same diagnostic: an unknown key (every
method), and a missing required key read through `RpcParams::at`.

### Typed SDK stubs

`tools/generate_rpc_contracts.py` parses the rows from source (the
`method_doc_scan` reader the gates share) and writes `tools/designer_rpc.py`,
checked in: a `TypedDict` per contracted request and a `RpcSurface` class
with one typed method per contracted RPC. `App` inherits it under
`TYPE_CHECKING` only, so the runtime surface stays the generated `describe`
one and mypy sees `app.add_mate(type="revolut")` as an error. The module also
carries each method's JSON Schema, so a test can compare the Python parser's
reading of a row with the server's.

### The revision guard

A part write may carry `expect_revision`: the `revision` object
`geometry_snapshot` returns (`{session, part_id, instance, version}`), which
also comes back from `get_state` and from every guarded write. The
dispatcher compares it with the active part's revision before the handler
runs and refuses `stale_revision` — naming the current revision — when the
part was replaced, reopened or regenerated since, or when the revision names
another part. A guarded call answers with the new `revision`, so a chain of
writes guards each against the last. `batch` takes `expect_revision` once,
checked before its first call. Scope: the contracted writes that act on the
active part (`add_feature`, `modify_feature`, `delete_feature`,
`set_material`) and `batch`.

### Methods contracted first

The most-called methods across tests/, generators/, benchmarks/ and the SDK,
omitting CAM (its keys go through table helpers) and free-form rows:

- server: `get_state`, `list_parts`, `add_part`, `add_assembly`,
  `rename_part`, `undo`, `redo`, `ping`, `describe`, `batch`, `view`
- project: `new_project`, `open_project`, `save_project`, `set_variable`,
  `list_variables`, `export_step`
- modeling: `add_feature`, `modify_feature`, `delete_feature`, `set_material`
- assembly: `set_active_part`, `delete_part`, `add_occurrence`,
  `set_occurrence_transform`, `add_mate`, `set_mate`, `remove_mate`,
  `list_occurrences`, `list_mates`, `solve_assembly`, `bom`
- inspection: `frame`, `list_elements`, `mass_properties`, `measure`,
  `check_geometry`, `interference`
- configurations: `add_configuration`, `set_active_configuration`,
  `list_configurations`
- drawings: `create_drawing`, `export_drawing`
- studies: `run_motion_study`, `run_study`

## Files

- `src/remote/RpcContract.{h,cpp}` (new, in designer_core),
  `src/remote/InspectionContracts.h` (dialect extensions, `param`/`accepted`,
  readable messages), `src/remote/RemoteHelpers.h` (column comment, the
  revision helper), `src/remote/RpcParams.h` (structured missing-key error)
- `src/remote/RemoteDispatch.cpp` (validation, describe, vocabulary table,
  batch, the guard, `get_state.revision`), `RemoteUiActions.cpp` (validation
  before parking), `RemoteInspection.cpp` (the shared revision)
- the rows in `Remote{Dispatch,Project,Modeling,Assembly,Inspection,
  Configurations,Drawings,Motion,Simulation}.cpp`
- `tools/method_doc_scan.py` (plain names + the typed contract),
  `tools/generate_rpc_contracts.py`, `tools/designer_rpc.py`,
  `tools/designer_contracts.py` (dialect), `tools/designer_sdk.py`
- `tests/test_rpc_contract.cpp`, `tests/rpc_params_gate.py`,
  `tests/rpc_contracts_smoke.py`, `tests/typing/rpc_*.py`,
  `tests/inspection_typing_gate.py`, `tests/CMakeLists.txt`
- docs: `docs/scripting/rpc.md`, `docs/scripting/sdk.md`,
  `docs/scripting/mcp.md` if the error text changes, `docs/roadmap.md`

## Verification

- `test_rpc_contract.cpp`: the grammar (every form, every malformed form
  refused naming the column), the schema it renders, the messages.
- `rpc_params_gate` grows three checks on contracted rows: every key typed;
  a key the handler reads with `at()` only is required and one read through
  `contains`/`value` is optional (`at` behind a `contains` counts as
  optional); every literal of an enum appears as a string literal in the
  branch.
- `rpc_contracts_generated`: regenerate `designer_rpc.py` and diff.
- `rpc_contracts_smoke` (app): every contracted method's `describe` schema
  equals the Python parser's; every `@vocab` is a published vocabulary; for
  each contracted method a missing required key, a wrong type, an enum miss
  and a range miss are refused with the diagnostic and change nothing; an
  unknown key on an uncontracted method carries a diagnostic; the guard
  passes a current revision, refuses a stale one, a foreign part's and a
  stale batch, and chains through the returned revision.
- `inspection_typing_gate` also type-checks a valid and a seeded-invalid
  script against `designer_rpc.py`.
- `refused_calls_smoke`, `sdk_smoke`, `inspection_contracts_smoke` stay green;
  the full headless suite, the GUI suite, `pygui` and the generators label.
