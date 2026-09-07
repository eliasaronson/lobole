#pragma once

// Shared fixture for the GUI test suite, which is split across
// src/app/guitests/GuiTests<Domain>.cpp. These were file-static helpers inside
// the single 8,036-line RegisterGuiTests; splitting the suite by domain means
// they need external linkage and, for the two pieces of STATE (s_App and the
// original-TestFunc side table), exactly one definition — hence this header
// plus GuiTestsCommon.cpp rather than a header-only include.

#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamPost.h"
#include "core/drawing/DrawingValidate.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/fem/FemStudy.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "remote/RemoteDrawings.h"
#include "render/Camera.h"
#include "render/FrameTimer.h"
#include "render/OutlineRenderer.h" // instance/asset counts (outline gates)
#include "render/RayTracingRenderer.h"
#include "ui/AppState.h"
#include "ui/CamPanel.h"
#include "ui/CamTool.h"
#include "ui/Commands.h"
#include "ui/ConflictsPanel.h"
#include "ui/DockLayout.h"
#include "ui/DragMath.h"
#include "ui/FeatureEditor.h" // kCamToolSlot
#include "ui/FileDialogs.h"
#include "ui/HistoryPanel.h"
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h"
#include "ui/PreviewHandles.h"
#include "ui/SettingsWindow.h"
#include "ui/SketchInteraction.h"
#include "ui/SketchToolbar.h"
#include "ui/TerminalPanel.h"

#include "IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

// Tests run inside the live application and reach it through the AppState.
// Named s_App for continuity: every test body refers to it by this name.
extern ui::AppState* s_App;

Document* Doc();

// --- CAM tests: the job tab (docs/cam/job-document.md) ---------------------
//
// CAM setups moved off the part Document onto a job document of their own, so
// a CAM test now builds a part and then MACHINES it: two tabs, not one.

// The active job, or null when the active tab is not a job.
cam::JobDocument* Job();
// The active job, creating one for the active PART if there is none. Every
// CAM test starts by building a part and cutting it, which is exactly what
// ui::CreateJobForPart does in a step.
cam::JobDocument* MakeJob();
// The part being machined: the active document on a part tab, the job's first
// source part on a job tab. A CAM test builds one part and cuts it, so this
// stays a single answer either side of MakeJob().
Document* CamPart();

// Between-test reset. The ImGui Test Engine runs every test back to back in
// one process, so state a test leaves behind leaks into the next one — most
// harmful is the camera zoom: a viewport-click test that passes alone fails
// mid-suite when a predecessor left the camera zoomed in and its UV click
// coordinates then project off-screen. Restoring the app's initial
// interaction state before each test makes full-suite runs behave like
// isolated ones. Applied as a TestFunc wrapper by RegisterGuiTests, so
// individual tests don't opt in.
void ResetAppForTest();

// Original TestFunc for each registered test; RunTestWithReset runs
// ResetAppForTest() then dispatches to it (TestFunc is a raw function pointer
// in this build, so we can't capture — hence the side table).
extern std::unordered_map<ImGuiTest*, void (*)(ImGuiTestContext*)>
    g_origTestFunc;
void RunTestWithReset(ImGuiTestContext* ctx);

// Window position of a world point, mapped through the central-node viewport
// rect (where the 3D scene actually renders), matching the picking math.
ImVec2 WorldToWindow(const glm::vec3& p);

// Standoff factor for a test that pins the camera to make a pick land on a
// known pixel. Pick tolerances are in PIXELS, so what has to be pinned is the
// MAGNIFICATION — and the same standoff shows a different amount of the model
// through a different lens. Multiply the distance a 45-degree lens wanted by
// this and the framing survives a change to Camera::kDefaultFovDegrees.
float LensScale();

// Click a model-toolbar icon button by its stable "###tb_*" id.
void ToolbarClick(ImGuiTestContext* ctx, const char* tbId);
// Create from the toolbar, then accept the preview (the real accept path).
void ToolbarCreateAccept(ImGuiTestContext* ctx, const char* tbId);

void BuildCantileverStudy(ImGuiTestContext* ctx, double height);
bool WaitForStudyResult(ImGuiTestContext* ctx, uint64_t studyId);

// --- U10 defaults gate: the tool table and its preconditions ---------------
enum class Needs {
  Nothing,
  Body,
  BodyAndOpenSketch,
  TwoBodies,
  ClosedSketch,
  TwoSketches,
  ProfileAndPath
};
enum class Expect {
  Changes,            // accept with defaults => the model is different
  RefusesUntilPicked, // Accept is disabled and the window says what is missing
  ArmsAPick,          // not a preview: the tool asks for a face/plane first
  HarmlessNoOp,       // accepting cleanly changes nothing, and that is correct
};
struct ToolCase {
  const char* name; // test name = the id without "###tb_"
  const char* id;
  Needs needs;
  Expect expect;
};
extern const ToolCase kToolCases[26];
const ToolCase* FindToolCase(const char* testName);

void EnterHoleSketchOnPlane(ImGuiTestContext* ctx, int planeIdx);
void EnterSketchOnPlane(ImGuiTestContext* ctx, int planeIdx);
void AddRectSketch(ImGuiTestContext* ctx, int planeIdx, float half,
                   float centreU = 0.0f);
void AddPathSketch(ImGuiTestContext* ctx, int planeIdx);
void SetupFor(ImGuiTestContext* ctx, Needs needs);

// One registrar per domain file; RegisterGuiTests calls them all.
void RegisterSketchGuiTests(ImGuiTestEngine* engine);
void RegisterModelingGuiTests(ImGuiTestEngine* engine);
void RegisterAssemblyGuiTests(ImGuiTestEngine* engine);
void RegisterDrawingGuiTests(ImGuiTestEngine* engine);
void RegisterCamGuiTests(ImGuiTestEngine* engine);
void RegisterSimulationGuiTests(ImGuiTestEngine* engine);
void RegisterProjectGuiTests(ImGuiTestEngine* engine);
void RegisterViewportGuiTests(ImGuiTestEngine* engine);
void RegisterShellGuiTests(ImGuiTestEngine* engine);
void RegisterTerminalGuiTests(ImGuiTestEngine* engine);
