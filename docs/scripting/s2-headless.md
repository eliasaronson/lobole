# S2 — Headless mode (`--headless`) — design

> **SHIPPED 2026-07-10** as designed, plus one addition found during T5
> review: a `list_elements` RPC/tool (stable face/edge names + `created_by`
> per body, pure document state) so agents can build Fillet/Chamfer edge
> refs — landed with the I2 step below.
>
> **2026-08-26:** a third mode sits between this and the GUI now:
> `--offscreen` runs the same engine+remote WITH a real Vulkan renderer and
> no window/display server — screenshot, render_still, render_sheet,
> set_render_options and select_face work there; only the ui_action tier
> still needs a window. The refusal table below is current for `--headless`
> only. See docs/scripting/mcp.md and tests/offscreen_smoke.py.

Run the document/geometry engine with **no window, no GPU, no display server**:
serve the JSON-RPC remote (and therefore MCP) from a machine with nothing but
a CPU. Payoff: geometry tests + scripted regeneration on CI, and agents can
drive Designer anywhere. Estimated **~2 days** including the mass-properties
RPC and a CI workflow.

## What already exists (grounded in code, Jul 2026)

- `--remote` is *window-visibility* headless only: `main.cpp` still runs
  `glfwInit` → Vulkan instance/device → swapchain → full ImGui + renderers; it
  just never maps the window (`pureHeadless` at `main.cpp:845`). GLFW alone
  makes it fail without a display server.
- The core is already GPU-free by construction: `designer_core` (Document,
  features, PlaneGCS solve, OCCT booleans/tessellation, Camera, FaceSelector)
  compiles without Vulkan/GLFW/ImGui — 227 unit tests prove it headless daily.
- `RemoteServer` is close to ready: a socket reader thread queues requests;
  `ProcessPending(state)` executes them on the caller's thread. Its only GUI
  couplings are the `ImGuiTestEngine*` (UI actions), the screenshot handshake
  with the frame loop, and null-unguarded uses of `state.camera` /
  `state.faceSelector` / `state.rayTracer` in a few handlers.
- `ui::AppState` holds owned `document`/`commands` and non-owning renderer
  pointers that are **already null-guarded** in `SyncDocumentRenderers` (and
  most of AppActions) — the struct works as a partial container.

## Design

### Same binary, skipped init

`designer --headless --remote <port>` stays in the existing binary (it links
GLFW/Vulkan/ImGui but never calls them — linking needs no device or display).
No second target, no `#ifdef` build: `main()` branches before `glfwInit`:

```
headless path: Logger → AppState{document, commands, camera} →
               RemoteServer::Start(port, /*engine=*/nullptr) →
               loop { ProcessPending; sleep 1ms } until SIGINT/SIGTERM
```

The headless path owns a real `Camera` (it's in `designer_core`), so view
state — `view`, `fit_view`, `get_state.camera` — behaves identically to the
GUI; a later GUI session opening the same project sees the same framing
conventions. `--headless` without `--remote` is an error (nothing to do).

### Per-method behavior headless

Full document/geometry surface works unchanged: `add_feature`,
`modify_feature`, `delete_feature`, `add_constraint`, sketch ops, variables,
undo/redo, project I/O, `import_step`, `get_state` (with `selection: null`,
no `mesh` ids on bodies), `view`/`fit_view`, and the new `mass_properties`.

Degraded methods respond with a **specific, actionable error** (never
"unknown method", never a crash):

| Method | Headless response |
|---|---|
| `screenshot` | error: "screenshot requires the GUI — start designer without --headless" |
| `render_still`, `rt_build` | error: same wording (needs a Vulkan device) |
| `select_face`, `orient_to_face` | error: "no render meshes in headless mode" (see Deferred) |
| `menu_click` … `scroll` (UI actions) | error: "UI actions unavailable in --headless" |

Mechanically: `RemoteServer` gains a `m_headless` flag (engine == nullptr);
`QueueUiAction` and the screenshot branch short-circuit to the error, and the
audit below adds null guards where handlers touch renderer state.

**Audit list** (every `HandleSemantic` branch that dereferences beyond
document/commands/camera): `get_state` (faceSelector, documentMeshes),
`rt_build`/`render_still` (rayTracer, `ui::RenderStillBlocking`),
`select_face` (faceSelector, meshRenderer), `orient_to_face` (faceSelector),
`accept_pending`/`cancel_pending` (preview code paths → verify
`ui::AcceptPendingFeature` is renderer-safe), `import_step`
(`ui::LoadSTEPFile` → `FitCameraToDocument` + sync, already guarded).

### I2 fold-in: `mass_properties` RPC + tool

The inspect plan calls I2 "nearly free — do early", and the scripting plan
flags it as the LLM verification gap. Landing it with S2 gives headless CI
runs a geometry oracle richer than volume/bbox:

- `mass_properties {body?: index}` (default: all bodies + a combined total) →
  per body: `volume`, `surface_area`, `center_of_mass`, `inertia` (3×3 about
  the CoM), `material`, `density`, `mass`. OCC `BRepGProp::
  VolumeProperties/SurfaceProperties` on `CADShape` (same GProp machinery
  `Volume()` already uses).
- `CADMaterials` gains **density** (kg/m³) per material; unassigned bodies
  report `mass: null` rather than assuming a density.
- MCP: a `mass_properties` tool + a line in the workflow prompt ("verify with
  mass_properties, not just volume").
- Tests: analytic solids (box, cylinder, extruded rect with hole, the
  filleted bracket) match volume/CoM/inertia to tolerance — pure unit tests
  in `designer_core`, no app needed.
- The I2 GUI panel is **not** in scope (command layer first; panel ships with
  the inspect workstream later).

### Tests

`tests/headless_smoke.py`, a ctest **without** the `gui` label (runs
everywhere, including CI): starts `designer --headless --remote`, then
- models the S1 bracket over RPC (sketch → constraints → extrude → fillet),
  asserts volumes + `mass_properties` against analytic values;
- exercises save → new → open round-trip and undo/redo;
- asserts each degraded method returns its specific error string;
- asserts the process exits cleanly on SIGTERM.

`remote_smoke` / `mcp_smoke` stay `gui`-labeled and unchanged (they cover the
screenshot/UI surface).

### CI (the payoff)

`.github/workflows/ci.yml` on ubuntu-latest: apt deps straight from the
README (also validates those instructions), `git submodule update --init`,
CMake + Ninja build (needs `glslc` for shaders; compiling SPIR-V needs no
GPU), then `ctest -LE gui` — 227 unit tests + `headless_smoke`, no display,
no Vulkan device. GUI tests stay local. Catch2 comes via `find_package` or
the existing FetchContent fallback.

### Deferred (explicitly not in S2)

- **`--run script.jsonl` batch mode**: `--headless --remote` + a 10-line
  python driver already covers scripted regeneration; a first-class replay
  format is S4's design question — don't preempt it.
- **GPU-free face queries**: `SyncDocumentRenderers` tessellates on the CPU
  (`GenerateMesh`) — only the mesh *ids* come from the renderer. A headless
  sync feeding `FaceSelector` with synthetic ids would light up
  `select_face`/face queries (and future `measure` refs) without a GPU.
  Valuable, but not needed for the CI payoff; do it when `measure` (I1)
  lands.
- **Nix-based CI**: the apt path exercises the README; revisit if the two
  dependency sets drift.

## Steps

1. **S2.1** `main.cpp` split + headless loop + RemoteServer degradation
   (audit list above), `--headless` flag + usage text.
2. **S2.2** `tests/headless_smoke.py` + ctest wiring (no `gui` label).
3. **S2.3** I2: density table, `mass_properties` RPC, unit tests, MCP tool +
   prompt line, FORMATS note.
4. **S2.4** GitHub Actions workflow; badge in README once green.
5. **S2.5** Docs: mark S2 shipped in plan.md/roadmap; update mcp.md with the
   headless recipe (`designer --headless --remote 8437` under an agent).

## Risks / notes

- **Hidden GUI couplings** in semantic paths (preview accept/cancel, STEP
  import) — the audit list is the mitigation; headless_smoke's error-path
  assertions pin the behavior.
- **CI OCCT version skew** (Ubuntu 24.04 ships 7.6 vs local 7.9): the CMake
  handles the 7.8 library rename in both directions and unit tests assert
  geometry to tolerance, so skew is signal (a real incompatibility), not
  noise.
- **Process lifetime**: the headless loop must exit non-zero if the remote
  port can't bind (today `Start` failure only logs) so CI fails fast.
