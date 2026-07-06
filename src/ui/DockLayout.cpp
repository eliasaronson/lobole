#include "ui/DockLayout.h"

#include <glm/glm.hpp>

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

  // Track the central node's rect (relative to the main viewport) so the 3D
  // scene renders in the visible region between the panels, not the whole
  // window. Falls back to the full work area before the layout exists.
  ImGuiViewport* vp = ImGui::GetMainViewport();
  if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
    state.viewportPos = glm::vec2(central->Pos.x - vp->Pos.x,
                                  central->Pos.y - vp->Pos.y);
    state.viewportSize = glm::vec2(central->Size.x, central->Size.y);
  } else {
    state.viewportPos = glm::vec2(0.0f, 0.0f);
    state.viewportSize = glm::vec2(vp->WorkSize.x, vp->WorkSize.y);
  }
}

} // namespace ui
