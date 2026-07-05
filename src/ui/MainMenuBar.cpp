#include "ui/MainMenuBar.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "OutlineRenderer.h"
#include "RayTracingRenderer.h"
#include "SketchManager.h"
#include "core/Feature.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"

#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace ui {

namespace {

void DrawFileMenu(AppState& state) {
  if (!ImGui::BeginMenu("File"))
    return;

  if (ImGui::MenuItem("New")) {
    ReplaceDocument(state, std::make_unique<Document>());
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::MenuItem("Open Project...")) {
    OpenProjectDialog(state);
  }
  if (ImGui::MenuItem("Save Project As...")) {
    SaveProjectDialog(state);
  }
#endif
  ImGui::Separator();
  if (ImGui::MenuItem("Import STEP...")) {
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

  SketchManager* sketch = state.sketchManager;
  if (ImGui::BeginMenu("Sketch")) {
    if (ImGui::MenuItem("Sketch on Front Plane") && sketch) {
      sketch->StartSketch(PlaneType::XY);
    }
    if (ImGui::MenuItem("Sketch on Top Plane") && sketch) {
      sketch->StartSketch(PlaneType::XZ);
    }
    if (ImGui::MenuItem("Sketch on Right Plane") && sketch) {
      sketch->StartSketch(PlaneType::YZ);
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Exit Sketch", nullptr, false,
                        sketch && sketch->IsSketchMode())) {
      sketch->ExitSketch();
    }

    ImGui::Separator();

    const bool hasSelectedFace =
        state.faceSelector && state.faceSelector->GetSelectedFace() != nullptr;
    if (ImGui::MenuItem("Sketch on Selected Face", nullptr, false,
                        hasSelectedFace &&
                            (!sketch || !sketch->IsSketchMode()))) {
      if (sketch && state.faceSelector && state.camera) {
        if (const Face* face = state.faceSelector->GetSelectedFace()) {
          glm::vec3 faceCenter =
              state.faceSelector->GetFaceCenter(face->meshId, face->faceIndex);
          glm::vec3 faceNormal =
              state.faceSelector->GetFaceNormal(face->meshId, face->faceIndex);
          state.camera->AlignToFace(faceCenter, faceNormal, 5.0f);
          sketch->StartSketch(PlaneType::XY); // Will be customized for face
          LOG_INFO("Started sketch on selected face");
        }
      }
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

} // namespace

void DrawMainMenuBar(AppState& state) {
  if (!ImGui::BeginMainMenuBar())
    return;

  DrawFileMenu(state);
  DrawEditMenu(state);
  DrawCreateMenu(state);
  DrawViewMenu(state);

  ImGui::EndMainMenuBar();
}

} // namespace ui
