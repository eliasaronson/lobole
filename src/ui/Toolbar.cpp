#include "ui/Toolbar.h"

#include "ui/AppActions.h"
#include "ui/PreviewFeature.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace ui {

namespace {

// An icon button with a tooltip. Returns true when clicked.
bool ToolButton(const char* icon, const char* tooltip, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::Button(icon);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", tooltip);
  ImGui::SameLine();
  return clicked;
}

void Divider() {
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
}

} // namespace

void DrawToolbar(AppState& state) {
  const bool hasDoc = state.document != nullptr;
  const bool idle = !HasPendingFeature(state); // no creation while previewing

  if (ToolButton(ICON_FA_PENCIL, "New Sketch", hasDoc && idle))
    StartNewSketch(state);

  Divider();

  if (ToolButton(ICON_FA_CUBE, "Box", hasDoc && idle))
    CreateBox(state);
  if (ToolButton(ICON_FA_CIRCLE, "Sphere", hasDoc && idle))
    CreateSphere(state);
  if (ToolButton(ICON_FA_DATABASE, "Cylinder", hasDoc && idle))
    CreateCylinder(state);

  Divider();

  if (ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Fit view", hasDoc))
    FitCameraToDocument(state);
  if (ToolButton(ICON_FA_TABLE_CELLS, "Toggle planes", hasDoc))
    state.showPlanes = !state.showPlanes;

  // The row ended each button with SameLine(); finish it.
  ImGui::NewLine();
}

} // namespace ui
