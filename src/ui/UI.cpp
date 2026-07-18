#include "ui/UI.h"

#include "Camera.h"
#include "FieldRenderer.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "SketchRenderer.h"
#include "core/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/DockLayout.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h"
#include "ui/MainMenuBar.h"
#include "ui/PreviewFeature.h"
#include "ui/PropertiesPanel.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/CommandPalette.h"
#include "ui/ConfigurationsPanel.h"
#include "ui/VariablesPanel.h"
#include "ui/ViewportInput.h"

#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>

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
                           ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoDocking;
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
  const char* prompt = ArmedPrompt(state);
  if (prompt == nullptr)
    return; // feedback shows only while its tool is armed
  const bool hasErr = !state.sketchEdit.actionMsg.empty();

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float cx = state.viewportPos.x + state.viewportSize.x * 0.5f;
  float y = state.viewportPos.y + 34.0f;
  const float padX = 10.0f, padY = 5.0f;
  const auto pill = [&](const std::string& text, ImU32 bg, ImU32 fg) {
    if (text.empty())
      return;
    const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
    const ImVec2 p0(cx - sz.x * 0.5f - padX, y);
    const ImVec2 p1(cx + sz.x * 0.5f + padX, y + sz.y + padY * 2.0f);
    dl->AddRectFilled(p0, p1, bg, (p1.y - p0.y) * 0.5f);
    dl->AddText(ImVec2(p0.x + padX, p0.y + padY), fg, text.c_str());
    y = p1.y + 6.0f;
  };
  if (prompt != nullptr)
    pill(prompt, IM_COL32(24, 30, 38, 235), IM_COL32(210, 220, 235, 255));
  if (hasErr)
    pill(state.sketchEdit.actionMsg, IM_COL32(56, 26, 26, 235),
         IM_COL32(240, 180, 170, 255));
}

// Axis triad (U5): a bottom-right mini-gizmo showing the camera's world
// orientation — X red, Y green, Z blue, back-facing axes drawn first and
// dimmed. Pure overlay (Shift+1..7 / View > Standard Views reorient).
void DrawAxisTriad(AppState& state) {
  if (!state.camera || state.drawing)
    return;
  const float r = 34.0f;
  const ImVec2 c(state.viewportPos.x + state.viewportSize.x - r - 18.0f,
                 state.viewportPos.y + state.viewportSize.y - r - 18.0f);
  const glm::mat3 view = glm::mat3(state.camera->GetViewMatrix());
  struct Axis {
    glm::vec3 v; // view-space direction
    ImU32 color;
    const char* label;
  };
  Axis axes[3] = {
      {view * glm::vec3(1, 0, 0), IM_COL32(235, 90, 90, 255), "X"},
      {view * glm::vec3(0, 1, 0), IM_COL32(110, 220, 110, 255), "Y"},
      {view * glm::vec3(0, 0, 1), IM_COL32(100, 150, 250, 255), "Z"}};
  std::sort(std::begin(axes), std::end(axes),
            [](const Axis& a, const Axis& b) { return a.v.z < b.v.z; });
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddCircleFilled(c, r + 10.0f, IM_COL32(20, 24, 30, 140));
  for (const Axis& a : axes) {
    const ImVec2 tip(c.x + a.v.x * r, c.y - a.v.y * r);
    const bool front = a.v.z >= 0.0f; // toward the camera
    const ImU32 col =
        front ? a.color : (a.color & 0x00FFFFFF) | (110u << 24);
    dl->AddLine(c, tip, col, front ? 2.4f : 1.4f);
    const ImVec2 sz = ImGui::CalcTextSize(a.label);
    dl->AddText(ImVec2(tip.x - sz.x * 0.5f + a.v.x * 8.0f,
                       tip.y - sz.y * 0.5f - a.v.y * 8.0f),
                col, a.label);
  }
}

// Empty-state hint (U5): a fresh part tells the user where to start instead
// of showing three anonymous planes.
void DrawEmptyStateHint(AppState& state) {
  if (!state.document || state.document->FeatureCount() > 0 ||
      state.sketchEdit.IsEditing() || state.sketchEdit.pickingPlane ||
      HasPendingFeature(state)) {
    return;
  }
  const char* line1 = "No features yet";
  const char* line2 =
      "Double-click a plane (or press S) to sketch - or Create > Box";
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
void DrawPartTabs(AppState& state) {
  EnsureParts(state);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = ImGui::GetFrameHeightWithSpacing() + 4.0f;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y +
                                                    vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoDocking;
  if (ImGui::Begin("##PartTabs", nullptr, flags)) {
    for (size_t i = 0; i < state.parts.size(); i++) {
      if (i > 0)
        ImGui::SameLine();
      const bool active = static_cast<int>(i) == state.activePart;
      if (active)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button((state.parts[i].name + "##part" + std::to_string(i))
                            .c_str()))
        SetActivePart(state, i);
      if (active)
        ImGui::PopStyleColor();
      // Right-click a tab to delete it (never the last).
      if (ImGui::BeginPopupContextItem(
              ("##pctx" + std::to_string(i)).c_str())) {
        const char* label = state.parts[i].kind == DocKind::Assembly
                                ? "Delete assembly"
                            : state.parts[i].kind == DocKind::Drawing
                                ? "Delete drawing"
                                : "Delete part";
        if (ImGui::MenuItem(label, nullptr, false, state.parts.size() > 1))
          DeletePart(state, i);
        ImGui::EndPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("+Part##addpart"))
      AddPart(state, "Part " + std::to_string(state.parts.size() + 1));
    ImGui::SameLine();
    if (ImGui::Button("+Asm##addasm"))
      AddAssembly(state, "Assembly " + std::to_string(state.parts.size() + 1));
    ImGui::SameLine();
    if (ImGui::Button("+Dwg##adddwg"))
      AddDrawing(state, "Drawing " + std::to_string(state.parts.size() + 1));
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
    DrawVariablesPanel(state);
    DrawConfigurationsPanel(state);
    DrawPropertiesPanel(state);
    DrawSketchToolbar(state);
  } else if (state.assembly) {
    DrawOccurrencePanel(state); // occurrence list + editor in the Model panel
  } else if (state.drawing) {
    DrawSheetPanel(state); // sheet settings + title block in the Model panel
  }
  DrawPreviewDialog(state);
  DrawFileBrowser(state);
  DrawControlsHelpOverlay(state);
  DrawModelToolPrompt(state);
  DrawEmptyStateHint(state);
  DrawAxisTriad(state);
  DrawCommandPalette(state);

  HandleViewportInput(state);
  // Renderer highlights are a projection of the Selection (U1b): one sync
  // point after input may have changed the set.
  SyncSelectionHighlights(state);

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
      quads = BuildPlaneQuads(EffectivePlaneVisibility(state),
                              state.sketchEdit.hoveredPlane);
      AppendDatumPlaneQuads(state, quads); // user datum planes (own visibility)
      overlay =
          BuildActiveSketchOverlay(state, state.sketchEdit.activeSketchId);
      BuildSketchOverlay(state, overlay);
      AppendDatumAxisLines(state, overlay); // user datum axes as line segments

      // While editing, draw a grid + subtle fill on the work plane so it's
      // clear what surface the sketch is on.
      if (SketchFeature* active = ResolveActiveSketch(state)) {
        AppendSketchGrid(active->plane, overlay);
        AppendPlaneQuad(active->plane, glm::vec3(0.30f, 0.36f, 0.48f), 5.0f,
                        quads);
      }
    }
    AppendOriginGizmo(overlay); // world origin + axes: parts and assemblies
    BuildMateToolOverlay(state, overlay); // A3 mate connector triads (assembly)

    // The occurrence drag gizmo goes to its own no-depth channel so its handles
    // stay visible/grabbable through geometry (A2.5).
    std::vector<LineVertex3D> gizmo;
    BuildGizmoOverlay(state, gizmo);
    AppendCogMarker(state, gizmo); // mass-props CoG cross (no-depth channel)
    AppendStudyMarker(state, gizmo); // FEM peak-field cross (no-depth channel)

    state.sketchRenderer->SetQuads(std::move(quads));
    state.sketchRenderer->SetOverlayLines(std::move(overlay));
    state.sketchRenderer->SetGizmoLines(std::move(gizmo));

    // The sketch lines, edit primitives, and 2D annotations below are all
    // part-tab geometry. On an assembly tab clear them and skip the overlays.
    if (state.document) {
      // Inactive sketches as thin lines; the active sketch as thick AA
      // primitives with vertex dots and hover/selection emphasis. Rebuilt each
      // frame so edit-mode, selection, and hover changes show immediately.
      UpdateSketchStatus(state); // DOF + conflicts for coloring/feedback
      state.sketchRenderer->SetStaticLines(
          BuildStaticSketchLines(state, state.sketchEdit.activeSketchId));
      std::vector<PrimVertex> prims =
          BuildActiveSketchPrimitives(state, state.sketchEdit.activeSketchId);
      AppendEdgePickPrimitives(state, prims); // fillet/chamfer edge picking
      AppendSelectionPrimitives(state, prims); // selected/hovered edges (U1b)
      AppendRegionPickPrimitives(state, prims); // armed Profiles slot (U2)
      state.sketchRenderer->SetPrimitives(std::move(prims));

      // Auto-constraint snap indicator + ghost glyphs, and dimension
      // annotations (2D overlays).
      DrawSnapGlyphs(state);
      DrawSelectionBox(state);
      DrawDatumLabels(state);
      DrawElementNameOverlay(state); // toponaming debug: hovered face's name
      DrawDimensions(state);
      DrawConstraintGlyphs(state);
      DrawToolPrompt(state);
      DrawInContextBanner(state);
    } else {
      state.sketchRenderer->SetStaticLines({});
      state.sketchRenderer->SetPrimitives({});
    }
  }

  // Point the renderers, camera, and picking at the central-node region so
  // the 3D scene is centered in the visible area between the panels.
  ImGuiIO& io = ImGui::GetIO();
  const glm::vec2 scale(io.DisplayFramebufferScale.x,
                        io.DisplayFramebufferScale.y);
  const glm::vec2 fbPos = state.viewportPos * scale;
  const glm::vec2 fbSize = state.viewportSize * scale;
  if (fbSize.x > 0 && fbSize.y > 0) {
    auto setVp = [&](auto* r) {
      if (r)
        r->SetViewport(static_cast<int32_t>(fbPos.x),
                       static_cast<int32_t>(fbPos.y),
                       static_cast<uint32_t>(fbSize.x),
                       static_cast<uint32_t>(fbSize.y));
    };
    setVp(state.meshRenderer);
    setVp(state.outlineRenderer);
    setVp(state.sketchRenderer);
    setVp(state.fieldRenderer);  // FEM/topology overlay — else it renders full-
                                 // window while the camera aspect is the sub-region
    if (state.camera)
      state.camera->SetAspectRatio(fbSize.x / fbSize.y);
  }
}

} // namespace ui
