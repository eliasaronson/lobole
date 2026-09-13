#pragma once

// The one click gesture and the one Delete verb for every LIST of things
// with ids — features in a part, occurrences in an assembly, operations in a
// job (docs/ui/u1-selection.md, "Lists"). The rows themselves stay in their
// panels: a feature row carries a rollback bar and a suppress item, an
// occurrence row a visibility box, an operation row an enabled box and a
// ghost button, so a shared row widget would be a configuration blob and
// would rename the item ids the GUI tests address. What IS shared is the
// part that had drifted: two lists could Ctrl-click, one could right-click
// Delete, none could Shift-click a range, and Delete on the keyboard reached
// only occurrences.

#include "ui/AppState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// A row of `kind` with `id` was clicked. `order` is the rows as the list
// shows them, which is what a Shift range spans. A plain click selects the
// row alone and becomes the range anchor; Ctrl toggles it; Shift selects
// from the anchor to it (replacing this kind); Ctrl+Shift adds that range.
// `ownKindOnly`: a plain click clears only items of this kind (an
// occurrence click keeps the selected mate, the assembly panel's rule) or
// the whole selection (a feature click drops picked faces, the tree's rule).
// `clickClears`: a plain click on the row that IS the whole selection of its
// kind clears it. For a list whose selected row opens an editor INSIDE the
// panel, with nothing to close: the row you just clicked is where a person
// aims to put it away, and it is the only affordance there. Off by default,
// because most lists auto-select what they just created — clicking the row
// to confirm what you are looking at must not make it vanish.
void ListRowClicked(AppState& state, SelectionItem::Kind kind, uint64_t id,
                    const std::vector<uint64_t>& order, bool ownKindOnly,
                    bool clickClears = false);

// A row was right-clicked for its context menu. Inside the selection it
// stays — collapsing to the clicked row is how a five-part selection is lost
// by aiming at it; outside it, the row becomes the selection so the menu's
// verbs act on what was aimed at.
void ListRowContext(AppState& state, SelectionItem::Kind kind, uint64_t id,
                    bool ownKindOnly);

[[nodiscard]] bool IsSelected(const AppState& state, SelectionItem::Kind kind,
                              uint64_t id);
// Every selected id of one kind, in selection order.
[[nodiscard]] std::vector<uint64_t> SelectedIds(const AppState& state,
                                                SelectionItem::Kind kind);

// "Delete feature" / "Delete 3 features": the menu item for what Delete
// would remove. `noun` is singular.
[[nodiscard]] std::string DeleteLabel(size_t count, const char* noun);

// The same shape for any other row verb: "Hide occurrence" / "Hide 3
// occurrences". A menu item that names one row while acting on five is how a
// multi-selection gets edited by accident, so every verb counts what it will
// touch the way Delete does.
[[nodiscard]] std::string RowActionLabel(const char* verb, size_t count,
                                         const char* noun);

// Removes whatever the selection holds of the kinds a list can delete —
// features on a part tab, occurrences on an assembly tab, operations on a
// job tab — as ONE undo step where the tab has undo. Returns how many went;
// 0 when nothing deletable is selected. The Delete key and every list's
// context-menu Delete call this and nothing else.
int DeleteSelection(AppState& state);

} // namespace ui
