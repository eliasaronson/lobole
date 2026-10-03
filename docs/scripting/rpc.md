# The remote RPC server

`designer --remote <port>` serves newline-delimited JSON-RPC over TCP:

```
request:   {"id": 1, "method": "add_feature", "params": {...}}\n
response:  {"id": 1, "result": {...}}\n
refusal:   {"id": 1, "error": "...", "diagnostic"?: {...}}\n
```

Instances, modes and clients are in [README.md](README.md). This page is the
server's contract: how a method is found, what its response carries, and what
a refusal must say.

## Dispatch

`RemoteSession.cpp` owns the socket: one listening socket bound in `Start`,
one reader thread per connection (up to eight), all feeding one queue.
`ProcessPending` (`RemoteUiActions.cpp`) drains the queue on the main thread,
once per frame in the GUI and once per tick headless, so a handler always
runs against a coherent document. Screenshots, `render_sheet` and UI actions
span frames; while one is in flight every later request waits behind it.
A CAM request whose passes are not cut yet is PARKED instead: only its own
connection's later requests wait behind it
([architecture.md](../architecture.md#requests-that-wait)).

`RemoteServer::HandleSemantic` (`RemoteDispatch.cpp`) handles the methods the
server owns (`batch`, `describe`, `ping`, `get_state`, camera, tabs, …) and
then offers the request to each domain module in turn —
`Remote{Modeling,Project,Assembly,Motion,Inspection,Drawings,Configurations,SheetMetal,Tube,Simulation,Cam,Catalog,Requirements,Templates}.cpp`,
each a `HandleXxxRpc(state, [doc], method, params, out) -> bool`. What no
module handles is a UI action (queued on the ImGui Test Engine, one at a
time), a capture, or `unknown method`.

A new RPC goes in its domain module, with a row in that module's `MethodDoc`
table beside the branch.

## The `MethodDoc` table

One row per method (`src/remote/RemoteHelpers.h`):

| column | meaning |
|---|---|
| `name` | the method literal |
| `doc` | prose: what it does, the units, the traps |
| `params` | accepted top-level keys, comma-separated; `""` for none; `"*"` for a free-form spec the handler reads opaquely. The dispatcher refuses any other key before the handler runs; under `"*"` the handler checks its own |
| `flags` | what the dispatcher needs to know (below) |
| `returns` | the response's top-level keys with nesting shown (`bodies[]{name,volume}`, `total{mass_g}`), required for every `list_*`, `get_*` and `check_*` method |

The flags are stated only where they differ from the default:

| flag | meaning |
|---|---|
| `NeedsPart` | refuses unless the active tab is a part ("the active document is an assembly, not a part; switch to a part tab") |
| `NotInBatch` | reaches outside the document (a file, the git tree, the window, the tab), so a batch rollback could not take it back; refused inside `batch` |
| `Deferrable` | its result does not depend on freshly built geometry, so `batch` may defer the regeneration past it |
| `UiAction` | goes on the ImGui queue instead of running here |

The tables are the registry: `describe`, the part gate, the batch rules and
the "is this a known method" check all read them, so there is no second list
to fall out of step. An unknown method has no flags, which is why a typo
falls through to `unknown method` rather than being told to switch tabs.

The rows are kept true by source-scanning gates: `rpc_params_gate` (params vs
the keys the handler reads, both directions), `rpc_returns_gate` (the
`returns` column), `rpc_units_gate` (a parameter naming a physical quantity
carries a unit suffix such as `_mm` or `_deg`, or its row names the unit),
`rpc_names_gate` (below), and `rpc_gate_smoke` (a `NeedsPart` method touches
the part document, or the test records why it is gated anyway).

## `describe`

`describe` returns every method with its `doc`, `params` (or
`params_free_form: true`), `requires_part_doc` and `returns`. `{method}`
narrows the answer to that one row, with only `app` and
`document_format_version` beside it, and refuses an unknown name naming the
close matches. Without it, `describe` also publishes the vocabularies an agent otherwise has
to guess: `feature_types` and `sketch_entity_types` (each with its field
list), `constraint_kinds`, `constraint_value_units`, `mate_types`,
`relation_kinds`, `extrude_modes`, `extrude_end_modes`, `axis_kinds`,
`drawing_dim_kinds`, `material_slugs`, the `document_format_version`, and the
`inspection_contract` version and hash (below). The vocabularies come from
the same tables the parsers refuse against, so `describe` cannot list a value
the parser rejects. A feature type the parser accepts but `describe` does not
list fails `FeatureFromJson` itself (`test_serialization.cpp`).

`describe` is the source of truth for the SDK's generated surface and for the
MCP server's `help`. A row that promises something the handler does not do is
worse than a missing row, because an agent trusts it instead of guessing.

## Parameters

- **Strict keys, before anything runs.** `ProcessPending` checks every
  request's top-level keys against the method's row — before parking, before
  the part gate, before the handler, before a UI action or a capture — and
  refuses a key the row does not list with a "did you mean" from an edit
  distance over the row (`UndeclaredParamsError`). `params` that are not an
  object are refused there too. `batch` checks every call's keys the same
  way before the first call runs. The row can carry this because
  `rpc_params_gate` holds it equal to the keys the handler reads; a `"*"`
  row (a free-form spec) is the handler's to check, and `set_settings`
  checks all of its keys before applying any. A method with an inspection
  contract refuses through that first, for its structured diagnostic.
  What remains for the handler is a key the row lists that THIS call's path
  did not read (`target` on an op whose depths were typed): `RpcParams`
  still records the keys read, and such a call is refused after the handler
  — and rolled back, below.
- **Strict feature JSON.** `add_feature` / `modify_feature` build the
  feature, serialize it back, and refuse any request key not in the round
  trip or the type's documented fields ("unknown field 'mode' for Fillet"). A
  documented key whose value is the default, and so does not serialize back
  (`units: "mm"`), is accepted.
- **Wrong types.** A `json::exception` from a handler is reported as a
  refusal naming the method, what each key it was handed actually is, which
  keys the handler read, and where the vocabulary is — never nlohmann's own
  "type must be string, but is boolean".
- **Units.** A parameter carrying a physical quantity names its unit
  (`angle_deg`, `tolerance_mm`, `deflection_mm`) or has it stated in its row.
- **Names.** `id` is the id of the thing the method is named after
  (`remove_mate(id)`, `set_occurrence_visible(id)`). `<noun>_id` refers to
  something the method is not about (`set_material(feature_id)`).
  `sketch_id` is deliberately narrower than `feature_id`: it says the feature
  must be a Sketch, so passing an extrude fails at the call. A method that
  resolves a feature takes `feature_id` (`delete_feature` and `set_rollback`
  also keep their older `id` / `index`). `rpc_names_gate` holds this.

### Reads name their part

The app holds exactly one live part document: a tab's document is moved into
`AppState` when the tab activates and stashed when it leaves. Switching tabs
clears the selection, the hidden-body set, the measure session, simulation
mode and the explode state (`ui::ResetActiveTabUiState`) — on somebody's
running instance, that is their work.

So a read-only RPC takes `part` (a slot name, a stable part id, or a slot
index) and answers about that part without switching: `ResolvePartParam` and
`SubjectPart` in `RemoteHelpers.h`. `get_state`, `list_elements`, `frame`,
`mass_properties`, `measure`, `section`, `query_geometry`, `curve_points`,
`tube_schedule`, `check_fit`, `list_tolerances`, `get_print_process`,
`print_allowance` and the sheet-metal reads
(`get_sheet_metal_rules`, `check_sheet_metal`, `list_bends`, `export_dxf`)
all do; the SDK's read paths pass `part=` and never activate. With `part`,
`get_state`'s tab-wide fields (selection, camera, undo/redo) still describe
the active tab.

Writes act on the active tab. A part's undo stack is stashed with it, and
mutating a part that is not on screen raises what Ctrl+Z would then mean;
`batch` is the answer to a burst of edits. `tabless_read_smoke` builds two
parts, reads every path on the first while the second is active, and asserts
the active tab never changed.

## What a response carries

- **`regen`** on every mutating response: `{ok, failed[], warnings[]}`
  (`get_state` adds `executed` / `cached`). `ok` reflects errors only; a
  warning means the feature built but likely did not do what was meant (a cut
  that removed no material). A mutating RPC that leaves a feature broken says
  so in its own response, never only in a later `get_state` —
  `regen_honesty_smoke`. That includes `undo` and `set_active_configuration`.
- **`digest`** on every geometry-mutating response: per body `{name,
  created_by, volume, bbox_min, bbox_max, solids}`, and a `delta` against the
  state before the call: `{volume, added[], removed[]}`. Bodies key on
  `created_by`, so a zero-volume add, a body removed, or a body that split
  into two lumps is visible in the call that caused it.
- **`receipt`** on every RPC that returns a verdict — the clash sweeps of
  `interference` and `contact_report`, `check_geometry`, `check_fits`,
  `compare_geometry`, `run_motion_study`, `check_fasteners`,
  `verify_assertions`, `check_requirements`, `check_drawing`, `cam_verify`,
  `check_layout`, a FEM
  `run_study`: `{schema: "designer.receipt/1", standing, summary, claims[]}`
  (`src/core/Receipt.h`). A claim's verdict is `pass | fail | unverifiable`
  and its basis `predicted | verified`; the roll-up is `verified |
  provisional | failed | unverified`. An oracle that could not run (a
  truncated sweep, a pair over budget) is `unverifiable`, never a pass, so
  "could not check" cannot read as "clean".

## Refusals

An error is read once and must be actionable without another round trip.
A failed call carries, one plain sentence each:

1. **The measured condition** that failed — "index 99 out of range — there
   are 3 features (0..2)", not "out of range"; "no sketch with id 9999", not
   "no such sketch".
2. **The vocabulary to correct it** — the candidates that do exist
   ("Sketches here: 5 (Outline), 12 (Profile)") or the accepted values
   ("views: front, back, left, right, top, bottom, iso"), capped at about ten
   with "+N more". The first rejection carries the whole correct template;
   never one constraint per round trip.
3. **The remedy or discovery call** when the candidates do not fit inline —
   "switch tabs with set_active_part (see list_parts)".
4. **What state survived**, when it matters — "the document is unchanged".

Not allowed: "(see log)" — the caller cannot read the log; name the usual
cause and the check to run instead ("an OpenCASCADE boolean threw, usually on
an invalid or empty body; run check_geometry"). A bare "no such X" or "X
failed" with no value given and no candidates. A collapsed multi-cause error
("unknown, invalid, or taken") that does not echo the input.

An input the server does not recognise — an enum string, a feature type, a
mate type — is refused naming what is accepted, never replaced by a default
(CLAUDE.md, "Refuse, don't substitute").

Helpers: `remote::KnownList` (capped candidate list), `remote::IndexError`
(ranged index), `UnknownParamError`, `TypeErrorMessage`. A kernel
`Standard_Failure` that escapes a handler becomes "OpenCASCADE error: …"
instead of terminating the process. `papercuts_smoke` pins the shape of
representative refusals; when you improve one, pin it there.

### A refused call changes nothing

An error means nothing happened. A caller told "refused" fixes the call and
sends it again; if the first one wrote its file, switched the tab or edited
the job, the second does it twice — `cam_post {setup, path, _timeout}` wrote
its G-code and then answered "unknown parameter '_timeout'". So:

- **Keys are refused by the dispatcher**, against the row, before the
  handler runs (above).
- **A handler reads and checks every parameter before its first side
  effect.** Values are read into locals, ranges and enums checked, the
  files a call will write planned whole — and only then is anything
  written, created, started or switched. `export_members` plans every
  member's file (two members with one name, a file already there) before
  making the directory; `compare_geometry` reads both diff paths before
  writing either; `set_reference_image` edits a copy of the image.
- **An edit that refuses is rolled back.** While a semantic handler runs,
  `ProcessPending` holds marks on the active tab's histories (`EditMarks`:
  the part's command stack, the assembly's, drawing's and job's snapshot
  stacks). If the handler throws — a type error on its fifth key, a check
  after the edit it guards — or a key it was sent went unread, everything
  pushed since the marks is undone, dropped from redo, and the project's
  saved state is put back; the refusal says so ("The call was rolled back
  (1 edit(s) undone), so the document is as it was."). `batch` uses the
  same marks. It covers what goes through those histories: an edit made
  outside them (a field set directly on a feature) is the handler's to make
  only after its checks.
- **`NotInBatch` methods are not rolled back** — they reach outside the
  document (a file, a tab, the settings, the machine store, git), which no
  undo stack holds. They are the ones that must check first, and the flag
  is how a method says it is one of them: `export_brep`, `delete_part`,
  `set_keybinding`, `save_machine` and `delete_machine` carry it too. A key
  such a method left unread is still refused, with "The call had already
  run" in place of a rollback, because that is the truth. A failure after
  the effect undoes it where it can: `split_for_printing` and
  `topo_to_body` delete the tabs they made.

`refused_calls_smoke` holds the rule from outside: every method with an
unknown key (the refusal must name it, on an assembly tab — so not
"missing required parameter" or "switch to a part tab", which would mean
something ran first); every method with every key of the wrong type on a
part, assembly, drawing and job tab; and the calls that write files or move
tabs from a valid baseline with each key made wrong in turn. After every
refusal the world must be as it was: the saved project, the tabs, undo and
redo, the files under the app's dirs and its cwd, the settings, the machine
store, the background jobs.

## `batch`

`batch {calls: [{method, params}, ...], stop_on_error: true}` runs the calls
as one transaction:

- **One regeneration.** Consecutive `Deferrable` calls run under one
  `Document::BulkEdit`; any other call flushes the pending regeneration
  first, so a `measure` or `list_elements` inside a batch never answers from
  the geometry as it was before the batch. `regenerations` in the response
  counts the flushes.
- **One undo step.** The calls' commands are coalesced into one ("Batch of N
  calls"); on an assembly tab, one snapshot.
- **Rollback.** With `stop_on_error` (default), a call that throws rolls the
  whole batch back — the part's commands and the assembly's, drawing's and
  job's snapshots, through the same `EditMarks` a single call uses
  ([above](#a-refused-call-changes-nothing)) — and the refusal names the
  call, the reason, what was undone and how many later calls did not run. With `false`, errors are
  reported in place and the rest run. A feature that fails to *build* is not
  a failed call; it is reported in the final `regen`, as on its own.
- **`$ref`.** `{"$ref": n}` anywhere in a call's params is the result of the
  n-th call (1-based) — its `id` by default, `{"$ref": n, "key": "..."}` for
  another field — so a sketch and the extrude that names it go in one batch.
  References point backward only; one to a call that failed, or to a key it
  did not return, is refused naming what it did return.
- **Results.** `{calls, results[{call, method, result}], regenerations,
  failed?, regen}`. Each call's own `regen` and `digest` are stripped: under a
  deferred regeneration they would describe a document that has not been
  rebuilt. Geometry is answered once, by the batch's `regen`.
- **Refused inside a batch:** another `batch`, any `NotInBatch` method
  (screenshots, exports, project and git verbs, `undo`/`redo`), UI actions,
  unknown methods, and a key outside a call's row — each checked before
  anything runs, so the document is unchanged.

The gain is largest for edits early in a long history, where each separate
call would rebuild everything after it; appends are already incremental.
`batch_smoke` gates the regeneration count, the single undo step, rollback on
a failed call, a rejected key refused before any call runs, `stop_on_error: false`, `$ref` and its
refusals, a mid-batch query seeing current geometry, and an assembly batch.

## Typed inspection contracts

Two methods — `geometry_snapshot` and `validate_snapshot` — carry
machine-readable request and result schemas. `api/inspection-contracts.json`
is the source; `python3 tools/generate_inspection_contracts.py` regenerates
the embedded server schemas (`src/remote/InspectionContracts.generated.h`)
and the Python records and facade (`tools/designer_inspection.py`). The
generated files are checked in and `inspection_contracts_generated` fails
when they are stale.

`describe` publishes each such method's `request_schema` / `result_schema`,
and `inspection_contract {version, hash, dialect}` independently of the
document format version. The server validates raw requests against the
schema (`ValidateInspectionRequest`); the SDK validates them before sending
and refuses to talk to a server whose contract hash differs
(`unsupported_contract`). The schema dialect is a subset of JSON Schema
2020-12 — `type`, `properties`, `required`, `additionalProperties: false`,
`enum`, `minimum`, `maximum`, `minLength`, `items`, `minItems`, `maxItems`,
`uniqueItems`, `default` — and the generator refuses any other keyword rather
than publishing a constraint nothing enforces.

A contract refusal is a `ContractError` whose `diagnostic` has `operation`,
`code`, `path`, `expected`, `observed`; batch failures keep the nested
diagnostics, including when the batch rolls back. Other RPCs' errors may
carry no diagnostic.

Snapshot semantics are in [sdk.md](sdk.md#measurement-snapshots). Extending
contracts to the rest of the surface is on the
[roadmap](../roadmap.md#6-the-agent-interface).
