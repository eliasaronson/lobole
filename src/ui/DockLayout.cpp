#include "ui/DockLayout.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API

namespace ui {

void BeginDockspace(AppState& state) {
  ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(
      0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

  if (state.buildDefaultDockLayout) {
    state.buildDefaultDockLayout = false;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId,
                                  ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f,
                                               nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f,
                                                nullptr, &center);
    ImGui::DockBuilderDockWindow("Model", left);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderFinish(dockspaceId);
  }
}

} // namespace ui
