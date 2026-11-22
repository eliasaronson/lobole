# The MCP server

`lobole.mcp` (`python/lobole/mcp.py`) exposes a running Loböle instance to
any MCP client (Claude Code, Claude Desktop, a custom agent). It is a stdio
server built on the official MCP Python SDK's `MCPServer` (mcp 2.x), a thin
layer over `lobole.client`: every tool ends in an RPC ([rpc.md](rpc.md)).

## Setup

Claude Code sessions started in this repo pick the server up from the
checked-in `.mcp.json`, which runs it through `tools/nix-run` (the `mcp` package
comes from `shell.nix`, or on macOS from the venv `packaging/macos/deps.sh`
makes). For other clients:

```
claude mcp add lobole -- env PYTHONPATH=/path/to/checkout/python python3 -m lobole.mcp
```

No port is needed; see "Finding the app" below. `--port` or `$LOBOLE_PORT`
(the flag wins) pins one instance.

### Installing without the checkout

The repo root `pyproject.toml` builds the `lobole` package (`python/lobole/`)
with two console scripts, `lobole-mcp` (the server) and `lobole-remote` (the
CLI client):

```
uv tool install /path/to/checkout       # or: uvx --from git+<url> lobole-mcp
claude mcp add lobole -- lobole-mcp
```

A release publishes it to PyPI (`uv tool install lobole`); between releases,
install from a path or a git URL. `mcp` is pinned to the 2.x line
(`>=2,<3`), the SDK `shell.nix` gives and so the one every test here runs:
1.x has no `MCPServer`, and the next major is a migration of the server, not
a loosened pin. `packaging_smoke` builds the wheel and imports every module
from it alone, and checks it carries the documents `help()` serves.

## Finding the app

The server resolves which instance to talk to, in this order:

1. **`--port` / `$LOBOLE_PORT`** — a pin. It never falls back to
   discovery, so a client told to drive one app cannot drift onto another.
2. **Exactly one running instance** in the registry
   ([README.md](README.md#the-instance-registry)) — attach to it.
3. **Several** — refuse, listing them with their modes. Guessing would answer
   questions about a document the user is not looking at.
4. **None** — autostart (below), or, with autostart off, refuse naming the
   exact start command for a Loböle that is actually installed here.

It connects lazily on the first tool call and reconnects across a
connection found dropped before a call is sent ([a failed
call](#a-failed-call)), so registering it before the app is running is
fine.
`describe` and `execute_script` report what they attached to (`attached`:
"Loböle on port 9090: bracket.lob (unsaved changes), gui"), and the
`instances` tool lists what is running. That line comes from `ping`, live.

### Autostart

With nothing running, the server spawns `lobole --offscreen --remote 0`,
waits for the child to announce its kernel-chosen port under its pid, or a
pid it started (an AppImage's runtime and `flatpak run` run the app as their
own child), attaches to it, and kills it when the server exits. The binary is
probed, not assumed:
`build/lobole` beside the server, then `lobole` on `PATH`, then the
flatpak `org.lobole.Lobole`. The conditions are what make it
safe:

- It triggers only when discovery finds nothing — a pinned port or any live
  instance always wins.
- The child is offscreen: there is no window to stand beside the user's own,
  and it still renders.
- The child gets a scratch `XDG_CONFIG_HOME` seeded with the real
  `settings.json` and `machines/` directory by name — never the whole
  directory, which may hold other credentials — so `set_settings` cannot
  rewrite the human's preferences and a CAM setup still finds its machine.
- It dies with the server. A spawn that exits or never announces raises with
  the tail of the child's own log.

`--no-autostart` or `LOBOLE_AUTOSTART=0` turns it off.
`mcp_autostart_smoke` pins that no process or registry entry outlives the
server.

## The tool surface

The announced tools are what a person asks an assistant for in a
conversation, plus two doors to everything else. The `@mcp.tool`
functions in `lobole/mcp.py` are the authority; grouped:

| group | tools |
|---|---|
| meta | `describe`, `help`, `rpc`, `execute_script`, `instances` |
| inspect | `get_state`, `check_geometry`, `measure`, `mass_properties`, `list_elements` |
| variables | `list_variables`, `set_variable` |
| look at it | `screenshot`, `render_still`, `view`, `set_camera`, `set_section`, `set_render_options` |
| project | `new_project`, `open_project`, `save_project`, `export_stl`, `export_step` |
| author | `add_feature`, `modify_feature`, `delete_feature`, `undo` |

Everything else — assemblies and mates, relations, motion studies, drawings,
FEM and topology studies, configurations, sketch constraints and ops,
checkpoints and history, tolerances, sections, CAM, the raw GUI actions — is
reached with **`rpc(method, params)`** and documented by **`help(method)`**.

- `rpc` sends the call straight to the app: its `describe` row is the
  contract and its defaults the only ones, so an RPC newer than this file
  works the day it lands. An unknown name comes back with close matches.
  `ui_action` is the one name that is this server's own, a door to the
  GUI-action RPCs.
- `help(method)` returns the app's own `describe` row — its parameters,
  `param_types` (each key's type in words, for a method with a typed
  contract, [rpc.md](rpc.md#typed-contracts)) and what comes back — with
  this server's longer prose in place of the row's one-liner where it keeps
  some (`_reference` in `lobole/mcp.py`: prose, never a wrapper). The lookup
  is `lobole.client.method_help`, which the python client's `help` also is.
  `help` also serves the documents the texts cite and the worked examples
  ([served documents](#served-documents), [worked examples](#worked-examples)).
- An announced tool in front of an RPC takes exactly that RPC's keys and
  passes only the ones given, so every default is the app's;
  `mcp_formats_gate` holds each tool's signature to its row.
- `screenshot` and `render_still` return the image inline.
- A refusal reaches the model with its text (`Error executing tool <name>:
  <the app's message>`), naming what is accepted. mcp 2 passes a tool's own
  message on only for its `ToolError` and reduces anything else to the bare
  prefix, so `_Server.call_tool` turns the `RuntimeError` (every
  `lobole.errors` refusal is one), `ValueError` and `OSError` the tools raise
  into one; any other exception is a bug here and stays a logged crash.
- A call is never sent twice ([below](#a-failed-call)).
- Tool calls run one at a time, in the order they arrive. mcp 2 runs each
  request as its own task and a sync tool on a worker thread, and two calls
  sharing the one app connection from two threads would interleave their
  JSON-RPC.

Keeping most RPCs behind `rpc`/`help` keeps the schema bulk every client loads
up front small; the `rpc-only` benchmark arm measures that trade
([benchmark.md](benchmark.md)).

`ui_action` (through `rpc`) needs the ImGui Test Engine. `ping` reports
`ui_actions` separately from `mode`, because they are two
questions: `mode` says whether a rendered frame exists, `ui_actions` whether
this build can drive its GUI.

### A failed call

`_call` sends each call once. It reconnects and sends again only when the
connection was found closed before the call was written
(`ConnectionLost.sent` false: the app restarted, or dropped an idle
connection), which the client checks before every write. A connection lost
after that, or a timeout, reaches the model with its message, which says
what is known ([the SDK's errors](sdk.md#errors)): a timeout, that the app
is still running the call and it must not be sent again; a loss mid-call,
that whether it was applied is unknown and `get_state`'s revision tells. It
used to catch every `OSError`, which a timeout and a loss both are, and send
the same call again on a new connection, so a slow `add_feature` landed
twice. A refusal's tool error ends "nothing was changed, so it is safe to
send again corrected". `client_transport_smoke` counts the sends.

### `execute_script`

Runs a Python snippet in a subprocess with `d` pre-bound to a connected
`lobole.client.Client` on the attached instance. `LOBOLE_PORT` is set,
the `lobole` package is on `sys.path` (so `lobole.sdk` imports), stdout and stderr come
back capped at 20000 characters each, relative paths land in a returned
scratch `cwd`, and `timeout` (default 120 s) bounds long solves. This is the
path for building a part with loops or computed arguments — one round trip
instead of a tool call per step — for an agent that has no shell of its own.
It is the same trust domain as `rpc`: the same instance, the same powers,
gated by the MCP host's tool approval. A script that exits 0 is not thereby one that built what it was
for: `expect` (the keys of a batch's [`expect`](rpc.md#expect)) is judged
by `check_expect` after it, and a miss is a tool error naming each number,
with the script's output attached.

### The workflow prompt

One MCP prompt, `modeling-workflow`, teaches the loop: variables first,
constrain fully, read each mutating response's `regen` and `digest`, verify
with `mass_properties` and `rpc('render_sheet')`, gate assemblies on
`contact_report`, run `check_literals` on a part built from literals (and
`extract_variables` to name them,
[../variables/README.md](../variables/README.md#literals-that-should-be-expressions)),
read `get_state`'s selection before acting on "this" or "the selected" and
ask rather than guess when it is empty, state what the design is for as
[requirements](../requirements/README.md) and check them before delivering
(only `pass` is met), give a part that carries a load a static study of each
way it is loaded and a [buckling](../simulation/README.md#buckling) study of
each load that compresses something, with requirements on
`factor_of_safety` and `buckling_factor`, judge a study before quoting it
(`check_sim_geometry` before meshing, suppressing with `set_sim_geometry`
what is smaller than its elements, `check_study`, the mesh report, whether the peak settled or sits on a
support's edge, `stale`, and what the model leaves out), and run
[the review checklist](../inspect/review-checklist.md) before delivery.
Changing it changes what the benchmark measures.

What it tells an agent it can rely on is held by a test. A claim with no
test is taken out of the prompt rather than left in untested.

| claim | pinned by |
|---|---|
| every call that rebuilds the part answers `regen` and a `digest`; a batch one for the whole | `rpc_digest_gate` (every handler, from the source), `workflow_claims_smoke` (`set_variable`, `batch`), `strict_inputs_smoke` |
| a broken feature is named in the call's own `regen` | `regen_honesty_smoke` |
| unknown fields are refused, naming the accepted vocabulary | `strict_inputs_smoke`, `refused_calls_smoke` |
| `describe` lists every feature type's params | `mcp_formats_gate`, `mcp_smoke` |
| `set_rollback` shows an intermediate state, its digest the bodies as of the bar | `workflow_claims_smoke` |
| `check_literals` names a literal equal to a variable, and a sketch or frame on a world plane where a face lies | `literals_smoke` |
| `get_state` carries `selection` and `selection_items` | `workflow_claims_smoke` |
| a buckling study's `buckling_factor` (and `factor_of_safety`) is its governing factor; a study requirement never checked is `unchecked` | `buckling_smoke` |
| the GUI-driving RPCs need a window; offscreen renders | `remote_smoke`, `offscreen_smoke` |
| a batch given `expect` answers `expect.ok`, or an error naming each number missed with the batch applied | `batch_smoke` |
| `help('examples')` lists builds that run to their stated numbers | `agent_examples_smoke`, `mcp_smoke` |
| `help('review-checklist')` serves the checklist | `mcp_smoke`, `packaging_smoke` |
| every name it uses is an announced tool or a registered RPC | `mcp_formats_gate`, `mcp_smoke` |

The last row is the drift gate, and it covers every text, not only this
one ([below](#every-text-names-only-what-exists)). The prompt names no
port: the human's instance is the registry's `gui` one the agent did not
start.

### Every text names only what exists

An agent on this surface reads prose, and copies the names in it. Every
text it reads is held to the method tables by `tests/agent_prose.py`, run
over all of them by `mcp_formats_gate` (headless, the default path):
`WORKFLOW`, the server's `INSTRUCTIONS`, `FEATURE_FORMATS`,
`CONSTRAINT_FORMATS`, every announced tool's description, every
`_reference` text `help()` serves, every `describe` row's prose, and the
served documents an agent is sent to follow ([below](#served-documents)).

`unknown_names(text, describe, tools, vocabulary)` returns what nothing
registers:

- a `snake_case` word that is not a method, a tool, a key or a value
  `describe` publishes (parameters, `returns`, every key and enum value of
  the request and result schemas, the feature types' contract keys and
  `params` text, and its name lists: mate types, material slugs, ...), nor a
  word of that text's entry in `VOCABULARY`, which says where each comes
  from; an `x_expr` counts as the twin of `x`;
- whatever `rpc('x')` or `help('x')` names that is not a method, a tool or
  a `help()` topic (a document's short name, `examples`);
- a key in `rpc('m', {a, b})`'s braces that `m`'s row does not take.

The prompt texts (`WORKFLOW`, `INSTRUCTIONS`, the two format texts) are
held to that alone. The rest describe what a call answers, which `describe`
publishes only where a row states `returns`, so for them a word the app's or
the package's source writes as a quoted literal (`"peak_at"`) counts as
existing too: a renamed key, whose old literal is gone, is still caught. The
gate plants a call, a word and a key in one text of each kind and fails
unless all three come back. `mcp_smoke` keeps what only the transport
shows: the prompt it is served is `WORKFLOW`, and names nothing the
announced tools and the app lack.

### Served documents

The prompt and the `describe` rows cite `docs/...` pages: the review
checklist, a section of the simulation README. An agent on this surface
alone (`uv tool install`, no checkout) could not open one, so `help()`
serves them: `help('docs/simulation/README.md#buckling')` returns that
section (from its heading to the next of the same level), the path alone
the whole page, and `help('review-checklist')` the checklist by its short
name (`DOC_TOPICS`). `help()` with no argument lists every served document.
They are every area document under `docs/`, the plans left out, shipped in
the wheel as package data (`pyproject.toml` maps `lobole._docs` to `docs/`;
in the checkout they are read where they are), and `packaging_smoke` holds
the wheel to carrying each one and `help` to reading the wheel's.

`mcp_formats_gate` fails on a `docs/` path in any agent-facing text that
`help()` does not serve, or whose `#anchor` names no heading there (the
anchor rule is the `[docs]` convention test's); a served document's own
relative links count as its citations. The served documents an agent is
sent to follow (`AGENT_DOCS`: the checklist) are scanned for names with the
other texts; the rest are reference pages written for this repo's
developers, full of test and GUI ids, and are held to their citations only.

### Worked examples

The prompt teaches by fragments; `EXAMPLES` in `lobole/mcp.py` are complete
builds, one per recipe it teaches: a plate with holes, a revolved bushing, a
boss sketched on a face, two parts mated in an assembly, a sheet-metal
bracket. Each is `{name, about, setup?, calls, expect, then}`: `calls` a
`batch`, `expect` its stated result (judged by the batch,
[rpc.md](rpc.md#expect)), `setup` the calls a batch cannot hold (making a
tab) sent one at a time before it, and `then` a variable changed with the
result stated after it. `help('examples')` lists them and
`help(example=name)` returns one, ready to send; `WORKFLOW` points at them
where it describes the loop. Each starts from a new project, so an element
name may carry its feature's id (`2/cap1`); the `about` says so.

`agent_examples_smoke` (headless) sends every example as written, fails on
an `expect_failed` before or after its `then`, and fails if an example with
one number changed passes. The numbers are the examples' own, so there is no
second copy of them to drift. The first fenced block of
[sdk.md](sdk.md) is run the same way, against the test's own instance, and
states its own volume with `check_expect`.

### Portable schemas

Some model vendors refuse a whole tool list over one `"type": [..]`. Every
alternative is written `anyOf`: `RpcContract.cpp` does so for a typed row's
column, and the MCP SDK for the tools' signatures. `test_rpc_contract.cpp`
walks every request and result schema `describe` publishes and fails on a
`type` that is an array (and on an `enum` beside one); `mcp_formats_gate`
does the same over those schemas as `describe` serves them and over every
announced tool's input schema, where the SDK is installed. Both check
themselves on a planted array.

## Format documentation

An agent can only use `add_feature` if the feature, entity and constraint
dialects are in front of it. `FEATURE_FORMATS` and `MATERIAL_SLUGS` in
`lobole/mcp.py` carry them as prose in the tool descriptions; the source of
truth is `SerializeFeatures.cpp` (`FeatureFromJson`, `EntityFromJson`), served
at runtime by `describe`. `mcp_formats_gate` (headless, on the default path)
fails when a feature type, sketch entity type or material slug in `describe`
is missing from the prose, and when a field in a type's typed contract
([rpc.md](rpc.md#typed-feature-specs)) is not named in it (an `x_expr` twin
by its `x`, which the prose states once; the keys a spec only carries back
from `get_state` are exempt), so a new feature type or field needs its row
and its prose.

## Testing

- `mcp_formats_gate` — the prose against `describe`, each announced
  tool's signature against its RPC's row, every agent-facing text's names
  ([above](#every-text-names-only-what-exists)), every cited `docs/` path
  and `#anchor` being one `help()` serves, no `help()` topic named as a
  method or a tool, and the schemas' portability, headless (each with a
  planted case).
- `client_transport_smoke` — a timeout, a loss mid-call and a stale socket
  against a stand-in server that counts what it is sent
  ([a failed call](#a-failed-call)).
- `agent_examples_smoke` (headless) — every worked example, and the first
  block of sdk.md, build what they state ([worked examples](#worked-examples)).
- `python_package_compiles` — compiles every module of the package on the
  default path, with no display and no SDK. `mcp_smoke` is GUI-labelled, so
  without it a syntax error in the server would pass every default run.
- `mcp_autostart_smoke` — the autostart lifecycle (skips without the `mcp`
  SDK or a Vulkan device).
- `mcp_smoke` (GUI) — models a dimensioned bracket end to end through the
  SDK's stdio client, so the MCP layer itself is exercised, and fails when
  the prompt it is served is not `WORKFLOW` or names something that is
  neither a tool nor an RPC; skips without the `mcp` SDK.
- `workflow_claims_smoke` (headless) and `rpc_digest_gate` (source, no
  app) — the prompt's claims in the table above.
- `packaging_smoke` — the wheel `pyproject.toml` builds, imported alone,
  carrying the served documents.

`--only-tools a,b,c` serves only the named tools and refuses loudly if it
cannot (an unknown name, or a tool still listed after `remove_tool`), because a
restriction that silently served the full surface would make a benchmark arm
a duplicate of another.
