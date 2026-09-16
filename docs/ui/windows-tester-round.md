# First outside tester: what the report found

A tester on real Windows hardware (RTX 3070, 2026-09-15) ran the build from
docs/packaging/windows-refresh-plan.md — the first person other than its author
to use this app. Seven items came back. This is the triage: what each one
actually is, with the evidence, and what fixing it means.

Two of them are worth stating up front because they change how much the rest
matters: **the app froze twice, and neither freeze is diagnosed.** Everything
else here is cosmetic or a wrong label next to correct geometry.

## 1. Two freezes — UNDIAGNOSED, and the app cannot tell us why

Both in sketch mode. The log simply stops: no crash, no stack, no error. The
crash handler covers SIGSEGV/SIGABRT and a freeze is neither.

Ruled OUT by measurement, so that the next person does not re-run these:

- *A slow or non-converging sketch solve.* `UpdateSketchStatus` does run a full
  `Solver::Load` + `Solve` EVERY FRAME (src/ui/UI.cpp:988), which is a real
  cost — 0.15 ms for a triangle, **3.7 ms/frame for a 60-segment chain** — but
  it does not stall: a degenerate zero-length rubber-band segment, which is
  what exists mid-draw between two clicks, solves in 0.11 ms.
- *A wrong DOF sending the solver into a loop.* The solver's answers are
  correct (see 4).

What would actually find it: **a frame watchdog**. A monitor thread that logs
when a frame exceeds a few seconds, naming the phase the main loop was last in
(a breadcrumb set around ui / solve / regen / render / file-dialog). That turns
"it hung" into "it hung in X" for this tester and every future one, and it is
the only item here that buys information rather than polish.

Until then the cheap question to ask the tester: start it with
`--remote 8800`, and when it freezes, see whether the app still answers
`ping` from another shell. Answering = the UI thread is stuck (a modal dialog,
a wait) while the app lives; silent = the whole process is blocked.

## 2. Icons are too small — and scaling the UI does not fix the ratio

The report is precise about this: *"the text was not hard to read to begin
with, only the icons"*, and raising the Font size preference to 140% scales
both, so the ratio never improves.

It is a ratio, and the numbers say so. Text is `16.0f * scale`
(src/ui/Theme.cpp:186); the Font Awesome icon font merged into it is
`14.0f * scale` with `GlyphMinAdvanceX = 15.0f * scale`
(src/ui/Theme.cpp:190-198). **The icons are 87% of the text size.** Toolbar
buttons pass no explicit size (src/ui/Toolbar.cpp:245-257 — `ImGui::Button(icon)`),
so a button is exactly one glyph plus padding. Every other CAD toolbar puts
icons well ABOVE body-text size; ours is the only element a user cannot read
and the only one drawn smaller than the prose next to it.

This is not a DPI bug. The display IS read — `glfwGetWindowContentScale` at
startup and on monitor change (src/app/main.cpp:1973-1983, 2323-2338), and the
one scalar `max(1, contentScale) * uiFontScale` drives the whole theme. That
path is correct and survives a font rebuild.

## 3. The toolbar row is cramped — and this one IS a scaling bug

The tester drew a line along the toolbar's top edge asking for more room.
src/ui/DockLayout.cpp:149-168 sets the row's padding and height from **raw
unscaled literals** — `WindowPadding = (6, 4)` and `oneRow = GetFrameHeight()
+ 12.0f` — pushed AFTER `ScaleAllSizes()` has run. So every other metric grows
with DPI and the font preference while the toolbar's breathing room stays at 4
and 12 physical pixels. At 150% the row is measurably tighter than at 100%.

## 4. "Fully defined" on a sketch that is not — it is the EMPTY sketch

The solver is not wrong. Measured directly against `sketch::Solver`:

| sketch | dof reported | correct |
|---|---|---|
| floating triangle, 3 sides dimensioned | 3 | 3 ✓ |
| same, one corner pinned to the origin | 1 | 1 ✓ |
| **empty sketch** | **0** | — |

An empty sketch has no free parameters, so `RunSolve`'s early-out returns
`dof = 0` (src/core/sketch/SketchSolver.cpp:100-113), and the pill prints
"Fully defined" (src/ui/SketchToolOverlay.cpp:502). The tester's screenshot is
a sketch opened seconds earlier with nothing drawn in it yet — the log agrees
("Started Sketch 3 (id 3)", then the shot).

Vacuously true, and a lie in the only sense that matters: it is the app's
answer to "am I done constraining?" before there is anything to constrain.
A sketch with no entities has no status to report and should say nothing.

## 5. Dimensions and constraint badges draw through everything

Two symptoms, one cause. `DrawDimensions` (src/ui/SketchDimensionDraw.cpp:251)
and `DrawConstraintGlyphs` (:816), plus snap glyphs and the selection box
(src/ui/SketchToolOverlay.cpp:242, :277), all draw into
`ImGui::GetForegroundDrawList()` — screen-space, no depth, composited after
every window.

- **Through an opaque body**: they carry no depth at all, so nothing can
  occlude them. (Sketch *entity lines* are a different path and ARE
  depth-tested — src/render/SketchRenderer.cpp:161-163, 272-274 — so they are
  not part of this.)
- **On top of the Preferences window**: the foreground list paints over every
  ImGui window, and the call site (src/ui/UI.cpp:1026-1037) has no test for a
  dialog being open. Preferences is not even a modal
  (src/ui/SettingsWindow.cpp:611), so nothing else holds it back either.

The second is unambiguously wrong: a dialog is in front, and the drawing must
not cross it. The first is a **design decision, not a defect** — SolidWorks and
Onshape both keep the active sketch's dimensions on top of the model while you
edit it, deliberately. What is not defensible is doing it for a sketch that is
NOT being edited, which is what the tester was looking at.

## 6. The uninstaller cleaned up correctly

Reported working. Recorded because nothing here tested it.
