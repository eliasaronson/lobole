#include "ui/ListSelection.h"

#include "core/Command.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPanel.h"
#include "ui/SelectionPipeline.h"

#include <imgui.h>

#include <algorithm>

namespace ui {

namespace {

SelectionItem Item(SelectionItem::Kind kind, uint64_t id) {
  return {kind, id, {}};
}

void EraseKind(Selection& sel, SelectionItem::Kind kind) {
  auto& items = sel.items;
  items.erase(std::remove_if(
                  items.begin(), items.end(),
                  [kind](const SelectionItem& it) { return it.kind == kind; }),
              items.end());
}

uint64_t& Anchor(AppState& state, SelectionItem::Kind kind) {
  return state.selection.rangeAnchor[static_cast<int>(kind)];
}

} // namespace

bool IsSelected(const AppState& state, SelectionItem::Kind kind, uint64_t id) {
  return state.selection.Has(Item(kind, id));
}

std::vector<uint64_t> SelectedIds(const AppState& state,
                                  SelectionItem::Kind kind) {
  std::vector<uint64_t> ids;
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == kind)
      ids.push_back(it.id);
  return ids;
}

void ListRowClicked(AppState& state, SelectionItem::Kind kind, uint64_t id,
                    const std::vector<uint64_t>& order, bool ownKindOnly,
                    bool clickClears) {
  const ImGuiIO& io = ImGui::GetIO();
  Selection& sel = state.selection;
  uint64_t& anchor = Anchor(state, kind);
  const auto pos = [&](uint64_t v) {
    return std::find(order.begin(), order.end(), v);
  };
  const auto anchorAt = pos(anchor);
  const auto clickedAt = pos(id);
  if (io.KeyShift && anchor != 0 && anchorAt != order.end() &&
      clickedAt != order.end()) {
    // Everything between the anchor and this row, in list order. Without
    // Ctrl the range IS the selection of this kind; with it the range joins
    // what was already picked (the anchor stays where the plain click put
    // it, so a second Shift-click re-spans from the same place).
    if (!io.KeyCtrl)
      EraseKind(sel, kind);
    auto lo = std::min(anchorAt, clickedAt), hi = std::max(anchorAt, clickedAt);
    for (auto it = lo; it <= hi; ++it) {
      const SelectionItem item = Item(kind, *it);
      if (!sel.Has(item))
        sel.items.push_back(item);
      if (it == hi)
        break;
    }
    return;
  }
  if (io.KeyCtrl) {
    sel.Toggle(Item(kind, id));
    anchor = id;
    return;
  }
  // Clicking the row that IS the whole selection clears it, where the list
  // asked for that (see the header). Clicking ONE row of a multi-row
  // selection still collapses to that row — the gesture people use to narrow
  // a selection down — so only the single-row case toggles.
  if (clickClears && SelectedIds(state, kind) == std::vector<uint64_t>{id}) {
    EraseKind(sel, kind);
    anchor = 0;
    return;
  }
  if (ownKindOnly)
    EraseKind(sel, kind);
  else
    sel.Clear();
  sel.items.push_back(Item(kind, id));
  anchor = id;
}

void ListRowContext(AppState& state, SelectionItem::Kind kind, uint64_t id,
                    bool ownKindOnly) {
  if (IsSelected(state, kind, id))
    return;
  Selection& sel = state.selection;
  if (ownKindOnly)
    EraseKind(sel, kind);
  else
    sel.Clear();
  sel.items.push_back(Item(kind, id));
  Anchor(state, kind) = id;
}

std::string RowActionLabel(const char* verb, size_t count, const char* noun) {
  if (count <= 1)
    return std::string(verb) + " " + noun;
  return std::string(verb) + " " + std::to_string(count) + " " + noun + "s";
}

std::string DeleteLabel(size_t count, const char* noun) {
  return RowActionLabel("Delete", count, noun);
}

int DeleteSelection(AppState& state) {
  if (state.document && state.commands) {
    // Features go through the command stack: one compound command, so a
    // five-feature delete is one Ctrl+Z (RemoveFeaturesCommand).
    std::vector<size_t> indices;
    for (const uint64_t id : SelectedIds(state, SelectionItem::Kind::Feature)) {
      size_t index = 0;
      if (state.document->FindFeatureById(id, &index) != nullptr)
        indices.push_back(index);
    }
    if (indices.empty())
      return 0;
    const size_t n = indices.size();
    if (!state.commands->Execute(
            std::make_unique<RemoveFeaturesCommand>(std::move(indices)),
            *state.document))
      return 0;
    // The removed ids dangle until PruneSelection runs next frame; drop them
    // now so a caller reading the selection after this sees the truth.
    EraseKind(state.selection, SelectionItem::Kind::Feature);
    return static_cast<int>(n);
  }
  if (state.assembly) {
    // Mates are a list in this panel like any other, so Delete reaches them
    // too — it used to reach only occurrences, which left the row's own
    // "Delete mate" button as the single way to remove one.
    //
    // Both kinds go, in ONE undo step. An occurrence click deliberately keeps
    // the selected mate (the assembly panel's rule), so the two selections
    // coexist routinely, and picking one of them to honour would mean Delete
    // silently ignored something the panel was showing as selected. What the
    // two lists show picked is what Delete takes.
    const std::vector<uint64_t> mates =
        SelectedIds(state, SelectionItem::Kind::Mate);
    if (mates.empty())
      return DeleteSelectedOccurrences(state);
    SnapshotAssembly(state);
    int removed = 0;
    for (const uint64_t id : mates)
      if (state.assembly->RemoveMate(id))
        removed++;
    EraseKind(state.selection, SelectionItem::Kind::Mate);
    removed += DeleteSelectedOccurrences(state, /*takeSnapshot=*/false);
    if (removed > 0)
      SolveActiveAssembly(state); // the parts a mate held are free again
    return removed;
  }
  if (state.job)
    return RemoveCamOperations(
        state, SelectedIds(state, SelectionItem::Kind::Operation));
  return 0;
}

} // namespace ui
