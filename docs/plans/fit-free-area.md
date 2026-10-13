# Fit into the free part of the viewport

## The problem

Fit (`F`, `fit_view`, every `FitCameraToDocument` after an open or import)
frames the model in the whole central dock node. The docked panels are
already outside that rect (`DockLayout.cpp` publishes the central node as
`viewportPos`/`viewportSize`). Several things still draw on top of it, and
fit knows about one of them, and only partly:

- **The floating tool window** (`###ToolWindow`, top right).
  `FitCameraToBox` (`ViewActions.cpp`) handles it as a vertical strip: it
  takes the window's horizontal overlap, caps it at 45 % of the width,
  narrows the horizontal half-FOV and then pans the solved aim by half the
  strip. It ignores the window's height. A short window, such as a Fillet
  with two rows, gives up the whole right-hand band, including the part
  below the window that nothing covers. A window wider than 45 % still
  covers the model. The pan is applied after the solve, at the target's
  depth, so in perspective the near end of a long part can still reach under
  the window.
- **The view cube** (`DrawViewCube`, 108 px, bottom right). It is never
  counted, so a fit that fills the frame puts the model's bottom-right
  corner underneath it.
- **The windows anchored `BelowToolWindow`** (template panel, CAM generate,
  occurrence panel), **the CAM playback bar** (`BottomCenter`), **the
  in-context banner** (top centre) and **the controls cheat sheet**
  (bottom left). None of them is counted.

`cam/fit_keeps_the_model_clear_of_the_tool` checks only that the part's
centre lies left of the window, which the strip rule satisfies even when a
corner is under the window.

## The shape of it

Use SolidCraft's approach (`crates/ui-egui/src/viewport_layout.rs`,
`free_rect`). Collect the rects that cover the viewport, list every
candidate rectangle whose edges come from the viewport and those rects,
keep the largest one that touches none of them, and fit the model into that
rectangle. The projection matrix, picking and the controls' anchors stay as
they are. Only the camera's pivot and distance change, so the view cube and
the tool window stay anchored to the viewport as they are today.

### 1. `LargestFreeRect`

`LargestFreeRect(view, obstacles)` is a pure function. Each obstacle is
clipped to the view, and empty obstacles are dropped. The candidates are
every pair of x edges combined with every pair of y edges. With a handful of
obstacles that is a few thousand rectangles, which is negligible for an
action taken once. It lives beside the fit in `ViewActions.cpp` and is
declared in `ViewActions.h` so that `test_actions.cpp` can test it directly.

There is a floor. If the free rect is narrower or shorter than half the
viewport, the fit uses the whole viewport. The floor is a fraction of the
viewport, so it is not a literal. A model fitted into a sliver is worse than
one partly covered by a window the user can move.

### 2. What counts as an obstacle

`AppState` gains `viewportObstacles`, a small list of screen rects. The
overlays rebuild it every frame as they draw, so it is one frame old, as
`toolWindowRect` already is. The view cube, the `BelowToolWindow` windows,
the playback bar, the banner and the cheat sheet each record their rect
once `Begin` has placed them. The tool window stays gated as it is now. Its
rect counts only while the session state says a session is open
(`HasPendingFeature`, `editFeatureId`, measure, dim, mate, `camToolOpen`).
That window is the one a remote call opens and closes before the UI draws,
so a stale rect would push the model aside for a window that has already
closed.

All rects are converted to one space, the main viewport's, which
`viewportPos` already uses. `toolWindowRect` is currently absolute and is
compared with `viewportPos` directly, which is correct only when the main
viewport sits at (0, 0).

The prompt pills are not counted. They are one line high, they move to
avoid the tool window (`PromptPillRect`), and a fit that avoided them would
give up the top strip in every sketch.

### 3. Fit into a sub-rectangle

`FitCameraToBox` replaces `coveredFrac` and the later pan. The free rect,
in NDC, has a centre `c` and half-extents `h` (each at most 1). The solve
uses `tanHalfH * h.x` and `tanHalfV * h.y` as its half-angles. Each round
pans the aim so that the projected centre lands on `c` instead of at 0. That
is the same loop the fit already runs, with a different target and frame
size. Because the result is still exact, no corner can leave the rect, which
the separate pan could not guarantee.

When `forAspect > 0` (a `render_still` or a sheet capture), there are no
obstacles. The still has no overlays, so it gets the whole frame as it does
today.

### Files

- `src/ui/ViewActions.{h,cpp}`: `LargestFreeRect` and the sub-rect solve in
  `FitCameraToBox`.
- `src/ui/AppState.h`: `viewportObstacles`.
- `src/ui/UI.cpp` (view cube, banner, cheat sheet), `src/ui/FloatingWindow.cpp`
  (every `BeginFloatingWindow` records its rect, so the tool window, the
  `BelowToolWindow` windows and the playback bar are covered in one place),
  and `src/ui/PreviewFeature.cpp` (`toolWindowRect` in viewport space).
- `docs/ui/README.md#viewport`: the "Fit fills the frame" bullet says the
  model fills the largest free rectangle and lists what counts as an
  obstacle.

## How it is verified

- **Unit (`[actions]`, `test_actions.cpp`)**: `LargestFreeRect` with no
  obstacle returns the view. With a top-right window and a bottom-right
  cube it returns the left band, or the band below the window when the
  window is short and wide. Obstacles outside the view are ignored.
- **GUI, `viewport/fit_lands_in_the_free_rect`**: a long box
  (200 x 40 x 20), with a feature-edit tool window open, fitted
  (`FitCameraToDocument`, then `StopAnimation`). All eight bbox corners are
  projected through `WorldToScreen`. Each must lie inside the viewport and
  outside both the tool window's rect and the view cube's ImGui window rect,
  which are read from ImGui rather than from `AppState`. The fit must also
  still fill the frame: the projected bbox spans at least 85 % of the free
  rect in its limiting dimension. Without that check, shrinking the model
  would pass. A second case uses a short tool window and asserts that the
  model is taller than the strip beside the window would have allowed,
  which shows the free rect was used rather than the strip.
- `cam/fit_keeps_the_model_clear_of_the_tool` is tightened from "centre
  left of the window" to all eight corners outside it.
- The existing still and framing tests (`render_still`, `CamSetupSheet`)
  must pass unchanged, which shows that `forAspect` fits still use the
  whole frame.
