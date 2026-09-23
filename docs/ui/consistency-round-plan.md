# A consistency round: floating windows, CAM tools, studies, drawings, words

## The problem

The CAM and assembly tidies (docs/ui/cam-panel-tidy-plan.md,
docs/ui/assembly-panel-tidy-plan.md) set the app's patterns. A list is rows
in the feature list's style (`ui::ListRow`): click selects, double-click
edits, right-click offers the rest, and a hover eye shows visibility. Editing
happens in the floating tool window, not inline under the list. A verb with a
result is a toolbar button that opens a window. Long work shows the shared
`DrawJobProgress`. An audit of every panel found where the app still does
something else. The most visible:

- **Floating windows collide.** The Generate window and the BOM window
  anchor at exactly the tool window's corner, so they open on top of it, and
  they anchor once and never again. The tool window re-anchors, clamps to the
  app and draws a shadow. Each window rolls its own placement.
- **A CAM tool cannot be deleted** from a setup: no button, no menu, no RPC
  path from the panel.
- **The Study panel is the old CAM panel.** A plain Selectable list, a big
  Delete button at the bottom, and the whole study editor, Run button and
  results inline under it. Assembly studies already use a floating window
  (`DrawSimulationToolWindow`), so one feature has two homes. Loads and kept
  faces are text rows with trash buttons and `PushID(int)`, which the test
  engine cannot address. The participant-role combo offers three of the four
  roles: Bonded is missing, though its help text names it. A running solve
  shows its own "Solving... 12s" line.
- **The Drawing panel** lists views and dimensions as bullets with "x" and
  "Bend table" buttons, and no selection or menu. An inline add-view form
  duplicates the toolbar's popup, which offers fewer orientations and no
  configuration. Dimension buttons, Update views, Fit, Undo and Redo duplicate
  the toolbar, and "Part note" exists only in the panel.
- **Words and glyphs.**
  - Section's window says "Done" while Exploded, Motion and Simulation say
    "Close", with a comment claiming Section as the rule.
  - The mate window's Accept and Cancel are plain text, while the feature
    tool window's carry icons and ids.
  - Two different trash glyphs.
  - Five toolbar glyphs mean two things each: Unfold and Fit, Loft and the
    arcs, Thread and Circle, Derive and Offset, the ellipsis for "more
    constraints" and "pattern callout".
  - The sketch toolbar's buttons have no `###tb_*` ids.

## The shape

**One floating-window helper.** `ui::BeginFloatingWindow` in
`ui/FloatingWindow.h` does what the tool window does (and the tool window
uses it): anchor to a viewport corner,
re-anchor when the viewport moves, clamp to the app, the elevated border,
`NoSavedSettings`, and the shadow. A top-right window stacks BELOW the tool
window when one is open (`state.toolWindowRect`), instead of on top of it.
Generate, the BOM and the CAM transport use it. The transport keeps its
bottom anchor.

**CAM tools as rows.** Each of a setup's tools becomes a `ListRow`: the
number and a glyph by type, the description, and the diameter as the tail.
Double-click opens the tool's fields in the tool window, and right-click
offers Edit and Delete. Delete REFUSES while an operation uses the tool, and
names those operations, rather than leaving them pointing at nothing.

**Studies as the assembly already has them.** The Study panel becomes a list
of `ListRow`s: a glyph for static, modal or topology, the name, and the state
(solved, stale, failed) as the tail. Double-click, or the toolbar's Run
(`tb_sim_run`), opens the study in the floating simulation window that
assembly studies use. There it is edited, run and read, with the solve in
`DrawJobProgress`. The inline editor, the panel's Run and the results block
go. Loads and kept faces become rows with Remove in the menu. The role combo
offers all four roles.

**Drawings as structure.** Views and dimensions become `ListRow`s (a view's
tail is its scale, a dimension's its value). Click selects on the sheet, and
right-click offers Delete and Add bend table. Add view is one path: the
toolbar's popup gains the configuration choice and all seven orientations,
and the panel's inline form goes. So do the panel's dimension, Update, Fit,
Undo and Redo buttons, and Part note moves to the toolbar
(`tb_dwg_part_note`). Section titles use `SeparatorText`.

**Words and glyphs, one set.**
- A window that applies as you go says **Close**. Section changes to it, and
  the comments stop claiming Section as the rule.
- A window that creates something says **Accept** and **Cancel**. The mate
  window uses the tool window's icon buttons and ids.
- One trash glyph (`ICON_FA_TRASH_CAN`).
- Distinct glyphs for the five pairs that share one.
- The sketch toolbar's buttons get `###tb_sk_*` ids.

Not in this round, because each is its own round: the Configurations
panel's checkbox columns, the History panel's row buttons, and the dropdown
sweep (a shared radio row for 2-4 choices across the feature editor, CAM and
Settings).

## Files

- `src/ui/FloatingWindow.{h,cpp}`: `BeginFloatingWindow`.
- `src/ui/CamGenerate.cpp`, `src/ui/OccurrencePanel.cpp` (BOM),
  `src/ui/CamPlayback.cpp`: the helper.
- `src/ui/CamPanel.cpp`, `src/ui/CamTool.cpp`: tool rows, tool editor,
  refused delete.
- `src/ui/StudyPanel.cpp`, `src/ui/Toolbar.cpp`: study rows, the floating
  study window for part studies, the progress bar, loads and kept faces as
  rows.
- `src/ui/DrawingPanel.cpp`, `src/ui/Toolbar.cpp`: view and dimension rows,
  one add-view path, Part note on the toolbar.
- `src/ui/PreviewFeature.cpp`, `src/ui/OccurrencePanel.cpp`,
  `src/ui/StudyPanel.cpp`, `src/ui/SketchToolbar.cpp`,
  `src/ui/Toolbar.cpp`: words, glyphs, ids.
- `src/app/guitests/*`: tests that click moved widgets follow them.

## Verified by

The GUI suite, with moved widgets' tests updated rather than deleted. New
tests:

- a floating window opened while the tool window is up lands below it, not on
  it;
- a CAM tool deletes from its row, and refuses while an op uses it;
- a part study opens in the floating window from its row and from the
  toolbar, and runs there;
- a drawing view deletes from its row menu, and the toolbar adds a view in
  every orientation;
- a glyph means one tool across the toolbars, and a feature's tree glyph is
  its button's (a convention test over both files).

## As built

As planned, with these differences.

- **Drawings have no selection**, so view and dimension rows have the menu
  gestures but a click highlights nothing on the sheet. The row menu does
  more than planned: it also sets a view's **Scale**, which only the add form
  could before, and which can now be changed after the view is placed.
- **Add view became a tool window, not a richer popup.** The panel's form
  (source, configuration, all seven orientations, scale) moved into the tool
  window the toolbar's Add view opens (`drawingAddViewOpen`,
  `DrawAddViewToolWindow`), with its widget ids unchanged. The toolbar's
  four-orientation popup went. Update views and the stale count: the button
  went, and the count stays in the panel as a line pointing at the toolbar.
- **Studies:** a part's list stays docked and its editor moved into the
  Simulation window, which part simulation mode now opens (an assembly's list
  and editor are both in that window, as before). The Run button stays in the
  editor, where the results are read. The "Bonded" help text was right and
  the combo was wrong: it offered three of the four roles, so the GUI could
  not bond a participant, and one bonded over the RPC showed as "Ignored".
  The combo now has all four. Topology and motion runs had drawn their own
  Cancel beside the shared bar; both use the bar's.
- **CAM tools:** Delete is disabled in the row menu while an operation uses
  the tool, with the operations named in its tooltip; `RemoveCamTool` refuses
  with the same sentence. Starting a verb now clears an op or tool editor,
  which it never did: the editor kept the window.
- **Glyphs:** the audit named five clashes, and the gate found more. Nine
  glyphs now differ:
  - Section, Derive, Loft, Thread, Corner relief and Unfold on the part bar;
  - the sketch Circle, Parallel and Slice;
  - the Ball mate;
  - Distance and Pattern callout on the drawing bar;
  - Profile, Drill, Whole and Index face on the job bar.

  The feature tree follows the toolbar for Thread, Hole, Corner relief,
  Unfold, Loft, Derived, Draft and Split. Since the icons round
  (docs/ui/icons-plan.md) both read ONE table, `src/ui/Icons.cpp`, so the
  tree cannot disagree with the button. The gate (`test_conventions.cpp`,
  "a toolbar glyph means one thing and a feature has one glyph") reads that
  table: it fails on a glyph shared by different commands, with an explicit
  list of same-idea groups (Extrude, Hole, Fillet and others on several
  bars), and on a panel asking it for a command it does not hold.
  `toolbar/no_duplicate_glyph` checks the drawn bars and flyouts.
- **Words:** Measure, Section, the fastener tool, the op, tool, occurrence
  and relation editors and the reference-image session say Close. The mate
  window uses the tool window's Accept and Cancel (`###tw_accept`,
  `###tw_cancel`) when making a mate and Close when editing one. The sketch
  bar's buttons answer to `###tb_sk_<command>`. Part note is on the drawing
  toolbar, with the `drawing.dim_part_note` command bound to L.

New GUI tests:
- `cam/generate_opens_below_the_tool_window`;
- `cam/a_tool_deletes_from_its_row_unless_in_use`;
- `sim/a_study_is_a_row_edited_in_the_window`;
- the drawing test's row-menu Scale and Delete.

Tests that clicked moved widgets were updated:
- the tool diameter, through the tool editor;
- the simulation helpers and tests, which now address the Simulation window;
- the three add-view tests, through the tool window;
- the bend table, through the row menu;
- the mate editor's Close.
