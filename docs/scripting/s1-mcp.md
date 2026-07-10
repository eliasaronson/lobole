# S1 — MCP server (`tools/designer_mcp.py`) — design

Package the existing remote-control surface as an MCP server so any MCP client
(Claude Code first) can drive a running Designer instance with first-class
tools instead of raw JSON-RPC strings. Estimated **~1 day**; the schema/
description writing is the long pole, the plumbing is thin.

## What already exists (grounded in code, Jul 2026)

`src/remote/RemoteServer.cpp` (755 lines) serves newline-delimited JSON-RPC
over TCP (`--remote <port>`), with three request classes:

- **Semantic** (`HandleSemantic`, executed on the main thread in
  `ProcessPending` before frame sync): `ping`, `get_state`, `add_feature`,
  `modify_feature`, `delete_feature`, `select_feature`, `set_material`,
  `add_constraint`, `delete_entity`, `mirror`, `fillet`, `pattern`, `offset`,
  `set_variable`, `delete_variable`, `rename_variable`, `list_variables`,
  `undo`, `redo`, `new_project`, `save_project`, `open_project`,
  `import_step`, `fit_view`, `select_face`, `orient_to_face`,
  `accept_pending`, `cancel_pending`, `rt_build`, `render_still`.
- **Screenshot**: deferred until after the next present, then written to a PNG
  path (`src/Screenshot.cpp` offscreen re-render).
- **UI actions** (queued through the ImGui Test Engine, one in flight):
  `menu_click`, `item_click`, `item_input`, `viewport_click`, `viewport_drag`,
  `key_press`, `scroll`.

`tools/designer_remote.py` is the stdlib client (`Designer.call`,
`Designer.wait_for`). The official MCP Python SDK is available as
`python3Packages.mcp` (1.27.0 verified in the current nixpkgs channel).

## Design

### Process & connection model

- **stdio MCP server**, `FastMCP`, launched by the client (e.g.
  `claude mcp add designer -- python3 tools/designer_mcp.py --port 8437`).
- It **connects to an already-running** `designer --remote <port>` (port from
  `--port` or `DESIGNER_PORT`, default 8437). Lazy connect on first tool call,
  reconnect on drop. If unreachable, tools fail with the remedy in the
  message: "start designer with `--remote 8437`".
- It does **not** spawn the app. Auto-spawning a GUI from an MCP server is
  surprising, and S2 (headless) is the right owner of that story.

### Tool surface — curated, same names as the RPC

Tool names stay **identical to the RPC methods** (docs, tests, and muscle
memory transfer 1:1); curation happens in what we expose and how we document
it, not in renaming:

| Group | Tools |
|---|---|
| Inspect | `get_state`, `list_variables` |
| Model | `add_feature`, `modify_feature`, `delete_feature`, `set_material` |
| Sketch | `add_constraint`, `delete_entity`, `mirror`, `fillet`, `pattern`, `offset` |
| Variables | `set_variable`, `rename_variable`, `delete_variable` |
| History | `undo`, `redo` |
| Project | `new_project`, `open_project`, `save_project`, `import_step` |
| View & verify | `screenshot`, `view` (new), `fit_view`, `select_face`, `orient_to_face`, `render_still` |
| Escape hatch | `ui_action` — one tool multiplexing the 7 raw UI events |

Not exposed: `ping` (the server health-checks internally), `rt_build`
(subsumed by `render_still`), `select_feature`/`accept_pending`/
`cancel_pending` (GUI-session concepts; the semantic API commits directly).
`ui_action` is described as "for driving/testing the Designer GUI itself —
prefer the semantic tools for modeling", which keeps modeling agents off it.

### `screenshot` returns the image

The RPC writes a PNG to a path; the MCP tool reads that file back and returns
an MCP **image content block** (base64 PNG) so the agent sees the viewport
inline — the single biggest ergonomics win over raw RPC. Default path goes to
a tempdir; an explicit `path` param also keeps the file. Composes with `view`:
`screenshot(view="iso")` orients + fits + captures in one call.

Assumption: server and app share a filesystem (both local). Fine for S1; a
`screenshot_b64` RPC could lift it later if remote use ever matters.

### `view` RPC — the one C++ addition

The framing gap called out in [plan.md](plan.md): an agent needs standard
views to compose a useful snapshot. New semantic method in `RemoteServer.cpp`:

```
view {name: front|back|left|right|top|bottom|iso, fit?: bool = true}
```

Sets the camera orientation from the named direction (same convention as the
app's Front/Top/Right default planes), keeps the pivot, then
`ui::FitCameraToDocument` when `fit`. Reuses the
`Camera::OrientationLookingAlong` machinery `AlignToFace` uses — no new camera
math. Covered by a `remote_smoke` case asserting `get_state.camera` moves.

### Format documentation — the real work

An LLM can only use `add_feature`/`add_constraint` if the JSON dialects are in
the tool descriptions. Single source of truth: a `FORMATS` reference in
`designer_mcp.py`, embedded into the relevant tool descriptions:

- **Features** (`Serialization.cpp` `FeatureFromJson`): `Box`
  {position,width,height,depth}, `Sphere` {center,radius}, `Cylinder`
  {center,direction,radius,height}, `Sketch` {plane{origin,normal,uAxis} |
  plane_ref, entities[], constraints[]}, `Extrude` {sketch_id, distance,
  distance_expr?, reversed?, mode: new_body|add|cut}, `Revolve`, `Boolean`,
  `Pattern`, `DatumPlane` {kind: offset|angled|through_points,
  parent_id|base_plane, …}, `DatumAxis`, `Import` {path}. Expression-capable
  fields (`distance_expr`, `offset_expr`, `angle_expr`) documented alongside
  their literal twins. `FeatureFromJson` is the single source of truth and
  grows with every modeling milestone — transcribe the exact field sets from
  it at implementation time.
- **Sketch entities**: point {p}, line {a,b}, circle {center,radius}, arc
  {center,radius,start_angle,end_angle}, ellipse
  {center,r_major,r_minor,rotation}; optional `construction: true`. Entity ids
  are assigned by the app — `add_feature` returns `entity_ids` in list order;
  clients must use those (never guess).
- **Constraints** (`add_constraint` params): the 19 kinds, `refs:
  [[entity_id, which], …]` with the `which` semantics (line 0=a 1=b; circle
  0=center; arc 0=center 1=start 2=end; whole-entity kinds ignore it), datum
  refs (origin `0xFFFFFF00`, X axis `+1`, Y axis `+2` — exposed as named
  constants in the docs), `value` vs `expr` (angles in degrees).

### Workflow prompt

One MCP prompt, `modeling-workflow`, encoding the loop the text-to-cad survey
validated: brief → `set_variable` for every driving dimension
(parameters-first, design intent in names) → sketch with constraints (aim for
fully-constrained) → extrude → **verify: `get_state` (volumes, bboxes) and
`screenshot` after every mutating step** → iterate. Plus the standing rules:
never guess entity ids; prefer expressions over literals; `undo` beats
manually reversing a bad edit.

### Smoke test — no LLM in the loop

`tests/mcp_smoke.py` (ctest `mcp_smoke`, label `gui`): launches
`designer --remote`, drives the MCP server **through the SDK's stdio client**
(so the MCP layer itself is exercised, not just the RPC underneath), and
models a small dimensioned bracket: variables → sketch (lines + constraints)
→ extrude → sketch fillet → asserts body volume from `get_state` within
tolerance → `screenshot` returns a decodable PNG. Follows the
`remote_smoke.py` pattern for app lifecycle.

## Steps

1. **S1.1** `view` RPC (C++) + `remote_smoke` coverage.
2. **S1.2** Server core: connection manager, all tools thin over
   `designer_remote.Designer`, image-returning `screenshot`.
3. **S1.3** `FORMATS` reference + tool descriptions + `modeling-workflow`
   prompt.
4. **S1.4** `mcp_smoke` ctest; `shell.nix` gets
   `python3.withPackages (ps: [ ps.mcp ])`.
5. **S1.5** Live end-to-end: model the bracket through the MCP tools from a
   real agent session; fix what chafes. Docs: `docs/scripting/mcp.md` usage +
   mark S1 in [plan.md](plan.md) / the roadmap table.

## Risks / notes

- **SDK drift**: the `mcp` package moves fast; stick to the stable `FastMCP`
  surface (tools + prompts + image content), pin expectations to ≥1.x.
- **Blocking calls**: `render_still` runs synchronously in the app's main
  loop; the tool passes a generous client timeout and documents that the GUI
  freezes for the duration (matches the GUI Render button behavior).
- **Tool-count budget**: ~27 tools is fine for current clients; the `ui_action`
  multiplex keeps the raw-event surface from adding 7 more.
- **Screenshot freshness**: the screenshot RPC already serializes behind
  pending semantic requests (`ProcessPending`), so a `screenshot` issued after
  `add_feature` captures the regenerated document — no explicit sync needed.
