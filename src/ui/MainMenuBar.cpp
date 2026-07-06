#include "ui/MainMenuBar.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "OutlineRenderer.h"
#include "RayTracingRenderer.h"
#include "core/Feature.h"
#include "core/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
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

  const bool canUndo = state.commands && state.commands->CanUndo();
  const bool canRedo = state.commands && state.commands->CanRedo();
  if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
    state.commands->Undo(*state.document);
  }
  if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
    state.commands->Redo(*state.document);
  }
  ImGui::EndMenu();
}

void AddPrimitive(AppState& state, std::unique_ptr<Feature> feature) {
  if (state.document && state.commands) {
    state.commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(feature)),
        *state.document);
  }
}

void DrawCreateMenu(AppState& state) {
  if (!ImGui::BeginMenu("Create"))
    return;

  const int n =
      state.document ? static_cast<int>(state.document->FeatureCount()) + 1 : 1;
  const glm::vec3 spot(-4.0f + (n - 1) * 2.5f, 0.0f, 0.0f);

  if (ImGui::MenuItem("Box")) {
    AddPrimitive(state, std::make_unique<BoxFeature>(
                            "Box " + std::to_string(n), spot, 2.0, 2.0, 2.0));
  }
  if (ImGui::MenuItem("Sphere")) {
    AddPrimitive(state, std::make_unique<SphereFeature>(
                            "Sphere " + std::to_string(n), spot, 1.0));
  }
  if (ImGui::MenuItem("Cylinder")) {
    AddPrimitive(state,
                 std::make_unique<CylinderFeature>(
                     "Cylinder " + std::to_string(n),
                     spot + glm::vec3(0.0f, -1.0f, 0.0f),
                     glm::vec3(0.0f, 1.0f, 0.0f), 0.8, 2.0));
  }
  ImGui::Separator();

  SketchEditState& edit = state.sketchEdit;
  if (ImGui::BeginMenu("Sketch")) {
    if (ImGui::MenuItem("New Sketch...")) {
      edit = SketchEditState{};
      edit.pickingPlane = true; // pick a plane/face in the viewport
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
    }
  }
  if (ImGui::MenuItem("Fit View")) {
    FitCameraToDocument(state);
  }
  ImGui::MenuItem("Show Planes", nullptr, &state.showPlanes);

  ImGui::Separator();

  static bool useRayTracing = false;
  if (ImGui::Checkbox("Ray Tracing", &useRayTracing)) {
    if (state.rayTracer && state.rayTracer->IsInitialized()) {
      state.rayTracer->SetRenderMode(useRayTracing);
    } else {
      useRayTracing = false;
      LOG_WARN("Ray Tracing: not available on this hardware");
    }
  }
  if (state.rayTracer && state.rayTracer->IsInitialized()) {
    if (ImGui::BeginMenu("Ray Tracing Settings")) {
      static int maxDepth = 4;
      if (ImGui::SliderInt("Max Ray Depth", &maxDepth, 1, 10)) {
        state.rayTracer->SetMaxRayDepth(maxDepth);
      }
      static int samples = 1;
      if (ImGui::SliderInt("Samples Per Pixel", &samples, 1, 4)) {
        state.rayTracer->SetSamplesPerPixel(samples);
      }
      ImGui::EndMenu();
    }
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
