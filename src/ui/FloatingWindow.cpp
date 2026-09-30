#include "ui/FloatingWindow.h"

#include "ui/AppState.h"
#include "ui/Theme.h" // ElevatedWindowBorder, DrawWindowShadow

#include "imgui_internal.h" // ImGuiWindow: forget a reappearing window's size

#include <algorithm>

namespace ui {

namespace {

// The gap between a floating window and the viewport's edge, and between two
// stacked windows.
constexpr float kGap = 12.0f;

// Is the tool window on screen? It publishes its rect every frame it draws,
// and may draw after the window asking, so "this frame or the last".
bool ToolWindowShown(const AppState& state) {
  return state.toolWindowLastFrame >= ImGui::GetFrameCount() - 1;
}

} // namespace

bool BeginFloatingWindow(AppState& state, const std::string& title, bool* open,
                         FloatingPlacement& placement, FloatAnchor anchor,
                         ImGuiWindowFlags flags, float bgAlpha) {
  glm::vec2 target;
  ImVec2 pivot;
  switch (anchor) {
  case FloatAnchor::TopRight:
  case FloatAnchor::BelowToolWindow:
    target = state.viewportPos + glm::vec2(state.viewportSize.x - kGap, kGap);
    pivot = ImVec2(1.0f, 0.0f);
    if (anchor == FloatAnchor::BelowToolWindow && ToolWindowShown(state))
      target.y = state.toolWindowRect.w + kGap;
    break;
  case FloatAnchor::BottomCenter:
    target = state.viewportPos + glm::vec2(state.viewportSize.x * 0.5f,
                                           state.viewportSize.y - kGap);
    pivot = ImVec2(0.5f, 1.0f);
    break;
  }

  // A window that has just (re)appeared is anchored afresh, whatever it was
  // doing before: it may have been dragged, or closed in one layout and
  // reopened in another.
  const int frame = ImGui::GetFrameCount();
  const bool reappearing = placement.lastFrame + 1 != frame;
  if (reappearing)
    placement.anchor = glm::vec2(-1.0f, -1.0f);
  // ...and SIZED afresh. The tool window is one ImGui window shared by every
  // session, and an auto-resized window measures the content it laid out
  // LAST frame -- at its old width, which a row aligned to the right edge
  // (ListRow's tail) fills whatever that width was. So the width outlived the
  // session: a Thread window opened after a wide Simulation window came up
  // 454 px wide, and its left edge covered the flank the user (and
  // thread/face_pick_fills_the_slot) was about to click. ImGui does this for
  // a reappearing popup: forget the size and the measured content, and hide
  // the frames that measure the new content. TWO of them: laid out at no
  // width, text wrapped to the window is one word a line, and the first fit
  // is far too tall.
  const bool remeasure =
      reappearing && (flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
  if (remeasure)
    if (ImGuiWindow* w = ImGui::FindWindowByName(title.c_str())) {
      w->Size = w->SizeFull = ImVec2(0.0f, 0.0f);
      w->ContentSize = w->ContentSizeIdeal = ImVec2(0.0f, 0.0f);
      // What Begin measures the content from (CalcWindowContentSizes).
      w->DC.CursorMaxPos = w->DC.IdealMaxPos = w->DC.CursorStartPos;
      w->HiddenFramesCannotSkipItems = 2;
    }
  placement.lastFrame = frame;
  if (glm::distance(placement.anchor, target) > 1.0f) {
    placement.anchor = target;
    // Placed on every frame until the size has settled, then left alone so a
    // drag sticks. The clamp below moves a window that is too tall for a
    // frame, and an anchor that stopped one frame early left the tool window
    // over the toolbar at the top of the app for the rest of its session.
    placement.anchorFrames = remeasure ? 3 : 2;
  }
  const ImGuiCond cond =
      placement.anchorFrames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing;
  if (placement.anchorFrames > 0)
    placement.anchorFrames--;
  ImGui::SetNextWindowPos(ImVec2(target.x, target.y), cond, pivot);
  ImGui::SetNextWindowBgAlpha(bgAlpha);

  flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
           ImGuiWindowFlags_NoSavedSettings;
  ImGui::PushStyleColor(ImGuiCol_Border, ElevatedWindowBorder());
  const bool visible = ImGui::Begin(title.c_str(), open, flags);
  ImGui::PopStyleColor();
  if (!visible)
    return false;

  // Inside the app window. The pivot uses the window's size, which is unknown
  // on the appearing frame of an auto-resized window, so clamp every frame
  // (a drag just stops at the edges).
  const ImGuiViewport* mainVp = ImGui::GetMainViewport();
  const ImVec2 wpos = ImGui::GetWindowPos();
  const ImVec2 wsize = ImGui::GetWindowSize();
  ImVec2 clamped = wpos;
  clamped.x = std::min(clamped.x,
                       mainVp->WorkPos.x + mainVp->WorkSize.x - wsize.x - 4.0f);
  clamped.y = std::min(clamped.y,
                       mainVp->WorkPos.y + mainVp->WorkSize.y - wsize.y - 4.0f);
  clamped.x = std::max(clamped.x, mainVp->WorkPos.x + 4.0f);
  clamped.y = std::max(clamped.y, mainVp->WorkPos.y + 4.0f);
  if (clamped.x != wpos.x || clamped.y != wpos.y)
    ImGui::SetWindowPos(clamped);
  const ImVec2 pos = ImGui::GetWindowPos();
  DrawWindowShadow(pos, ImVec2(pos.x + wsize.x, pos.y + wsize.y));
  return true;
}

} // namespace ui
