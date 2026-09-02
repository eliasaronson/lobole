#include "ui/UI.h"
#include "ui/AssemblyActions.h"
#include "ui/DrawingPanel.h"
#include "ui/OccurrencePanel.h"
#include "ui/Overlays.h"
#include "ui/SimulationActions.h"
#include "ui/StudyPanel.h"

#include "core/Conventions.h"
#include "core/features/SketchFeature.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/SceneRenderer.h"
#include "render/SketchRenderer.h"
#include "render/StockRenderer.h"
#include "render/UnderlayRenderer.h"
#include "ui/AppActions.h"
#include "ui/CamPanel.h"
#include "ui/CamPlayback.h" // DrawCamTransportWindow
#include "ui/CamTool.h"
#include "ui/CommandPalette.h"
#include "ui/Commands.h"
#include "ui/ConfigurationsPanel.h"
#include "ui/ConflictsPanel.h"
#include "ui/DockLayout.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h"
#include "ui/HistoryPanel.h"
#include "ui/MainMenuBar.h"
#include "ui/MeasureOverlay.h"
#include "ui/PerfHud.h"
#include "ui/PreviewFeature.h"
#include "ui/PreviewHandles.h"
#include "ui/SelectionPipeline.h"
#include "ui/SettingsWindow.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/TerminalPanel.h"
#include "ui/VariablesPanel.h"
#include "ui/ViewportHandleDraw.h"
#include "ui/ViewportInput.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cfloat> // FLT_MAX (folder flyout size constraint)
#include <cstdio> // snprintf (folder rename buffer)
#include <string>
#include <vector>

namespace ui {

namespace {
// In-context editing banner (A4): floats top-center over the viewport while
// a part is edited with its context assembly ghosted. Return (or any tab
// switch) exits the mode.
void DrawInContextBanner(AppState& state) {
  if (!state.document || !state.inContext.assemblySlotId)
    return;
  const auto asmIndex = SlotIndexById(state, state.inContext.assemblySlotId);
  if (!asmIndex)
    return;
  ImGui::SetNextWindowPos(
      ImVec2(state.viewportPos.x + state.viewportSize.x * 0.5f,
             state.viewportPos.y + 34.0f),
      ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                           ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
  if (ImGui::Begin("##InContextBanner", nullptr, flags)) {
    ImGui::Text("Editing %s in context of %s",
                state.parts[state.activePart].name.c_str(),
                state.parts[*asmIndex].name.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Return"))
      SetActivePart(state, *asmIndex); // clears the mode + re-solves if stale
  }
  ImGui::End();
}

// Model-space prompt pill (U1c): while a tool is armed (blend edges,
// measure, sim BC), a top-center pill says what's expected — the sketcher's
// staged prompt bar generalized to model space. Tool feedback (actionMsg)
// shows alongside in the error tint.
void DrawModelToolPrompt(AppState& state) {
  if (state.sketchEdit.IsEditing())
    return; // the sketch prompt bar owns this spot while sketching
  float y = state.viewportPos.y + 34.0f;
  // A geometry-delta overlay must say what it is measured against: red and
  // green solids on a model are not self-explanatory, and a part that looks
  // broken but is actually carrying an annotation is the exact confusion
  // worth spending a pill on.
  if (state.HasGeometryDelta())
    DrawPromptPill(state, y, state.deltaSummary, PromptPill::Status);

  const char* prompt = ArmedPrompt(state);
  // Plane/face picking has no armed CONTEXT (it is not a pick slot), so it
  // never had a pill — name the step, especially for the Hole tool whose
  // first stage is otherwise unexplained in the viewport.
  if (prompt == nullptr && state.sketchEdit.pickingPlane) {
    prompt = state.sketchEdit.purpose == SketchPurpose::Hole
                 ? "Hole (Step 1/3): click the flat face to drill into"
                 : "New Sketch: click a plane or a model face to sketch on";
  }
  if (prompt == nullptr)
    return; // feedback shows only while its tool is armed
  // One shared pill renderer with the sketch prompt bar (U8b).
  DrawPromptPill(state, y, prompt, PromptPill::Prompt);
  DrawPromptPill(state, y, state.sketchEdit.actionMsg, PromptPill::Warning);
}

// View cube (U5, upgraded from the triad on request): a bottom-right mini
// cube tracking the camera; clicking a visible face snaps the camera normal
// to it (same orientations as Shift+1..7). Lives in its own tiny ImGui
// window so clicks are captured instead of leaking into viewport picking.
void DrawViewCube(AppState& state) {
  if (!state.camera || state.drawing || state.sheetCapture)
    return;
  const float size = 108.0f;
  // The central node ends above the part-tabs bar (the dockspace reserves
  // that strip), so anchoring to the viewport bottom already clears it.
  ImGui::SetNextWindowPos(
      ImVec2(state.viewportPos.x + state.viewportSize.x - size - 8.0f,
             state.viewportPos.y + state.viewportSize.y - size - 8.0f),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(size, size), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); // no frame lines
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollbar;
  if (!ImGui::Begin("##ViewCube", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 c(wp.x + size * 0.5f, wp.y + size * 0.5f);
  const float h = size * 0.30f; // cube half-extent on screen
  const glm::mat3 view = glm::mat3(state.camera->GetViewMatrix());

  // Project the 8 corners (world axes through the view rotation; y flips
  // into screen space).
  ImVec2 corner2d[8];
  float cornerZ[8];
  for (int i = 0; i < 8; i++) {
    const glm::vec3 w((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                      (i & 4) ? 1.0f : -1.0f);
    const glm::vec3 v = view * w;
    corner2d[i] = ImVec2(c.x + v.x * h, c.y - v.y * h);
    cornerZ[i] = v.z;
  }

  struct CubeFace {
    glm::vec3 normal;
    int corners[4]; // CCW seen from outside
    const conv::NamedView* view;
  };
  // Geometry (corner indexing) is fixed; which face is CALLED "Top" comes
  // from the conventions table, the same one the view.* commands and the
  // drawings use — so a click lands on exactly the named view it labels.
  static const CubeFace kFaces[6] = {
      {{1, 0, 0}, {1, 3, 7, 5}, conv::ViewForDirection({1, 0, 0})},
      {{-1, 0, 0}, {0, 4, 6, 2}, conv::ViewForDirection({-1, 0, 0})},
      {{0, 1, 0}, {2, 6, 7, 3}, conv::ViewForDirection({0, 1, 0})},
      {{0, -1, 0}, {0, 1, 5, 4}, conv::ViewForDirection({0, -1, 0})},
      {{0, 0, 1}, {4, 5, 7, 6}, conv::ViewForDirection({0, 0, 1})},
      {{0, 0, -1}, {0, 2, 3, 1}, conv::ViewForDirection({0, 0, -1})},
  };

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##cube", ImVec2(size, size));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool hoveredWidget = ImGui::IsItemHovered();
  bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

  // Home corner: viewed head-on the cube shows ONE face, so there is no
  // corner left to click to get back to iso — the way out was Shift+7, which
  // nothing on screen mentions. This little cube always is that way out.
  const float homeSz = std::max(14.0f, size * 0.22f);
  // Top-left of the widget square: the cube itself only covers the middle
  // 60%, and the bottom edge is clipped by the window padding.
  const ImVec2 homeMin(origin.x, origin.y);
  const ImVec2 homeMax(origin.x + homeSz, origin.y + homeSz);
  const bool homeHot = hoveredWidget && mouse.x >= homeMin.x &&
                       mouse.x <= homeMax.x && mouse.y >= homeMin.y &&
                       mouse.y <= homeMax.y;
  if (homeHot) {
    clicked = false; // the cube must not also swallow this click
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      state.camera->AlignToFace(state.camera->GetTarget(),
                                glm::vec3(conv::FindView("iso")->toCamera));
    }
    ImGui::SetTooltip("Isometric view (Shift+7)");
  }

  const auto pointInQuad = [&](const ImVec2* q, const ImVec2& p) {
    for (int i = 0; i < 4; i++) {
      const ImVec2& a = q[i];
      const ImVec2& b = q[(i + 1) % 4];
      if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0.0f)
        return false; // outside a CW screen-space edge
    }
    return true;
  };

  // Corner and edge hotspots, tested BEFORE the faces: viewed head-on the
  // cube shows one face and the 45-degree directions are unreachable, so the
  // only way to a three-quarter view was a drag. A corner is the sum of its
  // three faces' normals, an edge the sum of its two (U10).
  ImDrawList* dl = ImGui::GetWindowDrawList();
  glm::vec3 hotDir(0.0f);
  bool hotIsCorner = false, hotAny = false;
  if (hoveredWidget && !homeHot) {
    const float grab = std::max(6.0f, h * 0.28f);
    float bestD2 = grab * grab;
    // The 8 corners.
    for (int i = 0; i < 8; i++) {
      if (cornerZ[i] <= 0.0f)
        continue; // facing away
      const float dx = mouse.x - corner2d[i].x, dy = mouse.y - corner2d[i].y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > bestD2)
        continue;
      bestD2 = d2;
      hotDir = glm::vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                         (i & 4) ? 1.0f : -1.0f);
      hotIsCorner = true;
      hotAny = true;
    }
    // The 12 edges: pairs of corners differing in exactly one bit.
    for (int i = 0; i < 8 && !hotIsCorner; i++) {
      for (int bit = 1; bit <= 4; bit <<= 1) {
        const int j = i ^ bit;
        if (j < i || cornerZ[i] + cornerZ[j] <= 0.0f)
          continue;
        const ImVec2 mid((corner2d[i].x + corner2d[j].x) * 0.5f,
                         (corner2d[i].y + corner2d[j].y) * 0.5f);
        const float dx = mouse.x - mid.x, dy = mouse.y - mid.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > bestD2)
          continue;
        bestD2 = d2;
        const glm::vec3 a((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                          (i & 4) ? 1.0f : -1.0f);
        const glm::vec3 b((j & 1) ? 1.0f : -1.0f, (j & 2) ? 1.0f : -1.0f,
                          (j & 4) ? 1.0f : -1.0f);
        hotDir =
            a + b; // the two shared components survive, the odd one cancels
        hotAny = true;
      }
    }
  }
  if (hotAny) {
    clicked = false; // the hotspot owns this click, not the face under it
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      state.camera->AlignToFace(state.camera->GetTarget(),
                                glm::normalize(hotDir));
    ImGui::SetTooltip(hotIsCorner ? "Corner view" : "Edge view");
  }

  for (const CubeFace& f : kFaces) {
    const glm::vec3 n = view * f.normal;
    if (n.z <= 0.02f)
      continue; // back-facing
    ImVec2 quad[4];
    ImVec2 center(0, 0);
    for (int i = 0; i < 4; i++) {
      quad[i] = corner2d[f.corners[i]];
      center.x += quad[i].x * 0.25f;
      center.y += quad[i].y * 0.25f;
    }
    const bool hot = hoveredWidget && pointInQuad(quad, mouse);
    const float shade = 0.35f + 0.45f * n.z;
    const ImU32 fill = hot ? IM_COL32(90, 140, 210, 235)
                           : IM_COL32(static_cast<int>(70 * shade + 30),
                                      static_cast<int>(80 * shade + 34),
                                      static_cast<int>(95 * shade + 40), 220);
    dl->AddConvexPolyFilled(quad, 4, fill);
    dl->AddPolyline(quad, 4, IM_COL32(150, 160, 175, 200), ImDrawFlags_Closed,
                    1.0f);
    if (n.z > 0.45f) { // label only when facing enough to read
      const ImVec2 sz = ImGui::CalcTextSize(f.view->label);
      dl->AddText(ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f),
                  hot ? IM_COL32_WHITE : IM_COL32(205, 215, 230, 230),
                  f.view->label);
    }
    if (hot && clicked)
      state.camera->AlignToFace(state.camera->GetTarget(), f.normal, 0.0f,
                                glm::vec3(f.view->up));
  }

  // Draw the home corner last so it sits above the cube's faces.
  {
    ImDrawList* d2 = ImGui::GetWindowDrawList();
    d2->AddRectFilled(homeMin, homeMax,
                      homeHot ? IM_COL32(90, 140, 210, 235)
                              : IM_COL32(60, 66, 78, 200),
                      3.0f);
    const char* glyph = "\xef\x80\x95"; // ICON_FA_HOUSE
    const ImVec2 gs = ImGui::CalcTextSize(glyph);
    const ImVec2 hc((homeMin.x + homeMax.x) * 0.5f,
                    (homeMin.y + homeMax.y) * 0.5f);
    d2->AddText(ImVec2(hc.x - gs.x * 0.5f, hc.y - gs.y * 0.5f),
                homeHot ? IM_COL32_WHITE : IM_COL32(205, 215, 230, 230), glyph);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// Empty-state hint (U5): a fresh part tells the user where to start instead
// of showing three anonymous planes.
// Which world direction a sketch axis points, as a label: "+X", "-Z"… The
// sketch grid colours U red and V green, but nothing said what they MEAN, and
// V = normal x uAxis makes the Right plane's V point along -Z, which has
// surprised every person who has correlated a screenshot with coordinates
// (see the V-axis note in the project memory). U10.
const char* WorldAxisLabel(const glm::vec3& dir) {
  const glm::vec3 a = glm::abs(dir);
  if (a.x >= a.y && a.x >= a.z)
    return dir.x >= 0.0f ? "+X" : "-X";
  if (a.y >= a.z)
    return dir.y >= 0.0f ? "+Y" : "-Y";
  return dir.z >= 0.0f ? "+Z" : "-Z";
}

// Labels for the sketch's U and V axes, in the same red and green the grid
// uses. Anchored a fixed screen distance from the sketch ORIGIN along each
// axis rather than at the axes' far ends, which are off-screen at any working
// zoom — a compass, not a ruler.
void DrawSketchAxisLabels(AppState& state) {
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || !state.sketchEdit.IsEditing())
    return;
  const glm::vec2 origin = SketchUVToScreen(state, sketch->plane, {0.0f, 0.0f});
  const glm::vec2 vp0 = state.viewportPos;
  const glm::vec2 vp1 = state.viewportPos + state.viewportSize;
  if (origin.x < vp0.x || origin.y < vp0.y || origin.x > vp1.x ||
      origin.y > vp1.y) {
    return; // the origin is off-screen; a compass with no needle helps nobody
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const struct {
    glm::vec2 uv;
    glm::vec3 dir;
    const char* axis;
    ImU32 color;
  } axes[2] = {
      {{1.0f, 0.0f},
       sketch->plane.GetUAxis(),
       "U",
       IM_COL32(217, 102, 107, 235)},
      {{0.0f, 1.0f},
       sketch->plane.GetVAxis(),
       "V",
       IM_COL32(115, 204, 128, 235)},
  };
  for (const auto& a : axes) {
    const glm::vec2 out = SketchUVToScreen(state, sketch->plane, a.uv) - origin;
    const float len = glm::length(out);
    if (len < 1e-3f)
      continue;
    const glm::vec2 at = origin + out / len * 58.0f;
    char label[16];
    std::snprintf(label, sizeof(label), "%s %s", a.axis, WorldAxisLabel(a.dir));
    const ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(at.x - sz.x * 0.5f, at.y - sz.y * 0.5f), a.color, label);
  }
}

void DrawEmptyStateHint(AppState& state) {
  if (!state.document || state.document->FeatureCount() > 0 ||
      state.sketchEdit.IsEditing() || state.sketchEdit.pickingPlane ||
      HasPendingFeature(state)) {
    return;
  }
  const char* line1 = "No features yet";
  // Name affordances that exist: the Create menu was deleted in favour of the
  // toolbar, and double-clicking a plane has never started a sketch (plane
  // picking is arm-then-click).
  const char* line2 = "Press S (or the toolbar's pencil), then click a plane "
                      "- or add a box from the toolbar";
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 c(state.viewportPos.x + state.viewportSize.x * 0.5f,
                 state.viewportPos.y + state.viewportSize.y * 0.72f);
  const ImVec2 s1 = ImGui::CalcTextSize(line1);
  const ImVec2 s2 = ImGui::CalcTextSize(line2);
  dl->AddText(ImVec2(c.x - s1.x * 0.5f, c.y), IM_COL32(200, 210, 225, 160),
              line1);
  dl->AddText(ImVec2(c.x - s2.x * 0.5f, c.y + s1.y + 6.0f),
              IM_COL32(160, 170, 185, 140), line2);
}

// Viewport-controls cheat sheet (View > Viewport Controls): a dismissable
// overlay in the viewport's bottom-left corner. Replaces the static Controls
// block that used to permanently occupy the Properties panel (U4).
void DrawControlsHelpOverlay(AppState& state) {
  if (!state.showControlsHelp)
    return;
  ImGui::SetNextWindowPos(
      ImVec2(state.viewportPos.x + 12.0f,
             state.viewportPos.y + state.viewportSize.y - 12.0f),
      ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.75f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##ControlsHelp", nullptr, flags)) {
    ImGui::TextUnformatted("Viewport controls");
    ImGui::SameLine();
    if (ImGui::SmallButton("x"))
      state.showControlsHelp = false;
    ImGui::Separator();
    ImGui::BulletText("Click: select edge/face (again: cycle, Ctrl: multi)");
    ImGui::BulletText("Shift+Click: align camera to face");
    ImGui::BulletText("Middle drag: rotate");
    ImGui::BulletText("Ctrl+Middle: pan   Shift+Middle: zoom");
    ImGui::BulletText("Alt+Middle: roll   Scroll: zoom");
    ImGui::BulletText("Arrows: rotate (Shift: 90\xc2\xb0)   Z: zoom");
  }
  ImGui::End();
}

// Part tabs pinned to the bottom of the screen (assemblies A1): one tab per
// part + a "+" to add. Clicking a tab switches the active part.
// What kind of document a tab holds, at a glance. The bar showed names only,
// so "Bracket" and "Bracket drawing" and a folder of them were three
// identical-looking buttons — you found out by clicking. Folders carry their
// own glyph plus a count.
const char* TabGlyph(DocKind kind) {
  switch (kind) {
  case DocKind::Part:
    return ICON_FA_CUBE;
  case DocKind::Assembly:
    return ICON_FA_CUBES;
  case DocKind::Drawing:
    return ICON_FA_FILE_LINES;
  case DocKind::Job:
    return ICON_FA_SCREWDRIVER_WRENCH;
  }
  return ICON_FA_CUBE;
}

const char* TabKindName(DocKind kind) {
  switch (kind) {
  case DocKind::Part:
    return "Part";
  case DocKind::Assembly:
    return "Assembly";
  case DocKind::Drawing:
    return "Drawing";
  case DocKind::Job:
    return "Job";
  }
  return "Part";
}

void DrawPartTabs(AppState& state) {
  EnsureParts(state);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = PartTabsBarHeight();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
  // The same vertical padding PartTabsBarHeight budgeted for. Pushed around
  // Begin (ImGui reads WindowPadding there) so the height and the padding
  // cannot disagree — which is how the bar came to be shorter than its own
  // content in the first place.
  //
  // WindowMinSize goes with it, and that one is a BUG FIX, not a tidy-up.
  // The theme sets a 180x60 floor so a dock node cannot be dragged down to a
  // sliver (Theme.cpp, U4) — and ImGui applies it to every non-child window,
  // AFTER any size constraint (CalcWindowMinSize is ImMax'd over the
  // constrained size, so SetNextWindowSizeConstraints cannot get under it
  // either). So this bar asked for 36 px, was silently given 60, and the
  // dockspace went on reserving 36: the bottom ~24 px of every docked panel
  // has been sitting underneath the tab bar. Lifting the floor for this one
  // window — which is not dockable and not resizable — makes the height it
  // asks for the height it gets.
  const ImGuiStyle& st = ImGui::GetStyle();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(st.WindowPadding.x, PartTabsBarPaddingY()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
  const bool barOpen = ImGui::Begin("##PartTabs", nullptr, flags);
  ImGui::PopStyleVar(2);
  if (barOpen) {
    // Folder-name entry state (rename, or naming a new folder for one doc).
    // One popup serves both: renameFolderTarget non-empty = rename mode,
    // otherwise newFolderDocId says which doc moves into the named folder.
    static char folderNameBuf[128];
    static std::string renameFolderTarget;
    static uint64_t newFolderDocId = 0;
    static bool openFolderNamePopup = false;

    // The registry is THE folder list (it can hold empty folders). Slot
    // folders something else set directly (import, hand-edited project.json)
    // self-register here so the bar and the next save both know them.
    for (const PartSlot& s : state.parts)
      if (!s.folder.empty() &&
          std::find(state.tabFolders.begin(), state.tabFolders.end(),
                    s.folder) == state.tabFolders.end())
        state.tabFolders.push_back(s.folder);
    // Iterate a copy: Dissolve (and rename merges) mutate the registry from
    // inside the loops below.
    const std::vector<std::string> folders = state.tabFolders;

    // Docs drag by stable id (indices shift on delete; ids never lie).
    const auto docDragSource = [&](size_t i) {
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("DOC_TAB", &state.parts[i].id,
                                  sizeof(uint64_t));
        ImGui::TextUnformatted(state.parts[i].name.c_str());
        ImGui::EndDragDropSource();
      }
    };
    // Makes the last item a drop target that files the dragged doc under
    // `folder` ("" = back to top level).
    const auto acceptDocDrop = [&](const std::string& folder) {
      if (!ImGui::BeginDragDropTarget())
        return;
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("DOC_TAB")) {
        const uint64_t id = *static_cast<const uint64_t*>(pl->Data);
        for (size_t i = 0; i < state.parts.size(); i++)
          if (state.parts[i].id == id)
            MoveToFolder(state, i, folder);
      }
      ImGui::EndDragDropTarget();
    };
    // "Move to folder" submenu shared by top-level tabs and flyout entries.
    const auto moveToFolderMenu = [&](size_t i) {
      if (!ImGui::BeginMenu("Move to folder"))
        return;
      for (const std::string& f : folders)
        if (f != state.parts[i].folder &&
            ImGui::MenuItem((f + "##mv" + f).c_str()))
          MoveToFolder(state, i, f);
      if (!folders.empty())
        ImGui::Separator();
      if (ImGui::MenuItem("New folder...##mvnew")) {
        newFolderDocId = state.parts[i].id;
        renameFolderTarget.clear();
        folderNameBuf[0] = '\0';
        openFolderNamePopup = true;
      }
      ImGui::EndMenu();
    };
    // Duplicate/delete menu items shared by top-level tabs and flyout entries.
    const auto duplicateMenuItem = [&](size_t i) {
      const char* label =
          state.parts[i].kind == DocKind::Assembly  ? "Duplicate assembly"
          : state.parts[i].kind == DocKind::Job     ? "Duplicate job"
          : state.parts[i].kind == DocKind::Drawing ? "Duplicate drawing"
                                                    : "Duplicate part";
      if (ImGui::MenuItem(label))
        DuplicateDocument(state, i);
    };
    // Delete menu item shared by top-level tabs and flyout entries.
    const auto deleteMenuItem = [&](size_t i) {
      const char* label =
          state.parts[i].kind == DocKind::Assembly  ? "Delete assembly"
          : state.parts[i].kind == DocKind::Job     ? "Delete job"
          : state.parts[i].kind == DocKind::Drawing ? "Delete drawing"
                                                    : "Delete part";
      // ASK first. This item used to be enabled whenever more than one tab
      // existed, call DeletePart, and drop both its `false` and its reason on
      // the floor — so on any real project, where every part is placed in an
      // assembly, clicking Delete did nothing at all and said nothing about
      // why. Reported from use; the same defect as File > Save doing nothing
      // on an assembly tab.
      const std::string blocked = WhyPartUndeletable(state, i);
      if (ImGui::MenuItem(label, nullptr, false, blocked.empty()))
        DeletePart(state, i);
      if (!blocked.empty() &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", blocked.c_str());
    };

    // The tabs live in their own scrolling child, and "+" sits OUTSIDE it,
    // pinned to the right. Before this, the tabs were plain buttons on one
    // SameLine run in a NoScrollbar window: open the Marsliden dock (17
    // documents) and the bar ran off the edge taking "+" — the only way to
    // add a document — with it. The bar did scroll, but with no scrollbar
    // nothing said the other fourteen tabs existed, which is the half of the
    // bug you cannot see in a screenshot.
    //
    // Same shape as the toolbar's wrap (Toolbar.cpp WrapIfNeeded): lose
    // nothing, and pay for it in height. Here the height only grows while
    // the strip actually overflows — see PartTabsBarHeight.
    const float plusW =
        ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float stripW =
        std::max(1.0f, ImGui::GetContentRegionAvail().x - plusW -
                           ImGui::GetStyle().ItemSpacing.x);
    // EXPLICIT height: one button row, plus a scrollbar while overflowing —
    // the same budget PartTabsBarHeight reserved. Not 0 ("whatever the bar
    // has left"), which inherits the parent's spare room and lets a squeezed
    // parent clip the tabs rather than fail visibly.
    const float stripH = ImGui::GetFrameHeight() +
                         (PartTabsOverflowing() ? st.ScrollbarSize : 0.0f);
    ImGui::BeginChild("##tabstrip", ImVec2(stripW, stripH),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Top-level tabs first, one button each; foldered documents (assembly
    // import drops its parts in a folder) collapse to one flyout per folder
    // so a 100-part import doesn't flood the bar.
    bool any = false;
    const auto sameLine = [&] {
      if (any)
        ImGui::SameLine();
      any = true;
    };
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (!state.parts[i].folder.empty())
        continue;
      sameLine();
      const bool active = static_cast<int>(i) == state.activePart;
      if (active)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      // "###": the id must not move when the label gains a glyph or the
      // document is renamed — that is what tests and scripts address.
      if (ImGui::Button((std::string(TabGlyph(state.parts[i].kind)) + "  " +
                         state.parts[i].name + "###part" + std::to_string(i))
                            .c_str()))
        SetActivePart(state, i);
      if (active)
        ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s: %s", TabKindName(state.parts[i].kind),
                          state.parts[i].name.c_str());
      docDragSource(i);
      acceptDocDrop(""); // dropping on a top-level tab = out of its folder
      // Right-click a tab: move to a folder, or delete it (never the last).
      if (ImGui::BeginPopupContextItem(
              ("##pctx" + std::to_string(i)).c_str())) {
        moveToFolderMenu(i);
        duplicateMenuItem(i);
        deleteMenuItem(i);
        ImGui::EndPopup();
      }
    }
    // Folder flyouts.
    for (const std::string& folder : folders) {
      size_t count = 0;
      bool activeInside = false;
      for (size_t i = 0; i < state.parts.size(); i++) {
        if (state.parts[i].folder != folder)
          continue;
        count++;
        activeInside |= static_cast<int>(i) == state.activePart;
      }
      sameLine();
      if (activeInside)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      const std::string label = std::string(ICON_FA_FOLDER) + "  " + folder +
                                " (" + std::to_string(count) + ")###folder" +
                                folder;
      if (ImGui::Button(label.c_str()))
        ImGui::OpenPopup(("##folderpop" + folder).c_str());
      if (activeInside)
        ImGui::PopStyleColor();
      acceptDocDrop(folder); // drag a tab onto the folder to file it
      // Right-click the folder: rename, or dissolve (contents move to the
      // top level — documents are never deleted with their folder).
      if (ImGui::BeginPopupContextItem(("##fctx" + folder).c_str())) {
        if (ImGui::MenuItem("Rename...")) {
          renameFolderTarget = folder;
          std::snprintf(folderNameBuf, sizeof(folderNameBuf), "%s",
                        folder.c_str());
          openFolderNamePopup = true;
        }
        if (ImGui::MenuItem("Dissolve folder"))
          DissolveFolder(state, folder);
        ImGui::EndPopup();
      }
      // Tall folders scroll instead of running off screen.
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, vp->WorkSize.y * 0.6f));
      if (ImGui::BeginPopup(("##folderpop" + folder).c_str())) {
        if (count == 0)
          ImGui::TextDisabled("(empty — drag tabs here)");
        for (size_t i = 0; i < state.parts.size(); i++) {
          if (state.parts[i].folder != folder)
            continue;
          const bool active = static_cast<int>(i) == state.activePart;
          if (ImGui::Selectable((std::string(TabGlyph(state.parts[i].kind)) +
                                 "  " + state.parts[i].name + "###fpart" +
                                 std::to_string(i))
                                    .c_str(),
                                active)) {
            SetActivePart(state, i);
          }
          docDragSource(i); // draggable out to the bar / another folder
          if (ImGui::BeginPopupContextItem(
                  ("##fpctx" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Move out of folder"))
              MoveToFolder(state, i, "");
            moveToFolderMenu(i);
            duplicateMenuItem(i);
            deleteMenuItem(i);
            ImGui::EndPopup();
          }
        }
        ImGui::EndPopup();
      }
    }
    // The strip's empty tail is a drop target: drag a doc out of a folder
    // and release it on open bar space to unfolder it. Inside the child, so
    // it is the TABS' leftover space — when they overflow there is none, and
    // it simply does not draw.
    if (any)
      ImGui::SameLine();
    // Measure the run OURSELVES, from the cursor, before the tail fills the
    // remainder. ImGui's own GetScrollMaxX() reads zero here on every frame —
    // it is derived from the previous frame's ContentSize, and the reserved
    // height depended on it, so the two sat in a stalemate: no extra height,
    // therefore a squeezed strip, therefore nothing that ever reported an
    // overflow. Measured live it is a plain fact about this frame's layout.
    // GetCursorPosX is a content coordinate (scroll already added back in),
    // so it is the full width of the tabs, not the visible part of it.
    const bool stripOverflows =
        ImGui::GetCursorPosX() > ImGui::GetContentRegionMax().x + 1.0f;
    const float tailW = ImGui::GetContentRegionAvail().x;
    if (tailW > 8.0f) {
      ImGui::InvisibleButton("##tabsdroptail",
                             ImVec2(tailW, ImGui::GetFrameHeight()));
      acceptDocDrop("");
    }
    ImGui::EndChild();
    SetPartTabsOverflowing(stripOverflows);

    // One "+" for all document kinds (was three separate buttons). Outside
    // the strip: it must stay reachable at any document count.
    ImGui::SameLine();
    if (ImGui::Button("+##addmenu"))
      ImGui::OpenPopup("##addmenupop");
    if (ImGui::BeginPopup("##addmenupop")) {
      if (ImGui::MenuItem("New Part##addpart"))
        AddPart(state, "Part " + std::to_string(state.parts.size() + 1));
      if (ImGui::MenuItem("New Assembly##addasm"))
        AddAssembly(state,
                    "Assembly " + std::to_string(state.parts.size() + 1));
      if (ImGui::MenuItem("New Drawing##adddwg"))
        AddDrawing(state, "Drawing " + std::to_string(state.parts.size() + 1));
      // A job is made the way an assembly is (docs/cam/job-document.md,
      // round two): here, then a machine, a stock and parts -- not from a
      // button on the part bar that built one around a single part.
      if (ImGui::MenuItem("New CAM Job##addjob"))
        AddJob(state, "Job " + std::to_string(state.parts.size() + 1));
      ImGui::Separator();
      if (ImGui::MenuItem("New Folder##addfolder")) {
        newFolderDocId = 0; // no doc to file: create the folder empty
        renameFolderTarget.clear();
        folderNameBuf[0] = '\0';
        openFolderNamePopup = true;
      }
      ImGui::EndPopup();
    }
    // Folder-name popup (rename / new folder), opened outside the context
    // menus so it survives their closing.
    if (openFolderNamePopup) {
      ImGui::OpenPopup("##foldername");
      openFolderNamePopup = false;
    }
    if (ImGui::BeginPopup("##foldername")) {
      ImGui::TextUnformatted(renameFolderTarget.empty() ? "New folder"
                                                        : "Rename folder");
      if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(180.0f);
      const bool entered =
          ImGui::InputText("##fname", folderNameBuf, sizeof(folderNameBuf),
                           ImGuiInputTextFlags_EnterReturnsTrue);
      if (entered) {
        if (folderNameBuf[0] != '\0') {
          if (!renameFolderTarget.empty()) {
            RenameFolder(state, renameFolderTarget, folderNameBuf);
          } else if (newFolderDocId != 0) {
            for (size_t i = 0; i < state.parts.size(); i++)
              if (state.parts[i].id == newFolderDocId)
                MoveToFolder(state, i, folderNameBuf);
          } else {
            CreateFolder(state, folderNameBuf); // "+ > New Folder": born empty
          }
        }
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
}
} // namespace

void DrawUI(AppState& state) {
  // One place to keep the selection valid across delete/undo/File > New:
  // items are stable ids, so pruning is "does the referent still exist".
  PruneSelection(state);

  DrawMainMenuBar(state);
  BeginDockspace(state);
  DrawPartTabs(state);

  // The part-editing panels apply to a part tab; an active assembly tab (A2)
  // has no `document`, so they'd have nothing to show. The viewport still
  // renders the assembly's occurrences (SyncDocumentRenderers handles both).
  if (state.document) {
    // Simulation mode turns the Model dock into the Studies panel, the way an
    // assembly tab turns it into the occurrence panel.
    if (state.simulation.IsActive()) {
      DrawStudyPanel(state);
      DrawFieldLegend(state); // colorbar overlay when a field is shown
    } else {
      DrawFeatureTreePanel(state);
    }
    DrawConfigurationsPanel(state);
  } else if (state.assembly) {
    DrawOccurrencePanel(state); // occurrence list + editor in the Model panel
  } else if (state.drawing) {
    DrawSheetPanel(state); // sheet settings + title block in the Model panel
  }
  // Variables spans the whole project, so it is drawn on every kind of tab.
  // It was inside the part-only branch above until 2026-09-01, and that is
  // the same defect that made File > Save a silent no-op outside a part tab:
  // PROJECT-scope variables live on the AppState, are undoable ACROSS tabs
  // and drive every part, so gating them on the active tab holding a part
  // made the one scope that spans the project unreachable from two of the
  // three kinds of tab. The panel itself skips the part-local half and the
  // units section when there is no document.
  DrawVariablesPanel(state);
  // Outside the tab-kind branches on purpose: a version is a property of the
  // PROJECT, so the history reads the same from a part, an assembly or a
  // drawing tab (F6).
  PollDiskState(state); // F8 — before the panels, so all of them agree
  DrawHistoryPanel(state);
  DrawConflictsPanel(state);
  DrawCamPanel(state);
  DrawTerminalPanel(state);
  DrawSettingsWindow(state);
  DrawPerfHud(state);
  DrawToolWindow(state);
  DrawCamTransportWindow(state); // the cutting simulation's transport
  DrawFileBrowser(state);
  DrawControlsHelpOverlay(state);
  DrawModelToolPrompt(state);
  DrawSketchAxisLabels(state);
  DrawEmptyStateHint(state);
  DrawViewCube(state);
  DrawCommandPalette(state);

  // Keyboard bindings, once per frame, AFTER every panel has drawn: that is
  // what makes io.WantTextInput accurate for this frame, so a bare `1` typed
  // into an occurrence's position field cannot also arm a mate tool. Before
  // this the bindings lived in four places at three different points in the
  // frame (docs/ui/u12-keybindings.md).
  DispatchShortcuts(state);

  HandleViewportInput(state);
  // Renderer highlights are a projection of the Selection (U1b): one sync
  // point after input may have changed the set.
  SyncSelectionHighlights(state);
  // The live measurement of whatever is under the cursor (M3). After the
  // pick pipeline, which is what decides "under the cursor"; cached inside,
  // so a still cursor does no OCC work.
  UpdateMeasureHover(state);
  // Pinned measurements track the document: an edit that CHANGES a measured
  // face moves the number, and one that DESTROYS it drops the pin instead of
  // leaving a stale dimension floating over geometry that is gone.
  RefreshPinnedMeasurements(state);

  // A drawing tab paints its 2D sheet into the (mesh-free) viewport.
  if (state.drawing)
    DrawSheet(state);

  // Refresh sketch overlays for this frame. Planes show when the toggle is on
  // or while picking a plane; the active sketch and rubber-band preview draw
  // as an overlay on top of the static sketch lines.
  if (state.sketchRenderer) {
    std::vector<LineVertex3D> quads;
    std::vector<LineVertex3D> overlay;
    // Sketch/plane/datum overlays are part-tab geometry; an assembly tab has
    // no document, so build them only for a part (the world-origin gizmo below
    // shows for both). Line segments are order-independent.
    if (state.document) {
      // Only ask for the extent when something will USE it: behind it is an
      // OCC bounding box per body, cached against the document version — and
      // a live edit (a sketch drag, a preview) bumps that version every
      // frame, so an argument evaluated whether or not the planes are drawn
      // is a bbox pass per frame on a part that is being dragged.
      const bool planesVisible = EffectivePlaneVisibility(state);
      quads =
          BuildPlaneQuads(planesVisible, state.sketchEdit.hoveredPlane,
                          planesVisible ? ScenePlaneHalfExtent(state) : 0.0f);
      AppendDatumPlaneQuads(state, quads); // user datum planes (own visibility)
      overlay =
          BuildActiveSketchOverlay(state, state.sketchEdit.activeSketchId);
      BuildSketchOverlay(state, overlay);
      AppendDatumAxisLines(state, overlay); // user datum axes as line segments
      AppendCurve3DLines(state, overlay);   // 3D curves (fit-spline/helix/...)

      // While editing, draw a grid + subtle fill on the work plane so it's
      // clear what surface the sketch is on.
      if (SketchFeature* active = ResolveActiveSketch(state)) {
        // The fill and the grid are ONE background: same extent, or the grid
        // runs off the lit patch it is supposed to be drawn on.
        const SketchGridMetrics grid = SketchGridFor(state, *active);
        AppendSketchGrid(active->plane, grid, overlay);
        AppendPlaneQuad(active->plane, glm::vec3(0.30f, 0.36f, 0.48f),
                        grid.half, quads);
      }
    }
    AppendOriginGizmo(state, overlay);    // world origin: parts and assemblies
    AppendThreadOverlays(state, overlay); // cosmetic threads (part tabs)
    BuildMateToolOverlay(state, overlay); // A3 mate connector triads (assembly)

    // Viewport handles (docs/ui/viewport-handles.md): every owner PUBLISHES
    // its arrows and arcs into one list, rebuilt every frame, which is what
    // hover and clicks hit-test against and what DrawViewportHandles draws
    // (below, once the FEM loads have published too). The preview handle's
    // decorations go to the same on-top channel the handles land in.
    state.viewportHandles.clear();
    PublishGizmoHandles(state);
    std::vector<PrimVertex> handleDecor;
    BuildPreviewHandleOverlay(state, handleDecor);
    // Markers that are not handles keep the no-depth 1 px line channel.
    std::vector<LineVertex3D> gizmo;
    AppendCogMarker(state, gizmo);   // mass-props CoG cross (no-depth channel)
    AppendStudyMarker(state, gizmo); // FEM peak-field cross (no-depth channel)

    // CAM's decision-time drawings (docs/cam/gui.md G3): the stock box and
    // the removed-volume ghost are translucent surfaces, so they have to go
    // into `quads` before it is handed over — but their outlines are
    // primitives, which are built further down. Collect them here and splice
    // them in below.
    // On a JOB tab too: a job has no `document`, and this gate was
    // `state.document` alone, so a job tab -- the one kind of tab CAM is
    // drawn on since v73 -- drew no backplot, no stock box and no ghost.
    // The GUI tests kept passing because they call the contributors
    // directly; `backplot_reaches_the_renderer_on_a_job_tab` reads the
    // renderer instead.
    std::vector<PrimVertex> camPrims;
    if (state.document || state.job)
      AppendCamVisualisation(state, camPrims, quads);

    state.sketchRenderer->SetQuads(std::move(quads));
    state.sketchRenderer->SetOverlayLines(std::move(overlay));
    state.sketchRenderer->SetGizmoLines(std::move(gizmo));

    // Most of what follows is part-tab geometry — sketches, CAM, FEM glyphs —
    // and an assembly tab has no `document` for it to read. But SELECTION and
    // MEASURE are not part-only: assembly measure ships an occurrence-aware
    // distance, and this `if` used to sit around the whole primitive build,
    // so on an assembly the readout said "Distance: 427.814 mm" while the
    // viewport drew nothing at all and a picked edge showed no highlight.
    // The contributors that work on both tabs are now outside it.
    std::vector<PrimVertex> prims;
    if (state.document) {
      // Inactive sketches as thin lines; the active sketch as thick AA
      // primitives with vertex dots and hover/selection emphasis. Rebuilt each
      // frame so edit-mode, selection, and hover changes show immediately.
      UpdateSketchStatus(state); // DOF + conflicts for coloring/feedback
      state.sketchRenderer->SetStaticLines(
          BuildStaticSketchLines(state, state.sketchEdit.activeSketchId));
      prims =
          BuildActiveSketchPrimitives(state, state.sketchEdit.activeSketchId);
      AppendEdgePickPrimitives(state, prims); // fillet/chamfer edge picking
    } else {
      state.sketchRenderer->SetStaticLines({});
    }
    AppendSelectionPrimitives(state, prims); // selected/hovered edges (U1b)
    if (state.document) {
      AppendRegionPickPrimitives(state, prims); // armed Profiles slot (U2)
      AppendCalibrationMarkers(state, prims);   // calibrate-image picks
      AppendSimulationBCGlyphs(state, prims);   // FEM load/fixed/keep glyphs
    }
    if (state.document || state.job) {
      AppendCamToolpathOverlay(state, prims); // CAM backplot
      prims.insert(prims.end(), camPrims.begin(), camPrims.end());
    }
    state.sketchRenderer->SetPrimitives(std::move(prims));

    // The measurement draws in its OWN channel, with depth test off: a
    // thickness or bore-to-wall dimension runs through material, and a
    // depth-tested one is drawn inside the solid where nothing can see it.
    std::vector<PrimVertex> measurePrims;
    AppendMeasureOverlay(state, measurePrims);
    // The handles: thick, on top, and drawn from the same records the hit
    // test uses. After AppendSimulationBCGlyphs above, which publishes the
    // force arrows.
    DrawViewportHandles(state, measurePrims);
    measurePrims.insert(measurePrims.end(), handleDecor.begin(),
                        handleDecor.end());
    state.sketchRenderer->SetOverlayPrimitives(std::move(measurePrims));

    // 2D annotations. DrawMeasureLabels is outside the part-tab gate for the
    // same reason the overlay above is.
    DrawMeasureLabels(state);
    if (state.document) {
      // Auto-constraint snap indicator + ghost glyphs, and dimension
      // annotations (2D overlays).
      DrawSnapGlyphs(state);
      DrawSelectionBox(state);
      DrawDatumLabels(state);
      DrawElementNameOverlay(state); // toponaming debug: hovered face's name
      DrawDimensions(state);
      DrawDimensionOnCreate(state); // typed-number field for the last entity
      DrawConstraintGlyphs(state);
      DrawToolPrompt(state);
      DrawInContextBanner(state);
    }
    DrawCamLabels(state); // CAM: the derived depth, on the part (job tab)
  }

  // Sketch reference images (v49): textured underlays on the active sketch
  // plane, shown only while editing that sketch. Each image's pixel corners
  // map through its placement into plane-UV, then to world space.
  if (state.underlayRenderer) {
    std::vector<UnderlayDraw> draws;
    if (state.document && state.sketchEdit.IsEditing()) {
      if (SketchFeature* active = ResolveActiveSketch(state)) {
        for (const sketch::ReferenceImage& img : active->images) {
          if (img.hidden || img.widthPx <= 0 || img.heightPx <= 0)
            continue;
          const double c = std::cos(img.rotation), s = std::sin(img.rotation);
          auto pxToWorld = [&](double px, double py) {
            const double x = px * img.unitsPerPixel, y = py * img.unitsPerPixel;
            const glm::vec2 uv(float(img.origin.x + c * x - s * y),
                               float(img.origin.y + s * x + c * y));
            return active->plane.PlaneToWorld(uv);
          };
          UnderlayDraw d;
          d.asset = img.asset;
          d.bytes = state.document->GetAsset(img.asset);
          d.opacity = static_cast<float>(img.opacity);
          d.corners[0] = pxToWorld(0, 0);
          d.corners[1] = pxToWorld(img.widthPx, 0);
          d.corners[2] = pxToWorld(img.widthPx, img.heightPx);
          d.corners[3] = pxToWorld(0, img.heightPx);
          draws.push_back(std::move(d));
        }
      }
    }
    // No underlays this frame (not editing / none visible): free cached
    // textures behind a wait-idle. Cheap no-op once the cache is already empty.
    if (draws.empty())
      state.underlayRenderer->ReleaseTextures();
    state.underlayRenderer->SetImages(draws);
  }

  // Point the renderers, camera, and picking at the central-node region so
  // the 3D scene is centered in the visible area between the panels.
  ImGuiIO& io = ImGui::GetIO();
  const glm::vec2 scale(io.DisplayFramebufferScale.x,
                        io.DisplayFramebufferScale.y);
  const glm::vec2 fbPos = state.viewportPos * scale;
  const glm::vec2 fbSize = state.viewportSize * scale;
  if (fbSize.x > 0 && fbSize.y > 0) {
    // Every renderer that draws into the 3D scene, aimed at the sub-region
    // between the panels — one loop over the registry, in the order it draws.
    // This used to be a hand-written list, and being missing from it is how
    // the FEM field (0346a8e) and the CAM stock (4a3083d) each shipped drawing
    // full-window while the camera aspect was this rectangle.
    for (SceneRenderer* r : state.sceneRenderers)
      r->SetViewport(
          static_cast<int32_t>(fbPos.x), static_cast<int32_t>(fbPos.y),
          static_cast<uint32_t>(fbSize.x), static_cast<uint32_t>(fbSize.y));
    if (state.camera)
      state.camera->SetAspectRatio(fbSize.x / fbSize.y);
  }
}

} // namespace ui
