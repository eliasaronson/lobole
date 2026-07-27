#include "ui/DockLayout.h"

#include "ui/Toolbar.h"

#include <glm/glm.hpp>

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

  // Toolbar row (padded child so the buttons aren't flush to the edge). Give it
  // an opaque background so it reads as a solid bar continuous with the menu
  // bar, rather than icons floating over the transparent viewport.
  ImVec4 barBg = ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg);
  barBg.w = 1.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, barBg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
  // When the window is narrower than the button row, scroll instead of
  // clipping (U4); the row grows by the scrollbar's height while overflowed
  // so the bar never overlays the buttons.
  static bool toolbarOverflow = false;
  const float toolbarH =
      ImGui::GetFrameHeight() + 12.0f +
      (toolbarOverflow ? ImGui::GetStyle().ScrollbarSize : 0.0f);
  ImGui::BeginChild("##ToolbarRow", ImVec2(0.0f, toolbarH),
                    ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  DrawToolbar(state);
  toolbarOverflow = ImGui::GetScrollMaxX() > 0.0f;
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Dockspace filling the rest; passthru central node shows the 3D behind it.
  // V2 (U8-0): versioned id — an imgui.ini written for the old layout has
  // no entry for this id, so the default layout rebuilds ONCE and stale
  // nodes (e.g. the deleted right panel node, which the Sketch window kept
  // resurrecting from its remembered dock id) are orphaned and dropped.
  ImGuiID dockspaceId = ImGui::GetID("MainDockSpaceV2");
  ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                   ImGuiDockNodeFlags_PassthruCentralNode);

  // Self-heal a missing or stale layout. If the dockspace root has no split
  // children, the Model panel isn't docked into it (a fresh run, or a
  // leftover imgui.ini from an older layout that left it floating). In
  // that state the panels can't track the window edges, so rebuild the default
  // layout. Once docked, the root has splits and this won't fire again. With
  // the panels docked, ImGui keeps the side panel a fixed width against its
  // edge and grows the central node on resize — no manual re-fit needed.
  if (!state.buildDefaultDockLayout) {
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (root == nullptr || root->IsLeafNode())
      state.buildDefaultDockLayout = true;
  }

  // A layout can be present-but-incomplete: an imgui.ini saved before a panel
  // existed has no dock entry for it, and DockBuilderDockWindow only applies
  // on a rebuild — so the new panel floats over the viewport forever (this is
  // how Configurations shipped floating). Once per launch, after the panel
  // windows have been created from settings, re-dock strays: secondary panels
  // join the node their sibling lives in; if the *core* Model panel is
  // adrift, rebuild the whole default layout. Once-per-launch so users can
  // still float panels freely within a session.
  if (!state.dockHomesEnforced && !state.buildDefaultDockLayout) {
    ImGuiWindow* model = ImGui::FindWindowByName("Model");
    if (model != nullptr) {
      state.dockHomesEnforced = true;
      if (model->DockId == 0) {
        state.buildDefaultDockLayout = true;
      } else {
        auto redock = [](const char* name, ImGuiID home) {
          ImGuiWindow* w = ImGui::FindWindowByName(name);
          if (w != nullptr && w->DockId == 0)
            ImGui::SetWindowDock(w, home, ImGuiCond_Always);
        };
        redock("Variables", model->DockId);
        redock("Configurations", model->DockId);
      }
    }
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
    // U7e: one docked side panel — the right node is gone (feature editing
    // and every tool session live in the floating Tool window), so the
    // viewport gets everything right of the tree.
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f,
                                               nullptr, &center);
    ImGui::DockBuilderDockWindow("Model", left);
    ImGui::DockBuilderDockWindow("Variables", left);      // tabbed behind Model
    ImGui::DockBuilderDockWindow("Configurations", left); // tabbed behind Model
    ImGui::DockBuilderFinish(dockspaceId);
    // The tab focused last wins the shared node; make sure that is Model —
    // with Sketch tabbed here too (U7e), it could otherwise end up on top
    // and hide the tree from both the user and the gui tests.
    ImGui::SetWindowFocus("Model");
  }

  ImGui::End(); // host window

  // Track the central node rect (relative to the main viewport) so the 3D
  // scene renders in the visible region between the panels.
  if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
    state.viewportPos =
        glm::vec2(central->Pos.x - vp->Pos.x, central->Pos.y - vp->Pos.y);
    state.viewportSize = glm::vec2(central->Size.x, central->Size.y);
  } else {
    state.viewportPos = glm::vec2(0.0f, 0.0f);
    state.viewportSize = glm::vec2(vp->WorkSize.x, vp->WorkSize.y);
  }
}

} // namespace ui
