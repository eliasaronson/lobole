# Wiring a renderer — the plan to stop getting it wrong

> Status: **step 1 DONE** (`d6dade3`, a conventions gate). **R1–R5 PLANNED**,
> not started, ~1 session. Written after the CAM stock renderer became the
> second renderer to ship with a full-window viewport, and costed against the
> code rather than by feel.

## The bug class

Adding a renderer that draws into the 3D scene means touching **about twenty
sites across three files**, in duties that are each a hand-maintained list:

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

**The compiler checks none of it, and every omission fails silently and
plausibly.** Nothing crashes; you get a picture that is wrong in a way that
looks like a modelling problem.

It has happened twice on the same list:

- **`0346a8e`** (2026-07-17), the FEM field: *"the FieldRenderer was never
  added to that list — so it kept a full-window viewport while the camera
  aspect was the sub-region… The Shape iso-surface was unaffected because it
  draws through the MeshRenderer, which is in the list."*
- **`4a3083d`** (2026-08-30), the CAM stock: reported from the field as a
  workpiece not parallel to the part with its cuts shifted sideways, while the
  cutter — an instance of the mesh renderer — sat exactly right.

And **`--offscreen` cannot show it**: no panels, so the 3D sub-region IS the
whole framebuffer. Every screenshot review and every headless test is blind to
this entire class.

## Step 1 — the gate (done)

`tests/test_conventions.cpp` fails if a renderer exposing
`void SetViewport(int32_t` is missing from `UI.cpp`'s `setVp` list. It selects
exactly the six scene renderers and excludes the three prepass ones. Verified
by deleting a line and watching it fail.

That stops **this** list from growing a hole again. It does nothing for the
other seven.

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

| | scope | size |
|---|---|---|
| **R1** | `SceneRenderer` interface; the six inherit it. No call site changes | ~1 h |
| **R2** | The registry, and the per-frame loops: viewport, camera, clips, render | ~2 h |
| **R3** | Construction and shutdown through the registry, plus the count assertion | ~1 h |
| **R4** | `SceneView` at `Render`; delete the three setters; delete `setVp`; retire the step-1 gate | ~2 h |
| **R5** | Verification: three suites and a screenshot pass | ~1 h |

R1–R3 are "step 2", R4 is "step 3". **Do them as one piece** — landing R1–R3
alone leaves the sticky state in place with a registry pointed at it, which is
the worst of both.

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

Two deletions belong in this phase, deliberately:

- **`UI.cpp`'s `setVp` block**, which no longer has anything to do.
- **The step-1 conventions gate.** There is no list left to be missing from, so
  a test policing it guards a vanished invariant — worse than no test, because
  it reads as coverage. Delete it in the same commit and say so in the message.

**Gate:** the whole per-frame contract reads

```cpp
for (SceneRenderer* r : g_Scene)
  r->Render(cmd, frame, view);
```

and adding a renderer is: declare it, construct it, register it. Twenty sites
become three.

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
- **Landing R1–R3 without R4.** Leaves a registry pointing at sticky state and
  a gate guarding a list that is half-gone. Do not stop there.

## The general shape

This is the same disease as [the up axis](../up-axis.md): a fact that lives
implicitly in N places instead of being stated once. There it is the world's up
vector in 69 literals; here it is the frame state in ~20 wiring sites. Both
cures are the same two moves — **state it once, then gate it** — and in both
cases the gate is worth having even if the restatement never happens.
