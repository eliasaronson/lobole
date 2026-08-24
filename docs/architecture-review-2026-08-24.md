# Architecture review + cleanup plan (2026-08-24)

> Status: **ALL FIVE STAGES SHIPPED 2026-08-24** (3016fda, 4aaeb6b, 6e73bb6,
> fbe53d3, 8398047), plus the `designer_app` library and the first headless
> tests of the actions layer (29402e7). The two follow-ups this review deferred
> are now RESOLVED at the bottom — one shipped, one struck because its premise
> turned out to be wrong.

A structural pass over the whole tree — layering, duplication, unit sizes,
target structure, dead code. The findings are measured on the tree at
`b1f0330`, not estimated.

| Stage | What | Risk | State |
|---|---|---|---|
| 1 | Dead code purge | low | **done** `3016fda` — 1,148 lines |
| 2 | `render/VulkanCommon` + the divergent contracts | medium | **done** `4aaeb6b` — −702 lines, 3 latent bugs closed |
| 3 | Split `RegisterGuiTests` by domain | low | **done** `6e73bb6` — 40.6 s → 3.7 s incremental |
| 4 | `kFeatureTypes` becomes authoritative | low | **done** `fbe53d3` — both drift directions now fail |
| 5 | Contained `ui/` decoupling | low | **done** `8398047` — `AppActions.h` 900 → 627 |

**How it was verified.** Every stage ran the full headless suite (779 → 782
tests) and the GUI suite (198). Stages 2 and 5 additionally re-rendered a
fixed three-body scene through the real GPU and diffed the PNG: **0 of
2,764,800 bytes differ**, with identical volumes — which is what pins the
renderer rewrite and the `anyWidgetActive` seam, neither of which any test
covers. Stage 3 diffed `designer --gui-test-list` against the pre-split
binary (198 names, byte-identical). Stage 4's gate was checked against
deliberate sabotage in both directions.

One false alarm worth recording: `fem_smoke` failed once mid-round and passed
alone. The mechanism was mine, not the code's — two GPU preview instances I
had left running were loading the machine, and that test polls an async solve
against a wall clock. `--headless` builds no renderer at all, so nothing stage
2 touched even executes there.

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

Those files also call 22 distinct
`MeshRenderer`/`OutlineRenderer`/`RayTracingRenderer` methods (`AddMesh`,
`CreateAsset`, `SetInstanceTransform`, `BuildAccelerationStructures`, …).

> **Correction (same day).** This section originally concluded that a library
> extraction therefore "needs a renderer seam" before it buys any testability.
> That was wrong, and the follow-ups section below records how it was
> disproved: the actions layer already null-checks every renderer pointer —
> `HeadlessMain.cpp` runs the whole app that way — so leaving them null in a
> test is the seam. The library shipped without one.

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

### 7. Contract drift in the RPC layer — FIXED, see follow-up C

`RemoteHelpers.h` documents `regen` as *"appended to every mutating RPC's
response."* Actual: `RemoteModeling` 7, `RemoteProject` 1, everything else 0.
`RemoteConfigurations`' `set_active_configuration` regenerates geometry, then
hand-rolls a **third** format — `{"active", "bodies", "volumes"}` —
reimplementing part of `BodyDigestJson` inline, with no `regen.failed`, so a
configuration that breaks a feature returns success. `Vec3ToJson` is defined
three times, one of them (`RemoteServer.cpp:313`) shadowing the
`RemoteHelpers.h` copy the same file includes.

*Fixed 2026-08-24* — see follow-up C below. It turned out to be a reproducible
defect (a success response carrying the unfilleted volume) rather than the
cosmetic inconsistency this section originally called it.

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

## Follow-ups — resolved 2026-08-24

**A. `designer_app` static library — SHIPPED** (`29402e7`), triggered by having
something real to test rather than done as cleanup.

`designer_app` is `app_sources` minus `main.cpp`, `HeadlessMain.cpp` and
`CrashHandler.cpp`, linked by both `designer` and `designer_tests`. Measured
cost: the test binary 10.0 → 12.7 MB, link time unchanged (4.7 → 4.3 s, noise).
One hazard worth knowing: ImGui comes along as a link dependency, so calling an
ImGui function without a context **crashes** rather than failing to compile.
Test the actions layer, not the widgets — click paths belong in the GUI suite,
where they are the actual subject.

**B. A renderer seam — STRUCK. The premise was wrong.**

The review argued that because the actions layer calls 22
`MeshRenderer`/`OutlineRenderer`/`RayTracingRenderer` methods, it could not be
tested without an interface over them. It already can: **every renderer pointer
in `AppState` is null-checked**, because `HeadlessMain.cpp` runs the whole
application that way. A test constructs an `AppState`, leaves those pointers
null, and the sync paths no-op exactly as they do headless. A throwaway spike
confirmed this before anything landed — a unit test calling `CreateBox`,
`AcceptPendingFeature`, `SyncDocumentRenderers` and `GlobalUndo` with null
renderers passed on the first run.

Measurements that settled it:

- **51 call sites**, not a diffuse dependency, in three files.
- `SyncDocumentRenderers` early-returns on an unchanged version counter, so 41
  of them fire on *edit*, not per frame.
- The one genuinely per-frame path is `PushOccurrenceLive` during a gizmo drag.
  Benchmarked in that exact shape (152 calls/frame, matching the 76-occurrence
  dock): virtual dispatch costs **13 ns/frame**, against that assembly's
  measured **6.34 ms** CPU frame — **0.0002%**. Performance was never the
  obstacle, in any variant.

A recording stub is still the only way to *assert* on renderer effects ("sync
added three meshes with these colours"), and that remains available for the 10
renderer-touching functions if a specific bug ever needs it. It is not a
prerequisite for anything.

**What the value actually was.** Not "covers untested code" — that framing was
also wrong. Of 176 public actions functions, 80 are already reachable from an
RPC handler and covered by the 28 headless smokes; most of the rest are reached
by GUI tests *through clicks*, which static call-graph analysis cannot see
(`HandleGizmoInput` looked untested and is covered by the `handles/gizmo_*`
cases). The real gain is **where** the coverage runs: the nearest GUI
equivalents of the new tests cost 0.87 s and 1.07 s each and need a GPU; the
eight headless tests total **6 ms** and check more.

### The worked example: `tests/test_actions.cpp`

Read that file before adding to this layer — its header comment is the pattern.
Two subjects, both chosen for real bug history and no direct coverage:

- **`RecenterSplitOffset`** — the `CLAUDE.md` derived-defaults rule as
  arithmetic. Pins: derived not literal, re-derives per plane on all three
  axes, never overwrites a user's expression, and finds the body both before
  the split runs and after (when the source is gone, replaced by pieces stamped
  with the split's own id).
- **`VisibleSceneBounds`** — what the camera frames and where a new section
  cuts. Pins: off-origin parts, empty document returns false rather than a
  zero-size box at the origin, assembly occurrences placed by their transforms,
  hidden ones excluded, and all **eight** corners transformed.

Every invariant was checked against deliberate sabotage, which is the part
worth copying. It caught a worthless test of my own: *"the offset is unchanged
on the same plane"* passes against the exact bug it was written to catch,
because finding nothing and re-deriving the same number are indistinguishable.
It now changes the plane after the split runs, so a stale value fails.

**When to add more.** The trigger is finding yourself writing a GUI test for
something that is not really about the GUI — a BOM total, an explode offset, a
camera-fit box. 32 of the current 174 GUI cases have that shape (few
interactions, assertions only about document state). They are not worth
migrating wholesale: each still verifies that the click path reaches the logic,
which is its own subject. Add the headless test *beside* the GUI one when the
arithmetic is what you are actually unsure about.

**C. RPC `regen`/`digest` contract (§7) — FIXED**, and it was a real defect,
not drift. Reproduced before touching anything: activating a configuration
whose override broke a fillet returned

```
{"active": 3, "bodies": 1, "volumes": [7999.999999999998]}
```

— a success, one body, and a plausible volume. `7999.999…` is exactly 20³, the
**unfilleted** box, so the response looked healthy while the model was broken
and there was nothing to poll. `undo` had the same hole and also left a feature
failed.

Now `regen` is attached by ~18 handlers across `RemoteServer` (undo/redo),
`RemoteConfigurations`, `RemoteModeling` (the sketch edits) and `RemoteProject`
(variables, `open_project`, `checkpoint_restore`, `history_restore`,
`clone_project`, `import_step`), through a `remote::AttachRegen` helper that
carries the "no active part document" guard so an assembly or drawing tab
cannot trip it.

Two judgement calls worth knowing:

- **Additive only.** `set_active_configuration`'s hand-rolled
  `{bodies, volumes}` stays, because `configuration_smoke.py` reads `volumes`
  and breaking a working caller to de-duplicate a response is a bad trade. It
  gains `regen` and `digest`; new callers should read those.
- **Studies are excluded on purpose.** `add_study` / `remove_study` /
  `set_study` mutate a document but cannot break a feature, so `regen` there
  would always say `ok` and tell a caller nothing. The contract is "every RPC
  that can leave a FEATURE broken reports it", not "every RPC that writes".

`tests/regen_honesty_smoke.py` pins it, sabotage-checked three ways (reverting
the configuration fix, the undo fix, and the `open_project` fix each fail it).

**D. The panel preamble (§8)** — still deferred. A behavior change to a surface
with live callers, and it fits the roadmap's "split what the next feature
extends, not big-bang" policy better than a standalone sweep.
