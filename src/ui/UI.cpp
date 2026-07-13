#include "ui/UI.h"

#include "Camera.h"
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
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/VariablesPanel.h"
#include "ui/ViewportInput.h"

#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

namespace {
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
        if (ImGui::MenuItem("Delete part", nullptr, false,
                            state.parts.size() > 1))
          DeletePart(state, i);
        ImGui::EndPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("+##addpart"))
      AddPart(state, "Part " + std::to_string(state.parts.size() + 1));
  }
  ImGui::End();
}
} // namespace

void DrawUI(AppState& state) {
  // One place to keep the selection valid across delete/undo/File > New
  if (state.document &&
      state.selectedFeature >=
          static_cast<int>(state.document->FeatureCount())) {
    state.selectedFeature = -1;
  }

  DrawMainMenuBar(state);
  BeginDockspace(state);
  DrawPartTabs(state);

  DrawFeatureTreePanel(state);
  DrawVariablesPanel(state);
  DrawPropertiesPanel(state);
  DrawSketchToolbar(state);
  DrawPreviewDialog(state);
  DrawFileBrowser(state);

  HandleViewportInput(state);

  // Refresh sketch overlays for this frame. Planes show when the toggle is on
  // or while picking a plane; the active sketch and rubber-band preview draw
  // as an overlay on top of the static sketch lines.
  if (state.sketchRenderer) {
    std::vector<Vertex3D> quads = BuildPlaneQuads(
        EffectivePlaneVisibility(state), state.sketchEdit.hoveredPlane);
    AppendDatumPlaneQuads(state, quads); // user datum planes (own visibility)

    std::vector<Vertex3D> overlay =
        BuildActiveSketchOverlay(state, state.sketchEdit.activeSketchId);
    BuildSketchOverlay(state, overlay);
    AppendOriginGizmo(overlay); // always show the world origin + axes
    AppendDatumAxisLines(state, overlay); // user datum axes as line segments

    // While editing, draw a grid + subtle fill on the work plane so it's
    // clear what surface the sketch is on.
    if (SketchFeature* active = ResolveActiveSketch(state)) {
      AppendSketchGrid(active->plane, overlay);
      AppendPlaneQuad(active->plane, glm::vec3(0.30f, 0.36f, 0.48f), 5.0f,
                      quads);
    }

    state.sketchRenderer->SetQuads(std::move(quads));
    state.sketchRenderer->SetOverlayLines(std::move(overlay));
    // Inactive sketches as thin lines; the active sketch as thick AA primitives
    // with vertex dots and hover/selection emphasis. Rebuilt each frame so
    // edit-mode, selection, and hover changes show immediately.
    UpdateSketchStatus(state); // DOF + conflicts for coloring/feedback
    state.sketchRenderer->SetStaticLines(
        BuildStaticSketchLines(state, state.sketchEdit.activeSketchId));
    std::vector<PrimVertex> prims =
        BuildActiveSketchPrimitives(state, state.sketchEdit.activeSketchId);
    AppendEdgePickPrimitives(state, prims); // fillet/chamfer edge picking
    state.sketchRenderer->SetPrimitives(std::move(prims));

    // Auto-constraint snap indicator + ghost glyphs, and dimension annotations
    // (2D overlays).
    DrawSnapGlyphs(state);
    DrawSelectionBox(state);
    DrawDatumLabels(state);
    DrawElementNameOverlay(state); // toponaming debug: hovered face's name
    DrawDimensions(state);
    DrawConstraintGlyphs(state);
    DrawToolPrompt(state);
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
    if (state.camera)
      state.camera->SetAspectRatio(fbSize.x / fbSize.y);
  }
}

} // namespace ui
