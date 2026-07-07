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
#include "ui/ViewportInput.h"

#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

void DrawUI(AppState& state) {
  // One place to keep the selection valid across delete/undo/File > New
  if (state.document &&
      state.selectedFeature >=
          static_cast<int>(state.document->FeatureCount())) {
    state.selectedFeature = -1;
  }

  DrawMainMenuBar(state);
  BeginDockspace(state);

  DrawFeatureTreePanel(state);
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

    std::vector<Vertex3D> overlay =
        BuildActiveSketchOverlay(state, state.sketchEdit.activeSketchId);
    BuildSketchOverlay(state, overlay);
    AppendOriginGizmo(overlay); // always show the world origin + axes

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
    state.sketchRenderer->SetStaticLines(
        BuildStaticSketchLines(state, state.sketchEdit.activeSketchId));
    state.sketchRenderer->SetPrimitives(
        BuildActiveSketchPrimitives(state, state.sketchEdit.activeSketchId));
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
