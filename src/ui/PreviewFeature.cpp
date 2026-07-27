#include "ui/PreviewFeature.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"              // occurrence names (measure rows)
#include "core/drawing/DrawingDocument.h" // DrawingDimKind (dim session)
#include "ui/AppActions.h"                // SelectFeature
#include "ui/FeatureEditor.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace ui {

uint64_t BeginPreviewFeature(AppState& state,
                             std::unique_ptr<Feature> feature) {
  // Added directly (not via a command) so it regenerates + renders while the
  // user decides; committed to the undo stack only on accept.
  state.document->AddFeature(std::move(feature));
  const size_t idx = state.document->FeatureCount() - 1;
  Feature* added = state.document->GetFeature(idx);
  state.pendingFeatureId = added->GetId();
  // Select-first (U7b): whatever was selected seeds the new tool's slots
  // before SelectFeature replaces the selection with the feature itself;
  // then the first unfilled required slot arms for the next viewport click
  // (nothing arms when the seed satisfied everything — Enter commits).
  SeedSlotsFromSelection(state, *added);
  SelectFeature(state, added->GetId());
  ArmNextUnfilledSlot(state, *added);
  return state.pendingFeatureId;
}

bool HasPendingFeature(const AppState& state) {
  return state.pendingFeatureId != 0;
}

Feature* ResolvePendingFeature(AppState& state, size_t* outIndex) {
  if (state.pendingFeatureId == 0 || !state.document)
    return nullptr;
  return state.document->FindFeatureById(state.pendingFeatureId, outIndex);
}

void AcceptPendingFeature(AppState& state) {
  size_t idx = 0;
  Feature* f = ResolvePendingFeature(state, &idx);
  if (!f) {
    state.pendingFeatureId = 0;
    return;
  }
  // Remove (id preserved) then re-add through a command, so accept is a single
  // undoable action and downstream id references stay valid.
  const uint64_t id = f->GetId();
  std::unique_ptr<Feature> owned = state.document->RemoveFeature(idx);
  state.pendingFeatureId = 0;
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(owned)),
                          *state.document);
  SelectFeature(state, id);
  state.slotPick = {}; // armed slots end with the session
}

void CancelPendingFeature(AppState& state) {
  size_t idx = 0;
  if (ResolvePendingFeature(state, &idx)) {
    state.document->RemoveFeature(idx);
  }
  state.pendingFeatureId = 0;
  state.selection.Clear();
  state.slotPick = {};
}

void BeginToolWindowFrame(AppState& state, const std::string& title) {
  // The floating tool-session window (U7): anchored top-right of the 3D
  // viewport when it (re)opens — NEVER bottom-center, which occluded the
  // viewport clicks GUI tests make — and draggable for the session. One
  // stable id: only one session exists at a time, and tests address rows
  // as //<Title>###ToolWindow/###slotN.
  const glm::vec2 tr = state.viewportPos +
                       glm::vec2(state.viewportSize.x, 0.0f) +
                       glm::vec2(-12.0f, 12.0f);
  ImGui::SetNextWindowPos(ImVec2(tr.x, tr.y), ImGuiCond_Appearing,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin((title + "###ToolWindow").c_str(), nullptr, flags);
}

namespace {

// Measure session (U7d): the picked-element list + readout, previously a
// Properties/Occurrence panel section. Rows keep the per-item "x".
void DrawMeasureToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Measure");
  ImGui::TextDisabled("Pick faces/edges - click again to deselect. Esc exits.");
  for (size_t i = 0; i < state.measure.picked.size(); i++) {
    const auto& e = state.measure.picked[i];
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      state.measure.picked.erase(state.measure.picked.begin() +
                                 static_cast<long>(i));
      UpdateMeasureReadout(state);
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    std::string where;
    if (e.occurrenceId != 0 && state.assembly)
      if (const Occurrence* occ = state.assembly->FindById(e.occurrenceId))
        where = occ->name + " / ";
    ImGui::TextWrapped("%s%s %s", where.c_str(), e.isEdge ? "Edge" : "Face",
                       e.ref.name.c_str());
    ImGui::PopID();
  }
  if (!state.measure.readout.empty())
    ImGui::TextWrapped("%s", state.measure.readout.c_str());
  ImGui::Separator();
  if (ImGui::Button("Done"))
    state.measure = MeasureState{};
  ImGui::End();
}

// Drawing-dimension session (U7d): armed kind, the refs picked so far (each
// removable — was accumulate-only), cancel. The 2D pick loop itself stays in
// HandleDimTool (sheet space; the 3D pipeline doesn't run there).
void DrawDimToolWindow(AppState& state) {
  auto& tool = state.dimTool;
  const int need = (tool.kind == DrawingDimKind::Radius ||
                    tool.kind == DrawingDimKind::Diameter)
                       ? 1
                       : 2;
  BeginToolWindowFrame(state, DrawingDimKindToString(tool.kind));
  ImGui::TextDisabled("Pick edge %d of %d in one view  (Esc cancels)",
                      static_cast<int>(tool.refs.size()) + 1, need);
  for (size_t i = 0; i < tool.refs.size(); i++) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      tool.refs.erase(tool.refs.begin() + static_cast<long>(i));
      if (tool.refs.empty())
        tool.viewId = 0; // no refs left: the next pick may fix any view
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    const ProjRef& r = tool.refs[i];
    ImGui::TextUnformatted(r.edge.IsSet() ? r.edge.name.c_str()
                                          : r.silFace.name.c_str());
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::Button("Cancel"))
    tool = AppState::DimTool{};
  ImGui::End();
}

} // namespace

void DrawToolWindow(AppState& state) {
  // Mate sessions (assembly tabs) share the window; drawn by the panel
  // module that owns the mate helpers.
  if (state.assembly && state.mateTool.active) {
    DrawMateToolWindow(state);
    return;
  }
  if (state.drawing && state.dimTool.active) {
    DrawDimToolWindow(state);
    return;
  }
  size_t idx = 0;
  Feature* feature = ResolvePendingFeature(state, &idx);
  if (!feature) {
    state.pendingFeatureId = 0;
    if (state.measure.active)
      DrawMeasureToolWindow(state); // measure never coexists with a preview
    return;
  }

  BeginToolWindowFrame(state, feature->TypeName());

  DrawFeatureParamWidgets(state, *feature, idx, /*commit=*/false);

  ImGui::Separator();
  bool accept = false, cancel = false;
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.25f, 0.65f, 0.30f, 1.0f));
  if (ImGui::Button(ICON_FA_CHECK " Accept"))
    accept = true;
  ImGui::PopStyleColor(2);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.22f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.72f, 0.27f, 0.27f, 1.0f));
  if (ImGui::Button(ICON_FA_XMARK " Cancel"))
    cancel = true;
  ImGui::PopStyleColor(2);

  // Enter accepts, Escape cancels (when not typing in a field). While a
  // handle drag is live, Escape belongs to the drag (aborts it, preview
  // survives — ViewportInput's ladder). The SAME physical keypress that
  // confirms/cancels a value field must not also fire here: the field
  // deactivates that frame, so require a full idle frame in between, and
  // ignore key REPEAT (a held Enter from the field would re-fire otherwise).
  static int idleFrames = 0;
  idleFrames = ImGui::IsAnyItemActive() ? 0 : idleFrames + 1;
  if (idleFrames > 1 && !state.handleDrag.active) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false))
      accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false))
      cancel = true;
  }

  ImGui::End();

  if (accept)
    AcceptPendingFeature(state);
  else if (cancel)
    CancelPendingFeature(state);
}

} // namespace ui
