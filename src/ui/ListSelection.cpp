// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ListSelection.h"

#include "core/Command.h"
#include "core/fem/SimDocument.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPanel.h"
#include "ui/DocumentSlots.h" // JobUndoGuard
#include "ui/SelectionPipeline.h"
#include "ui/SimTree.h" // SimRowKey, GeometryRowKey

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
  if (state.job) {
    // Every row kind the CAM panel lists, in ONE undo step (the Remove*
    // functions' own guards nest inside this one). Operations first, so a
    // tool they were the last users of is free to go in the same Delete;
    // setups last, since their fixtures and tools are inside them.
    JobUndoGuard undo(state);
    int removed = RemoveCamOperations(
        state, SelectedIds(state, SelectionItem::Kind::Operation));
    std::string refused;
    removed += RemoveCamTools(
        state, SelectedIds(state, SelectionItem::Kind::CamTool), &refused);
    removed += RemoveCamFixtures(
        state, SelectedIds(state, SelectionItem::Kind::Fixture));
    removed +=
        RemoveJobParts(state, SelectedIds(state, SelectionItem::Kind::JobPart));
    removed += RemoveCamSetups(
        state, SelectedIds(state, SelectionItem::Kind::CamSetup));
    // A tool still cut with stays, and says why where Delete was pressed.
    if (!refused.empty())
      state.camRefusal = refused;
    return removed;
  }
  if (state.sim && state.simulation.IsActive()) {
    // The simulation tree's studies and loads, in ONE undo step: a load of
    // a study that is going goes with it.
    using K = SelectionItem::Kind;
    const std::vector<uint64_t> loads = SelectedIds(state, K::SimLoad);
    const std::vector<uint64_t> studies = SelectedIds(state, K::SimStudy);
    // The rows with no id of their own: a load case, a kept face and a
    // keep-out by their study and place, a Geometry row by its kind and
    // place (SimRowKey, GeometryRowKey).
    const std::vector<uint64_t> cases = SelectedIds(state, K::SimCase);
    const std::vector<uint64_t> keeps = SelectedIds(state, K::SimKeep);
    const std::vector<uint64_t> keepOuts = SelectedIds(state, K::SimKeepOut);
    const std::vector<uint64_t> geometry = SelectedIds(state, K::SimGeometry);
    if (loads.empty() && studies.empty() && cases.empty() && keeps.empty() &&
        keepOuts.empty() && geometry.empty())
      return 0;
    const auto picked = [](const std::vector<uint64_t>& keys, uint64_t study,
                           size_t index) {
      return std::find(keys.begin(), keys.end(), SimRowKey(study, index)) !=
             keys.end();
    };
    int removed = 0;
    {
      SimUndoGuard guard(state);
      for (const fem::Study& s :
           std::vector<fem::Study>(state.sim->GetStudies().begin(),
                                   state.sim->GetStudies().end())) {
        fem::Study edited = s;
        int gone = static_cast<int>(
            std::erase_if(edited.loads, [&](const fem::Load& l) {
              return std::find(loads.begin(), loads.end(), l.id) != loads.end();
            }));
        // A load case goes when no load is left in it, as the case
        // window's Remove (its loads selected with it go first): a load is
        // never moved into another case behind the person's back.
        std::vector<fem::LoadCase> keptCases;
        for (size_t i = 0; i < edited.loadCases.size(); ++i) {
          const std::string& name = edited.loadCases[i].name;
          const bool holds = std::any_of(
              edited.loads.begin(), edited.loads.end(),
              [&](const fem::Load& l) { return l.loadCase == name; });
          if (picked(cases, s.id, i) && !holds)
            ++gone;
          else
            keptCases.push_back(edited.loadCases[i]);
        }
        edited.loadCases = std::move(keptCases);
        std::vector<ElementRef> keptFaces;
        for (size_t i = 0; i < edited.keepFaces.size(); ++i)
          if (picked(keeps, s.id, i))
            ++gone;
          else
            keptFaces.push_back(edited.keepFaces[i]);
        edited.keepFaces = std::move(keptFaces);
        std::vector<fem::KeepOut> keptOuts;
        for (size_t i = 0; i < edited.keepOuts.size(); ++i)
          if (picked(keepOuts, s.id, i))
            ++gone;
          else
            keptOuts.push_back(edited.keepOuts[i]);
        edited.keepOuts = std::move(keptOuts);
        if (gone == 0)
          continue;
        removed += gone;
        state.sim->ReplaceStudy(std::move(edited));
      }
      for (const uint64_t id : studies)
        if (state.sim->RemoveStudy(id))
          removed++;
      if (!geometry.empty()) {
        const auto has = [&](GeometryRow row, size_t i) {
          return std::find(geometry.begin(), geometry.end(),
                           GeometryRowKey(row, i)) != geometry.end();
        };
        // Putting a feature back or removing the cut never breaks a plan:
        // no refusal to ask for.
        fem::SimGeometry g = state.sim->Geometry();
        std::vector<fem::SuppressedFeature> still;
        for (size_t i = 0; i < g.suppressed.size(); ++i)
          if (has(GeometryRow::Suppressed, i))
            removed++;
          else
            still.push_back(g.suppressed[i]);
        g.suppressed = std::move(still);
        if (g.symmetry.On() && has(GeometryRow::Symmetry, 0)) {
          g.symmetry = {};
          removed++;
        }
        if (!(g == state.sim->Geometry()))
          state.sim->SetGeometry(std::move(g));
        fem::SimMesh mesh = state.sim->Mesh();
        std::vector<fem::Refinement> refs;
        for (size_t i = 0; i < mesh.refinements.size(); ++i)
          if (has(GeometryRow::Refinement, i))
            removed++;
          else
            refs.push_back(mesh.refinements[i]);
        if (refs.size() != mesh.refinements.size()) {
          mesh.refinements = std::move(refs);
          state.sim->SetMesh(std::move(mesh));
        }
      }
    }
    for (const K k : {K::SimLoad, K::SimStudy, K::SimCase, K::SimKeep,
                      K::SimKeepOut, K::SimGeometry})
      EraseKind(state.selection, k);
    SimulationState& sim = state.simulation;
    if (state.sim->FindStudy(sim.activeStudyId) == nullptr)
      sim.activeStudyId = state.sim->GetStudies().empty()
                              ? 0
                              : state.sim->GetStudies().front().id;
    // What the tool window showed may be what went.
    sim.focus = {};
    sim.selectedLoadId = 0;
    return removed;
  }
  return 0;
}

} // namespace ui
