# Scripting and the agent interface

Everything the GUI can do to a document is reachable from outside the
process, through one JSON-RPC server inside the app. A person in a Python
REPL, a generator script, a test and an LLM agent all drive the same command
layer the GUI drives. The standing rule: a new capability lands in the
command layer (an RPC with a `describe` row) first and in the GUI second.
A feature an agent cannot drive is half-built.

```
  designer --remote <port>          the app: one JSON-RPC server (src/remote/)
     ^            ^           ^
     |            |           |
  designer_remote.py   designer_sdk.py      designer_mcp.py
  (raw client)         (generated SDK)      (MCP server for agents)
```

Pages:

- [rpc.md](rpc.md) — the protocol, dispatch and the `MethodDoc` table, what
  a response and a refusal must carry, `batch` and `$ref`, typed inspection
  contracts.
- [mcp.md](mcp.md) — the MCP server: setup, finding the instance, autostart,
  the tool surface, `execute_script`.
- [sdk.md](sdk.md) — the Python SDK: the generated RPC surface, geometry
  references, validation stages, generator-owned components.
- [benchmark.md](benchmark.md) — the agent benchmark in `benchmarks/`.
- [../inspect/README.md](../inspect/README.md) — the inspection RPCs an agent
  verifies its work with.

## Instances: GUI, offscreen, headless

`--remote <port>` serves the RPC on 127.0.0.1 (`0` lets the OS choose; the
port is logged and announced). `--remote-bind-all` serves on 0.0.0.0 instead;
the interface is unauthenticated and reads and writes files, so that is for a
container with a namespaced network only.

| mode | flags | renderer | `ui_action` | use |
|---|---|---|---|---|
| gui | `--remote <port>` | yes | yes, when built with the ImGui Test Engine | the human's instance; GUI tests |
| offscreen | `--offscreen --remote <port>` | yes (Vulkan, no window, no display server) | no | agents and CI that need pictures |
| headless | `--headless --remote <port>` | none | no | pure document engine; CI without a GPU |

What a mode cannot do is refused naming the reason and the remedy: a
screenshot under `--headless` says to start without it or with `--offscreen`;
a UI action under `--offscreen` says there is no GUI to drive; a GUI build
configured with `-DENABLE_TEST_ENGINE=OFF` says the UI tier needs the test
engine. `--offscreen` conflicts with `--headless`, `--hidden` and
`--gui-tests` and requires `--remote`; each conflict is refused at argument
parsing rather than resolved by precedence.

An instance a program starts for its own use should die with that program:
`--exit-with-parent`, or `DESIGNER_EXIT_WITH_PARENT=<launcher pid>`, has the
kernel send it SIGTERM when its parent exits — SIGKILL included, where no
cleanup code runs — and the headless and offscreen loops stop cleanly on it.
The variable names the launcher, so an instance whose launcher died before
it started exits at once too; a value that is not a pid is refused, and so
is 1 (an orphan's parent reads 1, so it would arm nothing a launcher could
own -- a launcher that IS PID 1 leaves the variable unset). Linux
only (`prctl(PR_SET_PDEATHSIG)`); `tests/smoke_app.py` and the MCP server's
autostart set it.

An offscreen instance has no panels, so the viewport is the whole
framebuffer, and it projects the document into the renderers only when
something looks (see CLAUDE.md, "Driving / screenshotting the running app").
`tools/headless-preview.sh --start <port>` starts one in the background with
a scratch config directory and prints its pid; `--stop <port>` stops it
whatever `XDG_STATE_HOME` it was started under: it takes the pid from a
file `--start` writes in a state-independent run directory
(`$XDG_RUNTIME_DIR/designer-preview/<port>.pid`), else from the instance's
own `ping`, else from the registry under this shell's state dir and the
default one, and kills a candidate only when `/proc` shows a designer
serving that port. It clears the registry entry in the state dir the
instance itself was started with.

### The instance registry

Every instance that opens a remote port writes
`$XDG_STATE_HOME/designer/instances/<port>.json` —
`{app, pid, port, mode: gui|headless|offscreen, started}` — and removes it
when the port closes (`src/remote/InstanceRegistry.h`). A crash leaves the
file behind, so the pid is the truth: readers skip entries whose process is
gone and delete them. The file holds only what does not change while the app
runs; the open project and whether it is dirty come from `ping`, live.

`Designer.instances()` in `tools/designer_remote.py` reads the registry; the
MCP server and `designer_sdk.connect()` attach through it, so no client has
to guess a port. `tools/scratch_port.py` picks a port that is free per the
kernel, unclaimed per the registry, and never the human's.

`ping` reports `port`, `mode`, `ui_actions`, `project`, `dirty`, `gpu`,
`features` (optional subsystems compiled in, e.g. `cam`) and
`binary_replaced` — true when the executable on disk was rebuilt after this
process started, so the instance is serving old code.

## Clients

- **`tools/designer_remote.py`** — the stdlib client every script and smoke
  test imports. `Designer(port).call(method, **params)`; `request(method,
  params, timeout=)` for a dict and a per-call timeout; `wait_for(port)`;
  `instances()`. A refusal raises `RemoteError` (a `RuntimeError`) carrying
  the server's structured `.diagnostic` when there is one. `method` is
  positional-only, so `call("describe", method="add_mate")` works, and
  returns that method's row alone; an unknown name is refused with close
  matches. `help(method)` (also `call("help", method=...)`, answered in the
  client) is `method_help`, the lookup the SDK and the MCP server's `help`
  share: the method's doc, params (and `param_types` for a typed one) and
  returns from its `describe` row. Also
  a CLI:
  `designer_remote.py <port> <method> ['<params-json>']`.
- **`tools/designer_sdk.py`** — the SDK ([sdk.md](sdk.md)). Its RPC surface
  is generated from `describe` at connect time; the hand-written part is
  geometry references and compositions.
- **`tools/designer.py`** — a small fluent client (`d.sketch(...).line(...)
  .add()`, `d.extrude(...)`) over the same RPCs, tested by `pyclient_smoke`.
- **`tools/designer_mcp.py`** — the MCP server ([mcp.md](mcp.md)).

The server accepts up to eight connections at once. Connections are
concurrent; execution is not: every request goes through one main-thread
queue, and responses route back per connection. A screenshot, a sheet render
or a UI action holds the queue until it completes. A CAM request that waits
for toolpath passes (`cam_toolpath`, `cam_post`, `cam_verify`,
`cam_simulate`, `export_setup_sheet`) holds only its own connection: the others are answered
while it waits ([cam/README.md](../cam/README.md#requests-that-wait)).

## Recording and replay

`--record <file>` tees every successful semantic request to a JSONL file
(UI actions and screenshots are not recorded; they cannot replay headless).
`tools/designer_replay.py <port> <session.jsonl>` sends them to another
instance in order and stops at the first failure — a reported bug becomes a
replayable case. `replay_smoke` records a session and replays it into a
fresh headless instance.

## Fan-out: several agents, several parts, one project

Several agents can build the parts of one project at once when the work is
file-disjoint, which parts are: the `.dsgn` directory format is one file per
part, per mate, per occurrence, so git merges two agents' parts without
reading either.

```sh
git worktree add ../designer-wt1 -b wt1 && (cd ../designer-wt1 && tools/worktree-build.sh)
# worktree-build.sh clones the submodules from the main checkout (no
# network unless main lacks a pinned commit) and prints a scratch port
../designer-wt1/build/designer --headless --remote <port1> &
DESIGNER_PORT=<port1> claude -p "build the bracket in parts/bracket" ...
```

One worktree, one instance and one part per agent; the MCP server pins an
instance with `DESIGNER_PORT`. Integrate serially: merge one branch, run the
suites, then the next (CLAUDE.md, "Parallel work in git worktrees").
`branch_merge` refuses a merge that would produce duplicate ids;
`conflict_list` / `conflict_resolve` / `merge_finish` handle the rest.

Two instances on ONE project directory is the case to avoid, and it is loud:
the second `save_project` is refused because the project changed on disk
since it was opened (`project_changed_on_disk` asks; `force: true`
overwrites). That refusal is why a fan-out uses a worktree per agent.
Overlapping edits — two agents on one part, or on one assembly's mates — buy
nothing but a merge.

`fanout_smoke` (headless) gates it: three instances build three parts that
merge into one project, and a fourth instance opening a directory another is
editing has its save refused rather than clobbering the first.

## Iterating on generated models

A generator is a Python program that builds parts through the SDK
(`generators/`). Two SDK modules support re-running one without starting
over; both are described in [sdk.md](sdk.md#validation-stages):
`designer_validation.validate(app, stage=preview|local|full)` for staged
checks, and `designer_components.claim` / `plan` for rebuilding one
generator-owned part in place.
## Inspection snapshots

`tools/benchmark_inspection.py --project P --parts ... --output J` times live
circular-edge inspection against `geometry_snapshot` on a saved project, in its
own headless process with isolated config and state directories. It requires
both paths to agree on every edge name and centre before it accepts a timing.

## Where the code is

| what | where |
|---|---|
| socket, reader threads, recording | `src/remote/RemoteSession.cpp` |
| dispatch, `batch`, `describe`, `ping`, `get_state` | `src/remote/RemoteDispatch.cpp` |
| main-thread pump, strict-param check, error mapping, UI-action queue | `src/remote/RemoteUiActions.cpp` |
| domain handlers + their `MethodDoc` tables | `src/remote/Remote{Modeling,Project,Assembly,Motion,Inspection,Drawings,Configurations,SheetMetal,Tube,Simulation,Cam,Catalog,Requirements,Templates}.cpp` |
| shared helpers (`MethodDoc`, flags, regen/digest, `KnownList`, `IndexError`, `ResolvePartParam`) | `src/remote/RemoteHelpers.h` |
| instance registry | `src/remote/InstanceRegistry.{h,cpp}` |
| typed inspection contracts | `api/inspection-contracts.json`, `src/remote/InspectionContracts.h`, `tools/generate_inspection_contracts.py` |
| clients | `tools/designer_remote.py`, `designer_sdk.py`, `designer.py`, `designer_mcp.py`, `designer_replay.py` |
| SDK support modules | `tools/designer_validation.py`, `designer_components.py`, `designer_snapshots.py`, `designer_inspection.py` (generated), `designer_contracts.py` |
| packaging of the MCP server | `pyproject.toml`, `.mcp.json` |

## How it is tested

Headless, on the default `ctest -LE gui` path:

- `headless_smoke` — the document surface over `--headless`, including each
  mode refusal.
- `rpc_params_gate` — every `MethodDoc` row's parameter list matches the keys
  its handler reads, in both directions (source scan, `tools/rpc_params_scan.py`).
- `rpc_returns_gate` — every `list_*`, `get_*`, `check_*` row states its
  response shape.
- `rpc_names_gate` — a method that resolves a feature takes `feature_id`.
- `rpc_units_gate` — a parameter naming a physical quantity carries its unit.
- `rpc_gate_smoke` — every method flagged `NeedsPart` touches the part
  document in its handler, or says why it is gated anyway.
- `batch_smoke`, `regen_honesty_smoke`, `strict_inputs_smoke`,
  `papercuts_smoke`, `tabless_read_smoke`, `refused_calls_smoke` — the
  contracts in [rpc.md](rpc.md).
- `sdk_smoke`, `pyclient_smoke`, `replay_smoke`, `fanout_smoke`,
  `component_update_smoke`, `inspection_contracts_smoke`,
  `inspection_contracts_generated`, `inspection_typing_gate`.
- `mcp_formats_gate`, `mcp_server_compiles`, `py_client_compiles`,
  `packaging_smoke`, `mcp_autostart_smoke` — the MCP server ([mcp.md](mcp.md)).
- `offscreen_smoke` — the offscreen renderer over RPC (skips without a
  Vulkan device).

GUI-labelled: `remote_smoke` (the RPC surface including screenshots and UI
actions, and the measure parity gate) and `mcp_smoke` (a bracket modelled
through the MCP stdio layer).

## What it does not do yet

- A write to a part that is not the active tab. Reads name their part; writes
  act on the active tab, because a part's undo history is per tab
  ([rpc.md](rpc.md#reads-name-their-part)).
- Typed request/result contracts beyond the two inspection-snapshot methods
  ([../roadmap.md](../roadmap.md#6-the-agent-interface)).
- A channel for an unattended agent to ask the human a question mid-task;
  `background_jobs` covers the asynchronous half only.
