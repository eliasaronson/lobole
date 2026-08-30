#include "ui/DockLayout.h"
#include "ui/PanelTitles.h"

#include "common/Logger.h"

#include <algorithm>

#include <cstring>
#include <unordered_map>

#include "ui/Toolbar.h"

#include <glm/glm.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API

namespace ui {

namespace {
// Set by DrawPartTabs at the end of each frame; see the header.
bool s_PartTabsOverflow = false;
} // namespace

void SetPartTabsOverflowing(bool overflowing) {
  s_PartTabsOverflow = overflowing;
}

bool PartTabsOverflowing() { return s_PartTabsOverflow; }

float PartTabsBarPaddingY() { return ImGui::GetStyle().FramePadding.y; }

float DefaultSidePanelWidth(float dockWidth) {
  // A share of the window, but bounded at both ends — a bare fraction is
  // wrong on any screen far from the one it was chosen on. At 0.17 a 1280 px
  // window gives 218 px, which is what forced the dock tabs to go icon-only;
  // a 3200 px one gives 543 px of mostly empty tree. The bounds are in FONT
  // units so they follow the text they have to hold, not the pixels.
  const float em = ImGui::GetFontSize();
  const float want = dockWidth * kSidePanelFraction;
  return std::clamp(want, em * 15.0f, em * 24.0f);
}

namespace {

// The word for a panel title, or nullptr if it is not one of ours.
const char* PanelNameFor(const char* title) {
  for (const PanelInfo& p : kPanels)
    if (std::strcmp(p.title, title) == 0)
      return p.name;
  return nullptr;
}

// Name the hovered dock tab. This ImGui draws no tooltip for tab items at
// all, so icon-only tabs would otherwise be unlabelled everywhere — and
// there is no per-window API for it either, the tab belonging to the dock
// node rather than to the window. Hence the walk: find each panel's tab in
// its node's bar and hit-test it. Offset is relative to the bar and the bar
// scrolls, so ScrollingAnim comes off it.
void DrawPanelTabTooltips() {
  for (const PanelInfo& p : kPanels) {
    ImGuiWindow* w = ImGui::FindWindowByName(p.title);
    if (w == nullptr || w->DockNode == nullptr ||
        w->DockNode->TabBar == nullptr)
      continue;
    const ImGuiTabBar* bar = w->DockNode->TabBar;
    for (const ImGuiTabItem& tab : bar->Tabs) {
      if (tab.Window != w)
        continue;
      const float x = bar->BarRect.Min.x + tab.Offset - bar->ScrollingAnim;
      const ImRect r(x, bar->BarRect.Min.y, x + tab.Width, bar->BarRect.Max.y);
      if (r.Overlaps(bar->BarRect) &&
          ImGui::IsMouseHoveringRect(r.Min, r.Max, false))
        ImGui::SetTooltip("%s", p.name);
      break;
    }
  }
}

} // namespace

void PanelHeading(const char* panelTitle) {
  const char* name = PanelNameFor(panelTitle);
  if (name == nullptr)
    return;
  // Once per window per frame, whoever gets there first. The Model panel is
  // Begun by four different files in one frame — the feature tree, the
  // occurrence list, the study panel and the drawing panel, depending on the
  // document kind — and ImGui APPENDS to a window rather than restarting it,
  // so an unguarded heading would draw once for each of them. All four call
  // this; only the first does anything.
  static std::unordered_map<ImGuiID, int> s_drawnOn;
  int& frame = s_drawnOn[ImGui::GetCurrentWindow()->ID];
  if (frame == ImGui::GetFrameCount())
    return;
  frame = ImGui::GetFrameCount();
  // Icon then word, in the ordinary text colour, over a hairline — a title,
  // not another SeparatorText section (the panels below open with one of
  // those, and two stacked rules read as an empty section).
  //
  // The glyph is taken by POINTER RANGE, not by handing the title to
  // TextUnformatted: "###" is honoured in widget *labels*, and raw text is
  // drawn verbatim — the first cut of this rendered "###Model Model".
  const char* hash = std::strstr(panelTitle, "###");
  ImGui::TextUnformatted(panelTitle, hash);
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::TextUnformatted(name);
  ImGui::Separator();
}

// Derived from what the bar has to HOLD, not from a spacing constant that
// happened to look right. The old `GetFrameHeightWithSpacing() + 4` came out
// at the same 36 px at 1x, but by accident: it left a content region SHORTER
// than one button, and the tabs drew past it. Harmless while the strip was
// bare buttons; not once the strip is a child that also has to fit a
// scrollbar, which then had nowhere to go and was clipped away — the bar grew
// by 12 px and showed nothing for it.
float PartTabsBarHeight() {
  const float h = PartTabsBarPaddingY() * 2.0f + ImGui::GetFrameHeight();
  return s_PartTabsOverflow ? h + ImGui::GetStyle().ScrollbarSize : h;
}

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

  // Import progress. Here, at host-window level, because it must outlive the
  // toolbar branch it would otherwise be opened from — materializing an
  // import switches the active tab from part to assembly halfway through.
  DrawImportModal(state);

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
    ImGuiWindow* model = ImGui::FindWindowByName(kPanelModel);
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
        redock(kPanelVariables, model->DockId);
        redock(kPanelConfigurations, model->DockId);
        redock(kPanelHistory, model->DockId);
        redock(kPanelConflicts, model->DockId);
        redock(kPanelCam, model->DockId);
      }
    }
  }

  // A side panel wide enough to crowd the viewport out, inherited from some
  // other window size. Once per launch, and only past a share nobody picks
  // on purpose.
  //
  // ImGui gives a panel sitting beside a CENTRAL node its ABSOLUTE width and
  // lets the central node absorb every resize — imgui.cpp
  // DockNodeTreeUpdatePosSize, branch 3, "use explicit size from the other
  // and remainder for the central node". That is right for a side panel and
  // wrong across sessions: a width chosen on one window is the same pixel
  // count on every window that opens afterwards, forever. Measured on a real
  // imgui.ini: the same file gave a 757 px tree at 1999 px wide AND at 3196
  // px wide — 38% of the first window, 24% of the second, and it never
  // shrinks back on its own. Nothing the user did was wrong; there is simply
  // no path by which the layout recovers.
  //
  // A launch check rather than a clamp every frame: dragging the splitter
  // wider for a session has to keep working.
  if (!state.sidePanelWidthChecked && !state.buildDefaultDockLayout) {
    ImGuiWindow* model = ImGui::FindWindowByName(kPanelModel);
    ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (model != nullptr && model->DockNode != nullptr && root != nullptr &&
        root->Size.x > 1.0f) {
      state.sidePanelWidthChecked = true;
      ImGuiDockNode* node = model->DockNode;
      if (node->Size.x > root->Size.x * kMaxSidePanelFraction) {
        const float want = DefaultSidePanelWidth(root->Size.x);
        LOG_INFO("Dock: the model panel was {:.0f} px of a {:.0f} px window; "
                 "narrowing it to {:.0f}",
                 node->Size.x, root->Size.x, want);
        // SizeRef is what branch 3 reads; Size follows on the next update.
        node->SizeRef.x = want;
        node->Size.x = want;
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
    ImGuiWindow* term = ImGui::FindWindowByName(kPanelTerminal);
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

  // ...and once it is docked, give its node no tab bar at all while the
  // terminal is the only window in it. A single-window node draws a tab
  // reading "Terminal" directly above the panel's own bar reading "zsh" —
  // two rows of tab chrome, one of which carries nothing the other does not.
  //
  // NoTabBar rather than HiddenTabBar, which is a deliberate trade. ImGui
  // draws a small corner triangle for a HIDDEN bar (imgui.cpp, "Unhide tab
  // bar") that unhides it and drags to undock; NoTabBar skips the triangle
  // and with it the only way to drag this panel out of its node. Asked for
  // directly: the triangle is chrome for an affordance nobody here was
  // reaching for, and Reset Layout still moves the panel.
  //
  // Cleared again above one window, because NoTabBar is NOT auto-cleared the
  // way HiddenTabBar is — dock a second panel here and without this the two
  // would share a node with no way to switch between them. Every frame
  // rather than once: the panel is usually docked by a layout saved before
  // any of this existed, and neither DockBuilder branch fires for a window
  // that already has a home.
  if (state.settings.terminalPanel) {
    ImGuiWindow* term = ImGui::FindWindowByName(kPanelTerminal);
    if (term != nullptr && term->DockNode != nullptr) {
      ImGuiDockNode* node = term->DockNode;
      // AutoHide/Hidden are scrubbed as well as unset: an earlier build wrote
      // them into the saved layout, and they are what draws the triangle.
      ImGuiDockNodeFlags want =
          node->LocalFlags &
          ~(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_AutoHideTabBar |
            ImGuiDockNodeFlags_HiddenTabBar);
      if (node->Windows.Size == 1)
        want |= ImGuiDockNodeFlags_NoTabBar;
      // Guarded, or SetLocalFlags marks the .ini dirty on every frame.
      if (want != node->LocalFlags)
        node->SetLocalFlags(want);
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
    ImGuiID left = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Left, DefaultSidePanelWidth(avail.x) / avail.x,
        nullptr, &center);
    // U7e removed the right node; the terminal panel brings one back, but
    // only when it is enabled — an empty right column would otherwise eat
    // viewport width for a feature the user turned off.
    if (state.settings.terminalPanel) {
      ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f,
                                                  nullptr, &center);
      ImGui::DockBuilderDockWindow(kPanelTerminal, right);
    }
    ImGui::DockBuilderDockWindow(kPanelModel, left);
    ImGui::DockBuilderDockWindow(kPanelVariables, left);      // behind Model
    ImGui::DockBuilderDockWindow(kPanelConfigurations, left); // behind Model
    ImGui::DockBuilderDockWindow(kPanelHistory, left);        // behind Model
    // Only ever exists when there is something to resolve (F7), so it is
    // not a permanent tab — but when it appears it belongs here.
    ImGui::DockBuilderDockWindow(kPanelConflicts, left);
    ImGui::DockBuilderDockWindow(kPanelCam, left); // ditto
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
      if (ImGuiWindow* term = ImGui::FindWindowByName(kPanelTerminal))
        if (term->DockId != 0)
          ImGui::SetWindowDock(term, 0, ImGuiCond_Always);
    ImGui::DockBuilderFinish(dockspaceId);
    // The tab focused last wins the shared node; make sure that is Model —
    // with Sketch tabbed here too (U7e), it could otherwise end up on top
    // and hide the tree from both the user and the gui tests.
    ImGui::SetWindowFocus(kPanelModel);
  }

  ImGui::End(); // host window

  // Outside the host: a tooltip is its own window, and the tab bars it reads
  // belong to the dock nodes, which are laid out by the DockSpace() above.
  DrawPanelTabTooltips();

  // A panel asked to come forward. Repeated, because one SetWindowFocus on a
  // shared dock node does not reliably win the tab.
  if (state.focusPanelFrames > 0 && !state.focusPanel.empty()) {
    // By the name a person would say. ImGui matches the WHOLE window string,
    // "###" id and all, so asking for "CAM" would silently do nothing —
    // resolve it through the kPanels table.
    //
    // This used to strip the leading glyph off each window title and compare
    // what remained, which worked while a title read "<glyph> Variables###..."
    // and stopped the moment the tabs went icon-only: what remains is now the
    // empty string, and every focus_panel call would have matched nothing and
    // reported nothing. The table is the pairing; nothing else has to guess.
    const char* target = state.focusPanel.c_str();
    if (ImGui::FindWindowByName(target) == nullptr) {
      for (const PanelInfo& p : kPanels)
        if (state.focusPanel == p.name) {
          target = p.title;
          break;
        }
    }
    // Done the moment the tab is actually won — not when the frame budget
    // runs out. SetWindowFocus closes every popup over the focused window,
    // so a request left counting closes the menu a user (or test) opens
    // right after entering simulation mode: File > New snapped shut for a
    // second after the toolbar click.
    ImGuiWindow* win = ImGui::FindWindowByName(target);
    if (win != nullptr && win->DockNode != nullptr &&
        win->DockNode->SelectedTabId == win->TabId) {
      state.focusPanelFrames = 0;
      state.focusPanel.clear();
    } else {
      ImGui::SetWindowFocus(target);
      state.focusPanelFrames--;
    }
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
