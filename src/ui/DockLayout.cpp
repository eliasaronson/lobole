#include "ui/DockLayout.h"

#include "ui/Toolbar.h"

#include <glm/glm.hpp>

#include <cmath>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API

namespace ui {

void BeginDockspace(AppState& state) {
  ImGuiViewport* vp = ImGui::GetMainViewport();

  // Fullscreen, transparent host window covering the work area (below the
  // main menu bar). It holds the toolbar row and the dockspace so the toolbar
  // sits above all docked panels.
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  const ImGuiWindowFlags hostFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##DockHost", nullptr, hostFlags);
  ImGui::PopStyleVar(3);

  // Toolbar row (padded child so the buttons aren't flush to the edge)
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
  const float toolbarH = ImGui::GetFrameHeight() + 12.0f;
  ImGui::BeginChild("##ToolbarRow", ImVec2(0.0f, toolbarH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar);
  DrawToolbar(state);
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Dockspace filling the rest; passthru central node shows the 3D behind it.
  ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
  const ImVec2 dockAvail = ImGui::GetContentRegionAvail();
  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                   ImGuiDockNodeFlags_PassthruCentralNode);

  // Re-fit the root node to the current region whenever the window resizes, so
  // the side panels track the window edges (e.g. going fullscreen) instead of
  // keeping their old size and leaving a gap at the right/bottom. Children
  // rescale proportionally.
  if (!state.buildDefaultDockLayout && dockAvail.x > 1.0f && dockAvail.y > 1.0f &&
      (std::abs(dockAvail.x - state.lastDockSize.x) > 0.5f ||
       std::abs(dockAvail.y - state.lastDockSize.y) > 0.5f)) {
    ImGui::DockBuilderSetNodeSize(dockspaceId, dockAvail);
    state.lastDockSize = glm::vec2(dockAvail.x, dockAvail.y);
  }

  if (state.buildDefaultDockLayout) {
    state.buildDefaultDockLayout = false;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f)
      avail = vp->WorkSize; // first frame: content region not yet sized
    ImGui::DockBuilderSetNodeSize(dockspaceId, avail);
    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f,
                                               nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f,
                                                nullptr, &center);
    ImGui::DockBuilderDockWindow("Model", left);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderFinish(dockspaceId);
    state.lastDockSize = glm::vec2(avail.x, avail.y);
  }

  ImGui::End(); // host window

  // Track the central node rect (relative to the main viewport) so the 3D
  // scene renders in the visible region between the panels.
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
