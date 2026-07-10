#include "ui/MainMenuBar.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "OutlineRenderer.h"
#include "core/Feature.h"
#include "core/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

namespace {

void DrawFileMenu(AppState& state) {
  if (!ImGui::BeginMenu("File"))
    return;

  if (ImGui::MenuItem("New", "Ctrl+N")) {
    ReplaceDocument(state, std::make_unique<Document>());
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
    OpenProjectDialog(state);
  }
  if (ImGui::MenuItem("Save Project As...", "Ctrl+S")) {
    SaveProjectDialog(state);
  }
#endif
  ImGui::Separator();
  if (ImGui::MenuItem("Import STEP...", "Ctrl+I")) {
    OpenStepFileDialog(state);
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Exit")) {
    glfwSetWindowShouldClose(state.window, GLFW_TRUE);
  }
  ImGui::EndMenu();
}

void DrawEditMenu(AppState& state) {
  if (!ImGui::BeginMenu("Edit"))
    return;

  const bool pending = HasPendingFeature(state);
  const bool canUndo = !pending && state.commands && state.commands->CanUndo();
  const bool canRedo = !pending && state.commands && state.commands->CanRedo();
  if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
    state.commands->Undo(*state.document);
  }
  if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
    state.commands->Redo(*state.document);
  }
  ImGui::EndMenu();
}

void DrawCreateMenu(AppState& state) {
  if (!ImGui::BeginMenu("Create"))
    return;

  // Creation is disabled while a feature preview is pending.
  const bool idle = !HasPendingFeature(state);
  if (ImGui::MenuItem("Box", nullptr, false, idle))
    CreateBox(state);
  if (ImGui::MenuItem("Sphere", nullptr, false, idle))
    CreateSphere(state);
  if (ImGui::MenuItem("Cylinder", nullptr, false, idle))
    CreateCylinder(state);
  ImGui::Separator();

  SketchEditState& edit = state.sketchEdit;
  if (ImGui::BeginMenu("Sketch")) {
    if (ImGui::MenuItem("New Sketch...")) {
      StartNewSketch(state);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Sketch on Front Plane")) {
      const int n = static_cast<int>(state.document->FeatureCount()) + 1;
      BeginSketchOnPlane(state, DefaultPlane(0), "Sketch " + std::to_string(n));
    }
    if (ImGui::MenuItem("Sketch on Top Plane")) {
      const int n = static_cast<int>(state.document->FeatureCount()) + 1;
      BeginSketchOnPlane(state, DefaultPlane(1), "Sketch " + std::to_string(n));
    }
    if (ImGui::MenuItem("Sketch on Right Plane")) {
      const int n = static_cast<int>(state.document->FeatureCount()) + 1;
      BeginSketchOnPlane(state, DefaultPlane(2), "Sketch " + std::to_string(n));
    }

    ImGui::Separator();

    const bool hasSelectedFace =
        state.faceSelector && state.faceSelector->GetSelectedFace() != nullptr;
    if (ImGui::MenuItem("Sketch on Selected Face", nullptr, false,
                        hasSelectedFace && !edit.IsEditing())) {
      const Face* face = state.faceSelector->GetSelectedFace();
      glm::vec3 c =
          state.faceSelector->GetFaceCenter(face->meshId, face->faceIndex);
      glm::vec3 nrm =
          state.faceSelector->GetFaceNormal(face->meshId, face->faceIndex);
      glm::vec3 u =
          std::abs(nrm.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
      u = glm::normalize(u - glm::dot(u, nrm) * nrm);
      const int n = static_cast<int>(state.document->FeatureCount()) + 1;
      BeginSketchOnPlane(state, SketchPlane(c, nrm, u),
                         "Sketch " + std::to_string(n));
    }

    if (ImGui::MenuItem("Exit Sketch", nullptr, false, edit.IsEditing())) {
      edit = SketchEditState{};
    }

    ImGui::EndMenu();
  }

  ImGui::EndMenu();
}

void DrawViewMenu(AppState& state) {
  if (!ImGui::BeginMenu("View"))
    return;

  if (ImGui::MenuItem("Reset Camera")) {
    if (state.camera) {
      state.camera->SetPosition(glm::vec3(5.0f, 5.0f, 5.0f));
      state.camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
      // Trackball orbit carries the up vector as state, so it must be reset too
      // — otherwise a tumbled view leaves up near-parallel to the new forward
      // and the reset view is degenerate (right = forward x up collapses).
      state.camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    }
  }
  if (ImGui::MenuItem("Fit View")) {
    FitCameraToDocument(state);
  }
  ImGui::MenuItem("Show Planes", nullptr, &state.showPlanes);

  if (ImGui::MenuItem("Reset Layout")) {
    // Rebuild the default docking layout next frame — recovers from a panel
    // dragged huge or a stale imgui.ini.
    state.buildDefaultDockLayout = true;
  }

  ImGui::Separator();

  if (state.outlineRenderer && state.outlineRenderer->IsInitialized()) {
    if (ImGui::BeginMenu("Outline Rendering")) {
      OutlineSettings settings = state.outlineRenderer->GetOutlineSettings();
      bool changed = false;

      const char* outlineModes[] = {"None", "Edges", "Wireframe",
                                    "Silhouette"};
      int currentMode = static_cast<int>(settings.mode);
      if (ImGui::Combo("Mode", &currentMode, outlineModes, 4)) {
        settings.mode = static_cast<OutlineMode>(currentMode);
        changed = true;
      }

      if (settings.mode != OutlineMode::NONE) {
        changed |= ImGui::ColorEdit3("Outline Color", &settings.outlineColor.x);
        changed |=
            ImGui::SliderFloat("Outline Width", &settings.outlineWidth, 0.1f,
                               5.0f);
        if (settings.mode == OutlineMode::WIREFRAME) {
          changed |=
              ImGui::ColorEdit3("Wireframe Color", &settings.wireframeColor.x);
          changed |= ImGui::SliderFloat("Line Width", &settings.lineWidth,
                                        0.5f, 3.0f);
        }
        changed |= ImGui::Checkbox("Depth Test", &settings.enableDepthTest);
        changed |= ImGui::Checkbox("Blending", &settings.enableBlending);
      }

      if (changed) {
        state.outlineRenderer->SetOutlineSettings(settings);
      }
      ImGui::EndMenu();
    }
  }

  ImGui::EndMenu();
}

// Global keyboard shortcuts. Registered every frame; ImGui's routing lets a
// focused text field claim chords like Ctrl+Z for itself first.
void HandleShortcuts(AppState& state) {
  const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;

  // While a feature preview is pending, freeze the undo stack: Ctrl+Z / Escape
  // cancel the preview instead of touching history.
  if (HasPendingFeature(state)) {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route))
      CancelPendingFeature(state);
    return;
  }

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) {
    if (state.commands && state.commands->CanUndo())
      state.commands->Undo(*state.document);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route)) {
    if (state.commands && state.commands->CanRedo())
      state.commands->Redo(*state.document);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route)) {
    ReplaceDocument(state, std::make_unique<Document>());
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) {
    OpenProjectDialog(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) {
    SaveProjectDialog(state);
  }
#endif
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I, route)) {
    OpenStepFileDialog(state);
  }
}

} // namespace

void DrawMainMenuBar(AppState& state) {
  HandleShortcuts(state);

  if (!ImGui::BeginMainMenuBar())
    return;

  DrawFileMenu(state);
  DrawEditMenu(state);
  DrawCreateMenu(state);
  DrawViewMenu(state);

  ImGui::EndMainMenuBar();
}

} // namespace ui
