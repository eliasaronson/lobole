#include "ui/UI.h"

#include "Camera.h"
#include "ui/DockLayout.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h"
#include "ui/MainMenuBar.h"
#include "ui/PropertiesPanel.h"
#include "ui/SketchToolbar.h"

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
