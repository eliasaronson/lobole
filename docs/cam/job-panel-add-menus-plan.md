# Parts and fixtures: one Add menu each

## The problem

On a job with several parts in the project (the speaker box: `front_wall`
cut, and five more walls, a divider and a stray `Part 1` not cut), the CAM
panel's Parts and Fixtures sections have these problems:

1. **The fixture add buttons run off the panel.** They are one `SameLine`
   row with a button per part, so `divider` is clipped and nothing wraps.
2. **The same parts are offered twice.** Parts has a column of "Add
   bottom_wall…" (to cut it), and Fixtures has a row of "🔒 bottom_wall"
   (to clamp with it). Nothing says which is which.
3. **The add buttons look like the fixture rows.** "Keep-out box" is a
   button, but it reads as an existing item. The lock glyph on the part
   buttons reads as "locked", not "add as a clamp".
4. **An empty Fixtures section says nothing.** That hides the fact that
   verify is checking against no workholding at all.
5. **The placement fields have no labels.** A part's row is
   `0.000 0.000 0.0°`.

## The fix

**Parts: one "+ Add part" button** (`###cam_part_add_menu`) opens a popup
listing every project part the job does not hold, one menu item each
(`###cam_part_add`, under the part's id). With no such part, the button is
disabled and its tooltip says why.

**Fixtures: one "+ Add fixture" button** (`###cam_fixture_add_menu`) opens a
popup with two groups:
- "Keep-out box" first (`###cam_fixture_box`), with its tooltip: "No part
  needed".
- Then a disabled "Hold it with a part:" heading, and the project parts the
  job does not cut (`###cam_fixture_add`).

Each item's tooltip says what the choice does, so the difference between
cutting a part and clamping with it is written down where the choice is
made.

**The empty state.** A Fixtures section with no fixtures says, dimmed:
"Nothing holds the stock -- verify checks no clamps."

**Labels in the drag formats, and a row that fits.** The placement becomes
`X %.2f`, `Y %.2f` and `%.1f°`. The degree sign says which field is the
turn, and its tooltip names it. On a 220 px panel the old single row pushed
Cut and Remove off the edge, so those two move up beside the part's name,
and the three fields share the panel's width on their own line.

Popups, not combos: a combo gathers with an empty label and the GUI test
engine cannot address it (docs/cam/stock-and-origin.md W6). A popup's menu
items it can address, through `//$FOCUSED`.

## Files

- `src/ui/CamPanel.cpp`: the two sections.
- `src/app/guitests/GuiTestsCam.cpp`: the two tests that clicked the old
  buttons (`cam_fixture_box`, `cam_part_add`) go through the menus.
- docs/cam/gui.md: a line on the change.

## Verification

- **The two existing tests, through the menus.**
- **A new GUI test `cam/add_menus_offer_the_right_parts`:**
  - With three project parts, one of them cut, the part menu lists the two
    that are not cut and the fixture menu lists the same two plus the box.
  - Choosing a part from the fixture menu adds it as a fixture under that
    part's name.
- **A screenshot of the panel,** looked at: the menus, the labelled
  placement row, and the empty-state line. That line is plain text with no
  item id for the test engine to find, so the screenshot is its check.
- **Suites:** the GUI suite.
