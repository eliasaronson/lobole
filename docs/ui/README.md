# The user interface

Dear ImGui (docking branch) over a Vulkan viewport, immediate-mode: the whole
interface is rebuilt every frame from `AppState` on the main thread. This page
is the map; the topic pages hold the detail.

| page | what it covers |
|---|---|
| [selection.md](selection.md) | the selection set, the pick pipeline, picking through a section, list gestures and Delete |
| [tool-sessions.md](tool-sessions.md) | the preview flow, pick slots, the tool window, defaults and refusals, the Hole tool, sketch sessions |
| [panels.md](panels.md) | the dock panels (part, assembly, CAM, simulation, drawing), floating windows, shared row/header widgets |
| [keybindings.md](keybindings.md) | the one command table, contexts, dispatch, rebinding |
| [numeric-input.md](numeric-input.md) | expression entry in every numeric field, completion, units on screen |
| [viewport-handles.md](viewport-handles.md) | drag handles, the gizmo, snapping, mate rotate |
| [settings.md](settings.md) | user preferences: the three buckets, storage, the Preferences window |
| [long-operations.md](long-operations.md) | the background job registry: CAM passes, FEM, import, opening a project, motion |
| [terminal.md](terminal.md) | the embedded terminal panel |
| [testing.md](testing.md) | the GUI test suite, isolation, addressing widgets, coverage |

## Layout

From top to bottom: the menu bar; the **toolbar row** (`##ToolbarRow`), which
shows the toolbar of the active context — part, sketch, assembly, drawing,
CAM job, simulation — and wraps onto a second row rather than hiding buttons;
the dockspace; and the **document tab bar** at the bottom.

- **Document tabs** — one per part, assembly, drawing or CAM job in the
  project, with a glyph per kind and stable ids. The strip scrolls
  horizontally with a visible scrollbar when it overflows, and its `+`
  (the add menu) sits outside the scrolling strip, so it is always reachable.
  Tabs can be grouped into folders (flyouts). Right-clicking a tab (on the
  bar or in a flyout) renames, files, duplicates or deletes it, the same for
  every kind; Rename goes through `ui::RenameDocument`, as `rename_part`
  does, so a taken name is refused in the popup rather than accepted.
  Switching tabs stashes the active document and cancels a pending preview.
- **The left dock node** holds the panels as tabs: Model, Variables,
  Configurations, History, Conflicts, Requirements (while the project states
  any), and the CAM panel on a job tab
  (`src/ui/PanelTitles.h`). Panel tabs are **icons**; the name is in the
  tooltip and each panel draws its own heading, because at the default split
  the words were truncated to `Mo…`. The `###id` part of each title is what
  refs, `focus_panel` and dock lookups address, so the label is free to
  change.
- **The viewport** is the central node. There is no right-hand Properties
  panel: editing happens in the floating tool window, anchored top-right of
  the viewport ([tool-sessions.md](tool-sessions.md#the-tool-window)).
- **The terminal**, when enabled, docks in a right-hand node
  ([terminal.md](terminal.md)).

`DockLayout.cpp` builds the default layout. Once per launch, after the panel
windows exist, **dock-home enforcement** re-docks any panel a stale
`imgui.ini` left floating (a floating Model panel triggers a full rebuild):
`DockBuilder` entries only apply on a rebuild, so a panel added after a
user's ini was saved would otherwise float forever. The dockspace id is
versioned (`MainDockSpaceV2`), so an ini from an older layout rebuilds once
rather than resurrecting dead nodes. View > Reset Layout rebuilds on demand.
`style.WindowMinSize` (180 × 60) keeps a dock node from being dragged to a
sliver; ImGui applies it after any size constraint, so a window cannot ask
to be smaller.

The main window remembers its size and position in `settings.json`; a first
run takes 80 % of the monitor's work area, and a saved rect no current
monitor can show is dropped (`FitToMonitors`). `--hidden`, `--offscreen` and
GUI-test runs always use 1280 × 720, because layout tests assert what fits at
that size. The window title is the project name, `*` when dirty.

## Viewport

- Navigation: the arrows orbit (Ctrl pans, Alt rolls), `F` fits, `Shift+F`
  zooms to the selection, `Shift+1`..`7` are the named views (also View >
  Standard Views), and the **view cube** at the bottom right snaps to a
  clicked face, edge or corner, with a home button. View > Viewport Controls
  shows the mouse and trackpad cheat sheet.
- **Zoom heads for the cursor.** The wheel, and Shift+middle/right-drag
  from where the button went down, zoom toward what the pointer is over,
  and that point stays on its pixel. It is the surface under the cursor
  (`FaceSelector::PickFace`, so a section cut is honoured) or, over empty
  space, where the cursor's ray meets the plane through the pivot facing the
  camera. `Camera::ZoomAbout` scales the whole rig (position and pivot) about
  that point, so it holds at any depth and the orbit pivot moves with the
  zoom; at a distance limit nothing moves rather than the view sliding.
  Preferences > Navigation > Zoom to cursor (off) zooms at the centre
  instead (`ZoomAnchor` in `ViewportInput.cpp`).
- The right button orbits when dragged and is a click when it is not: on an
  assembly tab a right click on a part opens its menu
  ([selection](selection.md)).
- **A named view is an orientation, not a place.** The menu, the view cube,
  `Shift+1`..`7` and the palette all turn the camera about its own pivot
  (`StandardView` in `Commands.cpp`), so a part modelled away from the origin
  stays in front of you.
- **Fit fills the frame.** The camera's aim and standoff are solved together
  for the box's projected extent (in perspective the projected centre of a
  box is not its 3D centre), and fitting slides the view (`Camera::SetPivot`)
  rather than re-aiming.
- **Fit uses the free part of the viewport.** Things draw over the central
  dock node, and a fit into the whole of it parked the model behind them.
  The fit frames the model in `LargestFreeRect` (`ViewActions.cpp`): the
  largest rectangle that overlaps none of the obstacles, chosen from every
  pair of x and y edges the viewport and the obstacles give. The obstacles
  are the **view cube**, every **floating window** (`BeginFloatingWindow`:
  CAM generate, the playback transport, Save as template, the BOM),
  the **in-context banner** and the **controls cheat sheet**, each recording
  its rect (`RecordViewportObstacle`) as it draws into
  `viewportObstaclesNext`; `DrawUI` publishes that as `viewportObstacles` at
  the start of the next frame, so a fit taken mid-frame or from a remote call
  sees one complete, one-frame-old set. The **tool window** counts only
  while the session state says a session is open (a pending feature, a
  feature edit, measure, dimension, mate or the CAM verbs): a remote call
  opens and closes it before the UI draws, and a stale rect pushed the model
  aside for a window already gone. The prompt pills do not count — one line
  high, they already dodge the tool window (`PromptPillRect`), and avoiding
  them would give up the top strip in every sketch. All rects are in the
  main viewport's space, as `viewportPos` is.
- The free rect is solved as an **off-axis frustum**: its NDC centre and
  half-extents scale the half-angles and shear the camera's right and up
  axes, so the same exact standoff and re-centring loop as a whole-frame fit
  holds for it and no corner can leave it. Only the aim and distance change;
  the projection, picking and the overlays' anchors are the viewport's as
  before. A rect narrower or shorter than **half the viewport** is refused
  for the whole viewport — a model fitted into a sliver is worse than one a
  movable window partly covers. A fit for a picture (`forAspect`, a
  `render_still` or the CAM setup sheet) and a `render_sheet` capture get
  the whole frame: they draw no overlays. A named view with `fit` (the
  `view` RPC) orients first and fits after, because the fitted aim is off
  the model's centre whenever the free rect is. Pinned by
  `viewport/fit_lands_in_the_free_rect`,
  `viewport/fit_uses_the_band_under_a_short_window`,
  `cam/fit_keeps_the_model_clear_of_the_tool` (every corner, not the
  centre) and the `[camera]` unit tests in `test_actions.cpp`.
- The lens is 30° vertical (`Camera::kDefaultFovDegrees`).
- **Reference geometry is sized by the part**: the base planes, datum quads,
  frame triads and the sketch grid derive from the document's reach
  (`ScenePlaneHalfExtent`), with the old small sizes as the empty-workspace
  floor; the grid's step follows a 1-2-5 decade so the cell count, not the
  cell size, stays fixed. The drawn plane quad and the pickable one read the
  same number. The reach rides the renderer resync rather than the frame
  loop, because a live edit bumps the document version every frame.
- The prompt pill at the top of the viewport says what the armed tool wants
  ([tool-sessions.md](tool-sessions.md)). While a sketch is edited it shows
  the sketch's DOF status — and nothing at all for an empty sketch, since
  "Fully defined" would be the arithmetic truth about nothing.
- **Viewport annotations** (sketch dimensions, constraint glyphs, the sketch
  axis compass, snap glyphs, measure readouts) draw into
  `ui::ViewportDrawList()` — the dockspace host's draw list — not ImGui's
  foreground list, so any window in front of the viewport (Preferences, a
  floating tool) covers them. The active sketch's dimensions stay on top of
  the model while it is edited, as in other CAD tools; only the sketch being
  edited is annotated.
- Measure works on part and assembly tabs; on an assembly a measured element
  carries its occurrence, so the same face on two occurrences is two
  measurables.

## Chrome

- Text that might not fit is elided with a tooltip carrying the full string
  (`TextFit.h`), never clipped silently ([panels.md](panels.md#shared-widgets)).
- Floating windows share one placement helper with a border and a drawn
  shadow ([panels.md](panels.md#floating-windows)).
- The toolbar row is a borderless child, and ImGui ignores `WindowPadding` on
  a borderless child unless `ImGuiChildFlags_AlwaysUseWindowPadding` is set;
  it is, and the padding is scaled with the UI.
- Every widget a test or a script reaches has a stable `###id`, and repeated
  widgets put their unique part in that id rather than inside an integer
  `PushID` scope, which no path can address
  ([testing.md](testing.md#making-a-widget-addressable)).
- One dark theme (`Theme.cpp`): one value ramp and one accent. Scrollbar grabs
  sit one step above the surface colour so they stay visible beside buttons.

## Display scaling

`ApplyTheme(uiScale, framebufferScale)` (`src/ui/Theme.cpp`). The UI scale
is `max(1, content scale) × ui_font_scale`. Style metrics scale by
`uiScale / framebufferScale` (ImGui's logical coordinates), and fonts are
rasterized at `uiScale` physical pixels with `io.FontGlobalScale =
1 / framebufferScale`. On X11 the framebuffer scale is 1 and this is plain
content scaling; on Wayland GLFW scales the framebuffer itself, so scaling
the metrics by the content scale as well would apply it twice. The
content-scale callback rebuilds the theme and font atlas (between frames)
only when the value differs from the one applied, because Wayland re-sends it
on every configure. On Wayland the scale arrives shortly after the window is
created (the first value is 1.0), so the theme rebuilds once at startup.

`OnWayland()` (`src/app/main.cpp`) guards the three X11 assumptions: window
positions are neither set nor read (Wayland provides none, and a failed read
would zero the saved position), and the X11 show path is skipped.

## Icons

One table, `CommandIcon(id)` in `src/ui/Icons.{h,cpp}`, maps each command id
to its glyph. Toolbar buttons take their icon from it, and the feature tree
(`FeatureIcon`), the mate list and the CAM operation list ask it for the
glyph of the command that creates the thing, so a row cannot differ from the
button that made it. Panel titles, status glyphs and playback controls are not
commands and stay where they are.

Glyphs come from two fonts merged into the text font (Roboto Medium, 16 px ×
scale): Font Awesome 6 Free Solid, and **Loböle's own CAD icon font** for
what Font Awesome cannot draw (fillet, chamfer, shell, loft, draft, section,
mate types, dimensions, arcs, CAM operations, sheet-metal tools).
`tools/icons/build_icon_font.py` defines each glyph as geometry on a 14-unit
grid and writes `deps/fonts/designer-icons.ttf` and `deps/fonts/IconsDesigner.h`
(`ICON_DS_*`, from U+E900, append-only so codepoints never move); both are
committed and byte-reproducible, and the script's `--check` compares them.
Both icon fonts load at one fixed 14 px × scale advance, so no button changes
width with its glyph; `PixelSnapH` is off for Font Awesome (on, ImGui
truncates its centring offset) and on for Loböle's grid-drawn glyphs.

Gates: `toolbar/no_duplicate_glyph` opens every toolbar and flyout and fails
if two commands show one glyph or a button fell through to the unknown-command
glyph; `test_conventions.cpp` "a toolbar glyph means one thing and a feature
has one glyph" reads the table (a shared glyph must be on an explicit
same-idea list) and fails if a toolbar or glyph table names an icon literal
for something the table covers; ctest `icon_font_gate`
(`tests/icon_font_gate.py`) checks that the committed font and header still
match the SHA-256s the header records, with the standard library only,
because ctest and CI have neither fontTools nor shapely.

## The GUI and the script API are one interface

The project's goal is that an agent uses the same interface as a person. The
RPC layer drives the same actions the GUI does, and the GUI itself is
drivable remotely, like a human would:

- `ui_items` lists the widgets (label, id, window, disabled, screen centre);
  `item_click`, `item_double_click`, `item_input`, `item_hover`,
  `item_combo` (opens and picks in one action, because a popup does not
  survive between RPCs) act on a widget by `ref` or numeric `id`;
  `popup_items` / `popup_click` reach popups and context menus;
  `menu_items` / `menu_click` the menu bar; `key_press` a chord;
  `viewport_click` / `viewport_drag` take fractions of the **3D viewport**
  rect; `scroll_panel`, `focus_panel`.
- Item actions **fail** on a disabled widget ("item is disabled: <ref>")
  instead of reporting success, and failures carry the test engine's own
  reason.
- Every toolbar button has a `###tb_*` id (sketch buttons
  `###tb_sk_<command>`, apart from a few named by hand such as
  `###tb_sk_finish`); every panel control the tests reach has a stable
  `###id` ([testing.md](testing.md#making-a-widget-addressable)).
- A creation tool refused in the GUI is refused over RPC with the same
  reason (`Feature::WhyIncomplete`, [tool-sessions.md](tool-sessions.md)),
  and a direct edit that would not build — its Accept disabled in the GUI —
  is refused over RPC with its error
  ([modeling](../modeling/README.md#direct-edit)).
- A mutating RPC that leaves a feature broken says so in its own response.
- Export and other file-dialog commands are disabled, with the reason, where
  no system dialog exists; scripts use the export RPCs directly.

The empty-state hints and Help > Keyboard Shortcuts name only affordances
that exist.

## Defaults and refusals

A default carrying a unit is derived from what it acts on, never a literal,
and a tool that cannot act says so visibly. The `defaults/*` GUI suite holds
every creation tool to it. The rule, its examples and the gate are in
[tool-sessions.md](tool-sessions.md#defaults-and-refusals).

## Where the code is

`src/ui/` is one file per subject:

- **Frame and layout:** `UI.cpp` (`DrawUI`), `DockLayout.cpp`,
  `MainMenuBar.cpp`, `Toolbar.cpp`, `SketchToolbar.cpp`, `Theme.cpp`,
  `PanelTitles.h`, `Widgets.h`, `TextFit.h`, `PanelLayout.h`,
  `FloatingWindow.h`, `Overlays.cpp`.
- **State:** `AppState.h` — the one application state, including the
  selection, the tool sessions and `settings`.
- **Commands:** `Commands.cpp`, `CommandPalette.cpp`, `Icons.cpp`.
- **Input:** `ViewportInput.cpp` (the viewport's input chain), `SelectionPipeline.cpp`,
  `ListSelection.cpp`, `GizmoInput.cpp`, `PreviewHandles.cpp`,
  `ViewportHandle.h`, `ViewportHandleDraw.cpp`, `DragMath.h`, `RayMath.h`.
- **Sessions:** `PreviewFeature.cpp` (preview flow, tool window),
  `FeatureEditor.cpp` (feature parameters, pick slots), `CreateActions.cpp`,
  `SketchInteraction.cpp` / `SketchTools.cpp` / `SketchPick.cpp` /
  `SketchDimensions.cpp` / `SketchOps.cpp` (the sketch session),
  `MeasureActions.cpp`, `MateActions.cpp`, `FastenerTool.cpp`, `CamTool.cpp`.
- **Panels:** `FeatureTreePanel.cpp`, `OccurrencePanel.cpp`, `CamPanel.cpp`,
  `CamGenerate.cpp`, `StudyPanel.cpp`, `DrawingPanel.cpp`,
  `VariablesPanel.cpp`, `ConfigurationsPanel.cpp`, `HistoryPanel.cpp`,
  `ConflictsPanel.cpp`, `RequirementsPanel.cpp`, `DesignSweepPanel.cpp`,
  `TemplatePanel.cpp`, `SettingsWindow.cpp`,
  `TerminalPanel.cpp`.
- **Numbers:** `ExprInput.cpp`, `ExprComplete.cpp`.
- **Jobs:** `BackgroundJob(s).h`, `CamJobs.cpp`, `DrawingJob.cpp`,
  `ImportJob.cpp`, `OpenJob.cpp`, `SyncJob.cpp`, `TopoJob.h`.
- **Projection into the renderers:** `RendererSync.cpp`, `SceneOverlays.cpp`,
  `SketchOverlay.cpp`, `TessellationCache.cpp`.
- **Actions by subject:** `ProjectActions`, `DocumentSlots`, `ViewActions`,
  `DrawingActions`, `AssemblyActions`, `MotionActions`, `SimulationActions`,
  `VariableActions`, `ImportActions`, …; helpers shared between them live in
  `AppActionsInternal.h`.

Settings and the keymap's metadata are in `src/core/io/` so they can be unit
tested without a renderer. The GUI tests are in `src/app/guitests/`.

## Not built

- Depth cycling to a face hidden behind another; box or lasso selection in
  3D; a selection model on drawing sheets.
- A quick toolbar at the cursor; chord sequences; keymap presets.
- A light theme; toolbar grouping with captions.
- A separate scale for icons: toolbar icons are 14 px against 16 px text, so
  raising the font size scales both and the ratio stays; icons read as small.
- Handles on committed features, and the others listed on each topic page.
