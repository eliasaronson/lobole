# Checks on the agent surface

A build call that can say what it expects, tests that hold the prose an
agent reads to the app it describes, and four smaller repairs to the same
surface.

## The problem

- **A batch cannot state its outcome.** `batch` (src/remote/RemoteDispatch.cpp)
  answers one `regen` and one `digest`, and the caller compares the numbers
  itself, in a second step it can skip. `expect_revision` guards what the
  batch starts from; nothing guards what it produced. Requirements do
  (docs/requirements/README.md), but they are project objects with evidence
  and a check job, the wrong weight for "this step should leave one body of
  4000 mm3". `execute_script` (python/lobole/mcp.py) has the same gap: a
  script that exits 0 is reported as done whatever it built.
- **Only one text is held to the method tables.** `tests/mcp_smoke.py`
  checks every call-shaped name in `WORKFLOW` against the tools, the RPCs
  and the keys `describe` publishes. Nothing checks the server's
  `instructions`, the announced tools' descriptions, the 103 `_reference`
  texts `help()` serves, `CONSTRAINT_FORMATS`, the `MethodDoc` rows' own
  prose (a row that says "see update_drawing"), or
  docs/inspect/review-checklist.md, which the prompt sends agents to. And
  that one check runs only in the `gui`-labelled test, off the default path.
- **The prompt cites files its reader cannot open.** `WORKFLOW` and the
  `_reference` texts name `docs/inspect/review-checklist.md`,
  `docs/simulation/README.md#buckling` and others. An agent on the MCP
  surface alone (`uv tool install`, no checkout) has no way to read them:
  `help()` serves `_REFERENCE` and the `describe` rows, nothing else.
- **No worked example is run.** The prose teaches by fragments (a JSON
  shape, a sentence with a number in it). None is a complete build with a
  stated result, so none can be tested, and a field renamed under one is
  found by the agent that copies it.
- **A timed-out call is sent again.** `Client.request`
  (python/lobole/client.py) lets a socket timeout out as a bare
  `TimeoutError`; its own docstring says the app finishes the call anyway.
  `_call` in mcp.py catches `OSError`, which a timeout is, drops the
  connection and sends the same call a second time on a new one. A slow
  `add_feature` lands twice. `ConnectionLost` is resent the same way, to an
  app that may have restarted with another document.
- **A batch repeats one warning per feature.** `RegenSummaryJson`
  (src/remote/RemoteHelpers.h) writes a row per warning. Fifty cuts that
  each missed every body are fifty rows of the same sentence.
- **A drawing add cannot be asked first.** `add_drawing_view`,
  `add_drawing_dim` and `add_drawing_note` (src/remote/RemoteDrawings.cpp)
  auto-place and answer where things went. What a dimension would read, or
  whether a view would land over another, is known only after it is on the
  sheet.
- **Nothing keeps the schemas portable.** `RpcContract.cpp` writes
  alternatives as `anyOf`, and the MCP SDK does the same for the tools'
  signatures. Some model vendors refuse a whole tool list over one
  `"type": [..]`. No test would notice a change that introduced one.

## The shape of the fix

### 1. `expect` on a batch

`batch` takes `expect: {volume_mm3?, mass_g?, body_count?, bbox_mm?,
tolerance_pct?, part?}`. `bbox_mm` is `[x, y, z]`, the size of the part's
measured box; `tolerance_pct` applies to every number in it and is
required whenever one is given (refused without it, naming the key: how
close the caller's own arithmetic is, is the caller's to state);
`body_count` is exact. `part` names another part than the active one, as
the reads do.

It is read and checked before the first call, with the rest of the batch's
parameters: an unknown key, a negative volume, a `bbox_mm` that is not
three numbers, `body_count` or `bbox_mm` on an assembly tab (which takes
`volume_mm3` and `mass_g`), any of it on a drawing or a job, are refused
with the document unchanged.

It is judged after the batch's last regeneration, by the requirement code
and nothing else:

- `body_count` and `bbox` (with `axis: x | y | z`) become measure kinds in
  `req::kMeasureKinds` (src/core/Requirements.{h,cpp}), read through
  `partmeasure` functions (src/core/PartMeasures.{h,cpp}) on a `const
  Document&`, the box being the tight one the digest reports
  (`BodyBounds`). They are ordinary requirement kinds from then on: "fits a
  256 mm bed" can be stated with `add_requirement` and read by a design
  sweep's point (src/core/DesignSweep.cpp).
- `remote::JudgeExpectations(state, expect)` in
  src/remote/RemoteRequirements.cpp builds one transient
  `req::Requirement` per number (`volume`, `mass`, `body_count`, `bbox` per
  axis) with a `within` bound from the tolerance, measures each through
  `MeasureOn` and judges it through `Judge`. Nothing is stored and no
  evidence is written. A requirement stating the same number and this
  agree by construction.

The batch's result gains `expect: {ok, checks: [{quantity, expected,
measured, unit, deviation_pct, pass}]}`. When a check fails the batch
stays applied, as one undo step, and the message carries a top-level
`expect_failed` beside the result, attached where `nonfinite` is
(src/remote/RemoteSession.cpp): one sentence per miss, "volume_mm3:
measured 59680, expected 59824.9 (-0.24 %, tolerance 0.1 %)". The model is
kept because it is the evidence: the caller needs the bodies to see why.

`lobole/client.py` raises `ExpectationFailed` on it (a `LoboleError` that
is not a `Refused`, carrying `.result` and `.checks`, as `NonFiniteError`
carries its result), so the MCP server answers a tool error whose text is
those sentences plus "the batch was applied; undo takes it back".

`check_expect {…the same keys}` judges the active document without a
batch, with a `returns` row. `execute_script` takes `expect`, calls it
after a script that exits 0, and raises the same error with the script's
output attached. The SDK's `Batch` (python/lobole/sdk.py) takes
`expect=`.

### 2. Every agent-facing text names only what exists

The scan moves out of `mcp_smoke.py` into `tests/agent_prose.py`
(`unknown_names(text, describe, tools, vocabulary)`), with the vocabulary
list keyed by text, each word with where it comes from, as `PROMPT_WORDS`
is now. `tests/mcp_formats_gate.py`, which is headless and on the default
path, runs it over:

- `WORKFLOW`, the server's `instructions`, `FEATURE_FORMATS`,
  `CONSTRAINT_FORMATS`;
- every announced tool's description and every `_reference` text;
- every `MethodDoc` row's prose, as `describe` serves it;
- every document `help()` serves (below).

A planted unknown name must be caught in each, as the one in `WORKFLOW` is
today. `mcp_smoke.py` imports the same function and keeps only what needs
the MCP transport: that the served prompt is `WORKFLOW`.

A `docs/...` path in any of those texts must be one `help()` serves.
`help('review-checklist')` returns docs/inspect/review-checklist.md,
shipped as package data (pyproject.toml), and `help()` lists the topics; a
citation of a document that is not served is rewritten to name the RPC or
the topic that is, or removed.

### 3. Worked examples that run

`EXAMPLES` in python/lobole/mcp.py: a handful of complete builds, one per
recipe the prompt teaches (a parametric plate with holes, a revolved part,
a sketch on a face, a two-part assembly with a mate, a sheet-metal
bracket). Each is `{about, calls, expect, then?}`: `calls` is a `batch`,
`expect` its stated result, `then` a variable change with the result
expected after it. `help('examples')` lists them and `help(example=name)`
returns one, ready to send. `WORKFLOW` points at them where it describes
the loop.

`tests/agent_examples_smoke.py` (headless) sends every example as written
and fails on any `expect_failed`, then applies its `then` and checks
again. The stated numbers are the `expect` values, so there is no second
copy of them to drift. The first fenced block of docs/scripting/sdk.md is
run the same way, with a stated volume added to it.

### 4. A failed transport says what is safe

- A socket timeout in `Client.request` raises `CallTimedOut` (a
  `LoboleError` and a `TimeoutError`): "the app is still running <method>;
  do not send it again. `background_jobs` shows what is running; raise
  `timeout`, or use `async` where the method takes it."
- `ConnectionLost` says what is not known: whether the call was applied,
  and to compare `get_state`'s revision before repeating it.
- `_call` in mcp.py no longer resends. It reconnects and retries only when
  the failure came before the request was written (a stale socket found at
  send); anything after that surfaces with its message.
- `Refused` keeps its meaning (nothing changed), and the MCP error text for
  one ends with "nothing was changed, so it is safe to send again
  corrected".

### 5. One row per kind of warning in a batch

A batch's `regen.warnings` folds rows whose `warning` text is identical
into one: `{warning, count, features: [{index, name}]}`, the list capped
at ten with `more: n`. A single call's `regen` is unchanged, and so is
`get_state`'s.

### 6. A dry run for drawing adds

`add_drawing_view`, `add_drawing_dim` and `add_drawing_note` take
`dry_run: true`: the answer is what the call would have answered (a
view's fitted `scale` and `position`, a dimension's `value`, `text` and
placed `offset`, a note's resolved text), plus `issues`, the rows
`check_drawing` would add because of it (off the border, over another
view, a dangling ref). Nothing is added and no undo step is made. A view's
dry run places without projecting, so it costs nothing.

### 7. The schema lint

`tests/test_rpc_contract.cpp` walks every `request_schema` and result
schema `describe` publishes and fails on a `type` that is an array, and on
`enum` beside one. `mcp_formats_gate.py` does the same over the announced
tools' input schemas (it already loads them to check signatures).

## Stages

Each merges on its own.

- **A. The scan and the lint** (2, 7): tests only, plus whatever names they
  find wrong.
- **B. Transport errors and folded warnings** (4, 5).
- **C. `expect`** (1): the two measure kinds, the judge, `batch`,
  `check_expect`, the client error, `execute_script`, the SDK.
- **D. Examples and served documents** (3, the `help()` half of 2), which
  need C.
- **E. Drawing dry runs** (6).

No stage changes the document format. C adds two measure kinds a saved
requirement may then hold, which is a format change of the project file:
`vNEXT`, with a golden holding one of each.

## Files

- `src/core/Requirements.{h,cpp}`, `src/core/PartMeasures.{h,cpp}`,
  `src/core/DesignSweep.cpp`, `src/remote/RemoteRequirements.cpp` (the
  kinds, `JudgeExpectations`, `check_expect` and its row).
- `src/remote/RemoteDispatch.cpp` (the `batch` row and handler),
  `src/remote/RemoteSession.cpp` (`expect_failed`),
  `src/remote/RemoteHelpers.h` (the fold), `src/remote/RemoteDrawings.cpp`
  (`dry_run`).
- `python/lobole/client.py`, `errors.py`, `mcp.py`, `sdk.py`; regenerated
  `python/lobole/rpc.py` and `api/rpc-surface.json`; `pyproject.toml`
  (package data).
- `tests/agent_prose.py`, `tests/agent_examples_smoke.py` (new);
  `tests/mcp_formats_gate.py`, `tests/mcp_smoke.py`, `tests/batch_smoke.py`,
  `tests/requirements_smoke.py`, `tests/drawing_smoke.py`,
  `tests/papercuts_smoke.py`, `tests/test_rpc_contract.cpp`,
  `tests/CMakeLists.txt`; `tests/golden/vNEXT-expect-kinds.json` and its
  `.expect.json`.
- `docs/scripting/rpc.md` (`batch`, what a response carries, refusals),
  `docs/scripting/mcp.md` (the claims table, the scan's reach, examples,
  served documents), `docs/scripting/sdk.md` (the error tree),
  `docs/requirements/README.md` (the two kinds),
  `docs/drawings/README.md` (dry runs), `docs/versioning/format.md`.

## Verified by

- `batch_smoke`: a batch whose `expect` holds answers `expect.ok`; one
  that misses a volume, a count, a box axis and a mass each raises
  `ExpectationFailed` naming the measured and expected numbers, with the
  bodies still there and one `undo` removing them; a malformed `expect` is
  refused before any call runs; `check_expect` and a `volume` requirement
  on the same part read the same number.
- `requirements_smoke`: `body_count` and `bbox` stated, checked, failed by
  an edit, and read by a design sweep.
- `mcp_formats_gate`: clean over every text listed above, and a planted
  name caught in each; a planted `docs/` path that is not served is
  caught; a planted `"type": ["string", "null"]` fails the lint, in the
  C++ test and in the gate.
- `agent_examples_smoke`: every example builds to its `expect`, before and
  after its `then`; an example with one number changed fails.
- A client test with a call given a timeout shorter than it takes (a
  `check_requirements` under the test hold, `SetTestRequirementHold`):
  `CallTimedOut` is raised, the call lands once, and the MCP server sent
  it once. A connection closed mid-call raises `ConnectionLost` and is not
  resent.
- `batch_smoke`: fifty features with one warning answer one folded row
  with `count: 50`; the same features one call at a time answer a row
  each.
- `drawing_smoke`: each `dry_run` leaves the sheet's fingerprint and undo
  depth as they were, and the call made for real answers the same values.
- `rpc_units_gate`, `rpc_returns_gate`, `api_surface_gate` and
  `refused_calls_smoke` pass with the new keys and methods; the headless
  suite and `tools/ctest -L pygui` per stage.
