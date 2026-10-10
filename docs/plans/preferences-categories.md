# Preferences: categories on the left

## The problem

Preferences is one floating window (src/ui/SettingsWindow.cpp) that stacks
every section as a collapsing header in one scroll:

- four generated from the settings table: Appearance, General, Performance,
  Sharing;
- two hand-written: Keyboard and Machines.

Machines is a library (a list, then a form under it). Print profiles
(print-profiles.md) would be a third section of that kind. The single
scroll already mixes one-line toggles with whole editors:

- A machine's tool table pushes the rest of the window off screen.
- Finding a setting means opening and scrolling through headers.
- "Open Preferences › Machines" from the CAM panel opens the window
  wherever it was scrolled.

## The shape of the fix

**One page at a time.** The window is split in two:

- **Left: a category list,** an icon and a label per category (Font
  Awesome glyphs, as `CommandIcon` uses), the selected one highlighted.
- **Right: the selected category's page alone,** with its title at the
  top.

**The categories:**

- **Table sections:** each one becomes a category, in table order. The
  `Field::section` string stays the key, so a new row lands in its
  category with no window change.
- **Hand-written pages:** Keyboard, Machines and, once it exists, Print
  profiles, registered in one list beside the table's sections. That
  replaces today's two `SectionHeader` calls, so the window has a single
  place that knows the order.

**Library pages get the room they need.** The list goes on the left of the
page and the selected item's form on the right, instead of the form under
the list. Machines moves to this layout. Print profiles is built in it
from the start.

**Search:**

- A field above the category list filters by row label and blurb.
- While it holds text, the right side shows the matching rows from every
  table category, each under its category's name. The library pages match
  by item name and open on a click.
- This is docs/ui/settings.md's "a search field" under Not built.

**Footer:**

- `Reset <category> to defaults` for a table category. Library pages have
  none, since a library's items are the user's data.
- `Reset all to defaults` and the config file's path, as today.

**Opening on a page.** `OpenPreferences(state, category)` opens the window
on a page. The CAM setup's `Preferences › Machines` button and the Print
process section's `Preferences › Print profiles` button use it. The
selected category is remembered for the session only.

**What stays:**

- one floating window, not a dock tab;
- sizes measured in font units;
- every control's stable `###id`;
- the table driving the generated pages;
- the get/set settings RPCs.

## Files

- `src/ui/SettingsWindow.cpp`: the layout, the page registry, search,
  per-category reset; Machines' list-beside-form.
- `src/ui/SettingsWindow.h` (or the actions header that opens it):
  `OpenPreferences(state, category)`.
- `src/ui/AppState.h`: the selected category and the search text.
- `src/ui/CamTree.cpp`: its Preferences button opens on Machines.
- `src/core/io/Settings.{h,cpp}`: a category's display order and icon,
  if the section string alone is not enough.
- GUI tests that address `###sec_<Section>` (GuiTestsShell.cpp,
  GuiTestsCam.cpp): they select `###cat_<Section>` instead.
- docs/ui/settings.md ("The window"; search leaves Not built).

## Verification

- **Every table row is reachable from exactly one category,** and every
  category has at least one row (a test over `settings::Fields()` and the
  page registry).
- **A new table row with a new section string** appears as a new category
  without touching the window (the same test, run on a fixture table).
- **GUI tests:**
  - select each category and see its rows and nothing else;
  - search "shadow" and see the shadow rows from Performance;
  - reset one category and see the others keep their values;
  - the CAM panel's button opens the window on Machines;
  - a machine edited in the new list-beside-form layout saves, as today.
- **Window size and screenshots:** the window fits a 1280 x 720 viewport
  at font scale 100 % and 150 % with no horizontal scrolling. Screenshots
  of each page, taken offscreen with the GUI build, look right.
