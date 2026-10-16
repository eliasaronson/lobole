# The remote RPC server

`lobole --remote <port>` serves newline-delimited JSON-RPC over TCP:

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
A CAM request whose passes are not cut yet, and a drawing request whose
views are projecting (windowed or `--offscreen`), is PARKED instead: only
its own connection's later requests wait behind it
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

A line that is not JSON — or that holds a number past a double, `1e400`,
which the parser refuses rather than reading as infinity, or nests deeper
than 256 levels (`kMaxRequestDepth`: a parsed value is copied recursively,
and one line nested 100 000 deep overflowed the reader thread's stack) — is
answered
`{id: null, error: "parse: …"}`: the server cannot know the id. With one
request in flight it is that request's answer, and `lobole/client.py`
raises it as such rather than waiting for its own id. Every message goes out
as one line of valid UTF-8: a string holding bytes that are not (a parse
error quoting what it choked on) is written with U+FFFD in their place, never
left to throw on a reader thread.

## The `MethodDoc` table

One row per method (`src/remote/RemoteHelpers.h`):

| column | meaning |
|---|---|
| `name` | the method literal |
| `doc` | prose: what it does, the units, the traps |
| `params` | accepted top-level keys, comma-separated; `""` for none; `"*"` for a free-form spec the handler reads opaquely. The dispatcher refuses any other key before the handler runs; under `"*"` the handler checks its own. A row may type its keys, and is then a [contract](#typed-contracts) |
| `flags` | what the dispatcher needs to know (below) |
| `returns` | the response's top-level keys with nesting shown (`bodies[]{name,volume_mm3}`, `total{mass_g}`), required for every `list_*`, `get_*` and `check_*` method |

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
`rpc_params_gate` also holds a typed row to its handler
([below](#typed-contracts)).

## `describe`

`describe` returns every method with its `doc`, `params` (or
`params_free_form: true`), `requires_part_doc` and `returns`, and a
`request_schema` for a method with a [contract](#typed-contracts). `{method}`
narrows the answer to that one row, with only `app`, `api_version` and
`document_format_version` beside it, and refuses an unknown name naming the
close matches. Without it, `describe` also publishes the vocabularies an agent otherwise has
to guess: `feature_types` (each with its field list and its typed `contract`,
[below](#typed-feature-specs)) and `sketch_entity_types` (each with its field
list), `constraint_kinds`, `constraint_value_units`, `mate_types`,
`relation_kinds`, `extrude_modes`, `extrude_end_modes`, `axis_kinds`,
`drawing_dim_kinds`, `material_slugs` and `materials` (each slug's density,
modulus, Poisson's ratio, yield and interlayer strength, a field the library
does not know left out), `views`, the `document_format_version`, and the
`inspection_contract` version and hash (below). The vocabularies come from
the same tables the parsers refuse against, so `describe` cannot list a value
the parser rejects. A feature type the parser accepts but `describe` does not
list fails `FeatureFromJson` itself (`test_serialization.cpp`).

### The API version

`describe` publishes `api_version` (`src/remote/ApiVersion.h`), the version
of the RPC surface, independent of the document format's. It is raised when
a public method or one of its parameters is removed or renamed — the changes
that break a caller — and not when one is added: each row carries `since`,
the version it arrived in. `api/rpc-surface.json` is the surface as last
published, and `api_surface_gate` holds the rows to it: a method or a key
that disappears needs `kApiVersion` raised once the surface is released
(`released: true`), and the snapshot refreshed either way
(`tests/api_surface_gate.py --update`), so the next removal has something to
be caught against. Before the first release the version is 1 and is still
being written. The Python package is generated against one version
(`lobole.rpc.API_VERSION`), and `App` warns when the app reports another.

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
  contract refuses through that first. The refusal is
  [structured](#structured-errors) either way.
- **Typed values, before anything runs.** A method with a
  [contract](#typed-contracts) has its values checked in the same place:
  a missing key, a wrong type, an enum or a range is refused before the
  handler.
  What remains for the handler is a key the row lists that THIS call's path
  did not read (`target` on an op whose depths were typed): `RpcParams`
  still records the keys read, and such a call is refused after the handler
  — and rolled back, below.
- **Strict feature JSON.** `add_feature` / `modify_feature` build the
  feature, serialize it back, and refuse any request key not in the round
  trip or the type's documented fields ("unknown field 'mode' for Fillet"). A
  documented key whose value is the default, and so does not serialize back
  (`units: "mm"`), is accepted.
- **Wrong types.** On a method without a contract, a `json::exception` from
  a handler is reported as a refusal naming the method, what each key it
  was handed actually is, which keys the handler read, and where the
  vocabulary is — never nlohmann's own "type must be string, but is
  boolean".
- **Integers are not narrowed.** A handler reads an integer parameter
  through `remote::IntParam` (`src/remote/RemoteHelpers.h`), which refuses
  what is not a whole number in range and names the value sent: `.get<int>()`
  read 2^64 as INT_MIN ("body index -2147483648 out of range") and wrapped
  2^32 + 1 to body 1. `test_conventions` fails on a bare `.get<int>()` under
  `src/remote/`.
- **Units.** A parameter carrying a physical quantity names its unit
  (`angle_deg`, `tolerance_mm`, `deflection_mm`) or has it stated in its row.
- **Names.** `id` is the id of the thing the method is named after
  (`remove_mate(id)`, `set_occurrence_visible(id)`). `<noun>_id` refers to
  something the method is not about (`set_material(feature_id)`).
  `body_name` names one body as `get_state` lists it (`set_material`,
  `set_appearance`); a name two bodies share is refused naming both.
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
  created_by, volume_mm3, bbox_min, bbox_max, solids}`, and a `delta` against the
  state before the call: `{volume_mm3, added[], removed[]}`. Bodies key on
  `created_by`, so a zero-volume add, a body removed, or a body that split
  into two lumps is visible in the call that caused it.
- **`nonfinite`** on any message holding NaN or infinity: JSON has no
  spelling for them, so each is written `null`, and the message gains a
  top-level `nonfinite: ["/result/bodies/0/volume_mm3", …]` of JSON pointers to
  where they were; the app logs a warning naming the RPC. The call is not
  turned into an error after the fact — a mutating one has happened.
  `lobole/client.py` raises `NonFiniteError` on it in strict mode
  (`Client(strict=True)`, or `LOBOLE_REMOTE_STRICT=1`, which
  `tests/smoke_app.py` sets, so every smoke is strict) and warns otherwise.
- **`receipt`** on every RPC that returns a verdict — the clash sweeps of
  `interference` and `contact_report`, `check_geometry`, `check_fits`,
  `compare_geometry`, `run_motion_study`, `check_fasteners`,
  `verify_assertions`, `check_requirements`, `check_drawing`, `cam_verify`,
  `check_layout`, `check_print`, a FEM
  `run_study`: `{schema: "designer.receipt/1", standing, summary, claims[]}`
  (`src/core/Receipt.h`). A claim's verdict is `pass | fail | unverifiable`
  and its basis `predicted | verified`; the roll-up is `verified |
  provisional | failed | unverified`. An oracle that could not run (a
  truncated sweep, a pair over budget) is `unverifiable`, never a pass, so
  "could not check" cannot read as "clean".

## Long calls run as jobs

A call that can take minutes takes `{async: true}` and answers at once with
the id of a background job
([the registry](../ui/long-operations.md#the-registry)):
`run_study`, `mesh_study`, `run_motion_study`, `export_motion_video`,
`cam_toolpath`, `update_drawing`, `check_requirements` and `contact_report`.
`background_jobs` lists every running job with its kind and `fraction`. Each
domain has a status call that reports a running job and, once it has ended,
how (`done | cancelled | failed`) and its result: `study_status {job}` for
the study jobs, `motion_status`, `contact_status {job}` for a contact
report. A job started over RPC that belongs to a study or a contact report is
cancelled when its connection closes, unless the call said `outlive_client:
true`; `cancel_study {job}` or `cancel_background_job {kind}` cancels one by
hand. The instance answers other calls while a job runs.

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
(ranged index), `UnknownParamError`, `TypeErrorMessage`, and for reading
one: `remote::Vec3Param` / `Vec2Param` / `QuatParam` / `NumbersParam` (a
point, a direction, a quaternion: "'position' must be [x, y, z] (3 numbers);
got []") and `remote::ElementParam` (a `{feature_id, element}`). A deliberate
refusal throws `std::runtime_error`; anything else that reaches the
dispatcher is the handler falling over and is labelled `internal`
([structured errors](#structured-errors)). A kernel `Standard_Failure` that
escapes a handler becomes "OpenCASCADE error: …", also `internal`, instead of
terminating the process. `papercuts_smoke` pins the shape of
representative refusals; when you improve one, pin it there.

### A refused call changes nothing

An error means nothing happened. A caller told "refused" fixes the call and
sends it again; if the first one wrote its file, switched the tab or edited
the job, the second does it twice — `cam_post {setup, path, _timeout}` wrote
its G-code and then answered "unknown parameter '_timeout'". So:

- **Keys are refused by the dispatcher**, against the row, before the
  handler runs (above), and so are a contracted method's values.
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
  document (a file, a tab, the settings, the machine store, the print
  profile store, git), which no undo stack holds. They are the ones that must
  check first, and the flag is how a method says it is one of them:
  `export_brep`, `delete_part`, `set_keybinding`, `save_machine`,
  `delete_machine`, `save_print_profile` and `delete_print_profile` carry it
  too. A key
  such a method left unread is still refused, with "The call had already
  run" in place of a rollback, because that is the truth. A failure after
  the effect undoes it where it can: `split_for_printing` and
  `topo_to_body` delete the tabs they made.

- **A string holding a NUL byte is refused by the dispatcher**, anywhere in
  the params, before any handler runs (`code: invalid_argument`, the pointer
  in `path`): every file name, label and lookup below a handler reads a C
  string up to its first NUL, so `"nul\0byte"` named something else.
- **A file a call writes is checked before it is written.** Both atomic
  writers (`DocumentIO::WriteTextAtomic` and the binary one) refuse first,
  through `DocumentIO::AtomicTargetOk`, a path that names no file, a
  directory, a file name past 255 bytes or a directory that does not exist,
  and remove their temporary file when the rename fails; the exports and
  `compare_geometry` ask the same question of every path before they mesh
  or write anything.

`refused_calls_smoke` holds the rule from outside: every method with an
unknown key (the refusal must name it, on an assembly tab — so not
"missing required parameter" or "switch to a part tab", which would mean
something ran first); every method with every key of the wrong type on a
part, assembly, drawing and job tab; and the calls that write files or move
tabs from a valid baseline with each key made wrong in turn. After every
refusal the world must be as it was: the saved project, the tabs, undo and
redo, the files under the app's dirs and its cwd, the settings, the machine
store, the background jobs. `save_print_profile` validates the whole profile
(`printfit::ValidateProfile`) before its file is written, so a refused one
leaves the print profile store as it was
([print profiles](../tolerances/README.md#print-profiles)).

### The hostile sweep

`refused_calls_smoke` sends the wrong TYPE. `tests/hostile_params_smoke.py`
sends values of the right type that are still wrong — the ones that reach a
handler and the kernel — to every method of `describe`, every key of its row,
one value at a time: `1e400`, `1e308`, `-1`, `0`, `-0.0`, `2^64`, `""`, a
64 KB string, `[]`, `{}`, `null` and nesting 64 deep (`--full` adds ±1e308,
1e-308, 2^53+1, -2^63, 1.5, "NaN", an emoji, a lone surrogate, a 1 MB string,
`[0, 0, 0]`, nesting 10 000 deep, index 999999 and more). A typed row's keys
also get the extremes their own contract accepts (an `int>=0` gets 0, 2^31
and 2^63-1; an enum each literal with one character changed), and a path key
gets "", ".", "/", a directory, a file under a directory that does not exist,
a 5000-character name and a NUL byte, all inside the scratch dir. Each value
goes alone and inside a valid baseline where the method has one — the
outside-the-document calls `refused_calls_smoke` holds, then one built from a
typed row's `request_schema` with its ids from the fixture, kept only if it is
accepted as written. It also sets every field of every fixture feature through
`modify_feature`, sends degenerate sketch entities, the value as the whole
params, hostile `batch` calls and `$ref`s, and raw lines on a bare socket.

The fixture is `refused_calls_smoke`'s (`tests/smoke_world.py`, which holds
`World`, `tree`, `changed`, `build_fixture` and the outside baselines for
both) with a representative feature set on the part. A failure is: the
instance died (the case, the app's log and its crash report are printed, and
the sweep goes on on a fresh instance) or did not answer in 30 s (killed by
its pid); an answer that is not one object with the request's id and exactly
one of result or error; `code: internal`; a `nonfinite` field; a refusal that
changed the world (dirty against the fixture saved clean, the tabs, the parts'
identities, a job started, the files under the cwd and the scratch dir, and
for a `NotInBatch` method the whole `World`); a refusal with no measured
condition. An accepted hostile call is not a failure — `1e308` is a legal
depth — and the fixture is put back. Every open of the fixture waits out the
jobs the open itself starts (the job tabs' box warm,
[cam/gui.md](../cam/gui.md#job-tab-frame-cost)) before the world is read
again: a warm still running at one read and gone at the next — across a
refused outside-the-document call, or across the end-of-shard comparison of
two opens — read as the world moving, about once a suite run on a loaded
machine. A finding not fixed yet is listed in the
sweep's `KNOWN` table with its [known-bugs](../known-bugs.md) entry and
reported, not failed.

The methods are sharded by a stable hash of their name (the feature types'
fields, the sketch entities, `batch` and the raw lines are units of their
own), one ctest each: `hostile_params_<k>` (headless suite, so CI) and
`hostile_params_full_<k>` (`slow`; `tools/ci-local.sh --asan` runs it, which
is where UBSan reports a value narrowed past its type). Each prints its calls,
how many were accepted and refused by the dispatcher or the handler, its
resets and time, and its coverage: how many handlers a hostile value reached
through a valid baseline, and how many feature types the fixture covers.
`hostile_params_selftest` shows the sweep sees what it claims: `debug_fault`
(refused unless `LOBOLE_ALLOW_DEBUG_CRASH=1`) plants an internal error, a NaN
answer, a write before a refusal, an edit then a fall, a null write inside
an OpenCASCADE signal scope on the handler's thread and on a worker (which
must answer, not crash), and a kernel throw inside a `batch`, and `debug_crash` a SIGSEGV whose crash report must name
it; each must come out as its class, and the deliberate refusals as nothing.
It then opens the fixture twice on an instance whose jobs are held 3 s
(`--gui-test-job-latency-ms`) and requires the two worlds to agree.

## `batch`

`batch {calls: [{method, params}, ...], stop_on_error: true}` runs the calls
as one transaction:

- **One regeneration.** Consecutive `Deferrable` calls run under one
  `Document::BulkEdit`; any other call flushes the pending regeneration
  first, so a `measure` or `list_elements` inside a batch never answers from
  the geometry as it was before the batch. So does an `add_feature` of a
  primitive placed `on` a face or a datum, which reads that geometry when
  it is added: a Box and a Cylinder on its face build in one batch as they
  do one call at a time. `regenerations` in the response counts the flushes.
- **One undo step.** The calls' commands are coalesced into one ("Batch of N
  calls"); on an assembly tab, one snapshot.
- **Rollback.** With `stop_on_error` (default), a call that throws rolls the
  whole batch back — the part's commands and the assembly's, drawing's and
  job's snapshots, through the same `EditMarks` a single call uses
  ([above](#a-refused-call-changes-nothing)) — and the refusal names the
  call, the reason, what was undone and how many later calls did not run.
  Every exception counts, a kernel `Standard_Failure` included; a call that
  fell over carries `code: internal` in its `failures[].diagnostic`. A batch
  refused with nothing undone leaves the project saved if it was: its
  deferred regeneration moves no history. With `false`, errors are
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
  unknown methods, a key outside a call's row, and — under
  `stop_on_error` — a value a call's [contract](#typed-contracts) refuses,
  unless a `$ref` stands in it (that call is checked when it runs). Each is
  checked before anything runs, so the document is unchanged, and the
  refusal's diagnostic names the `call`.
- **Guarded.** `expect_revision` on the batch is checked once, before the
  first call ([the revision guard](#the-revision-guard)).

The gain is largest for edits early in a long history, where each separate
call would rebuild everything after it; appends are already incremental.
`batch_smoke` gates the regeneration count, the single undo step, rollback on
a failed call, a rejected key refused before any call runs, `stop_on_error: false`, `$ref` and its
refusals, a mid-batch query seeing current geometry, and an assembly batch.

## Typed contracts

A row whose `params` column types its keys is a contract. It is declared in
the row and nowhere else — the column every gate and the dispatcher already
read — so there is no second table to drift from it:

```
"feature_id?:int>=0, feature:object, expect_revision?:revision"
"name:string, expression:string|number, scope?:'project'|'part', comment?:string"
"type?:@mate_types, a:object, b:object, ball_rotation?:number[4], derive?:'twist'|'pose'"
```

- `name` is required, `name?` optional. A typed row types every key.
- Types: `int`, `number`, `bool`, `string`, `object` (opaque: its own
  parser checks inside it, e.g. a feature spec), `null`, `any`, `revision`;
  an array is `T[]` or `T[N]`.
- `|` is a union and `'x'` a literal, so an enum is a union of literals.
- `@name` is one of `describe`'s published vocabularies (`mate_types`,
  `relation_kinds`, `material_slugs`, `views`), resolved from the table the
  parser refuses against; a vocabulary can carry the remedy for its usual
  mistake (a coupling is a relation, not a mate), which ends the refusal.
- A range follows a numeric type: `int>=0`, `number>0`, `number>=0<=1`.

A row with no parameters (`""`) is a complete contract already. Typed today:
`add_assembly`, `add_configuration`, `add_feature`, `add_mate`,
`add_occurrence`, `add_part`, `batch`, `check_geometry`, `create_drawing`,
`delete_feature`, `delete_part`, `describe`, `export_drawing`,
`export_step`, `frame`, `get_state`, `interference`, `list_configurations`,
`list_elements`, `list_occurrences`, `mass_properties`, `measure`,
`modify_feature`, `open_project`, `remove_mate`, `rename_part`,
`run_motion_study`, `run_study`, `save_project`,
`set_active_configuration`, `set_active_part`, `set_mate`, `set_material`,
`set_occurrence_transform`, `set_variable`, `view` — the most-called
methods across the tests, generators, benchmarks and the SDK. CAM's rows,
whose keys go through field tables, and the free-form rows are not typed.

`src/remote/RpcContract.{h,cpp}` parses a column and renders it as the JSON
Schema subset [below](#inspection-contracts) (plus `anyOf`, `null`,
`exclusiveMinimum`/`exclusiveMaximum` and `description`). The dispatcher
validates a contracted request in `ProcessPending`, right after the
unknown-key check and before anything else; `HandleSemantic` validates again
for a batch's calls once their `$ref`s are resolved. `describe` publishes the
schema as the method's `request_schema`, and its `params` stays a list of
names. One validator (`ValidateInspectionValue`) serves both families, in a
fixed order — unknown keys, then missing ones, then each value in key order —
which `python/lobole/contracts.py` repeats client-side.

Kept true by:

- `rpc_params_gate` — a typed row parses, its `?` is exactly where the
  handler reads the key with `value()`/`contains()` (a key only ever read
  with `at()` is required), and every literal of an enum is one the branch
  compares against (a vocabulary the parser owns is `@name` instead).
- `rpc_contracts_smoke` — every contracted row's server schema equals the
  Python reading of it (`tools/rpc_contract.py`) and every `@name` is a
  published vocabulary; each typed method refuses a missing key, a wrong
  type, an enum miss and a range miss with its diagnostic and changes
  nothing; the SDK's preflight refusal equals the server's; and the
  revision guard below.
- `test_rpc_contract.cpp` — the grammar, its schema and its messages.

### Typed results

A row's `returns` column is a contract too, in the same grammar plus what an
answer nests and a request does not: object literals, `{key:type, …}`, and
nested arrays, `string[][]`.

```
"parts:{id:int, index:int, name:string, type:string, folder?:string}[], active:int, active_id:int, folders?:string[]"
"total:{volume_mm3:number, center_of_mass_mm:number[], mass_g:number|null}, bodies?:{…}[]"
"features:object[], selection:{mesh:int, face:int, normal:number[], center:number[]}|null"
```

- `key?` is a key the answer may leave out; `number|null` one it may send
  as null.
- An object closes over its keys: one the row does not list fails. `{…, *}`
  is an object that carries more than it lists (a user's machine file).
- A column types the answer's top level and one level inside it. What
  nests deeper is `object` or `object[]`: a CAM operation's fields, a
  receipt's claims, a feature's per-type keys each have a shape of their
  own, which a row would otherwise copy and let drift.
- A column still in prose (no `key:type` in it) is untyped.

`tools/rpc_contract.py` reads the column (`parse_result_column`,
`result_schema`); the server publishes it as written, in `returns`. From it
`tools/generate_rpc_contracts.py` writes each method's result `TypedDict`
and its JSON Schema (`lobole.rpc.RESULT_SCHEMAS`), and a **strict**
`lobole.client.Client` — every smoke test's, through `smoke_app` — holds
every typed answer to its schema, so a handler that adds, drops or retypes a
key without its row fails the test that saw it rather than reaching a caller
as a KeyError three calls later. `rpc_returns_gate` reads a typed column's
keys, and `rpc_units_gate` holds every dimensional result key to its unit.

### Typed feature specs

`add_feature` and `modify_feature` take `feature:object`; what is inside it
has a contract of its own, by its `type`. Each row of `kFeatureTypes`
(`src/core/io/SerializeFeatures.cpp`) carries its fields twice: `params`,
the prose an agent reads (defaults, axis conventions, what refuses), and
`contract`, the same fields typed in the request grammar:

```
"sketch_id:int>=0, distance?:number, distance_expr?:string, mode?:'new_body'|'add'|'cut', end_mode?:'blind'|'symmetric'|…"
```

The whole contract is the row's beside `type` and the keys every spec may
carry (`kFeatureCommonContract`: `name`, `id`, `material`, `appearance`,
`face_appearance`, `suppressed`); `describe` publishes it as each feature
type's `contract`. The dispatcher holds `params.feature` to it in
`ValidateRequestContract`, by path (`/params/feature/end_mode`), before
anything is built, after dropping what `get_state` stamps around a spec
(`kFeatureEnvelopeKeys`, so a read-modify-write of a `get_state` entry stays
valid). A primitive carrying its old world-coordinate keys
(`kPrimitiveRetiredKeys`) is left to the primitive parser, which refuses
each by name pointing at `on`. `RejectUnknownFeatureFields` stays behind it
for a spec that arrives another way, reading its keys from the same column.

Generated from the rows: a `TypedDict` per type (`ExtrudeSpec`, …) and their
union `FeatureSpec` in `lobole.rpc`, which `feature` accepts beside any
mapping, so a spec annotated as one is checked field by field and a plain
dict still passes; and `FEATURE_SCHEMAS`, which the SDK holds a spec to
before it is sent, with the server's diagnostic. `rpc_contracts_smoke`
refuses an unknown field, a wrong type, an enum miss and a missing field of
every type, alike from the server and the SDK, and holds `describe`'s
column equal to the generator's.

### Structured errors

A refusal carries the sentence in `error` and, where the dispatcher can say
it in fields, a `diagnostic`:

```
{operation, code, path, param?, expected, observed, accepted?, call?}
```

`code` is `missing_argument`, `unknown_argument`, `invalid_argument` (a
type, an enum, a range or a shape) or `stale_revision`, plus the inspection
methods' own (`stale_snapshot`, `budget_exceeded`, …), and **`internal`**:
the handler fell over rather than refused. That is any exception that is not
a `std::runtime_error` (which every deliberate refusal throws) or nlohmann's
`type_error` and `parse_error`: `std::logic_error` and its `out_of_range` and
`invalid_argument`, nlohmann's `out_of_range` (an `at` on a key nobody
checked), `bad_optional_access`, `bad_alloc`, a kernel `Standard_Failure`
that reached the last line of defence, an exception of no known type.
`observed` is the exception's type name (`std::out_of_range`,
`Standard_ConstructionError`). The sentence is what the exception said, and
the call is rolled back like any other refusal (`remote/RpcErrors.h`;
`test_conventions.cpp` holds the source to throwing `runtime_error` for a
refusal). `path` is a JSON
pointer under `/params`; `param` is its top-level key; `expected` is the
schema (or, for an unknown key, the keys the method takes); `observed` is
the value sent; `accepted` lists the values an enum or vocabulary takes, or
the keys a method takes; `call` is the 1-based call a batch refused before
running. Every method's unknown key carries one, and so does a missing key a
handler reads with `RpcParams::at`; a contracted method's every refusal of
its params does. The sentence names the method, the key, what it must be and
what it got:

```
add_mate: 'type' must be one of: fastened, revolute, slider, cylindrical,
planar, ball; got "revolut". To COUPLE two mates' degrees of freedom --
gears, a rack and pinion, a screw -- the thing to add is a RELATION, not a
mate: see add_relation
```

### The revision guard

A write built from a reading of a part can say which reading:
`expect_revision` is the part's revision `{session, part_id, instance,
version}` — the one `geometry_snapshot` binds its references to and
`get_state` reports as `revision`. The dispatcher compares it with the
active part's before the handler runs, and refuses `stale_revision` (naming
the current revision, and why: another part, another server session, the
part reopened or replaced, or regenerated since) when they differ. `version`
moves on every regeneration, so an edit that rebuilds nothing — a tolerance,
a comment — does not invalidate a reading. A guarded call answers with the
`revision` it left, so a chain of writes guards each against the last.

Guarded: `add_feature`, `modify_feature`, `delete_feature`, `set_material`,
and `batch`, which checks once before its first call and answers with the
revision after its last. The key is the dispatcher's, not the handler's: a
row lists it as `expect_revision?:revision` and `rpc_params_gate` knows no
branch reads it.

### Inspection contracts

Two methods — `geometry_snapshot` and `validate_snapshot` — carry
machine-readable request AND result schemas. `api/inspection-contracts.json`
is the source; `python3 tools/generate_inspection_contracts.py` regenerates
the embedded server schemas (`src/remote/InspectionContracts.generated.h`)
and the Python records and facade (`python/lobole/inspection.py`). The
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

A contract refusal is a `lobole.Refused` (the subclass for its code,
[lobole.errors](sdk.md#errors)) with the
[structured diagnostic](#structured-errors); batch failures keep the nested
diagnostics, including when the batch rolls back.

Snapshot semantics are in [sdk.md](sdk.md#measurement-snapshots). Typing the
rest of the surface, and result schemas beyond these two, is on the
[roadmap](../roadmap.md#6-the-agent-interface).
