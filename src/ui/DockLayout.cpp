#include "ui/DockLayout.h"

#include <algorithm>

#include <cstring>

#include "ui/Toolbar.h"

#include <glm/glm.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API

namespace ui {

float PartTabsBarHeight() { return ImGui::GetFrameHeightWithSpacing() + 4.0f; }

void BeginDockspace(AppState& state) {
  ImGuiViewport* vp = ImGui::GetMainViewport();

  // Fullscreen, transparent host window covering the work area (below the
  // main menu bar, above the part-tabs bar — that strip is reserved, not
  // overlaid, so a docked panel's bottom stays reachable). It holds the
  // toolbar row and the dockspace so the toolbar sits above all docked
  // panels.
  const ImVec2 hostSize(vp->WorkSize.x, vp->WorkSize.y - PartTabsBarHeight());
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(hostSize);
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
  // The row WRAPS rather than scrolling (see Toolbar.cpp WrapIfNeeded), so
  // its height depends on how many rows the current width needs. Measured
  // from the previous frame, like the scrollbar allowance it replaces: the
  // height has to be chosen before the content is laid out.
  static float toolbarRowsH = 0.0f;
  const float oneRow = ImGui::GetFrameHeight() + 12.0f;
  const float toolbarH = std::max(oneRow, toolbarRowsH);
  ImGui::BeginChild("##ToolbarRow", ImVec2(0.0f, toolbarH),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  DrawToolbar(state);
  toolbarRowsH = ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.5f;
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
        redock("History", model->DockId);
        redock("Conflicts", model->DockId);
        redock("CAM###CamPanel", model->DockId);
      }
    }
  }

  // Turning the terminal on mid-session: its window has no dock entry in any
  // saved layout, so without this it floats over the viewport forever (which
  // is exactly how the Configurations panel shipped once). Split a right node
  // out of the dockspace and put it there; the DockId check makes this fire
  // once, not every frame.
  if (state.settings.terminalPanel && !state.buildDefaultDockLayout) {
    // The FULL name: ImGui hashes from the "###", so looking up "Terminal"
    // finds nothing at all — silently, which is how this first shipped
    // floating over the model tree.
    ImGuiWindow* term = ImGui::FindWindowByName("Terminal###TerminalPanel");
    if (term != nullptr && term->DockId == 0) {
      ImGuiID right = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right,
                                                  0.28f, nullptr, nullptr);
      ImGui::DockBuilderFinish(dockspaceId);
      // SetWindowDock, not DockBuilderDockWindow: the latter only takes
      // effect for a window that has not been created yet, and by the time
      // the preference is flipped this one has been drawing for a frame.
      ImGui::SetWindowDock(term, right, ImGuiCond_Always);
    }
  }

  if (state.buildDefaultDockLayout) {
    state.buildDefaultDockLayout = false;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f)
      avail = hostSize; // first frame: content region not yet sized
    ImGui::DockBuilderSetNodeSize(dockspaceId, avail);
    ImGuiID center = dockspaceId;
    // U7e: one docked side panel — the right node is gone (feature editing
    // and every tool session live in the floating Tool window), so the
    // viewport gets everything right of the tree.
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f,
                                               nullptr, &center);
    // U7e removed the right node; the terminal panel brings one back, but
    // only when it is enabled — an empty right column would otherwise eat
    // viewport width for a feature the user turned off.
    if (state.settings.terminalPanel) {
      ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f,
                                                  nullptr, &center);
      ImGui::DockBuilderDockWindow("Terminal###TerminalPanel", right);
    }
    ImGui::DockBuilderDockWindow("Model", left);
    ImGui::DockBuilderDockWindow("Variables", left);      // tabbed behind Model
    ImGui::DockBuilderDockWindow("Configurations", left); // tabbed behind Model
    ImGui::DockBuilderDockWindow("History", left);        // tabbed behind Model
    // Only ever exists when there is something to resolve (F7), so it is
    // not a permanent tab — but when it appears it belongs here.
    ImGui::DockBuilderDockWindow("Conflicts", left);
    ImGui::DockBuilderDockWindow("CAM###CamPanel", left); // ditto
    // A window this rebuild did NOT give a home must forget its old one.
    // The terminal is docked only when its preference is on, so rebuilding
    // with it off deletes its node while the window keeps pointing at the
    // dead id — and ImGui then re-homes it into the CENTRAL node, where it
    // covers the entire 3D viewport. Measured: with the layout rebuilt while
    // the terminal was off, its rect came back exactly equal to the viewport
    // rect, so a click meant for the model landed in the terminal.
    //
    // Only the terminal is docked conditionally today; if another window ever
    // is, it needs the same treatment.
    if (!state.settings.terminalPanel)
      if (ImGuiWindow* term =
              ImGui::FindWindowByName("Terminal###TerminalPanel"))
        if (term->DockId != 0)
          ImGui::SetWindowDock(term, 0, ImGuiCond_Always);
    ImGui::DockBuilderFinish(dockspaceId);
    // The tab focused last wins the shared node; make sure that is Model —
    // with Sketch tabbed here too (U7e), it could otherwise end up on top
    // and hide the tree from both the user and the gui tests.
    ImGui::SetWindowFocus("Model");
  }

  ImGui::End(); // host window

  // A panel asked to come forward. Repeated, because one SetWindowFocus on a
  // shared dock node does not reliably win the tab.
  if (state.focusPanelFrames > 0 && !state.focusPanel.empty()) {
    // By the name a person would say. ImGui matches the WHOLE window string,
    // "###" id and all, so asking for "CAM" would silently do nothing —
    // resolve it to "CAM###CamPanel" by the label the tab shows.
    const char* target = state.focusPanel.c_str();
    if (ImGui::FindWindowByName(target) == nullptr) {
      const ImGuiContext& g = *ImGui::GetCurrentContext();
      for (ImGuiWindow* w : g.Windows) {
        const char* hash = std::strstr(w->Name, "###");
        const size_t label = hash != nullptr
                                 ? static_cast<size_t>(hash - w->Name)
                                 : std::strlen(w->Name);
        if (label == state.focusPanel.size() &&
            std::strncmp(w->Name, target, label) == 0) {
          target = w->Name;
          break;
        }
      }
    }
    ImGui::SetWindowFocus(target);
    state.focusPanelFrames--;
  }

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
