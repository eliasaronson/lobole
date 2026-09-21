# The assembly panel, tidied, and one disclosure header everywhere

## The problem

The assembly tab's Model panel is the CAM panel's old problem in another
room (docs/ui/cam-panel-tidy-plan.md):

- **Occurrences** are a checkbox and a name per row. The checkbox is
  visibility, and a column of blue ticks reads as "selected", not "shown".
  "(fixed)" and "[assembly]" are words appended to the name.
- **Mates** are a column of identical "Mate [revolute]" rows. Nothing says
  WHICH two occurrences a mate joins until you click it.
- **Editing is inline.** Selecting an occurrence or a mate appends a
  "Selected ..." section of fields under the list, so the panel changes
  length under the cursor, and the list you were reading scrolls away.
- **The BOM** is a two-column table in a narrow dock, with material and mass
  stacked under each name. The table's five-column form exists and only
  appears when the dock is wide, which it never is.
- **The disclosure header** (ImGui's CollapsingHeader: a full-width bar with
  a small triangle) is easy to miss as something that expands. It is the same
  widget in 12 places: the BOM, CAM's Parts / Stock / Tools, the Settings
  window's sections, Mass properties, Units, Tolerances, Sheet metal and
  Engineering catalogs.

## The shape

**Rows in the feature list's style, for occurrences, mates and relations.**

```
 Occurrences  22
   ▣  Frame                        ⚓
   ▣  Fork                          👁   <- eye on hover only
   ▣  Front wheel      [dimmed: hidden]  ⊘   <- always shown when hidden
   ▤  Drivetrain                         (a sub-assembly glyph)
 Mates  30
   ⟳  Frame – Fork                 revolute
   ⟳  Fork – Front wheel           revolute
   ▦  Frame – Stem                 fastened
   ⚠  Stem – Handlebar             revolute   (red: it failed to solve)
```

- **Occurrence:** a glyph (part, or sub-assembly), then the name. An anchor
  at the right means fixed, where the name used to say "(fixed)". Hidden rows
  grey out, as in other CAD programs. The **eye** appears at the right of the
  row on hover and toggles visibility; a hidden row always shows the
  struck-through eye, so the state reads without hovering. That replaces the
  checkbox column.
- **Mate:** a glyph per type, then its name, or "A – B" (the two occurrences
  it joins) when it has none; the type is a dim tail. A mate that failed to
  solve is red, with the reason in the tooltip.
- **Gestures, as features and CAM ops:** click selects (Ctrl/Shift extend),
  double-click edits, right-click for the menu (Edit, Hide/Show, Open part,
  Edit in context, Detach, Delete, as today).

**Edits float, as in CAM.** Double-click (or Edit) opens the occurrence's
properties (name, position, rotation, fixed, open the part, edit in context)
or the mate's (name, flip and its degrees of freedom) in the tool window, the
slot the mate tools already use to CREATE a mate. The inline "Selected
occurrence" and "Selected mate" sections go, so the panel is just the lists.

**The BOM becomes a floating window**, opened from the assembly toolbar,
because a table is what it is and it wants width. Name, qty, material, mass
and total as real columns, sortable, with the total mass in the header. The
narrow two-row form and its header in the panel go. A dock tab of its own
was the alternative, and loses because the BOM is looked at, not lived in.

**One disclosure header for the whole app** (`ui/Widgets.h`,
`SectionHeader`): a chevron that points right when closed and down when open,
the label in the heading weight, an optional count ("Occurrences 22"), and a
hover highlight across the whole row. It is drawn as a row, not as a filled
button bar. Its open state is kept as CollapsingHeader's was, so no layout
changes on upgrade. All 12 CollapsingHeaders move to it, which covers the
Settings window, and the plain `SeparatorText` section titles in the
occurrence panel become the same header, so its sections collapse too.

## Files

- `src/ui/Widgets.{h,cpp}`: `SectionHeader`, and a `ListRow` helper (glyph,
  label, tail, dimmed, hover-eye) that the occurrence and mate rows share.
- `src/ui/OccurrencePanel.cpp`: rows, edits to the tool window, the BOM
  moved out.
- A new `src/ui/BomWindow.cpp`, plus a toolbar button and command
  (`asm.bom`).
- The 12 CollapsingHeader call sites.
- `src/app/guitests/*`: the tests that click `###occ_vis_*`, the BOM header
  and the Selected sections follow them; new tests for the hover eye,
  double-click editing and the BOM window.

## Verified by

The GUI suite, with the moved widgets' tests updated rather than deleted.
New tests: the eye hides and shows and greys the row; a double-click opens
the occurrence's and the mate's editor, and an edit there moves the part; the
BOM window opens from the toolbar with its columns and total; and a
SectionHeader toggles and keeps its state.

## As built

As planned, with two placement differences. The BOM window's code stayed in
`OccurrencePanel.cpp` (`DrawBomWindow`) rather than a new file, beside the
BOM helpers it uses, and Check fasteners and Check fits moved into it with the
table: both are questions about what the assembly is made of, and both lived
under the BOM header. The eye and the rows come from one helper in
`ui/Widgets.h` (`ListRow`), which elides a name that does not fit and gives it
back at once on hover, as the TextFit rows always did. The section header's
label is in the normal text weight, not a heading weight: the app loads one
text font, and the chevron and the hairline under the row carry the "this
opens" that the bold was meant to.

The mate editor is the mate tool's own Edit Mate mode, extended with the
name and the type-specific fields (`DrawMateDofFields`, moved from the
"Selected mate" section). Occurrences and relations share one editor,
`DrawAssemblyEditWindow`, in the tool window's slot. A new command,
`assembly.bom` (B on the assembly tab), opens the BOM.

Six GUI tests followed the widgets to their new gestures. One is new,
`assembly/the_panel_lists_and_the_editors_float`, and `a_part_is_hidden_from_its_own_row`
now clicks the eye both ways.

