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

`RemoteServer::HandleSemantic` (`RemoteDispatch.cpp`) handles the methods the
server owns (`batch`, `describe`, `ping`, `get_state`, camera, tabs, …) and
then offers the request to each domain module in turn —
`Remote{Modeling,Project,Assembly,Motion,Inspection,Drawings,Configurations,SheetMetal,Tube,Simulation,Cam,Catalog}.cpp`,
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
| `params` | accepted top-level keys, comma-separated; `""` for none; `"*"` for a free-form spec the handler reads opaquely (then the strict-key check is off) |
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

- **Strict keys.** A key the handler never read is refused, with a
  "did you mean" from an edit distance over the keys it does read. For a
  single call the check runs after the handler (a method's vocabulary is
  learned by which keys it reads), so a call carrying an extra junk key still
  applies its recognised keys before the error; a misspelled required key
  fails before any mutation. Inside `batch` the refusal rolls the batch back.
- **Strict feature JSON.** `add_feature` / `modify_feature` build the
  feature, serialize it back, and refuse any request key not in the round
  trip or the type's documented fields ("unknown field 'mode' for Box"). A
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
`tube_schedule`, `check_fit`, `list_tolerances` and the sheet-metal reads
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
  `verify_assertions`, `check_drawing`, `cam_verify`, `check_layout`, a FEM
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
  whole batch back and the refusal names the call, the reason, how many edits
  were undone and how many later calls did not run. With `false`, errors are
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
  and unknown methods — each checked before anything runs, so the document is
  unchanged.

The gain is largest for edits early in a long history, where each separate
call would rebuild everything after it; appends are already incremental.
`batch_smoke` gates the regeneration count, the single undo step, rollback on
a failed call and on a rejected key, `stop_on_error: false`, `$ref` and its
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
