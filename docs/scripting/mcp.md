# The MCP server

`lobole.mcp` (`python/lobole/mcp.py`) exposes a running Loböle instance to
any MCP client (Claude Code, Claude Desktop, a custom agent). It is a stdio
server built on the official MCP Python SDK's `FastMCP`, a thin layer over
`lobole.client`: every tool ends in an RPC ([rpc.md](rpc.md)).

## Setup

Claude Code sessions started in this repo pick the server up from the
checked-in `.mcp.json`, which runs it inside `nix-shell` (the `mcp` package
comes from `shell.nix`). For other clients:

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
install from a path or a git URL. `mcp` is pinned
`<2`: mcp 2.x renamed `FastMCP` and changed other APIs, and the pin is lifted
by migrating the server, not by loosening it. `packaging_smoke` builds the
wheel and imports every module from it alone.

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

It connects lazily on the first tool call and reconnects once across a
dropped connection, so registering it before the app is running is fine.
`describe` and `execute_script` report what they attached to (`attached`:
"Loböle on port 9090: bracket.dsgn (unsaved changes), gui"), and the
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
- An announced tool in front of an RPC takes exactly that RPC's keys and
  passes only the ones given, so every default is the app's;
  `mcp_formats_gate` holds each tool's signature to its row.
- `screenshot` and `render_still` return the image inline.

Keeping most RPCs behind `rpc`/`help` keeps the schema bulk every client loads
up front small; the `rpc-only` benchmark arm measures that trade
([benchmark.md](benchmark.md)).

`ui_action` (through `rpc`) needs the ImGui Test Engine. `ping` reports
`ui_actions` separately from `mode`, because they are two
questions: `mode` says whether a rendered frame exists, `ui_actions` whether
this build can drive its GUI.

### `execute_script`

Runs a Python snippet in a subprocess with `d` pre-bound to a connected
`lobole.client.Client` on the attached instance. `LOBOLE_PORT` is set,
the `lobole` package is on `sys.path` (so `lobole.sdk` imports), stdout and stderr come
back capped at 20000 characters each, relative paths land in a returned
scratch `cwd`, and `timeout` (default 120 s) bounds long solves. This is the
path for building a part with loops or computed arguments — one round trip
instead of a tool call per step — for an agent that has no shell of its own.
It is the same trust domain as `rpc`: the same instance, the same powers,
gated by the MCP host's tool approval.

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
(only `pass` is met), and run
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
| the GUI-driving RPCs need a window; offscreen renders | `remote_smoke`, `offscreen_smoke` |
| every name it uses is an announced tool or a registered RPC | `mcp_smoke` |

The last row is the drift gate: `mcp_smoke` fails when a call-shaped name in
the prompt (an `rpc('x')`, or any snake_case word) is neither a tool, an
RPC, a key `describe` publishes, nor one of the few vocabulary words it
lists, and checks itself on a planted unknown name. The prompt names no
port: the human's instance is the registry's `gui` one the agent did not
start.

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

- `mcp_formats_gate` — the prose against `describe`, and each announced
  tool's signature against its RPC's row, headless.
- `python_package_compiles` — compiles every module of the package on the
  default path, with no display and no SDK. `mcp_smoke` is GUI-labelled, so
  without it a syntax error in the server would pass every default run.
- `mcp_autostart_smoke` — the autostart lifecycle (skips without the `mcp`
  SDK or a Vulkan device).
- `mcp_smoke` (GUI) — models a dimensioned bracket end to end through the
  SDK's stdio client, so the MCP layer itself is exercised, and fails when
  the workflow prompt names something that is neither a tool nor an RPC;
  skips without the `mcp` SDK.
- `workflow_claims_smoke` (headless) and `rpc_digest_gate` (source, no
  app) — the prompt's claims in the table above.
- `packaging_smoke` — the wheel `pyproject.toml` builds, imported alone.

`--only-tools a,b,c` serves only the named tools and refuses loudly if it
cannot (an unknown name, or FastMCP's registry having moved), because a
restriction that silently served the full surface would make a benchmark arm
a duplicate of another.
