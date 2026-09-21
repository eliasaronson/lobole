# The CAM panel, tidied

## The problem

The docked CAM panel does three jobs at once, and does one of them twice:

- **It is the job's structure:** setup, machine, parts, stock, tools and
  the operations. This is what a docked panel is for, and what the part tab's
  feature list is.
- **It is the run:** Generate, the cut order, one file per tool, Show path,
  a screenful of verdict lines, the cutting length and Save G-code. That is a
  verb with a result, and the toolbar already has the verb (the gears,
  `job.generate`). Pressing it generates and verifies silently, and the
  result lands in the panel, below the fold.
- **It is each operation's editor,** inline: every row carries a drag
  handle, a checkbox, "show", "x", a long sentence ("52 mm deep in 1 pass,
  climb" on a rotary op, where neither word means anything) and an
  "Advanced" tree.

The floating CAM window already shows the operation list, the verdict and
Show path for the machining verbs, so most of the run is drawn in two places.

## The shape

**The panel becomes the structure, styled as the feature list.** The setup
and machine row stays, and so do the Parts, Stock and safety and Tools
sections. The operations become rows like feature rows:

```
 ▾ Operations
   ◎  Rough 1 (helical, leave 1.5)        T1 6 mm ball
   ◎  Finish (helical)                    T1 6 mm ball
   ○  Finish (parallel)       [disabled, dimmed]
   + Add operation
```

- A type glyph, then the name as the selectable, full-width row. A dim tail
  gives the tool, and a tooltip gives the full summary and the move counts.
  A disabled op dims, as a suppressed feature does.
- **Click** selects (the existing `ListSelection`), **drag** reorders,
  **double-click** opens the op's editor, and **right-click** offers Edit,
  Enable/Disable, Show only this path, and Delete. Those are the feature
  list's gestures, where the op row used to have four buttons.
- The op's settings (what "Advanced" held) move to a floating **op editor**,
  the same tool window the machining verbs open. Parts already work this
  way: a feature's parameters are edited in a floating window, not in the
  tree.

**Generate becomes a tool, like Simulate.** The toolbar's gears open a
floating **Generate** window, and the run happens there:

```
 Generate ─────────────────────────────
  [progress, the shared widget, while it runs]
  ✓ ~118 min of cutting · 117.4 m · 133,936 lines
  ⚠ Leaves 1.26 mm on the end faces …
  ⚠ T1 cuts hardwood under the chip-load band …
  Cut: [as listed ▾]   [ ] One file per tool
  [x] Show path   [x] Stock
  [Regenerate]   [Save G-code…]          [Done]
```

It generates on open when the toolpaths are stale, shows the verdict where
the button was pressed, and saves from there. The panel keeps one dim line
under the operations (for example "Generated · ~118 min · 3 warnings"), which
opens the window on a click, so the state of the run is visible without its
text.

**While in there, the three wrong sentences from the screenshot:**

- An op's summary is type-aware. A rotary op says its pattern, lead and
  stepover instead of "deep in 1 pass, climb", and its move line gives a
  radius instead of "down to Z21.70".
- The verdict's end-face line counted end faces by REGION, and a dome
  region is mostly surface, so it printed "0 mm2 in 0 places" beside 1.26 mm.
  The end-face area is summed by cell instead.

## Files

- `src/ui/CamPanel.cpp`: the run block goes; op rows restyled; Advanced
  moves out.
- `src/ui/CamTool.cpp`: the Generate window, and the op editor (the Advanced
  contents, moved rather than rewritten).
- `src/ui/Commands.cpp`: `job.generate` opens the window.
- `src/core/cam/CamRotary.cpp`, `CamVerify.cpp`: end-face area by cell.
- `src/app/guitests/GuiTestsCam.cpp`: the nine tests that click the moved
  widgets, plus one for the new window and one for the op row's gestures.

## Verified by

The GUI suite (the moved widgets' tests updated, not deleted); a test that
the toolbar's Generate opens the window and saves from it; a test that
double-clicking an op opens its editor and right-click Disable dims it; and a
windowed screenshot of the panel before and after.

## As built

As planned, with three small differences. The Generate window closes with
its title-bar close box, not a Done button. The toolbar's gears always
regenerate; the panel's status line opens the window as it stands, which is
where "only when stale" ended up. And the row menu's path item is the existing
ghost, "Show what it removes", rather than a new "show only this path".

The op settings were moved, not rewritten: `DrawCamOpSettings`
is the old "Advanced" body behind a function, and `ApplyCamOpEdit` is the
per-op re-cut the panel always did. The run block became `CamGenerate.cpp` the
same way. Eight GUI tests clicked moved widgets and were updated to the new
gestures (the toolbar for Generate, a double-click for the settings, a row
drag for reorder, the menu for Delete). Two tests are new:
`cam/generate_is_a_tool_with_a_window`, which saves G-code from the window,
and `cam/an_op_row_has_the_feature_gestures`.

The end-face sentence sums the area by cell (`leftoverEndFaceArea`, and
`cam_verify`'s `leftover_end_face_area_mm2`).

No windowed screenshot was taken: `headless-preview.sh --gui` hangs on this
machine before its first edit (the app sleeps with the request queued), which
is its own bug and unrelated to the panel. The GUI suite drives the real
windowed panel under its own Xvfb runner, and passes.

