# Wiring a renderer — why it keeps going wrong, and the two fixes left

> Status: **step 1 DONE** (`d6dade3`, a conventions gate). Steps 2 and 3
> **PLANNED**, not started. Written after the CAM stock renderer became the
> second renderer to ship with a full-window viewport.

## The bug class

Adding a renderer that draws into the 3D scene means touching **about twenty
sites across three files**, in duties that are each a hand-maintained list:

| duty | sites | what a miss looks like |
|---|---|---|
| `Render` | **3 blocks x 6 = 18** — the windowed frame, the windowed screenshot, the offscreen screenshot | invisible on screen, or only in screenshots |
| `SetViewport` | **3** in `main.cpp` (two inits + resize) **+ 1** in `UI.cpp` | **drawn full-window at the wrong scale and offset** |
| `UpdateCamera` | 2 | geometry frozen in space |
| `SetClipPlanes` | 2 | does not section |
| `Initialize` / `make_unique` | 2 | null in one of the two modes |
| `reset` | 2 | leak / validation error at shutdown |
| `g_App.xRenderer =` | 2 | null pointer in the UI layer |
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

Two aggravating factors:

1. **The API is not uniform.** Most renderers have both
   `SetViewportSize(w, h)` and `SetViewport(x, y, w, h)`; three have only the
   first. Which one a call site wants is not obvious from the name.
2. **`--offscreen` cannot show it.** It has no panels, so the 3D sub-region IS
   the whole framebuffer. Every screenshot review and every headless test is
   blind to this entire class.

## Step 1 — the gate (done)

`tests/test_conventions.cpp` now fails if a renderer exposing
`void SetViewport(int32_t` is missing from `UI.cpp`'s `setVp` list. It selects
exactly the six scene renderers and excludes the three prepass ones. Verified
by deleting a line and watching it fail.

That stops **this** list from growing a hole again. It does nothing for the
other seven lists.

## Step 2 — an ordered registry (planned, ~half a session)

**Measured before estimating, and it is cheaper than it looks.** All six scene
renderers ALREADY have identical signatures for all five duties —
`Render(VkCommandBuffer, uint32_t)`, `UpdateCamera(mat4, mat4)`,
`SetClipPlanes(const ClipPlaneArray&)`, `SetViewport(int32_t, int32_t,
uint32_t, uint32_t)`, `Shutdown()`. Only Sketch and Underlay lack
`SetClipPlanes`, which the interface defaults to a no-op. So this is
`: public SceneRenderer` plus `override`, with **no signature changes at
all**.

And all three `Render` blocks already list the same six in the same order —
Mesh, Field, Stock, Outline, Underlay, Sketch — so one ordered vector
reproduces every one of them exactly. There is no behavioural difference to
reason about, which is the part that would have made this expensive.

What collapses: ~94 wiring calls in `main.cpp` (18 `Render`, 18 `SetViewport`,
18 `Initialize`, 18 `reset`, 12 `UpdateCamera`, 10 `SetClipPlanes`) become
~15 loops. `MeshRenderer` keeps 15 calls nothing else has (shadow casters, AO
and shadow-map rebinds, instance and asset management) and is untouched by
this.

Give the scene renderers one small interface and keep them in one ordered
vector; every duty above becomes a loop instead of a list.

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

`std::vector<SceneRenderer*> g_Scene` **in draw order** — the order is the one
thing that must stay explicit, because it is real information (underlays
before sketch lines, stock after the mesh, outline last). Adding a renderer
becomes: implement the interface, `push_back` once.

What it costs, honestly:

- It touches all three `Render` paths in `main.cpp`, which CLAUDE.md calls the
  untested Vulkan layer. The change is mechanical but wants the GUI suite and
  a screenshot pass, not just the headless one — the draw order is the thing
  to eyeball.
- `MeshRenderer` keeps extra duties nothing else has (shadow casters, AO and
  shadow-map rebinds, instance/asset management). It implements the interface
  for its *scene* duties and stays special for the rest — the registry is not
  an attempt to make it uniform.
- `SetViewportSize(w, h)` survives on four of them and is a DIFFERENT question
  ("the framebuffer changed size"). It stays off the interface; only the rect
  form belongs there. Renaming it is optional and separate.

**What it does not fix:** there are still three `Render` call sites and someone
must still register. It converts seven lists into one, which is most of the
win, but the remaining one is still a list.

## Step 3 — pass the frame state at `Render` time (planned, ~half a session)

The cure for the rest. Replace the sticky per-renderer state with an argument:

```cpp
struct SceneView {
  glm::ivec4 viewport;   // x, y, w, h — the sub-region between the panels
  glm::mat4 view, proj;
  ClipPlaneArray clips;
};
virtual void Render(VkCommandBuffer, uint32_t frame, const SceneView&) = 0;
```

Then `SetViewport`, `UpdateCamera` and `SetClipPlanes` **disappear entirely**,
and with them their forty call sites. A renderer *cannot* be drawn with a
stale or unset viewport, because the data arrives with the call — and the
three call sites already have all of it. Missing wiring stops being a silent visual
defect and becomes a compile error.

It also **retires the step-1 gate**: there is no list left to be missing from,
so the conventions test that guards it should be deleted in the same change
rather than left policing something that no longer exists.

Combined with step 2 the whole per-frame contract is:

```cpp
for (SceneRenderer* r : g_Scene)
  r->Render(cmd, frame, view);
```

Twenty sites become three: declare it, construct it, register it.

The one hole the refactor does not close by itself: the renderers are still
constructed in TWO places (the offscreen path and the windowed path), so a new
one could be registered in one and not the other. That has a one-line answer —
assert the registry's size against the number constructed — and it is worth
writing at the same time.

**Do 2 and 3 as one piece — about a session for both.** Step 2 is most of the work toward step 3, and
landing 2 alone leaves the sticky state in place with a registry pointed at
it — the worst of both, since the gate from step 1 would then be checking a
list that no longer exists.

## The general shape

This is the same disease as [the up axis](../up-axis.md): a fact that lives
implicitly in N places instead of being stated once. There it is the world's
up vector in 69 literals; here it is the frame state in ~20 wiring sites. Both
cures are the same two moves — **state it once, then gate it** — and in both
cases the gate is worth having even if the restatement never happens.
