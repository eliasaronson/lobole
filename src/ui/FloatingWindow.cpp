#include "ui/FloatingWindow.h"

#include "ui/AppState.h"
#include "ui/Theme.h" // ElevatedWindowBorder, DrawWindowShadow

#include "imgui_internal.h" // ImGuiWindow: forget a reappearing window's size

#include <algorithm>
#include <cmath>

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
  //
  // Its CHILD windows forget too. A child as wide as the window (the CAM
  // window's scrolling list) is a window of its own, auto-fitted to the
  // content IT measured last -- at the old width -- and the parent then
  // measured the child: the remeasured tool window came straight back at the
  // previous session's 786 px (cam/fit_keeps_the_model_clear_of_the_tool,
  // after cam/top_face_clamps_and_verbs_stay_armed).
  const bool remeasure =
      reappearing && (flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
  ImGuiWindow* existing = ImGui::FindWindowByName(title.c_str());
  if (remeasure && existing != nullptr) {
    const auto forget = [](ImGuiWindow* w) {
      w->Size = w->SizeFull = ImVec2(0.0f, 0.0f);
      w->ContentSize = w->ContentSizeIdeal = ImVec2(0.0f, 0.0f);
      // What Begin measures the content from (CalcWindowContentSizes).
      w->DC.CursorMaxPos = w->DC.IdealMaxPos = w->DC.CursorStartPos;
    };
    forget(existing);
    existing->HiddenFramesCannotSkipItems = 2;
    for (ImGuiWindow* w : GImGui->Windows)
      if (w != existing && w->RootWindow == existing &&
          (w->Flags & ImGuiWindowFlags_ChildWindow) != 0)
        forget(w);
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
  const bool anchoring = placement.anchorFrames > 0;
  if (placement.anchorFrames > 0)
    placement.anchorFrames--;

  // Inside the app window, every frame (a drag just stops at the edges) --
  // and decided BEFORE Begin, from the size Begin is about to give the
  // window. Moving it after Begin (ImGui::SetWindowPos) moves the cursor but
  // not the content region the frame is laid out in, so everything as wide
  // as the window -- a child list, a right-aligned tail -- reached past the
  // right edge by however far the clamp moved it. An auto-resized window
  // took that as its new width, outgrew the edge again by the same amount,
  // and was clamped again: the CAM window grew 97 px a frame across the
  // viewport, and the next session inherited it.
  const ImGuiViewport* mainVp = ImGui::GetMainViewport();
  const auto clampToApp = [&](ImVec2 p, ImVec2 size) {
    p.x = std::min(p.x, mainVp->WorkPos.x + mainVp->WorkSize.x - size.x - 4.0f);
    p.y = std::min(p.y, mainVp->WorkPos.y + mainVp->WorkSize.y - size.y - 4.0f);
    p.x = std::max(p.x, mainVp->WorkPos.x + 4.0f);
    p.y = std::max(p.y, mainVp->WorkPos.y + 4.0f);
    return p;
  };
  if (existing != nullptr && !remeasure) {
    const ImVec2 size = (flags & ImGuiWindowFlags_AlwaysAutoResize) != 0
                            ? ImGui::CalcWindowNextAutoFitSize(existing)
                            : existing->SizeFull;
    // A window still where the anchor put it -- not dragged -- keeps its
    // corner as it grows or shrinks: a list that gains a row widens the
    // window to the LEFT of the corner, not off the viewport's right edge.
    const ImVec2 was = existing->SizeFull;
    const bool atAnchor =
        std::abs(existing->Pos.x - (target.x - pivot.x * was.x)) < 0.5f &&
        std::abs(existing->Pos.y - (target.y - pivot.y * was.y)) < 0.5f;
    const ImVec2 want =
        anchoring || atAnchor
            ? ImVec2(target.x - pivot.x * size.x, target.y - pivot.y * size.y)
            : existing->Pos;
    const ImVec2 placed = clampToApp(want, size);
    if (anchoring || placed.x != existing->Pos.x || placed.y != existing->Pos.y)
      ImGui::SetNextWindowPos(placed, ImGuiCond_Always);
  } else {
    // Its size is not known yet: ImGui applies the pivot once it is (an
    // auto-resized window's first frames are hidden while it measures).
    ImGui::SetNextWindowPos(ImVec2(target.x, target.y),
                            anchoring ? ImGuiCond_Always : ImGuiCond_Appearing,
                            pivot);
  }
  ImGui::SetNextWindowBgAlpha(bgAlpha);

  flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
           ImGuiWindowFlags_NoSavedSettings;
  ImGui::PushStyleColor(ImGuiCol_Border, ElevatedWindowBorder());
  const bool visible = ImGui::Begin(title.c_str(), open, flags);
  ImGui::PopStyleColor();
  if (!visible)
    return false;

  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 wsize = ImGui::GetWindowSize();
  DrawWindowShadow(pos, ImVec2(pos.x + wsize.x, pos.y + wsize.y));
  return true;
}

} // namespace ui
