#include "ui/UI.h"

#include "Camera.h"
#include "SketchRenderer.h"
#include "core/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/DockLayout.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h"
#include "ui/MainMenuBar.h"
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
  DrawFileBrowser(state);

  HandleViewportInput(state);

  // Refresh sketch overlays for this frame. Planes show when the toggle is on
  // or while picking a plane; the active sketch and rubber-band preview draw
  // as an overlay on top of the static sketch lines.
  if (state.sketchRenderer) {
    const bool showPlanes = state.showPlanes || state.sketchEdit.pickingPlane;
    std::vector<Vertex3D> quads =
        BuildPlaneQuads(showPlanes, state.sketchEdit.hoveredPlane);

    std::vector<Vertex3D> overlay =
        BuildActiveSketchOverlay(state, state.sketchEdit.activeSketchId);
    BuildSketchOverlay(state, overlay);

    // While editing, draw a grid + subtle fill on the work plane so it's
    // clear what surface the sketch is on.
    if (SketchFeature* active = ResolveActiveSketch(state)) {
      AppendSketchGrid(active->plane, overlay);
      AppendPlaneQuad(active->plane, glm::vec3(0.30f, 0.36f, 0.48f), 5.0f,
                      quads);
    }

    state.sketchRenderer->SetQuads(std::move(quads));
    state.sketchRenderer->SetOverlayLines(std::move(overlay));
  }

  // Keep the camera's aspect ratio in sync with the framebuffer
  if (state.camera && state.window) {
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(state.window, &fbWidth, &fbHeight);
    if (fbWidth > 0 && fbHeight > 0) {
      state.camera->SetAspectRatio(static_cast<float>(fbWidth) /
                                   static_cast<float>(fbHeight));
    }
  }
}

} // namespace ui
