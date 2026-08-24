# Architecture review + cleanup plan (2026-08-24)

> Status: **IN PROGRESS.** Five stages, each its own commit. Stage status is
> tracked in the table below; deferred follow-ups are recorded at the bottom so
> they are decisions, not omissions.

A structural pass over the whole tree — layering, duplication, unit sizes,
target structure, dead code. The findings are measured on the tree at
`b1f0330`, not estimated.

| Stage | What | Risk | State |
|---|---|---|---|
| 1 | Dead code purge (1,125 lines) | low | pending |
| 2 | `render/VulkanCommon` + the divergent contracts | medium | pending |
| 3 | Split `RegisterGuiTests` by domain | low | pending |
| 4 | `kFeatureTypes` becomes authoritative | low | pending |
| 5 | Contained `ui/` decoupling | low | pending |

---

## What the survey found

~102k lines of source, 23k of tests.

| Layer | Lines | Library? | Unit-testable |
|---|---|---|---|
| `src/core` + `src/cad` + `src/common` | ~35k | `designer_core` | yes (63 test files) |
| `src/ui` | 31k | no | GUI engine only |
| `src/render` | 10.5k | no | no |
| `src/remote` | 8.7k | no | no |

**What is already good, and should not be "fixed":** layering is clean —
`core/` never includes `ui/`, `render/`, or `remote/`; `common/` includes
nothing up-stack. `core/features/` is well factored (38 files, none over 300
lines outside `SketchFeature`). The RPC domain split is real. `RpcParams`'
unknown-key rejection is a genuinely good design. Three TODOs in the entire
codebase, 16 raw `new`, 7 `catch(...)`. The problems below are concentrated,
not diffuse.

### 1. Duplicated Vulkan helpers have already diverged into a bug

`FindMemoryType` exists **11 times**, and the copies do not agree on what
failure means:

```
MeshRenderer, OutlineRenderer, RayTracingRenderer   throw std::runtime_error
SketchRenderer, FieldRenderer, UnderlayRenderer     throw std::runtime_error
SSAORenderer, ShadowRenderer, main.cpp              return 0          ← wrong
IBLTextures, Screenshot                             return UINT32_MAX
```

`return 0` is not a sentinel — memory type 0 is a valid index. Those copies
allocate from an arbitrary heap that lacks the requested properties instead of
failing, so a later `vkMapMemory` fails or an upload silently never lands.
`UINT32_MAX` is the only value `vkAllocateMemory` actually rejects.

`CreateBuffer` exists 7 times under **two incompatible contracts** —
`bool CreateBuffer(...)` (Mesh, SSAO, Underlay) vs
`VkBuffer CreateBuffer(...)` (Outline, RayTracing, and the two dead headers).
Same name, opposite meaning.

The `bool` one is unchecked at **17 call sites**. `MeshRenderer.cpp:1041`:

```cpp
VkBuffer vertexStagingBuffer;                  // uninitialized
VkDeviceMemory vertexStagingBufferMemory;      // uninitialized
CreateBuffer(vertexBufferSize, ..., vertexStagingBuffer, vertexStagingBufferMemory);
                                               // ↑ returns bool, dropped
vkMapMemory(m_device, vertexStagingBufferMemory, 0, vertexBufferSize, 0, &data);
```

On `vkAllocateMemory` failure `CreateBuffer` returns false with the out-param
never written — and it is passed to `vkMapMemory` anyway.

Measured: **`MeshRenderer.cpp` ↔ `OutlineRenderer.cpp` share 145 identical
lines across 10 runs** (longest 24, in `CopyBuffer`). All 8 renderers carry the
same 6-member tuple (`m_device`, `m_physicalDevice`, `m_graphicsQueue`,
`m_queueFamily`, `m_commandPool`, `m_renderPass`) plus their own
`FindMemoryType` / `CreateBuffer` / shader loader / buffer-destroy. Shader
loading exists in two styles (`LoadShader(spvName)` vs
`ReadFile` + `CreateShaderModule`), ~11 copies, also divergent on failure
(`SSAORenderer::CreateShaderModule` returns `VK_NULL_HANDLE`, Mesh/Outline
throw).

**The abstraction already exists in this repo**: `femgpu/ComputeContext.h` has
`Buffer`, `Pipeline`, and `bool CreateBuffer(size, out, std::string* error)`,
all checked. It was simply never applied to `render/`.

`roadmap.md` lists "extract a `VulkanContext` + shared renderer utilities" as a
standing item gated on 6b hybrid RT, reasoning that untestable code makes
refactoring pure regression risk. That reasoning holds for restructuring
`main.cpp`. It does not hold for the three contracts above: they are already
wrong, and unifying the helpers does not require touching `main.cpp`'s
structure.

### 2. Dead code, compiled into the binary

All verified zero external references:

| | Lines | What |
|---|---|---|
| `cad/Sketch.{h,cpp}` | 343 | `SketchElement`/`Point`/`Line`/`Circle`/`Rectangle`/`Sketch` — a **complete parallel sketch model**, superseded by `core/sketch/` + `SketchFeature`. Only `SketchManager.h` includes it. |
| `cad/SketchManager.{h,cpp}` | 262 | included only by its own `.cpp` |
| `cad/CADObject.{h,cpp}` | 202 | reachable only via one unused `#include` in `CADMeshConverter.h` |
| `render/Scene.{h,cpp}` | 100 | vestigial scene graph — `AddObject`/`GetObject`/`GetObjects` have **zero call sites**; `main.cpp` calls `Update(deltaTime)` every frame on a permanently empty vector; `AppState::scene` is assigned once and never read |
| `common/ColorPicker.h` | 102 | on the roadmap purge list since July |
| `render/FaceHighlighter.h` | 116 | same list — and it **received a bug fix on 2026-08-20** (`10c3524`) while dead |

The first four are new findings. All but the two headers are in `core_sources`,
so they compile into `designer_core` and ship.

### 3. One library target distorts where code lives

`designer_core` is the only unit-testable target; `ui/` + `render/` + `remote/`
(50k lines) are reachable only through `RegisterGuiTests` inside the app binary.
The codebase already works around this, visibly:

- `remote/InstanceRegistry.cpp` is pulled into `core_sources` with a comment
  saying why: *"Pure logic despite living under remote/ ... so the tests can
  link and exercise it."*
- `TerminalScreen.cpp` + `PtySession.cpp` (913 lines of VT emulation) live in
  `src/common/` — not because they are common, but because `test_terminal.cpp`
  must link them.

ImGui coupling in the actions layer is much lighter than its size suggests:

```
ui/AssemblyActions.cpp     1,433 lines   0 ImGui symbols  (but #includes imgui.h)
ui/SimulationActions.cpp     539 lines   0 ImGui symbols
ui/AppActions.cpp          4,707 lines   9 ImGui calls — 3 functions (866–968) + one at :3635
```

**But ImGui-free is not renderer-free.** Those files call 22 distinct
`MeshRenderer`/`OutlineRenderer`/`RayTracingRenderer` methods (`AddMesh`,
`CreateAsset`, `SetInstanceTransform`, `BuildAccelerationStructures`, …). A
library extraction therefore does not by itself buy testability — it needs a
renderer seam. See "Deferred" below.

### 4. `AppState` is the compile-time bottleneck

**214 members, 1,186 lines, included by 43 files**, dragging in 14 core headers
plus `nlohmann/json` and `glm`. `AppActions.h` sits on top with ~180
declarations spanning every domain in the app. From `.ninja_log`:

```
40.6s GuiTests.cpp    36.3s AppActions.cpp   30.9s RemoteServer.cpp
29.6s SerializeFeat.  28.6s RemoteInspect.   26.4s StudyPanel.cpp
```

Related: **5 of 47 `ui/*.cpp` have no header of their own** —
`AssemblyActions`, `DrawingPanel`, `OccurrencePanel`, `StudyPanel`,
`SimulationActions` declare their entry points inside `AppActions.h`.

### 5. Oversized units

```
8,036  RegisterGuiTests()      GuiTests.cpp:496  — ONE function, 175 tests, 40s to compile
  804  HandleSketchViewport()  SketchInteraction.cpp:1290  — max brace depth 7
  775  main()                  main.cpp:1120
  499  DrawHistoryPanel()      HistoryPanel.cpp:270
  479  FeatureFromJson()       SerializeFeatures.cpp:971
  466  DrawFeatureTreePanel()  FeatureTreePanel.cpp:155
```

### 6. Adding a feature type is shotgun surgery

~8 sites: `core/features/`, three separate places in `SerializeFeatures.cpp`
(the `FeatureToJson` cast chain, the `FeatureFromJson` string chain, and the
`kFeatureTypes` doc table), `FeatureEditor.cpp` (72 `dynamic_cast`s over 23
types), `FeatureTreePanel.cpp`, `AppActions.cpp`, `Toolbar.cpp`, `Commands.cpp`.

`SerializeFeatures.cpp:896` says *"Keep in lockstep with the FeatureFromJson
chain below"* — **no test enforces it.** A type with a `FromJson` branch but no
table row simply vanishes from `describe`, which is the same failure class as
the CAM/drawings gate lists recorded in `docs/cam/` and `docs/drawings/`.

Minor: `BoxFeature`, `SphereFeature`, `CylinderFeature`, `ImportFeature` are
defined inline in `core/Feature.h` while the other 20 live in `core/features/`.

### 7. Contract drift in the RPC layer

`RemoteHelpers.h` documents `regen` as *"appended to every mutating RPC's
response."* Actual: `RemoteModeling` 7, `RemoteProject` 1, everything else 0.
`RemoteConfigurations`' `set_active_configuration` regenerates geometry, then
hand-rolls a **third** format — `{"active", "bodies", "volumes"}` —
reimplementing part of `BodyDigestJson` inline, with no `regen.failed`, so a
configuration that breaks a feature returns success. `Vec3ToJson` is defined
three times, one of them (`RemoteServer.cpp:313`) shadowing the
`RemoteHelpers.h` copy the same file includes.

*Not scheduled in this round* — it is a behavior change to the RPC surface and
belongs with whoever next extends those modules, per the roadmap's
"split what the next feature extends" policy.

### 8. The docked-panel preamble

Six panels hand-repeat `Begin(kPanelX, nullptr, NoFocusOnAppearing)` /
`if (!visible) { End(); return; }`. `CamPanel.cpp:142` carries the comment
explaining what happens when you get it wrong — it shipped without the flag,
stole the shared dock node, and three GUI tests failed as "unable to hover
Model". `StudyPanel.cpp:445` ignores `Begin`'s return entirely.

*Not scheduled in this round* — same reasoning as §7.

---

## The plan

### Stage 1 — Dead code purge

Delete the ten files in §2. Drop the four `core_sources` entries, the unused
`#include "cad/CADObject.h"` in `CADMeshConverter.h`, the `Scene` wiring in
`main.cpp` (declaration, construction, per-frame `Update`, reset, include) and
`AppState::scene` plus its forward declaration.

Pure subtraction; no behavior change.

### Stage 2 — `render/VulkanCommon`

New `src/render/VulkanCommon.{h,cpp}`, free functions, one contract each:

- `FindMemoryType(phys, typeFilter, props)` → `UINT32_MAX` on failure, never `0`
- `CreateBuffer(dev, phys, size, usage, props, VkBuffer&, VkDeviceMemory&)`
  → `bool`, out-params set to `VK_NULL_HANDLE` on **every** failure path
- `DestroyBuffer`, `CopyBuffer`, `BeginSingleTimeCommands`/`EndSingleTimeCommands`
- `LoadShaderModule(dev, spvName)` — one style, resolving through
  `RuntimePaths::ShaderDir()`

Replace the copies across the 8 renderers, `IBLTextures`, `Screenshot` and
`main.cpp`, and **check the 17 dropped return values**.

Verified with the GUI suite on a real GPU plus before/after screenshots — none
of this is unit-testable, so the renderers must be seen working.

### Stage 3 — Split `RegisterGuiTests` by domain

8,036 lines in one function → `src/app/guitests/` per domain (sketch, modeling,
assembly, drawing, cam, simulation, project/versioning, ui-shell, terminal),
each exposing `RegisterXxxTests(engine, state)`, called from a thin
`GuiTests.cpp`.

**Test names stay byte-identical.** `gui-tests-xvfb.sh` builds its partition
from the binary's own `--gui-test-list` and asserts every listed test ran, so
any drift fails loudly rather than silently skipping.

### Stage 4 — `kFeatureTypes` becomes authoritative

Rather than a test that can only check one direction, gate `FeatureFromJson` on
the table up front: an unknown type is rejected before the `if`-chain is
reached. A new branch without a table row then fails loudly on its first
round-trip instead of silently vanishing from `describe`. Plus a test walking
every row through `FeatureFromJson`.

### Stage 5 — Contained `ui/` decoupling

- Move `AppActions.cpp`'s three ImGui overlay functions (`DrawDatumLabels`,
  `DrawPromptPill`, `DrawElementNameOverlay`) and the one `IsAnyItemActive()`
  use into `src/ui/Overlays.cpp`, dropping `imgui.h` from a 4,707-line file.
- Drop the unused `#include "imgui.h"` from `AssemblyActions.cpp`.
- Give the five headerless `ui/` modules their own headers so their
  declarations leave `AppActions.h`. Each moved panel declaration has only 2–3
  call sites, so the ripple is contained.

---

## Deferred (decisions, not omissions)

**A. `designer_app` static library.** Extract `ui/` + `render/` + `remote/`
(minus `main.cpp`, `GuiTests.cpp`, `HeadlessMain.cpp`, `CrashHandler.cpp`) into
a library `designer_tests` can link. Deferred because on its own it does not
produce a single new test: the actions layer calls 22 renderer methods, so a
test either constructs a real `MeshRenderer` (needs a GPU) or relies on
null-pointer guards that are not uniformly present. Worth doing **together with
B**, not before it.

**B. A renderer seam.** An interface (or null-object) over the
`MeshRenderer`/`OutlineRenderer`/`RayTracingRenderer` surface the actions layer
uses — `AddMesh`, `RemoveMesh`, `UpdateMesh`, `CreateAsset`, `AddInstance`,
`RemoveInstance`, `SetInstanceTransform`, `SetInstanceColor`,
`SetInstanceVisible`, `SetInstanceGhost`, `SetInstanceMaterial`,
`SetInstanceProcedural`, `SetMesh`, `SetMeshTransform`, `SetMeshColor`,
`SetMeshMaterial`, `SetMeshProcedural`, `AddGeometry`, `ClearGeometry`,
`AddMaterial`, `ClearMaterials`, `BuildAccelerationStructures`. With a stub
implementation, ~6,600 lines of document-mutating logic
(`CreateBox`…`CreateRib`, `AddPart`, `DeletePart`, `ComputeBom`, `AutoExplode`,
`SolveActiveAssembly`, folder management, project variables) becomes testable
in the 8-second headless loop instead of the 80-second GUI suite.

This is effectively its own workstream and should be scheduled like one — most
naturally alongside the `main.cpp` dismantle, which the roadmap already gates on
6b hybrid RT.

**C. RPC `regen`/`digest` contract (§7)** and **D. the panel preamble (§8)** —
both are behavior changes to surfaces with live callers, and both fit the
roadmap's "split what the next feature extends, not big-bang" policy better than
a standalone sweep.
