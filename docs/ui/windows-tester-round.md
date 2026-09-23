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
(src/ui/Theme.cpp); the two icon fonts merged into it (Font Awesome, and
Designer's own CAD glyphs since docs/ui/icons-plan.md) are `14.0f * scale`
with `GlyphMinAdvanceX = GlyphMaxAdvanceX = 14.0f * scale` (src/ui/Theme.cpp). **The icons are 87% of the text size.** Toolbar
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
The first answer — that the row's `WindowPadding` and height were raw
unscaled literals pushed after `ScaleAllSizes()` — was true and not the cause.
Measuring the rendered pixels found the real one: **the padding was never
applied at all.** Since ImGui 1.90 a child window with no border ignores
`WindowPadding` ("no padding by default for non-bordered child windows",
imgui.h), and `##ToolbarRow` is borderless. The buttons sat flush against the
top and left edges of the bar, and the only reason the bar looked padded
underneath is that the height calculation added padding the layout then
refused to apply, so all of it piled up below the icons.

Measured at scale 1.0, child rows 26-67 against button rows 26-51: **0 px
above, 16 below**. With `ImGuiChildFlags_AlwaysUseWindowPadding`: buttons at
34-59, **8 above and 8 below**.

Worth keeping as a method note. Three fixes in a row — scale the literals,
stop the height slop, double the padding — each moved the number and none
found the cause, because all three were read out of the source. The pixels
settled it in one measurement, and the way to get them was a screenshot with
the bar's background temporarily set to pure red, which makes the child's
real rect unambiguous.

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

**Through the solid: already what SolidWorks and Onshape do, so it stays.**
Both keep the dimensions of the sketch you are EDITING on top of the model,
deliberately. The claim once made here — that ours also does it for sketches
that are not being edited — was wrong: `DrawDimensions` and
`DrawConstraintGlyphs` both return early unless `edit.IsEditing()` and read
`ResolveActiveSketch`, so only the active sketch is ever annotated
(src/ui/SketchDimensionDraw.cpp:244, :810). No change.

(The sketch's entity LINES are the other way round — depth-tested with a
small bias, src/render/SketchRenderer.cpp:161-163 — so a solid hides them
while their dimensions stay on top. SolidWorks draws the active sketch's
geometry on top too. Nobody has complained, so it is noted, not changed.)

**Over the Preferences window: fixed.** Every viewport annotation now draws
into the dockspace host's draw list (`ui::ViewportDrawList`) instead of the
foreground list. ImGui renders `##DockHost` first, so anything the user puts
in front of the viewport covers the annotation, which is what "in front"
means. Ten call sites moved; a screenshot with a dimensioned sketch open in
edit mode behind the Preferences window shows the labels in the viewport and
a clean dialog.

Finding the last of them needed the picture. After the first six were moved,
the dialog still had the sketch's "U +X" / "V +Y" axis compass painted across
it — a seventh site in a different file that no grep for "dimension" would
have turned up.

## 6. The uninstaller cleaned up correctly

Reported working. Recorded because nothing here tested it.
