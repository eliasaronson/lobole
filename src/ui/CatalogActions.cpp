#include "ui/CatalogActions.h"
#include "core/Command.h"
#include "ui/AppState.h"
namespace ui {
bool InstallCatalog(AppState& state, const catalog::Json& entry) {
  auto next = state.catalogs;
  if (!catalog::Install(next, entry))
    return false;
  state.catalogHistory.undo.push_back({state.catalogs, NextActionStamp()});
  state.catalogHistory.redo.clear();
  state.catalogs = std::move(next);
  state.projectMetaDirty = true;
  return true;
}
bool UndoCatalog(AppState& state) {
  auto& history = state.catalogHistory;
  if (history.undo.empty())
    return false;
  auto entry = std::move(history.undo.back());
  history.undo.pop_back();
  history.redo.push_back({state.catalogs, entry.stamp});
  state.catalogs = std::move(entry.entries);
  state.projectMetaDirty = true;
  return true;
}
bool RedoCatalog(AppState& state) {
  auto& history = state.catalogHistory;
  if (history.redo.empty())
    return false;
  auto entry = std::move(history.redo.back());
  history.redo.pop_back();
  history.undo.push_back({state.catalogs, entry.stamp});
  state.catalogs = std::move(entry.entries);
  state.projectMetaDirty = true;
  return true;
}
} // namespace ui
