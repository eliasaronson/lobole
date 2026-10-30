// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
#include "core/features/CamProfileFeature.h"
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
#include "core/features/WormFeature.h"
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
#include <functional>
#include <string>
#include <unordered_map>

// Tests run inside the live application and reach it through the AppState.
// Named s_App for continuity: every test body refers to it by this name.
extern ui::AppState* s_App;

Document* Doc();

// --- CAM tests: the job tab (docs/cam/README.md#the-model) ---------------
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

// A stored Box / Sphere / Cylinder as a FIXTURE: what the toolbar made before
// the primitive tools stood on a face (2 x 2 x 2 box, r 1 sphere, r 0.8 x 2
// upright cylinder, along a ladder at x = -4 + 2.5 n). Most tests want "a
// body" and the picks they make are placed against exactly this geometry, so
// it stays -- added as one undoable command and selected, as an accept left
// it. Nothing the GUI reaches makes these any more
// (docs/ui/tool-sessions.md#primitives); the primitive tools are tested by
// the primitive/* cases.
// `k` scales the box and its spot on the ladder alike (2k cube at k times
// the spot): the defaults/scale cases set a part up at 2 mm and at 300 mm.
void AddFixtureBox(ImGuiTestContext* ctx, float k = 1.0f);
void AddFixtureSphere(ImGuiTestContext* ctx);
void AddFixtureCylinder(ImGuiTestContext* ctx);

// The primitive tool, the real way in: the toolbar arms the pick, and the
// base plane's row in the feature tree (PlaneType index, as DefaultPlane
// takes it) answers it. Leaves the preview pending.
void PrimitiveOnBasePlane(ImGuiTestContext* ctx, const char* tbId,
                          int planeIdx);

void BuildCantileverStudy(ImGuiTestContext* ctx, double height);
bool WaitForStudyResult(ImGuiTestContext* ctx, uint64_t studyId);

// --- Waiting for something asynchronous --------------------------------------
//
// Wait on the CONDITION, never a frame count
// (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count). Yields a
// frame at a time until `done()` holds, bounded by WALL time: a frame count
// is a guess at how long a worker takes, and someone else's build makes it
// wrong. Returns whether it held; a timeout is logged as an error naming
// `what`, so the IM_CHECK the caller puts on the result reads as the cause.
// Stops early once the test is in error.
constexpr double kWaitSeconds = 60.0; // under the engine's 300 s watchdog
bool WaitFor(ImGuiTestContext* ctx, const char* what,
             const std::function<bool()>& done, double seconds = kWaitSeconds);
// Every background job of `kind` finished and applied (ui/BackgroundJobs.h).
bool WaitForJobs(ImGuiTestContext* ctx, const char* kind);
// No project open in flight, the modal or not (ui/OpenJob.h).
bool WaitForOpen(ImGuiTestContext* ctx);
// Ask the disk poll (F8) now and wait for ITS answer: it runs off the frame
// (ui/DiskPoller.h), so a fixed number of frames is a guess. A poll already
// in flight was asked before whatever the caller just did, so it is let
// finish first. Returns at once when the banner is already up.
bool PollDiskNow(ImGuiTestContext* ctx);
// The conflicts scan the last action owed has landed (it runs on the disk
// poller's worker, ui/DiskPoller.h).
bool WaitForConflictScan(ImGuiTestContext* ctx);

// The CPU time the app's MAIN thread has used, in ms. What a frame COST, for
// a test that bounds it ("no frame ran the pass"): under load a frame's WALL
// time grows while it waits for a core, and a 250 ms wall bound fails on a
// frame that computed nothing. Wall time where there is no per-thread clock.
// NoteMainThread is called once, from the main thread, at registration.
void NoteMainThread();
double MainThreadCpuMs();

// What the last frame's update and ui phases cost the main thread, ms of CPU
// (FrameTimer::SetWorkCpuMs): the app's own work, without the present. Under
// Xvfb the present is a 3.7 MB copy to the X server, 20 ms of kernel CPU a
// frame on a fast core and 70 on a low-power one, plus whatever memory
// reclaim the machine bills to it; a bound on "this frame ran no pass"
// reads this, not MainThreadCpuMs around a Yield.
double LastFrameWorkCpuMs();

// --- U10 defaults gate: the tool table and its preconditions ---------------
enum class Needs {
  Nothing,
  Body,
  BodyAndOpenSketch,
  TwoBodies,
  ClosedSketch,
  TwoSketches,
  ProfileAndPath,
  // Sheet metal: the stock declared plus a closed profile, and then that
  // profile thickened into a blank. Setting the rules is part of the SETUP
  // and not of the tool — a flange has no thickness of its own.
  SheetRules,
  SheetBlank,
  // A blank with one wall bent up, and then that wall laid flat by an
  // Unfold: the two states the Unfold and Refold tools start from.
  SheetFlanged,
  SheetUnfolded,
  // Two walls meeting at a square corner, for the corner relief.
  SheetCornered,
  // A template in the project's library (a block's sketch and extrude),
  // for the Insert template tool.
  Template
};
enum class Expect {
  Changes,            // accept with defaults => the model is different
  RefusesUntilPicked, // Accept is disabled and the window says what is missing
  ArmsAPick,          // not a preview: the tool asks for a face/plane first
  PickThenChanges,    // asks for a face/plane; given the Top plane, Changes
  HarmlessNoOp,       // accepting cleanly changes nothing, and that is correct
};
struct ToolCase {
  const char* name; // test name = the id without "###tb_"
  const char* id;
  Needs needs;
  Expect expect;
};
extern const ToolCase kToolCases[44];
const ToolCase* FindToolCase(const char* testName);

// A file under tests/fixtures/ in the source tree ("images/checker16x12.png").
// Defined in GuiTestsSketch.cpp, the one file that knows the source dir.
std::string SourceFixture(const std::string& rel);

void EnterHoleSketchOnPlane(ImGuiTestContext* ctx, int planeIdx);
void EnterSketchOnPlane(ImGuiTestContext* ctx, int planeIdx);
void AddRectSketch(ImGuiTestContext* ctx, int planeIdx, float half,
                   float centreU = 0.0f);
void AddPathSketch(ImGuiTestContext* ctx, int planeIdx, float length = 6.0f);
// `k` scales every length the setup draws -- the box, the sketches, the
// sheet's gauge -- so the same document can be built at any size.
void SetupFor(ImGuiTestContext* ctx, Needs needs, float k = 1.0f);

// One registrar per domain file; RegisterGuiTests calls them all.
void RegisterSketchGuiTests(ImGuiTestEngine* engine);
void RegisterModelingGuiTests(ImGuiTestEngine* engine);
void RegisterAssemblyGuiTests(ImGuiTestEngine* engine);
void RegisterFlexibleGuiTests(ImGuiTestEngine* engine);
void RegisterDrawingGuiTests(ImGuiTestEngine* engine);
void RegisterCamGuiTests(ImGuiTestEngine* engine);
void RegisterSimulationGuiTests(ImGuiTestEngine* engine);
void RegisterProjectGuiTests(ImGuiTestEngine* engine);
void RegisterViewportGuiTests(ImGuiTestEngine* engine);
void RegisterShellGuiTests(ImGuiTestEngine* engine);
void RegisterTerminalGuiTests(ImGuiTestEngine* engine);
void RegisterDemoGuiTests(ImGuiTestEngine* engine);
