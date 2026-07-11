#include "ui/PreviewFeature.h"

#include "core/Command.h"
#include "core/Document.h"
#include "ui/FeatureEditor.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace ui {

uint64_t BeginPreviewFeature(AppState& state, std::unique_ptr<Feature> feature) {
  // Added directly (not via a command) so it regenerates + renders while the
  // user decides; committed to the undo stack only on accept.
  state.document->AddFeature(std::move(feature));
  const size_t idx = state.document->FeatureCount() - 1;
  Feature* added = state.document->GetFeature(idx);
  state.pendingFeatureId = added->GetId();
  state.selectedFeature = static_cast<int>(idx);
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
  std::unique_ptr<Feature> owned = state.document->RemoveFeature(idx);
  state.pendingFeatureId = 0;
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(owned)),
                          *state.document);
  state.selectedFeature = static_cast<int>(state.document->FeatureCount() - 1);
  state.edgePick = {}; // a blend preview's edge picking ends with it
}

void CancelPendingFeature(AppState& state) {
  size_t idx = 0;
  if (ResolvePendingFeature(state, &idx)) {
    state.document->RemoveFeature(idx);
  }
  state.pendingFeatureId = 0;
  state.selectedFeature = -1;
  state.edgePick = {};
}

void DrawPreviewDialog(AppState& state) {
  size_t idx = 0;
  Feature* feature = ResolvePendingFeature(state, &idx);
  if (!feature) {
    state.pendingFeatureId = 0;
    return;
  }

  // Floating panel anchored bottom-center of the viewport region.
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float cx = vp->WorkPos.x + vp->WorkSize.x * 0.5f;
  const float by = vp->WorkPos.y + vp->WorkSize.y - 24.0f;
  ImGui::SetNextWindowPos(ImVec2(cx, by), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin((std::string(feature->TypeName()) + "###PreviewDialog").c_str(),
               nullptr, flags);

  DrawFeatureParamWidgets(state, *feature, idx, /*commit=*/false);

  ImGui::Separator();
  bool accept = false, cancel = false;
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.30f, 1.0f));
  if (ImGui::Button(ICON_FA_CHECK " Accept"))
    accept = true;
  ImGui::PopStyleColor(2);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.22f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.27f, 0.27f, 1.0f));
  if (ImGui::Button(ICON_FA_XMARK " Cancel"))
    cancel = true;
  ImGui::PopStyleColor(2);

  // Enter accepts, Escape cancels (when not typing in a field)
  if (!ImGui::IsAnyItemActive()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
      accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      cancel = true;
  }

  ImGui::End();

  if (accept)
    AcceptPendingFeature(state);
  else if (cancel)
    CancelPendingFeature(state);
}

} // namespace ui
