#include "ui/MainMenuBar.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "OutlineRenderer.h"
#include "core/io/Checkpoints.h"
#include "core/Feature.h"
#include "core/io/Serialization.h"
#include "core/features/SketchFeature.h"
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
    state.parts.clear();
    EnsureParts(state, "Part 1");
    MarkProjectOpened(state, ""); // a fresh document has no home yet
  }
#ifdef HAVE_NATIVEFILEDIALOG
  if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
    OpenProjectDialog(state);
  }
  if (ImGui::MenuItem("Save Project As...", "Ctrl+S")) {
    SaveProjectDialog(state);
  }
#endif
  // Checkpoints (F4 v1): snapshot/restore full document states beside the
  // project. Disabled until the project has been saved somewhere.
  if (ImGui::BeginMenu("Checkpoints", !state.projectPath.empty())) {
    const auto list = Checkpoints::List(state.projectPath);
    if (ImGui::MenuItem(
            ("Create \"checkpoint-" + std::to_string(list.size() + 1) + "\"")
                .c_str())) {
      std::string error;
      if (state.document &&
          !Checkpoints::Create(*state.document, state.projectPath,
                               "checkpoint-" + std::to_string(list.size() + 1),
                               &error)) {
        LOG_ERROR("Checkpoint failed: {}", error);
      }
    }
    if (!list.empty())
      ImGui::Separator();
    for (const auto& info : list) {
      if (ImGui::MenuItem(("Restore " + info.name).c_str())) {
        std::string error;
        if (auto loaded = DocumentIO::Load(
                Checkpoints::PathOf(state.projectPath, info.name), &error)) {
          ReplaceDocument(state, std::move(loaded));
          FitCameraToDocument(state);
          MarkProjectRecovered(state, state.projectPath);
        } else {
          LOG_ERROR("Checkpoint restore failed: {}", error);
        }
      }
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Import STEP...", "Ctrl+I")) {
    OpenStepFileDialog(state);
  }
  // Assembly import extracts per-part geometry into the project folder; on
  // an unsaved project the dialog asks where to save first. (From
  // SolidWorks: Save As > STEP AP242, then import that.)
  if (ImGui::MenuItem("Import STEP as Assembly...")) {
    OpenStepAssemblyFileDialog(state);
  }
  if (state.projectPath.empty() && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("You'll be asked to save the project first - part "
                      "geometry is extracted into the project folder");
  }
  // Enabled for a part with bodies, an assembly (visual export walks its
  // occurrences), or a drawing with views (SVG/PDF sheet export).
  const bool canExport =
      (state.document && !state.document->GetBodies().empty()) ||
      (state.assembly && state.assembly->OccurrenceCount() > 0) ||
      (state.drawing && !state.drawing->GetViews().empty());
  if (ImGui::MenuItem("Export...", nullptr, false, canExport)) {
    ExportDialog(state);
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
  // Global undo (Phase V): the active tab's stack OR the cross-tab project-var
  // stack, whichever was most recently mutated (see GlobalUndo).
  const bool canUndo = !pending && CanGlobalUndo(state);
  const bool canRedo = !pending && CanGlobalRedo(state);
  if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
    GlobalUndo(state);
  }
  if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
    GlobalRedo(state);
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
  if (ImGui::MenuItem("Datum Plane", nullptr, false, idle))
    CreateDatumPlane(state);
  if (ImGui::MenuItem("Datum Axis", nullptr, false, idle))
    CreateDatumAxis(state);
  const size_t producers =
      state.document
          ? BodyProducers(*state.document, state.document->FeatureCount()).size()
          : 0;
  if (ImGui::MenuItem("Boolean", nullptr, false, idle && producers >= 2))
    CreateBoolean(state);
  if (ImGui::MenuItem("Mirror", nullptr, false, idle && producers >= 1))
    CreateMirror(state);
  if (ImGui::MenuItem("Pattern", nullptr, false, idle && producers >= 1))
    CreatePattern(state);
  const bool hasBody =
      state.document && !state.document->GetBodies().empty();
  if (ImGui::MenuItem("Fillet", nullptr, false, idle && hasBody))
    CreateFillet(state);
  if (ImGui::MenuItem("Chamfer", nullptr, false, idle && hasBody))
    CreateChamfer(state);
  if (ImGui::MenuItem("Rib", nullptr, false, idle && hasBody))
    CreateRib(state); // needs an open sketch profile too; no-op without one
  if (ImGui::MenuItem("Thread", nullptr, false, idle && hasBody))
    CreateThread(state); // cosmetic; arms the cylindrical-face pick

  // Curve-driven tools (curves-3d): a 3D curve, and the sweep/pipe/loft that
  // consume it. Gated on the references each needs.
  const size_t sketches =
      state.document ? SketchFeatureCount(*state.document) : 0;
  const bool hasPathLike =
      state.document && HasCurveOrSketch(*state.document);
  if (ImGui::MenuItem("3D Curve", nullptr, false, idle))
    CreateCurve3D(state);
  if (ImGui::MenuItem("Pipe", nullptr, false, idle && hasPathLike))
    CreatePipe(state);
  if (ImGui::MenuItem("Sweep", nullptr, false, idle && sketches >= 1))
    CreateSweep(state);
  if (ImGui::MenuItem("Loft", nullptr, false, idle && sketches >= 2))
    CreateLoft(state);
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
      const std::string name = "Sketch " + std::to_string(n);
      const SketchPlane facePlane(c, nrm, u);
      // Attach by the face's stable name when it has one; else bake the plane.
      if (auto ref = FaceElementRef(state, face->meshId, face->faceIndex)) {
        BeginSketchOnFace(state, *ref, facePlane, name);
      } else {
        BeginSketchOnPlane(state, facePlane, name);
      }
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
  if (ImGui::MenuItem("Fit View", "F")) {
    FitCameraToDocument(state);
  }
  if (ImGui::MenuItem("Zoom to Selection", "Shift+F")) {
    FitCameraToSelection(state);
  }
  ImGui::MenuItem("Show Planes", nullptr, &state.showPlanes);
  ImGui::MenuItem("Shadows", nullptr, &state.showShadows);
  ImGui::MenuItem("Ambient Occlusion", nullptr, &state.showAmbientOcclusion);
  // S3 quality settings; session state, mirrored by set_render_options.
  // The main loop recreates the GPU targets when these change.
  if (ImGui::BeginMenu("Render Quality")) {
    if (ImGui::BeginMenu("Shadow Map")) {
      if (ImGui::MenuItem("1024", nullptr, state.shadowMapSize == 1024))
        state.shadowMapSize = 1024;
      if (ImGui::MenuItem("2048", nullptr, state.shadowMapSize == 2048))
        state.shadowMapSize = 2048;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("SSAO")) {
      const bool on = state.showAmbientOcclusion;
      if (ImGui::MenuItem("Off", nullptr, !on))
        state.showAmbientOcclusion = false;
      if (ImGui::MenuItem("Half Resolution", nullptr, on && !state.ssaoFullRes)) {
        state.showAmbientOcclusion = true;
        state.ssaoFullRes = false;
      }
      if (ImGui::MenuItem("Full Resolution", nullptr, on && state.ssaoFullRes)) {
        state.showAmbientOcclusion = true;
        state.ssaoFullRes = true;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  ImGui::MenuItem("Show Element Names", nullptr, &state.showElementNames);
  ImGui::MenuItem("Viewport Controls", nullptr, &state.showControlsHelp);
  if (state.topoGhostStudyId != 0 &&
      ImGui::MenuItem("Hide Topology Ghost")) {
    // The translucent optimized-shape overlay a topology study leaves for
    // re-modeling; discoverable off-switch.
    state.topoGhostStudyId = 0;
    state.syncedDocumentVersion = UINT64_MAX;
  }

  // The Shift+1..7 views, discoverable (U5).
  if (ImGui::BeginMenu("Standard Views")) {
    const auto view = [&](const char* name, glm::vec3 n,
                          const char* shortcut) {
      if (ImGui::MenuItem(name, shortcut) && state.camera)
        state.camera->AlignToFace(glm::vec3(0.0f), n);
    };
    view("Front", {0, 0, 1}, "Shift+1");
    view("Back", {0, 0, -1}, "Shift+2");
    view("Left", {-1, 0, 0}, "Shift+3");
    view("Right", {1, 0, 0}, "Shift+4");
    view("Top", {0, 1, 0}, "Shift+5");
    view("Bottom", {0, -1, 0}, "Shift+6");
    view("Isometric", glm::normalize(glm::vec3(1, 1, 1)), "Shift+7");
    ImGui::EndMenu();
  }

  // Session font-size setting (U5); applied via the DPI rescale path.
  if (ImGui::BeginMenu("Font Size")) {
    const auto size = [&](const char* name, float scale) {
      if (ImGui::MenuItem(name, nullptr, state.uiFontScale == scale)) {
        state.uiFontScale = scale;
        state.fontRebuildRequested = true;
      }
    };
    size("Small", 0.85f);
    size("Default", 1.0f);
    size("Large", 1.2f);
    size("Extra Large", 1.4f);
    ImGui::EndMenu();
  }

  // Section view (I3): a viewport clipping plane. Cut on a base plane; drag
  // the offset. Zero sectionPlane = off.
  if (ImGui::BeginMenu("Section View")) {
    // sectionPlane = {normal.xyz, distance}; base-plane normal along one axis
    // makes distance == the offset.
    bool on = state.sectionPlane != glm::vec4(0.0f);
    static int planeIdx = 0; // 0 XY, 1 XZ, 2 YZ
    static float offset = 0.0f;
    const glm::vec3 normals[3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}};
    auto set = [&] {
      state.sectionPlane =
          on ? glm::vec4(normals[planeIdx], offset) : glm::vec4(0.0f);
    };
    if (ImGui::MenuItem("Enabled", nullptr, &on))
      set();
    const char* planes[] = {"XY (cut along Z)", "XZ (cut along Y)",
                            "YZ (cut along X)"};
    if (ImGui::Combo("Plane", &planeIdx, planes, 3) && on)
      set();
    if (ImGui::DragFloat("Offset", &offset, 0.1f) && on)
      set();
    ImGui::EndMenu();
  }

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
    GlobalUndo(state);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route) ||
      ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route)) {
    GlobalRedo(state);
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
