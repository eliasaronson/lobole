# Driving Designer over MCP

`tools/designer_mcp.py` exposes a running Designer instance to any MCP client
(Claude Code, Claude Desktop, custom agents). Design: [s1-mcp.md](s1-mcp.md).

## Setup

1. Start the app with the remote interface: `designer --remote 8437`
   — or with no window/GPU/display at all: `designer --headless --remote 8437`
   (full modeling + `mass_properties`/`list_elements`; `screenshot`,
   `render_still`, face selection and `ui_action` error with the remedy)
2. Register the server with your client. Claude Code sessions started in this
   repo pick it up automatically from the checked-in **`.mcp.json`**
   (approve the project server when prompted; `python3` must have the `mcp`
   SDK — run Claude Code from inside `nix-shell`, or `pip install mcp`).
   For other clients:

   ```
   claude mcp add designer -- python3 /path/to/Designer/tools/designer_mcp.py --port 8437
   ```

   The port also comes from `$DESIGNER_PORT`; the flag wins.

The server connects lazily — registering before the app is running is fine;
tools error with the remedy until it is reachable.

## Finding the app (no port to guess)

Every instance that opens a remote port announces itself in
`$XDG_STATE_HOME/designer/instances/<port>.json` (`pid`, `port`, `mode`,
`started` — see `src/remote/InstanceRegistry.h`) and withdraws on exit. A
crash leaves the file, so the **pid is the truth**: readers skip entries whose
process is gone and delete them as they pass.

The MCP server resolves what to talk to in that order:

1. **`--port` / `$DESIGNER_PORT`** — a pin. It never falls back to discovery,
   so a client told to drive one app cannot drift onto another.
2. **Exactly one running instance** → attach to it.
3. **Several** → refuse and list them. Guessing would answer questions about
   a document you are not looking at.
4. **None** → an error naming the exact command to start one, including the
   headless variant and what it cannot do (screenshots).

`describe` reports what it attached to (`attached`: *"Designer on port 8799:
bracket.dsgn (unsaved changes), gui"*), and the `instances` tool lists what is
running. That line comes from `ping`, live, not from the file — the file
carries only what does not change while the app runs.

Before this, the server assumed 8437 and reported "Designer is not reachable"
whenever the app was on another port; this repo's own `.mcp.json` hardcoded
8799 to work around it. It no longer passes a port at all.

`ui_action` (through `rpc`) needs the ImGui Test Engine, which is optional:
a build configured with `-DENABLE_TEST_ENGINE=OFF` refuses those calls naming
that reason, and `ping`/`describe` report `ui_actions: false` up front. Every
other tool, screenshots included, works there.

**Autostart is deliberately not implemented.** Spawning a window from an MCP
server would create a *second* instance while the user has one open, and the
agent would silently inspect a different document; a headless spawn cannot
screenshot, which is the main reason to use MCP over a script. The automated
case is already served by `designer --headless --remote <port>` in a script.
If it is ever added it must be opt-in, headless-only, and tied to the server's
lifetime (a spawned instance that outlives its parent sits on a port forever).

## What's exposed (revised 2026-08-17)

**Two dozen tools, plus a door to everything else.** Until August 2026 every
RPC was its own tool: 118 of them, ~34k characters of descriptions that every
client loaded before asking a single question (~8.5k tokens, before parameter
schemas). Most were one-line wrappers, and the ones that earn their place in a
*conversation* are far fewer — the rest are authoring calls that a script does
better, because a loop with logic between the steps is not expressible as one
tool call per step.

The exposed set is what a person asks an assistant for:

| Group | Tools |
|---|---|
| Meta | `describe`, `help`, `rpc` |
| Inspect | `get_state`, `check_geometry`, `measure`, `mass_properties`, `list_elements` |
| Variables | `list_variables`, `set_variable` |
| Look at it | `screenshot`, `render_still`, `view`, `set_camera`, `set_section` (and `add/list/update/remove_section` via `rpc`), `set_render_options` |
| Project | `new_project`, `open_project`, `save_project`, `export_stl`, `export_step` |
| Author | `add_feature`, `modify_feature`, `delete_feature`, `undo` |

Everything else — assemblies, drawings, studies, configurations, sketch
constraints and ops, checkpoints, topology, reference images, the raw GUI
actions — is reached through **`rpc(method, params)`**, and documented through
**`help(method)`**, which returns the same prose that used to sit in that
tool's description plus the wrapper's signature. Nothing was removed: the
wrappers still exist and `rpc` routes through them, so their argument shaping
and defaults are unchanged. A mistyped method suggests near matches.

`mass_properties` and `list_elements` remain the geometry oracles;
`screenshot`/`render_still` return inline images; `view` snaps standard views
and `set_camera` gives direct control. Topology `study_status`/`study_results`
(via `rpc`) report the solve's provenance (`solver`: `gpu: <device>` or
`cpu (...)`, plus elapsed seconds).

**When to use which.** MCP is for conversation: look at the model, measure it,
change one thing, show the user. For *building* a part — a parametric grid of
holes, a swept profile, anything with a loop or a computed argument — write a
Python script against `tools/designer_remote.py` and run it. The same RPCs,
one round trip, and arithmetic between the calls.

The remote server accepts several concurrent clients (e.g. the MCP server
plus a hand-driven `tools/designer_remote.py` shell); requests interleave
through the app's main thread and responses route back per connection.

The `modeling-workflow` prompt teaches the loop that works: variables first,
constrain fully, verify with `get_state` + `screenshot` after every step.

The feature/entity/constraint JSON dialects are documented in the tool
descriptions; the source of truth is `SerializeFeatures.cpp`
(`FeatureFromJson`), served at runtime by the `describe` RPC. Standing rule
(a feature an LLM can't drive is half-shipped), now test-enforced: a new
feature type must be added to the describe table beside the dispatch chain
(remote_smoke fails otherwise) and to `FEATURE_FORMATS` in
`designer_mcp.py` (mcp_smoke cross-checks it against describe).

## Testing

`ctest -R mcp_smoke` models a dimensioned bracket end-to-end through the MCP
layer (skips if the `mcp` SDK is missing). `remote_smoke` covers the RPC
underneath.

`mcp_smoke` is **gui-labeled**, so neither CI nor `ctest -LE gui` runs it —
which is how a plain syntax error in `designer_mcp.py` shipped on 2026-08-13
and went unnoticed for four days, with the server unloadable the whole time.
`mcp_server_compiles` and `py_client_compiles` now `py_compile` both files on
the default test path: milliseconds, no display, no SDK.
