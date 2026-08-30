# Wiring a renderer — the plan to stop getting it wrong

> Status: **R1–R3 DONE** (`b2f757a`, `1020252`, `2c452ae`) — one registration
> list, one teardown list, every per-frame duty a loop, both lists gated.
> **R4 NOT DONE**, and the plan below was wrong about it twice; see
> [what R1–R3 changed about R4](#what-r1r3-changed-about-r4).
> **R5 done for R1–R3** (byte-identical screenshots, all suites green).

## The bug class (what it was, before R1–R3)

Adding a renderer that draws into the 3D scene meant touching **about twenty
sites across three files**, in duties that were each a hand-maintained list:

| duty | sites | what a miss looks like |
|---|---|---|
| `Render` | **3 blocks x 6 = 18** — the windowed frame, the windowed screenshot, the offscreen screenshot | invisible on screen, or only in screenshots |
| `SetViewport` | **3** in `main.cpp` (two inits + resize) **+ 1** in `UI.cpp` | **drawn full-window at the wrong scale and offset** |
| `UpdateCamera` | 12 | geometry frozen in space |
| `SetClipPlanes` | 10 | does not section |
| `Initialize` / `make_unique` | 12 (two blocks) | null in one of the two modes |
| `reset` | 12 (two blocks) | leak / validation error at shutdown |
| `g_App.xRenderer =` | 12 | null pointer in the UI layer |
| declaration, forward declaration, global | 3 | — |

**The compiler checked none of it, and every omission failed late.** Usually
silently and plausibly — a picture that is wrong in a way that looks like a
modelling problem — and once as a SIGSEGV at process exit that went unexplained
long enough to earn the name "the long-standing shutdown crash".

**Every one of the last three renderers added arrived with a wiring bug**, in
two different lists, one of them a crash:

- **`0346a8e`** (2026-07-17), the FEM field: *"the FieldRenderer was never
  added to that list — so it kept a full-window viewport while the camera
  aspect was the sub-region… The Shape iso-surface was unaffected because it
  draws through the MeshRenderer, which is in the list."*
- **`4a3083d`** (2026-08-30), the CAM stock: reported from the field as a
  workpiece not parallel to the part with its cuts shifted sideways, while the
  cutter — an instance of the mesh renderer — sat exactly right.
- **`a222bc8`** (2026-07-23), the **shutdown** list, the underlay: *"main()'s
  shutdown resets every renderer global before CleanupVulkan destroys the
  Vulkan device — except g_UnderlayRenderer (the newest one; it was missed)…
  This is the long-standing 'shutdown crash.'"* A different list, and this one
  was a SIGSEGV that went unexplained for a while rather than a wrong picture.

(A fourth, `cc80f4d`, is the same viewport-versus-framebuffer confusion one
layer over in the AO sampling — adjacent rather than the same list.)

The rate matters as much as the count: **six of the nine renderers were created
between 2026-07-05 and 2026-08-27.** This is not a list that gets touched once
a year.

And **`--offscreen` cannot show it**: no panels, so the 3D sub-region IS the
whole framebuffer. Every screenshot review and every headless test is blind to
this entire class.

## Where it stands now

**Two lists remain, and both are gated.** `RegisterSceneRenderers()` (the draw
order) and `ShutdownSceneRenderers()` (ownership), adjacent to each other in
`main.cpp`. Everything per-frame — viewport, camera, clip planes, the three
render blocks — loops over the registry and enumerates nothing.

`tests/test_conventions.cpp` fails if a class deriving from `SceneRenderer` is
absent from either list. The discriminator is `: public SceneRenderer` — asking
who implements the frame contract — which selects exactly the six scene
renderers and excludes the three prepass ones. Verified both ways: by dropping
the stock from the registry and watching it fail.

A renderer constructed in one mode and not the other now names itself in the
log at startup, so the two construction paths cannot silently disagree.

Together those cover **both** ways this bug class has reached a user: the
silent wrong viewport, and the shutdown crash.

## What the measurements changed

Two facts make this much cheaper than it looks, and both were checked against
the code:

1. **All six scene renderers already have identical signatures** for all five
   duties — `Render(VkCommandBuffer, uint32_t)`, `UpdateCamera(mat4, mat4)`,
   `SetClipPlanes(const ClipPlaneArray&)`, `SetViewport(int32_t, int32_t,
   uint32_t, uint32_t)`, `Shutdown()`. Only Sketch and Underlay lack
   `SetClipPlanes`, which the interface defaults to a no-op. There are **no
   signature changes to make**.
2. **All three `Render` blocks already list the same six in the same order** —
   Mesh, Field, Stock, Outline, Underlay, Sketch. One ordered vector
   reproduces every block exactly, so there is no behavioural difference to
   reason about. That was the part that would have been expensive.

One constraint the plan must respect: **the typed pointers in `AppState` stay.**
The `ui/` layer uses them for CONTENT, not for scene wiring — `AddMesh`,
`RemoveMesh`, `SetMeshColor`, `SetInstanceGhost`, `SetInstanceVisible`,
`fieldRenderer->SetMesh`, `sketchRenderer->SetStaticLines`, and ~40 more. The
registry owns the **per-frame contract only**; it is not an attempt to hide the
renderers behind one type.

## The plan

| | scope | size | |
|---|---|---|---|
| **R1** | `SceneRenderer` interface; the six inherit it. No call site changes | ~1 h | **done** `b2f757a` |
| **R2** | The registry, and the per-frame loops: viewport, camera, clips, render | ~2 h | **done** `1020252` |
| **R3** | One teardown list; a named warning per construction path | ~1 h | **done** `2c452ae` |
| **R4** | `SceneView` at `Render`; delete the three setters | ~half a session | **not done**, and costlier than this said — see below |
| **R5** | Verification: three suites and a screenshot pass | ~1 h | **done for R1–R3** |

### R1 — the interface, changing nothing

```cpp
// render/SceneRenderer.h
class SceneRenderer {
public:
  virtual ~SceneRenderer() = default;
  virtual void SetViewport(int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;
  virtual void UpdateCamera(const glm::mat4& view, const glm::mat4& proj) = 0;
  virtual void SetClipPlanes(const ClipPlaneArray&) {}
  virtual void Render(VkCommandBuffer, uint32_t frame) = 0;
  virtual void Shutdown() = 0;
};
```

Six `: public SceneRenderer` and five `override` each. `main.cpp` is not
touched. **The tree stays green and the diff is trivially reviewable** — this
is the phase that proves the signatures really do line up, before anything
depends on it.

`SetViewportSize(w, h)` survives on four renderers and stays OFF the interface:
it answers a different question ("the framebuffer changed size"). Renaming it
is optional and separate.

**Gate:** it compiles; all three suites unchanged. Take the R5 "before"
screenshots here, as the first act of the workstream.

### R2 — the registry and the per-frame loops

```cpp
// main.cpp, ONE ordered list — the order is real information
static std::vector<SceneRenderer*> g_Scene;  // mesh, field, stock,
                                             // outline, underlay, sketch
```

Replace the per-frame lists with loops over it: the three `Render` blocks, the
three `SetViewport` sites in `main.cpp`, the `UpdateCamera` sites, the
`SetClipPlanes` sites, and the `setVp` list in `UI.cpp` (which becomes a loop
over the same registry, reached through `AppState`).

**The draw order is the one thing that must stay explicit and commented.** It
is not incidental: underlays draw before sketch lines, the stock after the
mesh, the outline last.

**Gate:** the GUI suite (it drives the windowed frame path) plus the two
screenshot paths, and eyes on a render — the order is the thing only eyes can
check.

### R3 — construction, shutdown, and the count

Two construction blocks (`main.cpp` ~1328–1370 offscreen, ~1875–1954 windowed)
and two shutdown blocks (~1529–1534, ~2476–2485), six entries each. Each
renderer is still constructed and `Initialize`d individually — they take
different arguments and some are allowed to fail — but registration and
teardown go through the registry.

Then the hole the refactor does not close by itself: **a renderer can still be
registered in one of the two construction paths and not the other.** Close it
with an assertion that the registry holds the expected count in both, so a
half-wired renderer fails loudly at startup instead of being invisible in one
mode.

**Gate:** `offscreen_smoke` and `headless_smoke` both still start and render;
the count assertion fires if a renderer is dropped from either path.

### R4 — pass the frame state at `Render` time

```cpp
struct SceneView {
  glm::ivec4 viewport;   // x, y, w, h — the sub-region between the panels
  glm::mat4 view, proj;
  ClipPlaneArray clips;
};
virtual void Render(VkCommandBuffer, uint32_t frame, const SceneView&) = 0;
```

`SetViewport`, `UpdateCamera` and `SetClipPlanes` **disappear entirely**, and
with them their ~40 call sites and the matching members in all six renderers.
A renderer *cannot* be drawn with a stale or unset viewport, because the data
arrives with the call — and the three call sites already have all of it.
Missing wiring stops being a silent visual defect and becomes a compile error.

`UI.cpp`'s `setVp` block is already gone — R2 replaced it with a loop over the
registry. The conventions gate STAYS: see below, the plan was wrong about that.

The catch R2 uncovered: `SSAORenderer::RecordSSAOPasses` takes a
`MeshRenderer&` and reads the camera matrices out of it on purpose — *"the
prepasses read them here so they are guaranteed identical to what the main pass
uses"*. Deleting the sticky state therefore means restructuring how the prepass
gets its scene state, in the AO and shadow path. That is the work this phase
actually is.

**Gate:** the whole per-frame contract reads

```cpp
for (SceneRenderer* r : g_Scene)
  r->Render(cmd, frame, view);
```

Adding a renderer is already down to *declare it, construct it, register it*
after R1–R3. What R4 adds is that getting the frame state wrong stops being
possible rather than merely being caught.

### R5 — verification

These render paths are what CLAUDE.md calls the untested Vulkan layer, so the
suites are necessary and not sufficient:

- headless (`ctest -LE gui`) — catches little here directly, but must stay green
- **GUI suite** — drives the windowed frame path, and carries the CAM
  simulation's viewport assertion
- the python `gui` three — `remote_smoke` covers the windowed screenshot path
- **screenshots, actually looked at**: a part with a section plane (clips), a
  sketch in edit (underlay + sketch order), a FEM field, and a CAM simulation
  (stock after mesh). Compare against the same four taken at R1.

## Risks

- **Draw order silently changing.** Mitigated by R2's comment, the before/after
  screenshots, and the fact that all three blocks already agree today.
- **A renderer wired into one construction path only.** That is R3's assertion.
- **`MeshRenderer` is not uniform** — 15 calls nothing else has (shadow
  casters, AO and shadow-map rebinds, instance/asset management). It implements
  the interface for its *scene* duties and stays special for the rest. The
  registry is not an attempt to make it uniform.
- ~~**Landing R1–R3 without R4.**~~ This warning was wrong — see
  [what R1–R3 changed about R4](#what-r1r3-changed-about-r4). R1–R3 is a
  coherent stopping point and is where this currently stands.

## What R1–R3 changed about R4

Two things the plan asserted turned out to be wrong, and both matter for
whether R4 is worth doing.

**1. R4 does NOT retire the step-1 gate — R2 re-pointed it, and it got
stronger.** The plan said "there is no list left to be missing from". False:
the REGISTRY is a hand-maintained list, and so is the teardown. The gate now
reads `RegisterSceneRenderers()` and `ShutdownSceneRenderers()` and checks
every `SceneRenderer` implementation appears in both — which covers *both* ways
this bug class has actually reached a user (a silent wrong viewport, and the
shutdown SIGSEGV). It is doing more work after R2 than before it.

**2. "R1–R3 without R4 is the worst of both" was wrong.** That warning assumed
the gate would be left guarding a vanished invariant. It isn't. R1–R3 is a
coherent, complete state: seven lists became two, both are gated, the per-frame
duties cannot be got wrong because nothing enumerates renderers any more, and a
renderer built in one mode and not the other now names itself in the log at
startup.

**And R4 costs more than costed.** `SSAORenderer::RecordSSAOPasses` takes a
`MeshRenderer&` and pulls the shadow casters AND the camera matrices out of it,
deliberately — *"the prepasses read them here so they are guaranteed identical
to what the main pass uses"* (MeshRenderer.h). Deleting the sticky state from
MeshRenderer therefore means restructuring how the prepass gets its scene
state, in the AO and shadow path, which has its own history of subtle bugs
(the SSAO halo, `cc80f4d`'s viewport-rect sampling). That is not the ~2 h of
mechanical edits estimated; call it half a session with real regression risk,
and it is the part of the renderer where a mistake is hardest to see.

**Where that leaves it.** R4 buys *impossible* over *guarded* — the viewport
becoming a compile error instead of a checked convention. That is genuinely
better, and it is no longer urgent: the three historical bugs are all caught by
the R2/R3 gates. Worth doing on a day with a clear session and a screenshot
budget, not as the tail of another change.

## The general shape

This is the same disease as [the up axis](../up-axis.md): a fact that lives
implicitly in N places instead of being stated once. There it is the world's up
vector in 69 literals; here it is the frame state in ~20 wiring sites. Both
cures are the same two moves — **state it once, then gate it** — and in both
cases the gate is worth having even if the restatement never happens.
