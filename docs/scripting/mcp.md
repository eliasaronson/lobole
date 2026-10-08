# The MCP server

`tools/designer_mcp.py` exposes a running Loböle instance to any MCP client
(Claude Code, Claude Desktop, a custom agent). It is a stdio server built on
the official MCP Python SDK's `FastMCP`, a thin layer over
`tools/designer_remote.py`: every tool ends in an RPC ([rpc.md](rpc.md)).

## Setup

Claude Code sessions started in this repo pick the server up from the
checked-in `.mcp.json`, which runs it inside `nix-shell` (the `mcp` package
comes from `shell.nix`). For other clients:

```
claude mcp add lobole -- python3 /path/to/Designer/tools/designer_mcp.py
```

No port is needed; see "Finding the app" below. `--port` or `$DESIGNER_PORT`
(the flag wins) pins one instance.

### Installing without the checkout

The repo root `pyproject.toml` packages `designer_mcp` and `designer_remote`
from `tools/` with two console scripts, `designer-mcp` (the server) and
`designer-remote` (the CLI client):

```
uv tool install /path/to/Designer       # or: uvx --from git+ssh://<remote> designer-mcp
claude mcp add lobole -- designer-mcp
```

The package name `designer-mcp` is a placeholder and is not on PyPI; install
from a path or a git URL. `mcp` is pinned `<2`: mcp 2.x renamed `FastMCP` and
changed other APIs, and the pin is lifted by migrating the server, not by
loosening it. `packaging_smoke` checks that every `py-modules` entry exists
and every console script resolves to a real callable.

## Finding the app

The server resolves which instance to talk to, in this order:

1. **`--port` / `$DESIGNER_PORT`** — a pin. It never falls back to
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
waits for the child to announce its kernel-chosen port under its pid, attaches
to it, and kills it when the server exits. The binary is probed, not assumed:
`build/lobole` beside the server, then `designer` on `PATH`, then the
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

`--no-autostart` or `DESIGNER_AUTOSTART=0` turns it off.
`mcp_autostart_smoke` pins that no process or registry entry outlives the
server.

## The tool surface

The announced tools are what a person asks an assistant for in a
conversation, plus two doors to everything else. The `@mcp.tool`
functions in `designer_mcp.py` are the authority; grouped:

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

- `rpc` routes through a kept wrapper when the server has one (so its
  argument shaping and defaults still apply), and otherwise passes the call
  straight to the app, so an RPC newer than this file still works. An unknown
  name comes back with close matches.
- `help(method)` returns the wrapper's prose and signature, or, for an RPC
  the server does not wrap, the app's own `describe` row — with
  `param_types`, each key's type in words, for a method with a typed
  contract ([rpc.md](rpc.md#typed-contracts)). The lookup is
  `designer_remote.method_help`, which the python client's `help` also is;
  this server only adds its wrapper prose.
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
`designer_remote.Designer` on the attached instance. `DESIGNER_PORT` is set,
`tools/` is on `sys.path` (so `designer_sdk` imports), stdout and stderr come
back capped at 20000 characters each, relative paths land in a returned
scratch `cwd`, and `timeout` (default 120 s) bounds long solves. This is the
path for building a part with loops or computed arguments — one round trip
instead of a tool call per step — for an agent that has no shell of its own.
It is the same trust domain as `rpc`: the same instance, the same powers,
gated by the MCP host's tool approval.

### The workflow prompt

One MCP prompt, `modeling-workflow`, teaches the loop: variables first,
constrain fully, read each mutating response's `regen` and `digest`, verify
with `mass_properties` and `render_sheet`, gate assemblies on
`contact_report`, run `check_literals` on a part built from literals (and
`extract_variables` to name them,
[../variables/README.md](../variables/README.md#literals-that-should-be-expressions)),
read `get_state`'s selection before acting on "this" or "the selected" and
ask rather than guess when it is empty, state what the design is for as
[requirements](../requirements/README.md) and check them before delivering
(only `pass` is met), and run
[the review checklist](../inspect/review-checklist.md) before delivery.
Changing it changes what the benchmark measures.

## Format documentation

An agent can only use `add_feature` if the feature, entity and constraint
dialects are in front of it. `FEATURE_FORMATS` and `MATERIAL_SLUGS` in
`designer_mcp.py` carry them as prose in the tool descriptions; the source of
truth is `SerializeFeatures.cpp` (`FeatureFromJson`, `EntityFromJson`), served
at runtime by `describe`. `mcp_formats_gate` (headless, on the default path)
fails when a feature type, sketch entity type or material slug in `describe`
is missing from the prose, so a new feature type needs a row in both places.

## Testing

- `mcp_formats_gate` — the prose against `describe`, headless.
- `mcp_server_compiles`, `py_client_compiles` — `py_compile` the server and
  client on the default path, with no display and no SDK. `mcp_smoke` is
  GUI-labelled, so without these a syntax error in the server would pass
  every default run.
- `mcp_autostart_smoke` — the autostart lifecycle (skips without the `mcp`
  SDK or a Vulkan device).
- `mcp_smoke` (GUI) — models a dimensioned bracket end to end through the
  SDK's stdio client, so the MCP layer itself is exercised; skips without the
  `mcp` SDK.
- `packaging_smoke` — the `pyproject.toml` metadata.

`--only-tools a,b,c` serves only the named tools and refuses loudly if it
cannot (an unknown name, or FastMCP's registry having moved), because a
restriction that silently served the full surface would make a benchmark arm
a duplicate of another.
