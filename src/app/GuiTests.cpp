#include "app/GuiTests.h"

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
#include "render/RayTracingRenderer.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/CamPanel.h"
#include "ui/CamTool.h"
#include "ui/ConflictsPanel.h"
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
static ui::AppState* s_App = nullptr;

static Document* Doc() { return s_App->document.get(); }

// Between-test reset. The ImGui Test Engine runs every test back to back in one
// process, so state a test leaves behind leaks into the next one — most harmful
// is the camera zoom: a viewport-click test that passes alone fails mid-suite
// when a predecessor left the camera zoomed in and its UV click coordinates
// then project off-screen. Restoring the app's initial interaction state before
// each test makes full-suite runs behave like isolated ones. Registered as a
// TestFunc wrapper below, so individual tests don't have to opt in.
static void ResetAppForTest() {
  if (s_App->camera) {
    s_App->camera->StopAnimation();
    s_App->camera->SetPosition(
        glm::vec3(8.0f, 6.0f, 8.0f)); // main.cpp defaults
    s_App->camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
  }
  s_App->showShortcuts = false; // a stray window eats later viewport clicks
  // Preferences are DEFAULTS for every test: otherwise the developer's own
  // config decides whether a test passes and CI disagrees with the desk. The
  // app already skips loading them under --gui-tests; this covers a test that
  // changed one.
  s_App->settings = settings::Settings{};
  s_App->settingsSaved = s_App->settings;
  s_App->settingsDirtyAt = -1.0;
  s_App->settingsWindowOpen = false;
  ui::ApplySettings(*s_App);
  s_App->selection.Clear();
  s_App->sketchEdit = {};
  s_App->sketchDrag = {};
  s_App->slotPick = {};
  s_App->measure = {};
  // Sections are view state that deliberately outlives its tool window, so
  // nothing in the app clears them — which makes them exactly the kind of
  // leak this reset exists for: a predecessor's open tool turns the next
  // test's "click the section button" into a click that CLOSES it.
  ui::ClearSections(*s_App);
  s_App->sectionToolOpen = false;
  s_App->handleDrag = {};
  s_App->hoveredHandle = 0;
  s_App->pick = {};
  s_App->viewportDrag = ui::ViewportDrag::None;
  // Feature sessions: a preview left in the document, or an open editor, is
  // a tool window over the next test's viewport.
  s_App->pendingFeatureId = 0;
  s_App->editFeatureId = 0;
  s_App->mateTool = {};
  s_App->dimTool = {};
  s_App->inContext = {};
  s_App->showCog = false;
  s_App->perfHudOpen = false;
  // CAM is a tool session plus a pile of cached results, none of which the
  // app clears on its own (a job outlives the window that made it, by
  // design). Its overlays draw over the viewport the next test clicks in.
  s_App->camToolOpen = false;
  s_App->camGhostOp = 0;
  s_App->camActiveSetup = 0;
  s_App->camTopFace = {};
  s_App->camRefusal.clear();
  s_App->camToolpaths.clear();
  s_App->camVerdict.clear();
  s_App->camLeftover.clear();
  s_App->camShowToolpath = true;
  // THE DOCK LAYOUT, and it is the one that actually bit us. Two of the
  // three order-dependent failures on 2026-08-22 were a left-node tab left
  // selected over the model tree — once by a panel that appeared without
  // NoFocusOnAppearing, once by a test that failed before its own cleanup.
  // Neither is an app-state field anyone could have thought to clear: the
  // state belongs to ImGui. Rebuilding the default layout is the only reset
  // that owns it, and it re-focuses Model as its last act.
  //
  // Unconditional on purpose (docs/ui/gui-test-isolation.md): tracking
  // whether a test "touched the layout" would have read clean in exactly the
  // case that mattered, because a window docking itself is not an action any
  // test performs.
  s_App->buildDefaultDockLayout = true;
  // A FRESH PROJECT, the same one File > New makes — which also returns the
  // app to a single part tab. Without it a test ending on an ASSEMBLY tab
  // leaves `document` null and the next test to touch Doc() segfaults. Not
  // hypothetical: --gui-test-shuffle seed 11 crashed a whole shard that way,
  // taking 49 tests with it, because an assembly test landed in front of one
  // that creates a sphere.
  ui::ReplaceDocument(*s_App, std::make_unique<Document>());
  s_App->parts.clear();
  s_App->tabFolders.clear();
  ui::EnsureParts(*s_App, "Part 1");
  ui::MarkProjectOpened(*s_App, "");
}

// Original TestFunc for each registered test, keyed by the test; the wrapper
// runs ResetAppForTest() then dispatches to it (TestFunc is a raw function
// pointer in this build, so we can't capture — hence the side table).
static std::unordered_map<ImGuiTest*, void (*)(ImGuiTestContext*)>
    g_origTestFunc;

static void RunTestWithReset(ImGuiTestContext* ctx) {
  ResetAppForTest();
  const auto it = g_origTestFunc.find(ctx->Test);
  if (it != g_origTestFunc.end() && it->second)
    it->second(ctx);
}

// Window position of a world point, mapped through the central-node viewport
// rect (where the 3D scene actually renders), matching the picking math.
static ImVec2 WorldToWindow(const glm::vec3& p) {
  const Camera& cam = *s_App->camera;
  glm::vec4 clip =
      cam.GetProjectionMatrix() * cam.GetViewMatrix() * glm::vec4(p, 1.0f);
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  const glm::vec2 pos = s_App->viewportPos;
  const glm::vec2 size = s_App->viewportSize;
  return ImVec2(pos.x + (ndc.x + 1.0f) * 0.5f * size.x,
                pos.y + (ndc.y + 1.0f) * 0.5f * size.y);
}

// Click a model-toolbar icon button by its stable "###tb_*" id. The buttons
// live in a child window under the dock host, so we descend with a "**/"
// wildcard; the "###" id makes the match independent of the icon glyph.
static void ToolbarClick(ImGuiTestContext* ctx, const char* tbId) {
  ctx->SetRef("//##DockHost");
  ctx->ItemClick((std::string("**/") + tbId).c_str());
  ctx->Yield();
  // Leave the ref on the menu bar, matching the old Create-menu helper: several
  // callers follow up with MenuClick("Edit/Undo") etc. and rely on that ref.
  ctx->SetRef("//##MainMenuBar");
}

// Most tests want a committed feature: create from the toolbar, then accept the
// preview (exercises the real accept path).
static void ToolbarCreateAccept(ImGuiTestContext* ctx, const char* tbId) {
  ToolbarClick(ctx, tbId);
  ui::AcceptPendingFeature(*s_App);
  ctx->Yield();
}

// A steel cantilever with a fixed end and a distributed load on its top
// face, placed by clicking, left ready to run. 200 x 10 x 20 mm: three
// different dimensions, so an axis mix-up cannot hide (the drawings round
// learned that on a square plate). The load goes on the TOP face so the
// default "into the face you click" direction is the -Y transverse load
// beam theory describes — no hand-typed world vector anywhere.
//
// `height` is the one thing the two callers disagree on: the stress gate
// wants a SLENDER beam (h = 10) so Euler-Bernoulli applies, and topology
// wants a DEEP one (h = 60) so there is an interior to carve — on a slender
// part the frozen support/load skin IS the whole part.
static void BuildCantileverStudy(ImGuiTestContext* ctx, double height) {
  ctx->SetRef("//##MainMenuBar");
  ToolbarCreateAccept(ctx, "###tb_box");
  // The box tool's own dimension fields are not addressable yet, so the
  // shape is set through the command layer; everything from here down is
  // clicked, which is the half this test exists for.
  const size_t index = Doc()->FeatureCount() - 1;
  auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(index));
  IM_CHECK_SILENT(box != nullptr);
  nlohmann::json before = DocumentIO::FeatureToJson(*box);
  box->position = glm::vec3(0.0f);
  box->width = 200.0;
  box->height = height;
  box->depth = 20.0;
  nlohmann::json after = DocumentIO::FeatureToJson(*box);
  s_App->commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
  ctx->Yield(2);

  ToolbarClick(ctx, "###tb_simulation");
  ctx->Yield(2);
  const uint64_t createdBy = Doc()->GetBodies()[0].createdBy;
  ctx->ItemClick(
      ("//###Model/###material_" + std::to_string(createdBy)).c_str());
  ctx->Yield();
  IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
  ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
  IM_CHECK_SILENT(popup != nullptr);
  ctx->ItemClick((std::string("//") + popup->Name + "/**/Steel").c_str());
  ctx->Yield(2);

  glm::vec3 lo(0.0f), hi(0.0f);
  Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
  const glm::vec3 mid = (lo + hi) * 0.5f;
  s_App->camera->StopAnimation();
  s_App->camera->SetTarget(mid);
  s_App->camera->SetUp(glm::vec3(0, 1, 0));

  // Fixed on the x=0 end face, looking straight at it.
  s_App->camera->SetPosition(mid - glm::vec3(400.0f, 0.0f, 0.0f));
  ctx->Yield(3);
  ctx->ItemClick("//###Model/###sim_bc_fixed");
  ctx->Yield();
  ctx->MouseMoveToPos(WorldToWindow(glm::vec3(lo.x, mid.y, mid.z)));
  ctx->Yield();
  ctx->MouseClick(ImGuiMouseButton_Left);
  ctx->Yield(2);

  // 100 N spread over the top face, from above.
  ctx->ItemClick("//###Model/###sim_bc_force");
  ctx->Yield();
  ctx->ItemInputValue("//###Model/###sim_magnitude", "100");
  ctx->Yield();
  s_App->camera->SetUp(glm::vec3(0, 0, -1));
  s_App->camera->SetPosition(mid + glm::vec3(0.0f, 400.0f, 0.0f));
  ctx->Yield(3);
  ctx->MouseMoveToPos(WorldToWindow(glm::vec3(mid.x, hi.y, mid.z)));
  ctx->Yield();
  ctx->MouseClick(ImGuiMouseButton_Left);
  ctx->Yield(2);

  fem::Study* st = Doc()->FindStudy(s_App->simulation.activeStudyId);
  IM_CHECK_SILENT(st != nullptr);
  IM_CHECK_EQ(st->loads.size(), (size_t)2);
  // The default direction is into the picked face: -Y, from the top.
  const fem::Load& force = st->loads[1];
  IM_CHECK(force.type == fem::LoadType::Force);
  IM_CHECK(std::abs(force.direction.y + 1.0) < 1e-6);
  IM_CHECK(std::abs(force.magnitude - 100.0) < 1e-9);
}

// The solve runs on a worker thread now (clicking Run used to block the
// draw), so pump frames until the result lands. Bounded: a hang must fail
// the test rather than spin forever.
static bool WaitForStudyResult(ImGuiTestContext* ctx, uint64_t studyId) {
  for (int i = 0; i < 6000 && !Doc()->StudyResults(studyId); ++i)
    ctx->Yield();
  if (Doc()->StudyResults(studyId))
    return true;
  // An IM_CHECK here would return from THIS function and let the test walk on
  // into a null result, which is a segfault rather than a failure report.
  const fem::Study* st = Doc()->FindStudy(studyId);
  ctx->LogError("no result for study %llu: %s", (unsigned long long)studyId,
                st && !st->error.empty() ? st->error.c_str() : "(no error)");
  return false;
}

// --- U10 defaults gate: the tool table and its preconditions ----------------

// What the document must contain before the tool's button is even enabled.
enum class Needs {
  Nothing,
  Body,
  BodyAndOpenSketch,
  TwoBodies,
  ClosedSketch,
  TwoSketches,
  ProfileAndPath
};
// What must happen when the tool opens with its defaults and no picks.
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

static const ToolCase kToolCases[] = {
    {"box", "###tb_box", Needs::Nothing, Expect::Changes},
    {"sphere", "###tb_sphere", Needs::Nothing, Expect::Changes},
    {"cylinder", "###tb_cylinder", Needs::Nothing, Expect::Changes},
    {"curve3d", "###tb_curve3d", Needs::Nothing, Expect::Changes},
    {"datum_plane", "###tb_datum_plane", Needs::Nothing, Expect::Changes},
    {"datum_axis", "###tb_datum_axis", Needs::Nothing, Expect::Changes},
    {"extrude", "###tb_extrude", Needs::ClosedSketch, Expect::Changes},
    {"revolve", "###tb_revolve", Needs::ClosedSketch, Expect::Changes},
    {"sweep", "###tb_sweep", Needs::ProfileAndPath, Expect::Changes},
    {"pipe", "###tb_pipe", Needs::ProfileAndPath, Expect::Changes},
    {"loft", "###tb_loft", Needs::TwoSketches, Expect::RefusesUntilPicked},
    {"boolean", "###tb_boolean", Needs::TwoBodies, Expect::Changes},
    {"mirror", "###tb_mirror", Needs::Body, Expect::Changes},
    {"pattern", "###tb_pattern", Needs::Body, Expect::Changes},
    {"split", "###tb_split", Needs::Body, Expect::Changes},
    // Refine on an already-clean box has nothing to merge: no change here is
    // the honest answer, so this case pins "accepts cleanly", not "changes".
    {"refine", "###tb_refine", Needs::Body, Expect::HarmlessNoOp},
    {"shell", "###tb_shell", Needs::Body, Expect::RefusesUntilPicked},
    {"draft", "###tb_draft", Needs::Body, Expect::RefusesUntilPicked},
    {"fillet", "###tb_fillet", Needs::Body, Expect::RefusesUntilPicked},
    {"chamfer", "###tb_chamfer", Needs::Body, Expect::RefusesUntilPicked},
    {"thread", "###tb_thread", Needs::Body, Expect::RefusesUntilPicked},
    {"rib", "###tb_rib", Needs::BodyAndOpenSketch, Expect::Changes},
    {"hole", "###tb_hole", Needs::Body, Expect::ArmsAPick},
    {"new_sketch", "###tb_new_sketch", Needs::Nothing, Expect::ArmsAPick},
};

static const ToolCase* FindToolCase(const char* testName) {
  for (const ToolCase& tc : kToolCases)
    if (std::strcmp(tc.name, testName) == 0)
      return &tc;
  return nullptr;
}

// Enter a sketch on a base plane (0 = front/XY, 1 = top/XZ, 2 = right/YZ) via a
// direct edit-mode entry — the same call the feature-tree "Default geometry"
// double-click and the retired Create/Sketch menu used. These tests exercise
// what happens *inside* a sketch; the interactive toolbar -> viewport plane
// pick that opens one is covered on its own by
// sketch/toolbar_plane_pick_enters_edit.
// A Hole session on a base plane: the deterministic way into stage B
// (placing centres) without a face pick.
static void EnterHoleSketchOnPlane(ImGuiTestContext* ctx, int planeIdx) {
  ui::BeginSketchOnPlane(*s_App, ui::DefaultPlane(planeIdx), "Hole points",
                         ui::SketchPurpose::Hole);
  ctx->Yield();
  if (s_App->camera)
    s_App->camera->FinishAnimation();
  ctx->Yield();
}

static void EnterSketchOnPlane(ImGuiTestContext* ctx, int planeIdx) {
  const std::string name =
      "Sketch " + std::to_string(Doc()->FeatureCount() + 1);
  ui::BeginSketchOnPlane(*s_App, ui::DefaultPlane(planeIdx), name);
  ctx->Yield();
  // Jump the align-to-plane animation to its end: clicks computed via
  // SketchUVToScreen must not race a camera still in flight (the infer/
  // snap tests were load-dependent flaky exactly because of this).
  if (s_App->camera)
    s_App->camera->FinishAnimation();
  ctx->Yield();
}

// Builds the minimum document a tool needs before its button is enabled.
// Sketches are drawn through the model, not the mouse: this suite is about
// what the TOOL does with its defaults, not about drawing accuracy.
static void AddRectSketch(ImGuiTestContext* ctx, int planeIdx, float half,
                          float centreU = 0.0f) {
  EnterSketchOnPlane(ctx, planeIdx);
  auto* sk = dynamic_cast<SketchFeature*>(
      Doc()->GetFeature(Doc()->FeatureCount() - 1));
  IM_CHECK_SILENT(sk != nullptr);
  const size_t index = Doc()->FeatureCount() - 1;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  const float u0 = centreU - half, u1 = centreU + half;
  sk->AddEntity(sketch::Line{{u0, -half}, {u1, -half}});
  sk->AddEntity(sketch::Line{{u1, -half}, {u1, half}});
  sk->AddEntity(sketch::Line{{u1, half}, {u0, half}});
  sk->AddEntity(sketch::Line{{u0, half}, {u0, -half}});
  nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  s_App->commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
  ctx->Yield();
  ToolbarClick(ctx, "###tb_sk_finish");
  ctx->Yield();
}

static void AddPathSketch(ImGuiTestContext* ctx, int planeIdx) {
  EnterSketchOnPlane(ctx, planeIdx);
  auto* sk = dynamic_cast<SketchFeature*>(
      Doc()->GetFeature(Doc()->FeatureCount() - 1));
  IM_CHECK_SILENT(sk != nullptr);
  const size_t index = Doc()->FeatureCount() - 1;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  sk->AddEntity(sketch::Line{{0.0f, 0.0f}, {0.0f, 6.0f}}); // an open path
  nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  s_App->commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
  ctx->Yield();
  ToolbarClick(ctx, "###tb_sk_finish");
  ctx->Yield();
}

static void SetupFor(ImGuiTestContext* ctx, Needs needs) {
  switch (needs) {
  case Needs::Nothing:
    break;
  case Needs::Body:
    ToolbarCreateAccept(ctx, "###tb_box");
    break;
  case Needs::TwoBodies:
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    break;
  case Needs::ClosedSketch:
    // Off the origin on purpose: a revolve whose profile STRADDLES its axis
    // is geometrically impossible, and refusing it is correct behaviour, not
    // a default worth testing. A user drawing a revolve puts the profile
    // beside the axis, so the setup does too.
    AddRectSketch(ctx, 1, 1.0f, /*centreU=*/3.0f);
    break;
  case Needs::TwoSketches:
    AddRectSketch(ctx, 1, 1.5f);
    AddRectSketch(ctx, 1, 1.0f);
    break;
  case Needs::BodyAndOpenSketch: {
    // A rib grows a thin web from an open profile down onto a body, so the
    // gate needs both: the default box, and a line hanging above it.
    ToolbarCreateAccept(ctx, "###tb_box");
    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    // The web grows along the sketch plane's NORMAL, so the profile has to
    // face the body: a plane hanging above the box, looking down at it.
    const glm::vec3 over((lo.x + hi.x) * 0.5f, hi.y + 1.0f,
                         (lo.z + hi.z) * 0.5f);
    ui::BeginSketchOnPlane(
        *s_App, SketchPlane(over, glm::vec3(0, -1, 0), glm::vec3(1, 0, 0)),
        "Rib profile");
    ctx->Yield();
    if (s_App->camera)
      s_App->camera->FinishAnimation();
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK_SILENT(sk != nullptr);
    const size_t index = Doc()->FeatureCount() - 1;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{-0.4f, 0.0f}, {0.4f, 0.0f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
    ctx->Yield();
    ToolbarClick(ctx, "###tb_sk_finish");
    break;
  }
  case Needs::ProfileAndPath:
    AddRectSketch(ctx, 1, 1.0f); // profile on the top plane
    AddPathSketch(ctx, 0);       // path rising in the front plane
    break;
  }
  ctx->Yield();
}

void RegisterGuiTests(ImGuiTestEngine* engine, ui::AppState* state) {
  s_App = state;

  ImGuiTest* t = nullptr;

  t = IM_REGISTER_TEST(engine, "app", "boots_and_renders");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->Yield(3);
    IM_CHECK(ctx->UiContext->WithinFrameScope == false ||
             ctx->UiContext->FrameCount > 0);
  };

  t = IM_REGISTER_TEST(engine, "app", "core_windows_exist");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(ctx->GetWindowByRef("//###Model") != nullptr);
  };

  t = IM_REGISTER_TEST(engine, "document", "parametric_box_undo_redo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(Doc() != nullptr);
    // TWO baselines. A datum plane or a sketch is a feature that produces no
    // body, so measuring bodies against the FEATURE count only holds for a
    // document containing nothing but solids — found by --gui-test-shuffle
    // putting a datum test in front of this one.
    const size_t before = Doc()->FeatureCount();
    const size_t bodiesBefore = Doc()->GetBodies().size();

    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 1);

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
  };

  t = IM_REGISTER_TEST(engine, "document", "create_sphere_and_cylinder");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const size_t before = Doc()->FeatureCount();
    const size_t bodiesBefore = Doc()->GetBodies().size(); // see above

    ToolbarCreateAccept(ctx, "###tb_sphere");
    ToolbarCreateAccept(ctx, "###tb_cylinder");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 2);

    ctx->MenuClick("Edit/Undo");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);
  };

  t = IM_REGISTER_TEST(engine, "curves", "create_helix_from_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const size_t before = Doc()->FeatureCount();
    ToolbarCreateAccept(ctx, "###tb_curve3d");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    auto* cv = dynamic_cast<CurveFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(cv != nullptr);
    IM_CHECK(cv->kind == Curve3DKind::Helix);
    IM_CHECK(cv->Polyline().size() >= 2); // a real curve was built
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);
  };

  t = IM_REGISTER_TEST(engine, "curves", "pipe_along_helix_makes_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // A helix (now the most recent curve), then a pipe that defaults its path
    // to it. Body count is checked as a delta, so prior state doesn't matter.
    ToolbarCreateAccept(ctx, "###tb_curve3d");
    const size_t bodiesBefore = Doc()->GetBodies().size();
    ToolbarCreateAccept(ctx, "###tb_pipe");
    auto* pi = dynamic_cast<PipeFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(pi != nullptr);
    IM_CHECK(pi->pathId != 0); // seeded from the helix
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 1); // a tube body
  };

  t = IM_REGISTER_TEST(engine, "document", "new_project_clears_features");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK(Doc()->FeatureCount() > 0);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "document", "feature_tree_drag_reorder");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");    // Box 1    -> index 0
    ToolbarCreateAccept(ctx, "###tb_sphere"); // Sphere 1 -> index 1
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    const std::string a = Doc()->GetFeature(0)->GetName();
    const std::string b = Doc()->GetFeature(1)->GetName();

    // Drag the second row onto the first: two independent solids reorder
    // freely, so the move commits.
    ctx->ItemDragAndDrop(("//###Model/" + b + "##1").c_str(),
                         ("//###Model/" + a + "##0").c_str());
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetFeature(0)->GetName(), b);
    IM_CHECK_EQ(Doc()->GetFeature(1)->GetName(), a);

    // One undoable command puts the order back.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetFeature(0)->GetName(), a);
    IM_CHECK_EQ(Doc()->GetFeature(1)->GetName(), b);
  };

  t = IM_REGISTER_TEST(engine, "properties", "edit_box_width_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");

    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0);

    // U7e: committed-feature editing is an explicit session — double-click
    // the tree row, edit in the Tool window.
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    // Labels-left rows (U4): the widget id is "##<label>", the visible label
    // is a separate Text item.
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 5.0f);
    ctx->Yield();

    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 5.0);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 5.0);
  };

  t = IM_REGISTER_TEST(engine, "shortcuts", "undo_redo_keyboard");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "middle_drag_rotates_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");

    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->Yield();

    const glm::vec3 posAfter = s_App->camera->GetPosition();
    IM_CHECK(glm::length(posAfter - posBefore) > 0.1f);
    // Rotation keeps the distance to the target
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "ctrl_middle_drag_pans_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const glm::vec3 targetBefore = s_App->camera->GetTarget();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2(center.x + 100.0f, center.y + 60.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();

    // Pan moves the orbit target
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.05f);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "ctrl_mid_drag_switches_to_pan");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // Pressing Ctrl after the middle button is already down must switch the
    // drag from rotate to pan
    const glm::vec3 targetBefore = s_App->camera->GetTarget();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Middle); // starts as rotate
    ctx->KeyDown(ImGuiMod_Ctrl);             // ...now becomes pan
    ctx->MouseMoveToPos(ImVec2(center.x + 90.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();

    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.05f);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "scroll_zooms");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ctx->MouseMoveToPos(ImVec2(size.x * 0.5f, size.y * 0.5f));
    ctx->MouseWheelY(2.0f); // scroll up = zoom in
    ctx->Yield();

    IM_CHECK(s_App->camera->GetDistance() < distBefore);
  };

  // Trackpad navigation: Alt+left-drag orbits (middle-button substitute) and
  // the click never reaches picking; Shift+scroll pans instead of zooming.
  t = IM_REGISTER_TEST(engine, "viewport", "alt_left_drag_orbits");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->camera->StopAnimation();
    ctx->Yield();
    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Alt);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Alt);
    ctx->Yield();

    IM_CHECK(glm::length(s_App->camera->GetPosition() - posBefore) > 0.1f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None); // drag ended
  };

  // Right-drag navigates like the middle button (no-middle-button / trackpad
  // path): plain orbits, +Ctrl pans. Left stays selection.
  t = IM_REGISTER_TEST(engine, "viewport", "right_drag_orbits");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->camera->StopAnimation();
    ctx->Yield();
    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Right);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Right);
    ctx->Yield();

    // Orbit moved the eye but kept the distance (not a zoom), and the drag
    // released cleanly on the right button.
    IM_CHECK(glm::length(s_App->camera->GetPosition() - posBefore) > 0.1f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None);

    // +Ctrl mid-scheme = pan: moves the orbit target, keeps distance.
    const glm::vec3 targetBefore = s_App->camera->GetTarget();
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseDown(ImGuiMouseButton_Right);
    ctx->MouseMoveToPos(ImVec2(center.x + 60.0f, center.y));
    ctx->MouseUp(ImGuiMouseButton_Right);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.01f);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "shift_scroll_pans");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const glm::vec3 targetBefore = s_App->camera->GetTarget();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ctx->MouseMoveToPos(ImVec2(size.x * 0.5f, size.y * 0.5f));
    ctx->KeyDown(ImGuiMod_Shift);
    ctx->MouseWheelY(2.0f);
    ctx->KeyUp(ImGuiMod_Shift);
    ctx->Yield();

    // Pan moves the orbit target and keeps the distance.
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.01f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.01f * distBefore);
  };

  // Entering simulation or sketch mode drops an armed measure tool — it owns
  // the viewport while active and those modes have no toggle to disarm it.
  t = IM_REGISTER_TEST(engine, "measure", "cleared_on_mode_entry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");

    s_App->measure.active = true;
    ui::EnterSimulation(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    ui::ExitSimulation(*s_App);
    ctx->Yield();

    s_App->measure.active = true;
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    s_App->sketchEdit = ui::SketchEditState{}; // leave sketch edit
    // Entering the sketch aligned the camera with an animation; don't leak it
    // into the next test's camera framing (it overrides setters until done).
    s_App->camera->StopAnimation();
    ctx->Yield();
  };

  // Simulation is the one workspace that emits numbers a person would act on,
  // and its setup defaults were world literals: gravity along -Z (sideways,
  // since world up is +Y), a force direction chosen before the face was
  // picked, and a magnitude of 0 N that places a load doing nothing.

  t = IM_REGISTER_TEST(engine, "sim", "gravity_pulls_down");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarClick(ctx, "###tb_simulation"); // the button had no id before this
    ctx->Yield();
    IM_CHECK(s_App->simulation.active);
    ctx->ItemClick("//###Model/###study_gravity");
    ctx->Yield();
    const fem::Study* study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    // -Y. World up is +Y here, so self-weight along -Z sagged the part
    // sideways and bent it about the wrong axis.
    IM_CHECK(std::abs(study->gravity.y + 9.81) < 1e-9);
    IM_CHECK(study->gravity.x == 0.0 && study->gravity.z == 0.0);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "sim", "force_pushes_into_the_picked_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    // A study needs stiffness data, and a new body has none: the panel says
    // so and offers the fix in place, rather than sending the user out of
    // simulation mode to find the feature's Material combo.
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield();
    const uint64_t createdBy = Doc()->GetBodies()[0].createdBy;
    const std::string materialRef =
        "//###Model/###material_" + std::to_string(createdBy);
    IM_CHECK(ctx->ItemExists(materialRef.c_str()));
    // Two steps, and the popup addressed by its own window name: a combo
    // popup does not take focus, and ComboClick wants one that does. Same
    // dance as loft/two_profiles_via_the_section_combo.
    ctx->ItemClick(materialRef.c_str());
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Steel").c_str());
    ctx->Yield();
    // The pick landed on the body's creating feature, so the study can solve.
    IM_CHECK(!Doc()->GetBodies()[0].material.empty());
    IM_CHECK(CADMaterials::MaterialLibrary::GetMaterial(
                 CADMaterials::MaterialLibrary::FromSlug(
                     Doc()->GetBodies()[0].material))
                 .youngGPa > 0.0f);

    // Arm the force tool: the magnitude seeds from the part's own weight, so
    // the click cannot place a silent 0 N load.
    ToolbarClick(ctx, "###tb_sim_force");
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingMagnitude > 0.0);

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(
        mid + glm::normalize(glm::vec3(0.9f, 0.5f, 0.4f)) * 12.0f);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(hi.x, mid.y, mid.z)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    const fem::Study* study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    IM_CHECK_EQ(study->loads.size(), (size_t)1);
    const fem::Load& load = study->loads[0];
    IM_CHECK(load.type == fem::LoadType::Force);
    IM_CHECK(load.magnitude > 0.0);
    // Into the face, not the old (0,0,-1) world literal.
    IM_CHECK(std::abs(load.direction.x + 1.0) < 1e-6);
    IM_CHECK(std::abs(load.direction.y) < 1e-6);
    IM_CHECK(std::abs(load.direction.z) < 1e-6);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // The force arrow must POINT the way the force pushes, and be draggable to
  // change its magnitude (docs/ui/viewport-handles.md). Both asserted on the
  // drawn primitives and the study, not on pixels.
  t = IM_REGISTER_TEST(engine, "sim", "force_arrow_points_along_the_force");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);

    // A force on a known face, pushing -Y.
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.names.empty());
    fem::Load load;
    load.id = Doc()->AllocateId();
    load.type = fem::LoadType::Force;
    load.face =
        ElementRef{body.createdBy, body.elementMap.names.begin()->second};
    load.magnitude = 100.0;
    load.direction = glm::dvec3(0.0, -1.0, 0.0);
    study->loads.push_back(load);
    ctx->Yield(2);

    // The glyph: the arrow's HEAD is the end the force points toward, so
    // head-minus-tail must run along the force direction. (The head is drawn
    // as two barbs at the far end; the shaft is the first primitive pair.)
    std::vector<PrimVertex> prims;
    s_App->viewportHandles.clear();
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    const glm::vec3 shaftA = prims[0].posThis, shaftB = prims[0].posOther;
    const glm::vec3 along = glm::normalize(shaftB - shaftA);
    IM_CHECK(std::abs(along.y + 1.0f) < 1e-3f); // points -Y, like the force
    IM_CHECK(std::abs(along.x) < 1e-3f);
    IM_CHECK(std::abs(along.z) < 1e-3f);

    // And it published a draggable handle, whose axis is the same direction.
    const ui::ViewportHandle* h = nullptr;
    for (const ui::ViewportHandle& v : s_App->viewportHandles)
      if (ui::OwnerOf(v.id) == ui::HandleOwner::Load)
        h = &v;
    IM_CHECK(h != nullptr);
    IM_CHECK_EQ(ui::SubOf(h->id), load.id);
    IM_CHECK(std::abs(h->dir.y + 1.0f) < 1e-3f);

    // Drag it along the force: the magnitude grows, as ONE undo entry.
    // (Creating the box is itself an undo entry, so "can undo" is already
    // true here — what matters is that the DRAG adds exactly one more.)
    s_App->camera->StopAnimation();
    glm::vec3 lo(0.0f), hi(0.0f);
    body.shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(mid + glm::vec3(0.0f, 0.0f, 1.0f) * 14.0f);
    ctx->Yield(2);
    s_App->viewportHandles.clear();
    prims.clear();
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    for (const ui::ViewportHandle& v : s_App->viewportHandles)
      if (ui::OwnerOf(v.id) == ui::HandleOwner::Load)
        h = &v;
    IM_CHECK(h != nullptr);
    const glm::vec2 grab =
        ui::WorldToScreen(*s_App, h->origin + h->dir * (h->length * 0.5f));
    const glm::vec2 pull =
        ui::WorldToScreen(*s_App, h->origin + h->dir * (h->length * 1.4f));
    ctx->MouseMoveToPos(ImVec2(grab.x, grab.y));
    ctx->Yield(2);
    IM_CHECK_EQ(ui::OwnerOf(s_App->hoveredHandle), ui::HandleOwner::Load);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(pull.x, pull.y));
    ctx->Yield();
    IM_CHECK(s_App->simulation.loadDrag.active);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr && !study->loads.empty());
    IM_CHECK(study->loads[0].magnitude > 100.0); // pulling grew it
    // ONE undo restores the grab-time magnitude — not a step per frame.
    IM_CHECK(s_App->commands->CanUndo());
    s_App->commands->Undo(*Doc());
    ctx->Yield();
    study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr && !study->loads.empty());
    IM_CHECK(std::abs(study->loads[0].magnitude - 100.0) < 1e-6);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

#ifdef HAVE_CAM
  // CAM (docs/cam/plan.md): a setup on a part, an operation with derived
  // depths, and a backplot drawn from the SAME toolpath the post consumes.
  // G1 (docs/cam/gui.md): Z zero is PICKED, not typed. Getting it wrong —
  // zeroing on the table instead of the stock top — is the mistake that
  // breaks tools, so the panel refuses to guess it.
  // A new document takes the CAM job with it. This state used to survive:
  // the panel claimed "Z zero at 17.000" and offered to Save G-code for a
  // part that no longer existed, and the armed face pick came along too —
  // into a document where the button that re-arms it is DISABLED while a pick
  // is live, so the panel was simply stuck (docs/cam/dogfood-round.md,
  // round 2).
  t = IM_REGISTER_TEST(engine, "cam", "new_document_takes_the_job_with_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    cam::Operation op;
    op.id = Doc()->AllocateId();
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    Doc()->FindCamSetup(setup.id)->operations.push_back(op);
    ctx->Yield(2);
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f) {
          ui::SelectionItem top;
          top.kind = ui::SelectionItem::Kind::Face;
          top.element = ElementRef{b.createdBy, name};
          ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top");
          ui::CamFacePicked(*s_App, top);
        }
      }
    ctx->Yield(2);
    IM_CHECK(s_App->camTopFace.IsSet());
    IM_CHECK(!s_App->camToolpaths.empty());
    // Now throw the document away.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ctx->Yield();
    ctx->MenuClick("File/New");
    ctx->Yield(3);
    IM_CHECK(!s_App->camTopFace.IsSet());
    IM_CHECK(s_App->camToolpaths.empty());
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK_EQ(s_App->camActiveSetup, (uint64_t)0);
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0);
  };

  t = IM_REGISTER_TEST(engine, "cam", "pick_top_face_sets_z_zero");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    IM_CHECK(!s_App->camTopFace.IsSet()); // says so rather than assuming 0

    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ctx->Yield();
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);

    // A curved or downward face is refused with a reason, not accepted
    // quietly — the work origin must be where the user pointed.
    const Body& body = Doc()->GetBodies()[0];
    ui::SelectionItem side;
    side.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.5f) {
        side.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    if (side.element.IsSet()) {
      ui::CamFacePicked(*s_App, side);
      IM_CHECK(!s_App->slotPick.miss.empty()); // refused, visibly
      IM_CHECK(!s_App->camTopFace.IsSet());
    }

    // The top face sets Z zero for the job.
    ui::SelectionItem top;
    top.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && n.z > 0.99f) {
        top.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    IM_CHECK(top.element.IsSet());
    ui::CamFacePicked(*s_App, top);
    ctx->Yield(2);
    IM_CHECK(s_App->camTopFace.IsSet());
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0); // slot disarmed
    const cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK(std::abs(live->origin.z - 12.0) < 1e-6); // the plate's top
    // The pick says where Z zero is, and NOTHING about how much material sits
    // above the part — it used to zero that, which threw away an allowance
    // already typed and made "Face the top" refuse right after the pick meant
    // to set the job up (docs/cam/dogfood-round.md, round 2).
    IM_CHECK(std::abs(live->stock.margin.z - 1.0) < 1e-9);
    const cam::DerivedOp face = cam::DeriveOperation(
        *live, Doc()->GetBodies(), cam::OpType::Face, ElementRef{});
    IM_CHECK(face.ok); // so the first verb on a rough board just works
  };

  // A tool session belongs to the document it is editing. File > New used to
  // leave every one of them open on a document that no longer existed —
  // including a feature-editor session naming a feature that had just been
  // discarded. The one that bit was camToolOpen: it is a TOGGLE, so a stale
  // one turned OFF at the next click on the button meant to turn it on, and
  // the failure surfaced in an unrelated test (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "app", "new_document_closes_tool_sessions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);

    // Every transient session at once, so this test fails if a NEW one is
    // added without being cleared alongside the rest.
    s_App->editFeatureId = Doc()->GetFeature(0)->GetId();
    s_App->camToolOpen = true;
    s_App->measure.active = true;
    s_App->dimTool.active = true;
    s_App->mateTool.active = true;
    ctx->Yield(2);

    ctx->MenuClick("File/New");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->editFeatureId, (uint64_t)0);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(!s_App->measure.active);
    IM_CHECK(!s_App->dimTool.active);
    IM_CHECK(!s_App->mateTool.active);
  };

  // A standard view is an ORIENTATION, not a place. Every one of them —
  // the menu, the view cube, the Shift+1..7 shortcuts, the command palette —
  // passed the world origin as the point to look at, so fitting the view on a
  // part modelled away from the origin and then pressing Front left you
  // looking at empty space beside it (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "view", "standard_views_keep_the_pivot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Modelled well away from the origin, which is where this went wrong.
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Plate", glm::vec3(200, 100, 50), 60, 40, 12));
    ctx->Yield(2);
    ui::FitCameraToDocument(*s_App);
    ctx->Yield(4);
    const glm::vec3 fitted = s_App->camera->GetTarget();
    IM_CHECK(glm::length(fitted) > 100.0f); // it really is off the origin

    ctx->MenuClick("View/Standard Views/Front");
    ctx->Yield(180); // AlignToFace animates
    ctx->LogInfo("target after Front: %.1f %.1f %.1f",
                 s_App->camera->GetTarget().x, s_App->camera->GetTarget().y,
                 s_App->camera->GetTarget().z);
    IM_CHECK(glm::length(s_App->camera->GetTarget() - fitted) < 1.0f);
    // …and it did turn: looking along -Z, the app's "Front".
    const glm::vec3 fwd = glm::normalize(s_App->camera->GetForward());
    IM_CHECK(std::abs(fwd.z + 1.0f) < 0.02f);
  };

  // Fitting the view with a tool window open puts the model in the band the
  // window LEAVES. The pan was the wrong way round — it pushed the model
  // under the very window the branch exists to clear — and nothing noticed,
  // because "the model moved sideways" looks the same either way until you
  // ask which side (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "fit_keeps_the_model_clear_of_the_tool");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    s_App->camToolOpen = true;
    ctx->Yield(4);
    ImGuiWindow* win = ImGui::FindWindowByName("CAM###ToolWindow");
    IM_CHECK(win != nullptr);
    ui::FitCameraToDocument(*s_App);
    ctx->Yield(4);

    // Where the part's centre lands on screen. It must sit left of the
    // window's left edge, not under it.
    const glm::vec2 screen =
        ui::WorldToScreen(*s_App, glm::vec3(30.0f, 20.0f, 6.0f));
    ctx->LogInfo("part centre at x=%.0f, window starts at x=%.0f", screen.x,
                 win->Pos.x);
    IM_CHECK(screen.x < win->Pos.x);
    s_App->camToolOpen = false;
  };

  // The floating tool window FOLLOWS the viewport. Anchoring it once, on the
  // appearing frame, placed it against a size AlwaysAutoResize had not
  // computed yet, and any later layout change left it stranded over the
  // middle of the model it is asking you to click (dogfooding round 2).
  t = IM_REGISTER_TEST(engine, "cam", "tool_window_follows_the_viewport");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    s_App->camToolOpen = true;
    ctx->Yield(4); // one frame to size it, one to place it against that size

    const auto rightEdge = [&]() {
      ImGuiWindow* w = ImGui::FindWindowByName("CAM###ToolWindow");
      return w != nullptr ? w->Pos.x + w->Size.x : -1.0f;
    };
    const float want = s_App->viewportPos.x + s_App->viewportSize.x - 12.0f;
    IM_CHECK(rightEdge() > 0.0f);
    IM_CHECK(std::abs(rightEdge() - want) < 2.0f);

    // Now move the viewport under it: the terminal panel docks into the
    // central node, so turning it on genuinely narrows the 3D view. The
    // window must come with it rather than stay where it was.
    const bool hadTerminal = s_App->settings.terminalPanel;
    s_App->settings.terminalPanel = true;
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(8);
    const float narrowed = s_App->viewportPos.x + s_App->viewportSize.x - 12.0f;
    IM_CHECK(narrowed < want - 10.0f); // the viewport really did move
    IM_CHECK(std::abs(rightEdge() - narrowed) < 2.0f);

    s_App->settings.terminalPanel = hadTerminal;
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(8);
    IM_CHECK(std::abs(rightEdge() - (s_App->viewportPos.x +
                                     s_App->viewportSize.x - 12.0f)) < 2.0f);
    s_App->camToolOpen = false;
  };

  // The view a machinist works in. The app's up is +Y and CAM's is +Z, so the
  // plan view — down the spindle — is filed under "Front" in Standard Views
  // and the entry called "Top" shows the part edge-on. CAM offers the view it
  // means, in the frame THIS setup cuts in (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "look_down_the_spindle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    // 3-axis: the spindle points down the world's -Z, so the camera looks
    // that way whatever it was doing before.
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ctx->ItemClick("//CAM###ToolWindow/###cam_view_spindle");
    ctx->Yield(150); // AlignToFace animates; wait for it to land
    glm::vec3 fwd = glm::normalize(s_App->camera->GetForward());
    IM_CHECK(std::abs(fwd.z + 1.0f) < 0.02f); // looking along -Z
    IM_CHECK(std::abs(fwd.x) < 0.02f);
    IM_CHECK(std::abs(fwd.y) < 0.02f);

    // Indexed: the table has turned 90 degrees about X, so the spindle now
    // looks down the part's +Y. Verifying against the WORLD axes here would
    // pass for a job cut in the wrong frame.
    Doc()->FindCamSetup(setup.id)->rotationA = 90.0;
    ctx->Yield(2);
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ctx->ItemClick("//CAM###ToolWindow/###cam_view_spindle");
    ctx->Yield(150);
    fwd = glm::normalize(s_App->camera->GetForward());
    const glm::dvec3 spindle = cam::FromSetupSpace(
        *Doc()->FindCamSetup(setup.id), glm::dvec3(0, 0, 1));
    const glm::vec3 want = -glm::normalize(glm::vec3(spindle));
    IM_CHECK(glm::length(fwd - want) < 0.03f);
    s_App->camToolOpen = false; // the toolbar button TOGGLES: do not leak it
  };

  // G2-G4 (docs/cam/gui.md): the verbs. Two clicks — the verb, then the face
  // — produce a machinable operation with nothing typed, the viewport shows
  // what will be removed, and the verdict arrives without being asked for.
  t = IM_REGISTER_TEST(engine, "cam", "verbs_two_clicks_to_a_pocket");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A plate with a pocket cleared from its top down to z = 12.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 12), 30, 20, 20));
    const uint64_t voidId = Doc()->GetFeature(1)->GetId();
    Doc()->AddFeature(std::make_unique<BooleanFeature>("Pocket", plate, voidId,
                                                       BooleanOp::Subtract));
    ctx->Yield(2);

    // Click 1: the verb. The toolbar opens the session; no setup exists yet,
    // and the verb makes one rather than sending the user to the panel.
    ToolbarClick(ctx, "###tb_cam");
    ctx->Yield(2);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(Doc()->GetCamSetups().empty());
    ctx->ItemClick("//CAM###ToolWindow/###cam_verb_pocket");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamPocketFloor);

    // Click 2: the face. (The viewport click routes here through
    // SlotPickClicked; calling it directly keeps the test off the camera.)
    // First a curved face, which must be refused in words rather than
    // silently turned into a depth.
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Post", glm::vec3(200, 0, 0), glm::vec3(0, 0, 1), 5.0, 10.0));
    ctx->Yield(2);
    for (const Body& b : Doc()->GetBodies()) {
      if (b.createdBy != Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetId())
        continue;
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u))
          continue; // planar: not the barrel
        ui::SelectionItem curved;
        curved.kind = ui::SelectionItem::Kind::Face;
        curved.element = ElementRef{b.createdBy, name};
        ui::CamFacePicked(*s_App, curved);
        IM_CHECK(!s_App->slotPick.miss.empty());
        IM_CHECK(s_App->slotPick.miss.find("curved") != std::string::npos);
        break;
      }
      break;
    }
    // The slot stays armed through a refusal: the next click is almost always
    // the face they meant, and re-pressing the verb first is a wasted step.
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);

    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - 12.0f) < 1e-4f)
          floor.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(floor.element.IsSet());
    ui::CamFacePicked(*s_App, floor);
    ctx->Yield(2);

    // An operation with every depth derived: the floor is the face, the top
    // is the stock, the stepdown is half the cutter. Nothing was typed.
    const cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    const cam::Operation& op = live->operations[0];
    // 9 mm deep with a 6 mm cutter, so the verb chose adaptive clearing —
    // one verb, the strategy a consequence of the geometry (C8). Without the
    // LGPL library this build clears it concentrically instead.
    IM_CHECK(op.type == (cam::AdaptiveAvailable() ? cam::OpType::Adaptive
                                                  : cam::OpType::Pocket));
    IM_CHECK(std::abs(op.bottomZ - 12.0) < 1e-4);
    IM_CHECK(op.topZ > op.bottomZ);
    IM_CHECK_EQ(op.targets.size(), (size_t)1);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0); // disarmed on success

    // G4: the verdict arrived by itself, in words — and for a job that cuts
    // the part correctly it says nothing ALARMING. The panel checks on a
    // 0.4 mm grid and the RPC defaulted to 0.25, and a cell centre landing on
    // the corner of an island counted the island's whole height as a gouge:
    // "Cuts 6.00 mm INTO the part — that is scrap" about a part the RPC's own
    // check called clean (docs/cam/dogfood-round.md, round 2). Leftover is a
    // WARN and expected here — nothing has cut the outside yet.
    IM_CHECK(!s_App->camVerdict.empty());
    for (const cam::VerdictLine& line : s_App->camVerdict) {
      IM_CHECK_SILENT(line.level != cam::VerdictLine::Level::Bad);
      if (line.level == cam::VerdictLine::Level::Bad)
        ctx->LogError("verdict: %s", line.text.c_str());
    }

    // G3: the picture. While the session is open the stock box and the
    // removed-volume ghost are drawn; the ghost's walls are translucent
    // surfaces, so they land in the quad channel.
    std::vector<PrimVertex> prims;
    std::vector<LineVertex3D> quads;
    ui::AppendCamVisualisation(*s_App, prims, quads);
    IM_CHECK(!prims.empty());
    IM_CHECK(!quads.empty());
    // The ghost spans the cut, top to bottom — it is the material coming off,
    // not an outline of the floor.
    float lowest = 1e9f, highest = -1e9f;
    for (const LineVertex3D& v : quads) {
      lowest = std::min(lowest, v.position.z);
      highest = std::max(highest, v.position.z);
    }
    IM_CHECK(lowest <= (float)op.bottomZ + 1e-3f);
    IM_CHECK(highest >= (float)op.topZ - 1e-3f);

    // Closing the session drops the decision-time drawings and keeps the
    // backplot: the job survives the window that made it.
    ctx->ItemClick("//CAM###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolOpen);
    std::vector<PrimVertex> afterPrims;
    std::vector<LineVertex3D> afterQuads;
    ui::AppendCamVisualisation(*s_App, afterPrims, afterQuads);
    IM_CHECK(afterQuads.empty()); // no stock box, no ghost
    std::vector<PrimVertex> backplot;
    ui::AppendCamToolpathOverlay(*s_App, backplot);
    IM_CHECK(!backplot.empty());
  };

  t = IM_REGISTER_TEST(engine, "cam", "panel_generates_and_backplots");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);

    cam::Setup& setup = Doc()->AddCamSetup("Side A");
    setup.safeZ = 20.0;
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    // Reach the panel the way a user does: click its dock tab. Model owns the
    // shared node (CAM used to steal it by appearing without
    // NoFocusOnAppearing, which hid the model tree), so the tab click is a
    // required step rather than a formality. A tab item's id IS the window's
    // TabId; FocusWindow deliberately does not select dock tabs.
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);

    // The panel's "Add operation" derives its depths from the model rather
    // than defaulting to zeros — the u10 rule: a default that cuts nothing
    // (or everything) is a bug.
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    const cam::Operation& op = live->operations[0];
    IM_CHECK(op.topZ > op.bottomZ);
    // The STOCK top (plate 12 + 1 mm of margin), not the model top: material
    // sits above the part, and a cut that starts below it leaves a skin.
    IM_CHECK(std::abs(op.topZ - 13.0) < 1e-6);
    IM_CHECK(op.stepdown > 0.0);

    // A second tool AND a second operation: tool numbers and operation ids
    // are both small ints, so PushID(int) put them in one id space and ImGui
    // raised "2 visible items with conflicting ID" in a modal. One of each
    // never collides — which is why the first version of this test passed
    // while the panel was broken in front of the user.
    ctx->ItemClick("###cam_tools"); // expand the Tools header
    ctx->Yield();
    ctx->ItemClick("###cam_add_tool");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(3);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);

    // Generate: toolpaths exist, and the backplot draws them.
    ctx->ItemClick("###cam_generate");
    ctx->Yield(2);

    // Hand the shared dock node back to Model NOW, not at the end. A test
    // that fails partway never reaches its own cleanup, and the tab it left
    // selected covers the model tree for everything after it in the shard —
    // which is how a stale assertion here broke tree/context_rename two
    // tests later. Everything below asserts on app state, not on widgets.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel)) {
      ctx->ItemClick(modelWin->TabId);
      ctx->Yield(2);
    }
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);

    std::vector<PrimVertex> prims;
    ui::AppendCamToolpathOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());
    // The tool is drawn to scale where it enters — the "will my cutter fit"
    // picture (docs/cam/gui.md). Its circle sits INSIDE the path's extent, so
    // the check is its geometry: vertices one tool radius from the entry
    // point. (A bounding-box check passed for the wrong reason and was
    // replaced.)
    glm::dvec3 entry(0.0);
    for (const cam::Move& m : s_App->camToolpaths[0].moves)
      if (m.kind == cam::Move::Kind::Cut) {
        entry = m.to;
        break;
      }
    const cam::Setup* job = Doc()->FindCamSetup(s_App->camActiveSetup);
    IM_CHECK(job != nullptr && !job->tools.empty());
    const float radius = (float)(job->tools.front().diameter * 0.5);
    int onRing = 0;
    for (const PrimVertex& v : prims) {
      const float dx = v.posThis.x - (float)entry.x;
      const float dy = v.posThis.y - (float)entry.y;
      if (std::abs(std::sqrt(dx * dx + dy * dy) - radius) < 0.05f)
        onRing++;
    }
    IM_CHECK(onRing > 20); // a 32-segment circle, drawn as thick lines
    // Turning the picture off turns it off — a backplot that cannot be
    // hidden buries the model it is drawn over.
    s_App->camShowToolpath = false;
    std::vector<PrimVertex> none;
    ui::AppendCamToolpathOverlay(*s_App, none);
    IM_CHECK(none.empty());
    s_App->camShowToolpath = true;

    // The toolpath the overlay drew is the one the post would consume: same
    // vector, so the picture cannot disagree with the file.
    std::string gcode, error;
    IM_CHECK(cam::PostProcess(live->post, *live, s_App->camToolpaths, {}, gcode,
                              &error));
    IM_CHECK(gcode.find("G90 G91.1 G21") != std::string::npos);
    IM_CHECK(gcode.find("nan") == std::string::npos);
  };
#endif // HAVE_CAM

#ifdef HAVE_TERMINAL
  // The terminal panel (docs/ui/terminal-panel.md). Off by default, docked
  // right when on, and — the part that decides whether it feels safe — the
  // keyboard goes to it ONLY while the pointer is over it.
  t = IM_REGISTER_TEST(engine, "terminal", "off_by_default_docks_right_on");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    // Off by default: the window does not exist at all.
    IM_CHECK(!s_App->settings.terminalPanel);
    // NoError: this asks a question whose answer is expected to be "no". A
    // bare WindowInfo LOGS an error when it cannot find the window, and
    // ImGuiTestContext::WindowInfo starts with `if (IsError()) return
    // ItemInfoNull()` — so one expected-null lookup poisons the context and
    // EVERY later lookup returns null, whatever the UI is doing.
    //
    // It only bit after a test that had previously turned this panel on: the
    // window object then still exists but is not being submitted, which is a
    // different path inside WindowInfo and the one that logs. That is why it
    // passed alone and failed behind cam/tool_window_follows_the_viewport,
    // and why five fixes aimed at docking, timing and the PTY all missed.
    IM_CHECK(
        ctx->WindowInfo("//Terminal###TerminalPanel", ImGuiTestOpFlags_NoError)
            .Window == nullptr);

    s_App->settings.terminalPanel = true;
    // Wait for the CONDITION, not for a frame count. Turning the panel on
    // creates a window, docks it, and forks a shell through a PTY, and a
    // fixed Yield(6) covered all three only on an idle machine — under four
    // parallel shards the spawn misses the deadline and the test fails on a
    // null window. It passed 4/4 in isolation, which is how a load-dependent
    // wait advertises itself.
    // 90 frames is ~1.5 s — generous for a fork+PTY and quick to fail.
    //
    // DIAGNOSED, not fixed (2026-08-22, second session). It is NOT a timeout
    // and NOT the PTY. Instrumenting DrawTerminalPanel through the failing
    // pair shows the window IS created, IS active and IS docked (DockId 4) —
    // with `Hidden` true, so Begin() returns false, the body is never
    // submitted, and ###term_claude below cannot be found. WindowInfo also
    // declines to return a hidden window, which is the null seen here.
    //
    // NOTE it passes under the default 4-shard run, which puts the two tests
    // in different processes — `--serial` is 177/178. A green suite is not
    // evidence this is fixed.
    //
    // Reproduces deterministically and serially, no load involved:
    //   gui-tests-xvfb.sh -f "tool_window_follows_the_viewport,\
    //                         off_by_default_docks_right_on" --serial
    // and that CAM test is the only one that turns this panel on and off and
    // rebuilds the dock layout (legitimately — it narrows the viewport).
    //
    // Four fixes tried and REJECTED, so nobody repeats them: starting from a
    // rebuilt default layout; waiting on FindWindowByName instead of
    // WindowInfo; ctx->ItemClick(win->TabId); poking
    // DockNode->SelectedTabId. None makes the body submit. The remaining
    // suspect is which node the window lands in after a rebuild, not when.
    ImGuiWindow* win = nullptr;
    for (int i = 0; i < 90; i++) {
      ctx->Yield();
      // NoError, or the first miss poisons the context and the loop can
      // never succeed however long it waits (see the note above).
      win = ctx->WindowInfo("//Terminal###TerminalPanel",
                            ImGuiTestOpFlags_NoError)
                .Window;
      if (win != nullptr && win->DockId != 0 && ui::TerminalRunning(*s_App))
        break;
    }
    IM_CHECK(win != nullptr);
    IM_CHECK(win->DockId != 0); // docked, not floating over the model tree
    // And it opened a shell by itself: the chooser it used to show was a
    // one-shot menu you could only get back to by restarting the app.
    IM_CHECK(ui::TerminalRunning(*s_App));
    // The Claude Code action stays available for the whole session, rather
    // than being a mode chosen once.
    IM_CHECK(ctx->ItemExists("//Terminal###TerminalPanel/###term_claude"));
    // On the RIGHT: its left edge is past the middle of the viewport.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    IM_CHECK(win->Pos.x > vp->Pos.x + vp->Size.x * 0.5f);

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  t = IM_REGISTER_TEST(engine, "terminal", "runs_a_command_and_shows_output");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);

    ui::StartTerminalCommand(*s_App, "/bin/sh",
                             {"-c", "printf 'hello-from-panel'; sleep 30"});
    // Frames are what pump the session, so this is also the proof that the
    // panel drains its PTY while drawing.
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen != nullptr)
        seen = screen->Line(0).find("hello-from-panel") != std::string::npos;
    }
    IM_CHECK(seen);
    IM_CHECK(ui::TerminalRunning(*s_App));

    // Turning the preference off ends the session — a process kept alive
    // behind a closed panel is one nobody can see or stop.
    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
    IM_CHECK(!ui::TerminalRunning(*s_App));
  };

  t = IM_REGISTER_TEST(engine, "terminal", "drag_selects_and_copies_text");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);

    ui::StartTerminalCommand(*s_App, "/bin/sh",
                             {"-c", "printf 'copyme-token\\n'; sleep 30"});
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen != nullptr)
        seen = screen->Line(0).find("copyme-token") != std::string::npos;
    }
    IM_CHECK(seen);

    // Drag across the first line, from its start to past the token.
    ImGuiWindow* win = ctx->WindowInfo("//Terminal###TerminalPanel").Window;
    IM_CHECK(win != nullptr);
    ImGuiTestItemInfo grid =
        ctx->ItemInfo("//Terminal###TerminalPanel/###term_grid");
    IM_CHECK(grid.ID != 0);
    const term::Screen* screen = ui::TerminalScreenOf(*s_App);
    IM_CHECK(screen != nullptr);
    const float cellW = grid.RectFull.GetWidth() / (float)screen->Cols();
    const float cellH = grid.RectFull.GetHeight() / (float)screen->Rows();
    const ImVec2 rowStart(grid.RectFull.Min.x + cellW * 0.25f,
                          grid.RectFull.Min.y + cellH * 0.5f);
    const ImVec2 rowEnd(grid.RectFull.Min.x + cellW * 12.0f,
                        grid.RectFull.Min.y + cellH * 0.5f);
    ctx->MouseMoveToPos(rowStart);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(rowEnd);
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);

    // Copy needs the keyboard, which means the pointer must be over the panel
    // — the same rule as everything else typed here.
    ImGui::SetClipboardText("");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C);
    ctx->Yield(3);
    const char* clip = ImGui::GetClipboardText();
    IM_CHECK(clip != nullptr);
    IM_CHECK_STR_EQ(clip, "copyme-token");

    // A click with no drag clears the selection, as a terminal does — a
    // highlight that never goes away is worse than none.
    ctx->MouseMoveToPos(rowStart);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    ImGui::SetClipboardText("");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C);
    ctx->Yield(3);
    clip = ImGui::GetClipboardText();
    IM_CHECK(clip == nullptr || clip[0] == '\0');

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  t = IM_REGISTER_TEST(engine, "terminal", "hover_decides_who_gets_the_keys");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // one undoable feature
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);

    s_App->settings.terminalPanel = true;
    ctx->Yield(6);
    ui::StartTerminalCommand(
        *s_App, "/bin/sh",
        {"-c", "read line; printf 'got:%s' \"$line\"; sleep 30"});
    ctx->Yield(10);

    ImGuiWindow* win = ctx->WindowInfo("//Terminal###TerminalPanel").Window;
    IM_CHECK(win != nullptr);
    const ImVec2 inside(win->Pos.x + win->Size.x * 0.5f,
                        win->Pos.y + win->Size.y * 0.6f);
    ctx->MouseMoveToPos(inside);
    ctx->Yield(2);
    IM_CHECK(ui::TerminalCapturesKeys(*s_App));

    // Typing reaches the process...
    ctx->KeyChars("hi");
    ctx->KeyPress(ImGuiKey_Enter);
    bool echoed = false;
    for (int i = 0; i < 200 && !echoed; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen == nullptr)
        continue;
      for (int r = 0; r < screen->Rows() && !echoed; r++)
        echoed = screen->Line(r).find("got:hi") != std::string::npos;
    }
    IM_CHECK(echoed);

    // ...and Ctrl+Z here suspends a job rather than undoing a feature. This
    // is the whole reason the gate exists: one chord, two meanings, and the
    // pointer is what says which.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield(4);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the box is still there

    // Pointer over the viewport: the app has its shortcuts back.
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + s_App->viewportSize.x * 0.5f,
               s_App->viewportPos.y + s_App->viewportSize.y * 0.5f));
    ctx->Yield(2);
    IM_CHECK(!ui::TerminalCapturesKeys(*s_App));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield(4);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // undone

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
    IM_CHECK(!ui::TerminalRunning(*s_App));
  };
#endif // HAVE_TERMINAL

  // Frame timing is always measured; the HUD is not shown unless asked for
  // (docs/rendering/frame-timing.md). Both halves asserted here — a
  // measurement nobody can reach and an overlay nobody asked for are the two
  // ways this feature goes wrong.
  // Category "frametime", NOT "perf": the test engine routes a category of
  // exactly "perf" into ImGuiTestGroup_Perfs, which this suite never queues
  // (it queues ImGuiTestGroup_Tests) — the test would list but never run.
  t = IM_REGISTER_TEST(engine, "frametime",
                       "frame_stats_measure_hud_stays_hidden");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(5); // some frames to measure

    // Measured: the app has run frames, and the phases account for all of
    // each one (an unattributed sliver would read as "fast phases, slow
    // frame").
    const render::FrameTimer::Stats fs = s_App->frameTimer.Snapshot();
    IM_CHECK(fs.frames > 0);
    IM_CHECK(s_App->frameTimer.TotalFrames() >= fs.frames);
    IM_CHECK(fs.avgMs > 0.0 && fs.fps > 0.0);
    IM_CHECK(fs.maxMs >= fs.p95Ms && fs.p95Ms >= fs.p50Ms);
    double phaseSum = 0.0;
    for (size_t i = 0; i < render::kPhaseCount; i++)
      phaseSum += fs.phaseMs[i];
    IM_CHECK(std::abs(phaseSum - fs.avgMs) < 0.05 * fs.avgMs + 0.05);

    // Hidden: nothing in the UI opened it, and the window does not exist.
    IM_CHECK(!s_App->perfHudOpen);
    // NoError: asking whether a window is absent must not log an error, or
    // every later lookup in this test returns null. Latent here — it only
    // bites once something else in the process has opened the HUD first.
    IM_CHECK(ctx->WindowInfo("//Frame time###PerfHud", ImGuiTestOpFlags_NoError)
                 .Window == nullptr);

    // The undocumented chord is the door.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    ctx->Yield(2);
    IM_CHECK(s_App->perfHudOpen);
    ImGuiWindow* hud = ctx->WindowInfo("//Frame time###PerfHud").Window;
    IM_CHECK(hud != nullptr);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F);
    ctx->Yield(2);
    IM_CHECK(!s_App->perfHudOpen); // and it closes again
  };

  // Simulation is a mode, and its studies belong to the document. File > New
  // used to leave the simulation toolbar up over a brand-new part.

  // Nothing had ever RUN a study by clicking — the four tests above all stop
  // at setup — and nothing had ever touched topology optimization at all. A
  // 2026-08-23 dogfooding round did both by hand and found the GPU solver
  // returning a ZERO displacement field on a beam-shaped part, which reads
  // downstream as a structure carrying no load: zero compliance, zero
  // sensitivities, and a "converged" report on the third iteration with the
  // volume constraint abandoned (0.107 against a 0.30 target). These two run
  // the real thing end to end and check the answer.

  t = IM_REGISTER_TEST(engine, "sim", "a_static_run_matches_beam_theory");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ctx->ItemClick("//###Model/###study_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Doc()->StudyResults(studyId);

    // Euler-Bernoulli for a uniformly distributed load, steel E = 200 GPa:
    // I = b h^3/12 = 20*10^3/12 = 1666.7 mm^4, w = 100/200 = 0.5 N/mm, so
    // the tip deflection w L^4 / (8 E I) = 0.300 mm. Measured: 0.2961.
    IM_CHECK(rt->maxDisplacement > 0.255);
    IM_CHECK(rt->maxDisplacement < 0.345);
    // The peak stress sits at the clamp; the coarse mesh samples it at the
    // first node inboard (x = 10.6), where w(L-x)^2/2 * c/I = 26.9 MPa.
    // Measured: 27.1. A load along the wrong axis misses this by far more
    // than the band.
    IM_CHECK(rt->maxVonMises > 23.0);
    IM_CHECK(rt->maxVonMises < 31.5);
  };

  t = IM_REGISTER_TEST(engine, "sim", "topology_holds_its_volume_target");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 60.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    // Topology is reachable ONLY through this combo, which had no ### id and
    // so could not be clicked by a test at all — which is why the whole
    // workstream had no clicked coverage.
    ctx->ItemClick("//###Model/###study_analysis");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Topology").c_str());
    ctx->Yield(2);
    const fem::Study* st = Doc()->FindStudy(studyId);
    IM_CHECK(st != nullptr);
    IM_CHECK(st->type == fem::AnalysisType::Topology);

    // The auto mesh is sized for a stress field; a shape needs one that can
    // hold a shape. Four iterations is enough for this gate: the volume
    // constraint is enforced by the OC bisection on every one of them.
    ctx->ItemInputValue("//###Model/###study_mesh_size", "10");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Model/###topo_max_iterations", "4");
    ctx->Yield(2);

    ctx->ItemClick("//###Model/###topo_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Doc()->StudyResults(studyId);
    // The OC bisection enforces the volume every iteration, so this holds
    // whatever the compliance does — UNLESS every sensitivity is zero, which
    // is exactly what a zero displacement field produces. That was the shape
    // the GPU failure took, and it landed on 0.107 against this 0.30.
    IM_CHECK(std::abs(rt->volumeFractionAchieved - st->volumeFraction) < 0.02);
    // A structure carrying a load has compliance > 0.
    IM_CHECK(!rt->complianceHistory.empty());
    IM_CHECK(rt->complianceHistory.back() > 0.0);
  };

  t = IM_REGISTER_TEST(engine, "sim", "new_document_leaves_the_mode");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield();
    IM_CHECK(s_App->simulation.active);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(!s_App->simulation.active);
    ctx->SetRef("//##DockHost");
    IM_CHECK(ctx->ItemExists("**/###tb_box")); // the model toolbar is back
    ctx->SetRef("//##MainMenuBar");
  };

  t = IM_REGISTER_TEST(engine, "viewport", "click_selects_facing_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 bodyCenter = (min + max) * 0.5f;

    // File/New does not reset the camera, so the orbit/pan/zoom left by earlier
    // viewport tests leaks in (the default box is off-origin, so it can even
    // project behind a docked panel). Frame it deterministically: a clean iso
    // direction, centred on the box, far enough back (~2x the fit distance)
    // that the box stays small and the deselect click below lands on empty
    // space rather than the box itself.
    const float radius = glm::length(max - min) * 0.5f;
    s_App->camera->SetTarget(bodyCenter);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(bodyCenter +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   radius * 5.0f);
    ctx->Yield();

    // Click the +X face's center pixel: the ray hits that face's interior, a
    // full half-extent from every edge. (The body-center pixel is unusable
    // since U1b: on a diagonal view it lines up with the near corner, whose
    // edges correctly outrank the face in the unified pick.)
    const glm::vec3 faceCenter(max.x, bodyCenter.y, bodyCenter.z);
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    const Face* face = s_App->faceSelector->GetSelectedFace();
    IM_CHECK(face != nullptr);

    // Selected face must face the camera and sit on the near side of center
    glm::vec3 normal =
        s_App->faceSelector->GetFaceNormal(face->meshId, face->faceIndex);
    glm::vec3 toCamera =
        glm::normalize(s_App->camera->GetPosition() - face->center);
    IM_CHECK(glm::dot(normal, toCamera) > 0.3f);

    // Clicking empty space clears the selection
    ctx->MouseMoveToPos(ImVec2(WorldToWindow(bodyCenter).x,
                               ImGui::GetMainViewport()->Size.y * 0.92f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->faceSelector->GetSelectedFace() == nullptr);
  };

  t = IM_REGISTER_TEST(engine, "sketch", "front_plane_enters_edit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    // A SketchFeature was added and we entered sketch edit (Select is default).
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(Doc()->GetFeature(0)->TypeName() == "Sketch");
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    // Escape leaves sketch edit (nothing pending, Select tool, no selection).
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // The primary way a user opens a sketch now that the Create menu is gone:
  // the toolbar's sketch button arms plane-picking, then a viewport click on a
  // base-plane quad enters edit mode. The other sketch tests take the direct
  // EnterSketchOnPlane() shortcut, so this is the one that guards the whole
  // interactive entry path end to end.
  t = IM_REGISTER_TEST(engine, "sketch", "toolbar_plane_pick_enters_edit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_new_sketch");
    IM_CHECK(s_App->sketchEdit.pickingPlane); // armed, waiting for a plane
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    // Frame the front (XY) plane head-on. The doc is empty, so all three base
    // quads pass through the origin; looking straight down +Z makes a click
    // near the visible outer edge, at world (7,7,0), land unambiguously on XY
    // (the other two are edge-on there).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 0));
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(glm::vec3(0, 0, 10));
    ctx->Yield();

    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(7.0f, 7.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    IM_CHECK(!s_App->sketchEdit.pickingPlane); // pick consumed
    IM_CHECK(s_App->sketchEdit.IsEditing());   // now editing the new sketch
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(sk != nullptr);
    IM_CHECK(sk->TypeName() == "Sketch");
  };

  t = IM_REGISTER_TEST(engine, "sketch", "draw_line_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 1);
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);

    // Entering a sketch aligns the camera to the plane over a 1s animation.
    // Wait for it to settle before projecting click coordinates — otherwise
    // SketchUVToScreen reads a mid-flight camera and the clicks land off
    // target.
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();

    // Select is the default tool now; pick Line, then two viewport clicks place
    // a line. Anchor at the projected origin and offset in screen pixels (an
    // empty sketch is fit tightly, so a UV offset would project off-screen).
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    glm::vec2 c = ui::SketchUVToScreen(*s_App, sketch->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(c.x - 60.0f, c.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(c.x + 60.0f, c.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    // Committing the line rebuilds the sketch feature through the command
    // system, so the pointer captured above now dangles — re-fetch it before
    // reading the entity list (this stale-pointer read, not the clicks, was why
    // this test used to fail).
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    IM_CHECK_EQ(sketch->entities.size(), (size_t)1);

    // Undo removes the line but keeps the sketch
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    IM_CHECK_EQ(sketch->entities.size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "sketch", "rect_extrude_produces_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();

    // Inject a rectangle deterministically via a command (viewport click
    // coordinates are resolution-dependent; drawing is covered separately)
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2}); // 4 lines + corner/H/V constraints
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Extrude from the sketch toolbar (U8a) → preview, then accept
    ToolbarClick(ctx, "###tb_sk_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App)); // extrude is a pending preview
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();

    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->TypeName() == "Extrude");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() > 1.0); // rect x default dist
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_accept_commits_once");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    // Pending: in the doc + rendered, but NOT on the undo stack yet
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(!s_App->commands->CanUndo());

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(s_App->commands->CanUndo()); // now one undoable action

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_cancel_discards");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
    IM_CHECK(!s_App->commands->CanUndo()); // stack untouched
  };

  t = IM_REGISTER_TEST(engine, "preview", "ctrl_z_cancels_pending");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    // While pending, Ctrl+Z cancels the preview rather than touching history
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
  };

  // The model toolbar can extrude a FINISHED sketch. Before U9 the only
  // extrude button lived inside the sketch session, so closing a sketch with
  // ✓ stranded it — the most standard flow in CAD was a dead end.
  t = IM_REGISTER_TEST(engine, "sketch", "model_toolbar_extrudes_finished");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();

    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Finish the sketch — the state the user was previously stuck in.
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->TypeName() == "Extrude");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
  };

  // A pending preview is added straight to the Document (not via a command),
  // so switching tabs used to stash it into the old part permanently — off
  // the undo stack, unremovable. The tab switch must discard it first.
  t = IM_REGISTER_TEST(engine, "preview", "tab_switch_discards_pending");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the preview

    ui::AddPart(*s_App, "Part 2"); // stashes the first tab
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));

    ui::SetActivePart(*s_App, 0); // back to the tab that had the preview
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // not stranded
    IM_CHECK(!s_App->commands->CanUndo());         // and never committed
  };

  // U9: the Hole tool. A hole is NOT sugar for an extruded cut (its tool is
  // built from primitives, so counterbore/countersink are impossible as a
  // cut), and before this it could only be created over the script API.
  t = IM_REGISTER_TEST(engine, "hole", "sk_hole_button_on_existing_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // something to drill into
    ctx->Yield();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    // A plain (Freeform) sketch with two points, then the sketch-toolbar
    // Hole button — the "I already have a point sketch" path.
    EnterSketchOnPlane(ctx, 2); // right/YZ: UV is (y, z), drilling -X
    auto* sketch = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(sketch != nullptr);
    nlohmann::json b4 = DocumentIO::FeatureToJson(*sketch);
    // The default box sits at negative x, so a hole drilled along -X from the
    // YZ plane runs through it; centre it on the box's y/z.
    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    sketch->AddEntity(sketch::Point{{mid.y - 0.3f, mid.z - 0.3f}});
    sketch->AddEntity(sketch::Point{{mid.y + 0.3f, mid.z + 0.3f}});
    nlohmann::json af = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                 Doc()->FeatureCount() - 1, b4, af),
                             *Doc());
    ctx->Yield();

    ToolbarClick(ctx, "###tb_sk_hole");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->TypeName() ==
             "Hole");
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < before - 1e-6);
  };

  // Enter opens the params window; it must NOT also accept it (the pending
  // preview survives the same frame's keypress).
  t = IM_REGISTER_TEST(engine, "hole", "enter_opens_params_without_accepting");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    EnterHoleSketchOnPlane(ctx, 2);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Hole);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Point);
    auto* sketch = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    nlohmann::json b4 = DocumentIO::FeatureToJson(*sketch);
    sketch->AddEntity(sketch::Point{{1.0f, 1.0f}});
    nlohmann::json af = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                 Doc()->FeatureCount() - 1, b4, af),
                             *Doc());
    ctx->Yield();

    const size_t features = Doc()->FeatureCount();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(3);
    IM_CHECK(ui::HasPendingFeature(*s_App));          // params are open
    IM_CHECK_EQ(Doc()->FeatureCount(), features + 1); // the preview only
    ui::CancelPendingFeature(*s_App);
  };

  // Cancelling discards the sketch the tool created when the user drew
  // nothing, and keeps it once they did — undoably either way.
  t = IM_REGISTER_TEST(engine, "hole", "escape_discards_empty_keeps_drawn");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const size_t base = Doc()->FeatureCount();

    EnterHoleSketchOnPlane(ctx, 2); // empty session
    IM_CHECK_EQ(Doc()->FeatureCount(), base + 1);
    ui::CancelHoleSession(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(Doc()->FeatureCount(), base); // discarded
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), base + 1); // and undoable
    ui::GlobalRedo(*s_App);
    ctx->Yield();

    EnterHoleSketchOnPlane(ctx, 2); // this time the user draws
    const size_t withSketch = Doc()->FeatureCount();
    auto* sketch =
        dynamic_cast<SketchFeature*>(Doc()->GetFeature(withSketch - 1));
    nlohmann::json b4 = DocumentIO::FeatureToJson(*sketch);
    sketch->AddEntity(sketch::Point{{1.0f, 1.0f}});
    nlohmann::json af = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(withSketch - 1, b4, af), *Doc());
    ctx->Yield();
    ui::CancelHoleSession(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), withSketch); // kept
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Freeform);
  };

  // The model toolbar arms a face pick; a curved face is refused (and the
  // tool stays armed) while a flat one opens the centre-placing session.
  t = IM_REGISTER_TEST(engine, "hole", "curved_face_refused_flat_accepted");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_cylinder");
    ctx->Yield();
    const size_t base = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_hole");
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Hole);

    // The default cylinder stands on +Y at x = -4 (NextSpot): radius 0.8,
    // spanning y -1..1. Look at it from +Z so the lateral face fills the
    // middle of the view (a model face always outranks the base planes).
    const glm::vec3 axis(-4.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(axis);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(axis + glm::vec3(0, 0, 10));
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(axis + glm::vec3(0.0f, 0.0f, 0.8f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane); // still armed
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(Doc()->FeatureCount(), base);       // nothing created
    IM_CHECK(!s_App->sketchEdit.actionMsg.empty()); // and it says why

    // The flat top cap is accepted.
    s_App->camera->SetTarget(axis + glm::vec3(0, 1, 0));
    s_App->camera->SetUp(glm::vec3(0, 0, -1));
    s_App->camera->SetPosition(axis + glm::vec3(0, 11, 0));
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(axis + glm::vec3(0.0f, 1.0f, 0.0f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Hole);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Point);
  };

  // The whole clicks-only flow the GUI dogfood could not do at all: arm the
  // tool, pick the face, click centres, Enter, Accept — and one Ctrl+Z takes
  // the hole back out.
  t = IM_REGISTER_TEST(engine, "hole", "face_pick_to_accept");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    // Straight down at the middle of the box's top face. Primitives are
    // placed by their min corner, so this must come from the bounding box —
    // a guessed centre lands on a corner edge, which outranks the face.
    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 top((lo.x + hi.x) * 0.5f, hi.y, (lo.z + hi.z) * 0.5f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(top);
    s_App->camera->SetUp(glm::vec3(0, 0, -1));
    s_App->camera->SetPosition(top + glm::vec3(0, 10, 0));
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_hole");
    ctx->Yield();
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Point);
    const size_t withSketch = Doc()->FeatureCount();

    // Two centres, well clear of the face's edges so nothing snaps to them.
    ctx->MouseMoveToPos(WorldToWindow(top + glm::vec3(-0.35f, 0.0f, 0.3f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->MouseMoveToPos(WorldToWindow(top + glm::vec3(0.35f, 0.0f, -0.3f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(3);
    IM_CHECK(!s_App->sketchEdit.IsEditing()); // handed off to the preview
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();

    IM_CHECK_EQ(Doc()->FeatureCount(), withSketch + 1);
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->TypeName() ==
             "Hole");
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < solid - 1e-6);

    ctx->MenuClick("Edit/Undo"); // one step, not two
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), withSketch);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - solid) < 1e-6);
  };

  // Select-first is the house convention: with a planar face already picked,
  // the tool skips straight to placing centres.
  t = IM_REGISTER_TEST(engine, "hole", "preselected_face_skips_pick");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 top((lo.x + hi.x) * 0.5f, hi.y, (lo.z + hi.z) * 0.5f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(top);
    s_App->camera->SetUp(glm::vec3(0, 0, -1));
    s_App->camera->SetPosition(top + glm::vec3(0, 10, 0));
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->faceSelector->GetSelectedFace() != nullptr);

    ToolbarClick(ctx, "###tb_hole");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane); // no second pick asked for
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Hole);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Point);
  };

  // Residue of the clicks-only rounds: with a plane pick armed, the named
  // rows in the feature tree ARE pickable (they used to swallow the click).
  t = IM_REGISTER_TEST(engine, "sketch", "tree_plane_row_consumes_pick");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_new_sketch");
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    ctx->ItemClick("//##DockHost/**/Top Plane");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane); // consumed
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);

    // Unarmed, a single click still only selects (double-click opens) — the
    // gesture must not change for ordinary browsing.
    ctx->ItemClick("//##DockHost/**/Top Plane");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // U10 — the defaults-and-refusals gate.
  //
  // For EVERY creation tool, with its own defaults and no picks made, exactly
  // one of two things must be true: it changes the document, or it refuses
  // visibly. "Accepts and changes nothing" is the bug this suite exists to
  // catch — five of them shipped and were found by hand months later (a hole
  // whose countersink defaulted four times the bore, a split plane at the
  // origin missing the body, drawing views stacked at 1:1, occurrences at one
  // point, an edge-less fillet committing a no-op feature).
  //
  // One registered test per tool (named for its toolbar id) so a failure says
  // WHICH tool regressed; the case is looked up by test name because
  // ImGuiTest::TestFunc is a raw function pointer and cannot capture.
  for (const ToolCase& tc : kToolCases) {
    t = IM_REGISTER_TEST(engine, "defaults", tc.name);
    t->TestFunc = [](ImGuiTestContext* ctx) {
      const ToolCase* tc = FindToolCase(ctx->Test->Name);
      IM_CHECK_SILENT(tc != nullptr);
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New");
      ctx->Yield();
      SetupFor(ctx, tc->needs);
      const ui::DocumentFingerprint before = ui::FingerprintOf(*Doc());

      ToolbarClick(ctx, tc->id);
      ctx->Yield();

      if (tc->expect == Expect::ArmsAPick) {
        // Not a preview at all: the tool asks for a face/plane first.
        IM_CHECK(s_App->sketchEdit.pickingPlane);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        return;
      }

      // Whatever else it is, the click must have DONE something. A toolbar
      // button that is enabled and silently does nothing is the same bug in
      // its earliest form.
      IM_CHECK(ui::HasPendingFeature(*s_App));

      if (tc->expect == Expect::RefusesUntilPicked) {
        const ImGuiTestItemInfo accept =
            ctx->ItemInfo("//###ToolWindow/###tw_accept");
        IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) != 0);
        ui::CancelPendingFeature(*s_App);
        ctx->Yield();
        return;
      }

      // Accept must be OFFERED, not just survivable: a tool that opens
      // already failing (the revolve whose default axis was not in the
      // sketch plane) is exactly what this suite is for. Check the button
      // before taking the programmatic path.
      const ImGuiTestItemInfo accept =
          ctx->ItemInfo("//###ToolWindow/###tw_accept");
      IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);

      ui::AcceptPendingFeature(*s_App);
      ctx->Yield();
      Feature* made = Doc()->GetFeature(Doc()->FeatureCount() - 1);
      IM_CHECK(made != nullptr);
      IM_CHECK(made->GetError().empty());
      if (tc->expect == Expect::Changes)
        IM_CHECK(!(ui::FingerprintOf(*Doc()) == before));
    };
  }

  // U10: a tool that runs cleanly and changes nothing says so — and is still
  // acceptable, because Refine on an already-clean body is a fair answer.
  t = IM_REGISTER_TEST(engine, "defaults", "no_op_warns_without_blocking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // a clean box: nothing to merge
    ctx->Yield();

    ToolbarClick(ctx, "###tb_refine");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    // The warning is a draw-list line, so assert what it is derived from:
    // the feature changed no body volume, and Accept is NOT disabled.
    size_t idx = 0;
    IM_CHECK(ui::ResolvePendingFeature(*s_App, &idx) != nullptr);
    const std::vector<Body>* after = Doc()->CachedBodiesAfter(idx);
    const std::vector<Body>* before = Doc()->CachedBodiesAfter(idx - 1);
    IM_CHECK(after != nullptr && before != nullptr);
    IM_CHECK_EQ(after->size(), before->size());
    IM_CHECK(std::abs((*after)[0].shape.Volume() -
                      (*before)[0].shape.Volume()) < 1e-9);
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // U10: a Body slot is fillable from the named row in the tree, not only
  // from the viewport — and a pick that lands on nothing says so.
  t = IM_REGISTER_TEST(engine, "slots", "body_row_fills_an_armed_slot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t firstBody = Doc()->GetFeature(0)->GetId();

    ToolbarClick(ctx, "###tb_shell"); // Body defaults to the NEWEST producer
    ctx->Yield();
    auto* sh = dynamic_cast<ShellFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sh != nullptr);
    IM_CHECK(sh->bodyId != firstBody);

    ctx->ItemClick("//###ToolWindow/###slot4"); // arm the Body slot
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active());
    ctx->ItemClick("//##DockHost/**/Box 1##body"); // the row, not the viewport
    ctx->Yield();
    IM_CHECK_EQ(sh->bodyId, firstBody);

    // A pick that hits nothing explains itself instead of vanishing.
    ctx->ItemClick("//###ToolWindow/###slot5"); // Open faces (Face kind)
    ctx->Yield();
    // Aim at empty space so the click certainly hits nothing, and use the
    // viewport's own centre so it certainly IS over the viewport.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f, 400.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 400.0f, 40.0f));
    ctx->Yield(2);
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + s_App->viewportSize.x * 0.5f,
               s_App->viewportPos.y + s_App->viewportSize.y * 0.5f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.miss.empty());
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // Coverage gaps the U10 gate exposed: these tools were only ever proved to
  // OPEN. A gate case says "it changes the document or refuses"; it does not
  // say a human can finish the tool and get the geometry they meant.

  // Loft collects its sections from a combo rather than a pick slot, so the
  // gate can only watch it refuse — before this, nothing had ever built one
  // through the GUI at all.
  t = IM_REGISTER_TEST(engine, "loft", "two_profiles_via_the_section_combo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Two closed profiles on parallel planes, 6 apart.
    AddRectSketch(ctx, 1, 1.5f);
    auto* lower = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(lower != nullptr);
    ui::BeginSketchOnPlane(
        *s_App,
        SketchPlane(glm::vec3(0, 6, 0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)),
        "Upper");
    ctx->Yield();
    if (s_App->camera)
      s_App->camera->FinishAnimation();
    auto* upper = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(upper != nullptr);
    const size_t idx = Doc()->FeatureCount() - 1;
    nlohmann::json b4 = DocumentIO::FeatureToJson(*upper);
    upper->AddEntity(sketch::Line{{-0.75f, -0.75f}, {0.75f, -0.75f}});
    upper->AddEntity(sketch::Line{{0.75f, -0.75f}, {0.75f, 0.75f}});
    upper->AddEntity(sketch::Line{{0.75f, 0.75f}, {-0.75f, 0.75f}});
    upper->AddEntity(sketch::Line{{-0.75f, 0.75f}, {-0.75f, -0.75f}});
    nlohmann::json af = DocumentIO::FeatureToJson(*upper);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(idx, b4, af), *Doc());
    ctx->Yield();
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();

    ToolbarClick(ctx, "###tb_loft");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    // Empty, it refuses — the gate's half of the story.
    IM_CHECK((ctx->ItemInfo("//###ToolWindow/###tw_accept").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);
    // The "+ Add section" combo is how sections are collected.
    // Open the combo, pick the section: two steps, because the popup only
    // exists between them. The combo is addressable as "###Add" — see
    // LabeledCombo on why it is "###" and not "##".
    const auto addSection = [&](const char* name) {
      ctx->ItemClick("//###ToolWindow/###Add");
      ctx->Yield();
      // Address the popup by its own window name, not "//$FOCUSED": a combo
      // popup does not take window focus, so $FOCUSED is still the tool
      // window and the entry is nowhere to be found under it.
      IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
      ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
      IM_CHECK_SILENT(popup != nullptr);
      ctx->ItemClick((std::string("//") + popup->Name + "/**/" + name).c_str());
      ctx->Yield();
    };
    addSection("Sketch 1");
    addSection("Upper");
    auto* loft = dynamic_cast<LoftFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(loft != nullptr);
    IM_CHECK_EQ(loft->sketchIds.size(), (size_t)2);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    Feature* made = Doc()->GetFeature(Doc()->FeatureCount() - 1);
    IM_CHECK(made->GetError().empty());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // A frustum between a 3x3 and a 1.5x1.5 square, 6 tall: the exact volume
    // is h/3 * (A1 + A2 + sqrt(A1*A2)) = 2 * (9 + 2.25 + 4.5) = 31.5.
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 31.5) < 0.2);
  };

  // Revolve's default axis was wrong for a top-plane sketch until the gate
  // caught it; this walks the whole tool and checks the solid it makes.
  t = IM_REGISTER_TEST(engine, "revolve", "profile_beside_the_axis_rings");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddRectSketch(ctx, 1, 1.0f, /*centreU=*/3.0f); // 2x2 square at u = 3
    ToolbarClick(ctx, "###tb_revolve");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    Feature* made = Doc()->GetFeature(Doc()->FeatureCount() - 1);
    IM_CHECK(made->GetError().empty());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Pappus: a 2x2 section whose centroid orbits at r = 3 sweeps
    // 2*pi*3 * 4 = 75.4.
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 75.398) < 0.5);
  };

  // Sweep seeded its profile with "the most recent sketch" — which is the
  // path — until the gate caught it. This finishes the tool.
  t = IM_REGISTER_TEST(engine, "sweep", "profile_along_a_path_makes_a_bar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddRectSketch(ctx, 1, 1.0f); // a 2x2 profile on the top plane
    AddPathSketch(ctx, 0);       // a 6-long path rising in the front plane
    ToolbarClick(ctx, "###tb_sweep");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    Feature* made = Doc()->GetFeature(Doc()->FeatureCount() - 1);
    IM_CHECK(made->GetError().empty());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // 2x2 swept 6 = 24, give or take the end conditions.
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 24.0) < 1.0);
  };

  // Chamfer shares FilletFeature with op = Chamfer, so the geometry is well
  // covered — its own button and edge slot were not.
  t = IM_REGISTER_TEST(engine, "chamfer", "edge_via_click_makes_a_chamfer");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(mid +
                               glm::normalize(glm::vec3(1, 1, 1)) * 12.0f);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_chamfer");
    ctx->Yield();
    // The near vertical edge of the box, dead centre of an iso view.
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(hi.x, mid.y, hi.z)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* blend = dynamic_cast<FilletFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(blend != nullptr);
    IM_CHECK_EQ(blend->edges.size(), (size_t)1);
    IM_CHECK(blend->TypeName() == "Chamfer"); // not a fillet in disguise
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < solid - 1e-6);
  };

  // A datum plane is only useful if you can sketch on it — and with a plane
  // pick armed, its row in the tree is a pick, like the base planes (U10).
  t = IM_REGISTER_TEST(engine, "datum", "plane_offset_then_sketch_on_its_row");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_datum_plane");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ctx->ItemInputValue("//###ToolWindow/**/##Offset", 5.0f);
    ctx->Yield();
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    auto* datum = dynamic_cast<DatumPlaneFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(datum != nullptr);
    IM_CHECK(std::abs(datum->offset - 5.0) < 1e-6);

    ToolbarClick(ctx, "###tb_new_sketch");
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    ctx->ItemClick("//##DockHost/**/Plane 1##0"); // the datum's row
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane); // consumed
    IM_CHECK(s_App->sketchEdit.IsEditing());
    auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(sk != nullptr);
    // The sketch tracks the datum, so its plane sits at the datum's offset.
    IM_CHECK(std::abs(sk->plane.GetOrigin().z - 5.0f) < 1e-3f);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
  };

  // U9: the shortcut list. Ctrl+K (and the whole sketch keymap) was
  // discoverable only by reading the source.
  t = IM_REGISTER_TEST(engine, "help", "shortcuts_window_opens");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Help/Keyboard Shortcuts");
    ctx->Yield();
    IM_CHECK(ctx->ItemExists("//###ShortcutsWindow"));
    IM_CHECK(s_App->showShortcuts);
    ctx->WindowClose("//###ShortcutsWindow");
    ctx->Yield();
    IM_CHECK(!s_App->showShortcuts);
  };

  // U9: Shell/Draft/Split/Refine had parameters (and mostly slots) but no
  // creation path outside the script API — a human could not reach them.

  // Shell defaults to the newest body, so the only pick needed is which face
  // to open.
  t = IM_REGISTER_TEST(engine, "shell", "open_face_pick_hollows_the_box");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(
        mid + glm::normalize(glm::vec3(0.4f, 0.9f, 0.7f)) * 12.0f);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_shell");
    ctx->Yield();
    auto* sh = dynamic_cast<ShellFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sh != nullptr);
    IM_CHECK_EQ(sh->bodyId, boxId); // pre-targeted, no body pick needed

    // The dialog says why it cannot finish yet, and Accept is refused until
    // it can (the failure was only a red row in the feature tree before).
    IM_CHECK(
        !Doc()->FindFeatureById(s_App->pendingFeatureId)->GetError().empty());
    IM_CHECK((ctx->ItemInfo("//Shell###ToolWindow/###tw_accept").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);

    ctx->ItemClick("//Shell###ToolWindow/###slot5"); // Open faces
    IM_CHECK(s_App->slotPick.Active());
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(mid.x, hi.y, mid.z)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(sh->openFaces.size(), (size_t)1);
    IM_CHECK(
        Doc()->FindFeatureById(s_App->pendingFeatureId)->GetError().empty());
    IM_CHECK((ctx->ItemInfo("//Shell###ToolWindow/###tw_accept").ItemFlags &
              ImGuiItemFlags_Disabled) == 0);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    const double hollow = Doc()->GetBodies()[0].shape.Volume();
    IM_CHECK(hollow > 0.1 && hollow < solid - 1e-3);
  };

  // Draft has no body slot, and nothing in the UI ever wrote bodyId — Execute
  // rejects faces whose ref does not match it, so the pick must set it.
  t = IM_REGISTER_TEST(engine, "draft", "face_pick_names_the_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(
        mid + glm::normalize(glm::vec3(1.0f, 0.5f, 0.6f)) * 12.0f);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_draft");
    ctx->Yield();
    auto* dr = dynamic_cast<DraftFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(dr != nullptr);
    IM_CHECK_EQ(dr->bodyId, (uint64_t)0);
    IM_CHECK(s_App->slotPick.Active()); // the face slot armed itself

    // The +X side face: a face parallel to the XY neutral plane cannot be
    // tapered, so the top/bottom are the wrong pick here.
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(hi.x, mid.y, mid.z)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(dr->faces.size(), (size_t)1);
    IM_CHECK_EQ(dr->bodyId, boxId); // derived from the pick

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - before) > 1e-3);
  };

  // Split's default knife is the XY plane at the body's mid-height, so the
  // preview shows two pieces before the user touches anything.
  t = IM_REGISTER_TEST(engine, "split", "default_plane_cuts_the_body_in_two");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    ToolbarClick(ctx, "###tb_split");
    ctx->Yield();
    auto* sp = dynamic_cast<SplitFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sp != nullptr);
    IM_CHECK(sp->bodyId != 0);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2); // live preview

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    double total = 0.0;
    for (const Body& b : Doc()->GetBodies())
      total += b.shape.Volume();
    IM_CHECK(std::abs(total - solid) < 1e-3); // split keeps every piece
  };

  // The bug this guards: the offset is measured FROM the chosen plane, and
  // it used to be derived once (as the body's mid-HEIGHT) when the tool
  // opened. Pick a datum plane afterwards and that number means something
  // else — a user's datum sat on the box's top face, the stale 1.0 put the
  // knife above the body, and the split refused while the default plane had
  // cut it cleanly.
  t = IM_REGISTER_TEST(engine, "split",
                       "switching_the_plane_re_centers_the_knife");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    // A datum plane on the box's top face — the worst case, and the one hit
    // in practice: tangent, so any offset in the wrong direction misses.
    ToolbarClick(ctx, "###tb_datum_plane");
    ctx->Yield();
    auto* dp = dynamic_cast<DatumPlaneFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(dp != nullptr);
    glm::vec3 bmin, bmax;
    Doc()->GetBodies()[0].shape.GetBoundingBox(bmin, bmax);
    dp->offset = bmax.z; // exactly the top face
    Doc()->Regenerate();
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();

    ToolbarClick(ctx, "###tb_split");
    ctx->Yield();
    auto* sp = dynamic_cast<SplitFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sp != nullptr);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2); // default plane cuts

    // Switch the knife to the datum. Same two-step combo dance as the loft
    // sections: the popup only exists between the clicks, and it does not
    // take window focus.
    ctx->ItemClick("//###ToolWindow/###Plane");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick(
        (std::string("//") + popup->Name + "/**/" + dp->GetName()).c_str());
    ctx->Yield();

    IM_CHECK(sp->datumId == dp->GetId());
    IM_CHECK(sp->GetError().empty()); // ...not "does not divide"
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2); // still two pieces
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
  };

  // Help > About did nothing for as long as it had existed: OpenPopup() was
  // called inside the BeginMenu scope, so the id was registered against the
  // menu window and the BeginPopupModal outside never matched it. Nothing
  // caught it because "a menu item opens a window" had no test.
  t = IM_REGISTER_TEST(engine, "help", "about_opens_and_names_the_build");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(!s_App->showAbout);
    ctx->MenuClick("Help/About Designer");
    ctx->Yield();
    IM_CHECK(s_App->showAbout);
    // It is a real window with the build in it, not just a flag flip.
    ctx->SetRef("//###AboutDesigner");
    IM_CHECK(ctx->ItemExists("###about_copy"));
    // The attributions are a tree node, collapsed by default; open it so the
    // test covers the table actually building.
    ctx->ItemClick("Third-party components");
    ctx->Yield();
    ctx->ItemClick("###about_close");
    ctx->Yield();
    IM_CHECK(!s_App->showAbout);
  };

  // Section view on an ASSEMBLY tab, through the tool. The bugs this pins:
  // the cut had no filled caps (you saw the inside of hollow shells), the
  // plane defaulted to the world origin (a model built elsewhere lost
  // everything or nothing), and a second cut was impossible.
  t = IM_REGISTER_TEST(engine, "section",
                       "assembly_cut_is_capped_and_centered");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Modeled a metre from the origin: a world-origin plane misses it.
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Block", glm::vec3(1000, 0, 0), 40, 20, 20));
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    for (int i = 0; i < 2; i++) {
      Occurrence occ;
      occ.name = i ? "B2" : "B1";
      occ.partId = s_App->parts[0].id;
      occ.fixed = true;
      occ.position = glm::vec3(0.0f, i * 100.0f, 0.0f);
      s_App->assembly->AddOccurrence(occ);
    }
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)0);

    // The toolbar button opens the tool AND seeds the first cut.
    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK(s_App->sectionToolOpen);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);

    // Derived, not zero: the plane sits inside the model's own span.
    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 p0 = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::vec3(p0);
    IM_CHECK(p0.w > glm::dot(bmin, n) && p0.w < glm::dot(bmax, n));

    // One cap per cut body per occurrence — the whole point.
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)2);

    // Reverse keeps the other side: same surface, opposite half-space.
    const uint64_t firstId = s_App->sections[0].id;
    ctx->SetRef("//Section###ToolWindow");
    ctx->ItemClick("**/###reverse");
    ctx->Yield(3);
    IM_CHECK(s_App->sections[0].reverse);
    const glm::vec4 p1 = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    IM_CHECK(glm::dot(glm::vec3(p1), n) < 0.0f);            // flipped normal
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)2); // still capped

    // A second cut, which must intersect the first (and trim its caps).
    ctx->ItemClick("###add");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)2);
    IM_CHECK(s_App->sections[1].id != firstId);
    IM_CHECK_GT(s_App->sectionCapMeshes.size(), (size_t)0);

    // Deleting a row takes its cut with it.
    ctx->ItemClick("###clear");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)0);
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)0);
  };

  // The drag arrow, driven THROUGH THE VIEWPORT — a real mouse press at the
  // arrow's screen position, not a direct call into the handler.
  //
  // The first version of this test called ui::HandlePreviewHandleInput
  // itself and passed while the feature was broken for every user:
  // ViewportInput only routed clicks to the handle layer when a pending
  // FEATURE existed, so a section drag fell through to the picker and
  // selected whatever part sat behind the arrow. Testing the handler is not
  // testing the path.
  // P2: a cut can follow a datum plane and be tilted off it, and a source
  // that stops resolving stops the CUT rather than moving it somewhere else.
  t = IM_REGISTER_TEST(engine, "section", "datum_source_and_tilt");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
    auto datum = std::make_unique<DatumPlaneFeature>("D");
    datum->basePlane = PlaneType::XY;
    datum->offset = 6.0;
    Doc()->AddFeature(std::move(datum));
    ctx->Yield(2);
    // AFTER insertion: the document owns id assignment, so an id read from
    // the feature before AddFeature is not the one it ends up with.
    const uint64_t datumId = Doc()->GetFeature(1)->GetId();
    IM_CHECK(datumId != 0);

    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    ui::AppState::SectionView& sec = s_App->sections[0];

    // Point it at the datum: the cut plane lands ON the datum, not on the
    // world origin.
    sec.planeId = datumId;
    sec.offset = 0.0f;
    ctx->Yield();
    glm::vec4 p = ui::SectionPlaneOf(*s_App, sec);
    IM_CHECK(std::abs(p.w - 6.0f) < 1e-3f);
    IM_CHECK(std::abs(p.z - 1.0f) < 1e-3f);
    IM_CHECK(ui::SectionSourceResolved(*s_App, sec));

    // Tilt 30 degrees: the normal turns, and the plane still passes through
    // the point it was pivoted about (dot(anchor, n) == d). A tilt that also
    // slid the cut would break the second check.
    sec.tiltU = 30.0f;
    ctx->Yield();
    p = ui::SectionPlaneOf(*s_App, sec);
    IM_CHECK(std::abs(p.z - 1.0f) > 1e-3f); // no longer straight up
    const glm::vec3 anchor(0.0f, 0.0f, 6.0f);
    IM_CHECK(std::abs(glm::dot(anchor, glm::vec3(p)) - p.w) < 1e-3f);

    // Delete the datum: the reference dangles, so the cut stops and says so
    // rather than quietly cutting along Z=0.
    Doc()->RemoveFeature(1);
    ctx->Yield(2);
    IM_CHECK(!ui::SectionSourceResolved(*s_App, s_App->sections[0]));
    const ClipPlaneArray planes = ui::SectionPlanes(*s_App);
    IM_CHECK(planes[0].w == 0.0f && planes[0].x == 0.0f &&
             planes[0].y == 0.0f && planes[0].z == 0.0f);
    ui::ClearSections(*s_App);
    s_App->sectionToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "section", "offset_arrow_drags_the_cut");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    const float before = s_App->sections[0].offset;
    s_App->selection.Clear();

    // Where the arrow is on screen: anchor + half its length along the normal.
    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 plane = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::normalize(glm::vec3(plane));
    const glm::vec3 c = (bmin + bmax) * 0.5f;
    const glm::vec3 anchor = c - n * (glm::dot(c, n) - plane.w);
    const float L = 0.9f * ui::GizmoScale(*s_App);
    const glm::vec2 grab = ui::WorldToScreen(*s_App, anchor + n * (0.5f * L));
    const glm::vec2 to = ui::WorldToScreen(*s_App, anchor + n * (2.0f * L));

    ctx->MouseMoveToPos(ImVec2(grab.x, grab.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active); // the arrow took the click
    IM_CHECK(s_App->handleDrag.sectionId == s_App->sections[0].id);
    IM_CHECK(s_App->handleDrag.featureId == 0); // no feature involved
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);

    IM_CHECK(std::abs(s_App->sections[0].offset - before) > 1e-3f);
    IM_CHECK(!s_App->handleDrag.active);
    // ...and the click went to the ARROW, not through it to the geometry.
    IM_CHECK(s_App->selection.Empty());
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the document is untouched
  };

  // The same drag on an ASSEMBLY tab. It highlighted on hover and then did
  // nothing: the section arrow is the only direct-manipulation handle an
  // assembly has, and the block that routes clicks to it lived only in the
  // part path — so the click fell through to the picker.
  t = IM_REGISTER_TEST(engine, "section", "offset_arrow_drags_on_an_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::AddAssembly(*s_App, "Rig");
    ctx->Yield();
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly != nullptr); // we really are on an assembly tab

    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    const float before = s_App->sections[0].offset;
    s_App->selection.Clear();
    ui::SelectOccurrence(*s_App, 0); // no gizmo to compete for the click
    ctx->Yield();

    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 plane = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::normalize(glm::vec3(plane));
    const glm::vec3 c = (bmin + bmax) * 0.5f;
    const glm::vec3 anchor = c - n * (glm::dot(c, n) - plane.w);
    const float L = 0.9f * ui::GizmoScale(*s_App);
    const glm::vec2 grab = ui::WorldToScreen(*s_App, anchor + n * (0.5f * L));
    const glm::vec2 to = ui::WorldToScreen(*s_App, anchor + n * (2.0f * L));

    // Hover first: the highlight and the grab must agree about what is there.
    ctx->MouseMoveToPos(ImVec2(grab.x, grab.y));
    ctx->Yield(2);
    IM_CHECK_EQ(ui::OwnerOf(s_App->hoveredHandle), ui::HandleOwner::Section);

    // A rebuild removes and re-adds every mesh (and recomputes an OCC
    // half-space boolean per body), so mesh ids moving mid-drag IS the lag:
    // on an assembly that ran for every part, every frame.
    IM_CHECK(!s_App->documentMeshes.empty());
    const uint32_t meshIdBefore = s_App->documentMeshes[0].meshId;

    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    IM_CHECK_EQ(s_App->handleDrag.sectionId, s_App->sections[0].id);
    // Mid-drag: the cut previews through the shader clip, and NOTHING was
    // rebuilt. The caps settle on release, exactly as they already did for
    // the tool window's offset slider.
    IM_CHECK_EQ(s_App->documentMeshes[0].meshId, meshIdBefore);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(3);
    IM_CHECK(std::abs(s_App->sections[0].offset - before) > 1e-3f);
    IM_CHECK(s_App->selection.Empty()); // the arrow took it, not the part
    // ...and on release the caps DO follow: the rebuild happened.
    IM_CHECK(s_App->documentMeshes[0].meshId != meshIdBefore);
  };

  // Inserting a SUB-ASSEMBLY through the UI. Until this landed the only way
  // to nest an assembly was the RPC — the insert popup filtered to
  // DocKind::Part and silently skipped everything else.
  t = IM_REGISTER_TEST(engine, "assembly", "insert_places_a_sub_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    // A unit holding that part, then a top assembly to place it in.
    const size_t unitIdx = ui::AddAssembly(*s_App, "Unit");
    ui::SetActivePart(*s_App, unitIdx);
    Occurrence inner;
    inner.name = "Inner";
    inner.partId = s_App->parts[0].id;
    inner.fixed = true;
    s_App->assembly->AddOccurrence(inner);
    const size_t topIdx = ui::AddAssembly(*s_App, "Top");
    ui::SetActivePart(*s_App, topIdx);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_insert");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    // The assembly is offered with its part count, in its own section.
    ctx->ItemClick(
        (std::string("//") + popup->Name + "/**/Unit  (1 parts)").c_str());
    ctx->Yield(3);

    IM_CHECK_EQ(s_App->assembly->OccurrenceCount(), (size_t)1);
    const Occurrence& placed = s_App->assembly->GetOccurrences()[0];
    IM_CHECK(placed.refKind == OccurrenceRef::Assembly);
    IM_CHECK(placed.partId == s_App->parts[unitIdx].id);
    // ...and it renders: the leaf inside it reaches the scene.
    ctx->Yield(2);
    IM_CHECK_GT(s_App->documentMeshes.size(), (size_t)0);

    // The assembly being edited is never offered — it cannot contain itself.
    ToolbarClick(ctx, "###tb_insert");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK(!ctx->ItemExists(
        (std::string("//") + popup->Name + "/**/Top  (1 parts)").c_str()));
    ctx->KeyPress(ImGuiKey_Escape);
  };

  // Dragging a SUB-ASSEMBLY: its parts must move WITH it, keeping their
  // places inside the unit. PushOccurrenceLive used to set every mesh of the
  // dragged occurrence to the occurrence's own matrix — true when a mesh IS
  // the part, but for a unit's leaves it collapsed them all onto its origin
  // for the length of the drag; they snapped back only because releasing
  // triggers a resync that recomposes them.
  t = IM_REGISTER_TEST(engine, "assembly", "dragging_a_unit_keeps_its_shape");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    // A unit with two parts 500 mm apart, so a collapse is unmistakable.
    const size_t unitIdx = ui::AddAssembly(*s_App, "Unit");
    ui::SetActivePart(*s_App, unitIdx);
    for (int i = 0; i < 2; i++) {
      Occurrence inner;
      inner.name = i ? "Far" : "Near";
      inner.partId = s_App->parts[0].id;
      inner.fixed = true;
      inner.position = glm::vec3(i * 500.0f, 0.0f, 0.0f);
      s_App->assembly->AddOccurrence(inner);
    }
    const size_t topIdx = ui::AddAssembly(*s_App, "Top");
    ui::SetActivePart(*s_App, topIdx);
    Occurrence unit;
    unit.name = "Unit1";
    unit.partId = s_App->parts[unitIdx].id;
    unit.refKind = OccurrenceRef::Assembly;
    s_App->assembly->AddOccurrence(unit);
    ctx->Yield(3);

    // Each leaf knows where it sits inside the unit — 0 and 500 apart.
    std::vector<float> localX;
    for (const ui::DocumentRenderMesh& m : s_App->documentMeshes)
      localX.push_back(m.localXf[3].x);
    IM_CHECK_EQ(localX.size(), (size_t)2);
    IM_CHECK(std::abs(std::abs(localX[0] - localX[1]) - 500.0f) < 1e-3f);

    // Move the unit the way a drag does, and the composed transforms must
    // stay 500 apart rather than both landing on the new origin.
    Occurrence* placed =
        s_App->assembly->FindById(s_App->assembly->GetOccurrences()[0].id);
    placed->position = glm::vec3(1000.0f, 0.0f, 0.0f);
    ui::PushOccurrenceLive(*s_App, placed->id);
    ctx->Yield();
    // Ask the RENDERER what it is drawing, not the arithmetic this test
    // could do itself — the first version of this check recomputed
    // base * localXf and passed with the fix reverted.
    std::vector<float> worldX;
    for (const ui::DocumentRenderMesh& m : s_App->documentMeshes) {
      const auto xf = s_App->meshRenderer->GetInstanceTransform(m.meshId);
      IM_CHECK_SILENT(xf.has_value());
      worldX.push_back((*xf)[3].x);
    }
    IM_CHECK_EQ(worldX.size(), (size_t)2);
    IM_CHECK(std::abs(std::abs(worldX[0] - worldX[1]) - 500.0f) < 1e-3f);
    IM_CHECK(std::abs(std::min(worldX[0], worldX[1]) - 1000.0f) < 1e-3f);
  };

  // Mating onto a face INSIDE a placed unit, through the viewport. The core
  // could express it (Connector::innerPath, v57) but nothing could produce
  // one: both pickers resolved the hit occurrence through PartDocById, which
  // is null for a sub-assembly, so a click inside a placed unit returned no
  // candidate at all and the mate tool simply ignored it.
  t = IM_REGISTER_TEST(engine, "assembly", "mate_picks_a_face_inside_a_unit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const size_t unitIdx = ui::AddAssembly(*s_App, "Unit");
    ui::SetActivePart(*s_App, unitIdx);
    Occurrence inner;
    inner.name = "Inner";
    inner.partId = s_App->parts[0].id;
    inner.fixed = true;
    const uint64_t innerId = s_App->assembly->AddOccurrence(inner);
    const size_t topIdx = ui::AddAssembly(*s_App, "Top");
    ui::SetActivePart(*s_App, topIdx);
    Occurrence unit;
    unit.name = "Unit1";
    unit.partId = s_App->parts[unitIdx].id;
    unit.refKind = OccurrenceRef::Assembly;
    unit.fixed = true;
    s_App->assembly->AddOccurrence(unit);
    ctx->Yield(3);

    // Aim at the unit's geometry and ask the mate picker what is there.
    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec2 screen =
        ui::WorldToScreen(*s_App,
                          glm::vec3(bmax.x, bmax.y, (bmin.z + bmax.z) * 0.5f) -
                              glm::vec3(0.1f, 0.1f, 0.0f)) -
        s_App->viewportPos;
    const Ray ray = s_App->faceSelector->ScreenToWorldRay(
        screen.x, screen.y, static_cast<int>(s_App->viewportSize.x),
        static_cast<int>(s_App->viewportSize.y), *s_App->camera);
    ui::AssemblyFacePick pick;
    IM_CHECK(ui::PickAssemblyFace(*s_App, ray, pick)); // was false: no pick
    IM_CHECK(pick.occurrenceId == s_App->assembly->GetOccurrences()[0].id);
    IM_CHECK(!pick.face.name.empty());
    // ...and it says WHICH leaf, which is what the connector needs.
    IM_CHECK_EQ(pick.innerPath.size(), (size_t)1);
    IM_CHECK(pick.innerPath[0] == innerId);
  };

  // Refine's whole point is the seam a boolean leaves behind: count the
  // B-Rep faces through the render mesh, which needs no naming.
  t = IM_REGISTER_TEST(engine, "refine", "merges_the_seam_left_by_a_union");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    auto* box2 = dynamic_cast<BoxFeature*>(Doc()->GetFeature(1));
    IM_CHECK(box2 != nullptr);
    box2->position = glm::vec3(-2.0f, 0.0f, 0.0f); // shares box 1's +X face
    Doc()->Regenerate();
    ctx->Yield();

    ToolbarCreateAccept(ctx, "###tb_boolean"); // union of the two producers
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    const auto faceCount = [](const CADShape& shape) {
      const CADShape::Mesh mesh = shape.GenerateMesh();
      return mesh.triangleFaces.empty()
                 ? (uint32_t)0
                 : *std::max_element(mesh.triangleFaces.begin(),
                                     mesh.triangleFaces.end()) +
                       1;
    };
    const uint32_t seamed = faceCount(Doc()->GetBodies()[0].shape);
    const double volume = Doc()->GetBodies()[0].shape.Volume();

    ToolbarCreateAccept(ctx, "###tb_refine");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(faceCount(Doc()->GetBodies()[0].shape) < seamed);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - volume) < 1e-3);
  };

  // Direct-manipulation preview handles (docs/ui/direct-manipulation.md):
  // drag the viewport arrow to set the pending extrude's distance.
  t = IM_REGISTER_TEST(engine, "handles", "extrude_arrow_drag_sets_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    ToolbarClick(ctx, "###tb_sk_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    auto* ext =
        dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(ext->distanceExpr.empty());

    // Frame deterministically from an iso direction so the extrude axis is
    // draggable on screen (facing the plane it would be view-parallel). The
    // target sits below the profile so the handle projects in the upper half
    // of the viewport, clear of the bottom-center preview dialog.
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const glm::vec3 N = glm::normalize(sketch->plane.GetNormal());
    const glm::vec3 C = sketch->plane.PlaneToWorld(glm::vec2(2.0f, 1.0f));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(C + glm::vec3(0.0f, -3.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(C + glm::vec3(0.0f, -3.0f, 0.0f) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   12.0f);
    ctx->Yield(2);

    // Grab the arrow mid-shaft and pull one world unit further out.
    const double d0 = ext->distance;
    const float L = 0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
    const glm::vec3 anchor = C + N * static_cast<float>(d0);
    const glm::vec3 grab = anchor + N * (0.45f * L);
    ctx->MouseMoveToPos(WorldToWindow(grab));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(grab + N * 1.0f));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active); // the click grabbed the handle
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - (d0 + 1.0)) < 0.05);
    IM_CHECK(ui::HasPendingFeature(*s_App)); // still previewing

    // Esc mid-drag restores the grab-time value and keeps the preview.
    const double d1 = ext->distance;
    const glm::vec3 anchor1 = C + N * static_cast<float>(d1);
    ctx->MouseMoveToPos(WorldToWindow(anchor1 + N * (0.45f * L)));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(anchor1 + N * (0.45f * L + 1.0f)));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - d1) < 1e-6);
    IM_CHECK(ui::HasPendingFeature(*s_App)); // Esc consumed by the drag only

    // A click away from the handle falls through to picking: no value change,
    // preview still pending.
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + 30.0f, s_App->viewportPos.y + 30.0f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - d1) < 1e-6);
    IM_CHECK(ui::HasPendingFeature(*s_App));

    // Accept commits the dragged value as one undoable feature.
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(s_App->commands->CanUndo());
  };

  t = IM_REGISTER_TEST(engine, "handles", "fillet_marker_drag_sets_radius");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    // Preview a fillet on a known edge (the picking flow is covered by the
    // fillet tests; this one is about the handle).
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.edgeNames.empty());
    const ElementRef ref{body.createdBy,
                         body.elementMap.edgeNames.begin()->second};
    ui::BeginPreviewFeature(*s_App,
                            std::make_unique<FilletFeature>(
                                "Fillet", std::vector<ElementRef>{ref}, 0.4));
    ctx->Yield(2);
    auto* fil = dynamic_cast<FilletFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(fil != nullptr);

    // Recompute the handle's anchor/direction the way PreviewHandles does:
    // the picked edge resolved on the blend's INPUT body (the displayed body
    // is already blended and renames its edges), pulled away from the body
    // center ⟂ to the edge.
    const std::vector<Body>* input = Doc()->CachedBodiesAfter(0);
    IM_CHECK(input != nullptr);
    const auto eb = Measure::ElementBounds(*input, ref);
    IM_CHECK(eb.has_value());
    const glm::vec3 P = (eb->min + eb->max) * 0.5f;
    const glm::vec3 T = glm::normalize(eb->max - eb->min);
    glm::vec3 bmin, bmax;
    (*input)[0].shape.GetBoundingBox(bmin, bmax);
    const glm::vec3 bodyCenter = (bmin + bmax) * 0.5f;
    glm::vec3 out = P - bodyCenter;
    out -= glm::dot(out, T) * T;
    IM_CHECK(glm::length(out) > 1e-4f);
    out = glm::normalize(out);

    // Frame from iso, target below the body so the handle clears the dialog.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(bodyCenter + glm::vec3(0.0f, -2.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(bodyCenter + glm::vec3(0.0f, -2.0f, 0.0f) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   (glm::length(bmax - bmin) * 2.5f));
    ctx->Yield(2);

    const float L = 0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
    const double r0 = fil->radius;
    ctx->MouseMoveToPos(WorldToWindow(P + out * (0.45f * L)));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(P + out * (0.45f * L + 0.3f)));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    fil = dynamic_cast<FilletFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(fil != nullptr);
    IM_CHECK(std::abs(fil->radius - (r0 + 0.3)) < 0.05);

    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "rib", "menu_rib_previews_and_adds_volume");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Box + an open-line profile above its top (the test_rib.cpp setup).
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Base", glm::vec3(0), 10.0, 10.0, 10.0));
    auto sk = std::make_unique<SketchFeature>(
        "P", SketchPlane(glm::vec3(5, 5, 15), glm::vec3(0, 0, -1),
                         glm::vec3(1, 0, 0)));
    sk->AddEntity(sketch::Line{{-3, 0}, {3, 0}});
    Doc()->AddFeature(std::move(sk));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    const double v0 = Doc()->GetBodies()[0].shape.Volume();

    ToolbarClick(ctx, "###tb_rib");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK(ui::ResolvePendingFeature(*s_App)->TypeName() == "Rib");
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)3);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() > v0 + 1.0);
  };

  t = IM_REGISTER_TEST(engine, "thread", "face_pick_fills_the_slot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 20.0));
    ctx->Yield();

    ToolbarClick(ctx, "###tb_thread");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK(s_App->slotPick.Active()); // the face slot armed itself

    // Click the boss's camera-facing flank: the slot routes the face pick.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 10));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0, 0, 10) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   40.0f);
    ctx->Yield(2);
    const glm::vec3 flank =
        glm::normalize(glm::vec3(1, 1, 0)) * 4.0f + glm::vec3(0, 0, 10);
    ctx->MouseMoveToPos(WorldToWindow(flank));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);

    auto* th = dynamic_cast<ThreadFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(th != nullptr);
    IM_CHECK(!th->face.name.empty());
    IM_CHECK(th->GetError().empty()); // resolved + cylindrical + M8 fits r=4
    IM_CHECK(th->overlay.valid);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
  };

  // Repro for the reported circular-pattern segfault (dogfooding backlog):
  // preview a pattern on a body, switch it to Circular, and work the axis/
  // count/angle widgets across frames — the crash lived in this flow.
  t = IM_REGISTER_TEST(engine, "pattern",
                       "circular_preview_param_edits_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_pattern");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    ctx->SetRef("//Pattern###ToolWindow");
    ctx->ComboClick("###Type/Circular");
    ctx->Yield(2);
    {
      auto* pat =
          dynamic_cast<PatternFeature*>(ui::ResolvePendingFeature(*s_App));
      IM_CHECK(pat != nullptr);
      IM_CHECK_EQ((int)pat->kind, (int)PatternKind::Circular);
    }
    ctx->ComboClick("###Axis/Z");
    ctx->Yield(2);
    // Type into count/angle, then mouse-DRAG both (the per-frame regen path
    // the crash log showed: one resync per frame).
    ctx->ItemInputValue("Count/##count", 6);
    ctx->Yield(2);
    // Sustained param editing — the reported crash came after hundreds of
    // per-frame regens (outline mesh ids in the 500s). Mutate + Regenerate
    // per frame is byte-identical to what the dialog's angle DragScalar does
    // (HandleEdit with commit=false); 400 frames of it storms the resync +
    // silhouette-rebuild path the crash log pointed at.
    for (int i = 0; i < 400; i++) {
      auto* pat =
          dynamic_cast<PatternFeature*>(ui::ResolvePendingFeature(*s_App));
      IM_CHECK(pat != nullptr);
      pat->angleDeg = 30.0 + (i % 33) * 10.0;
      pat->count = 3 + (i / 40) % 5;
      Doc()->Regenerate();
      ctx->Yield();
    }

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->GetError().empty());
    IM_CHECK(Doc()->GetBodies().size() >= (size_t)1);
  };

  // A linear pattern's default spacing must clear the source body — copies
  // of a wide body used to overlap-fuse into one blob ("pattern did
  // nothing"). 3 separated copies = 3x the volume and a wider bbox.
  t = IM_REGISTER_TEST(engine, "pattern", "linear_default_spacing_separates");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Wide", glm::vec3(0), 10.0, 4.0, 4.0));
    ctx->Yield();
    const double v0 = Doc()->GetBodies()[0].shape.Volume();

    ToolbarClick(ctx, "###tb_pattern");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    // Three clear copies: total volume 3x the source (fused copies would be
    // barely more than 1x), spread over ~2*spacing + width along X.
    double total = 0.0;
    for (const Body& b : Doc()->GetBodies())
      total += b.shape.Volume();
    IM_CHECK(std::abs(total - 3.0 * v0) < 1e-3);
  };

  t = IM_REGISTER_TEST(engine, "view", "planes_autohide_with_geometry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(ui::EffectivePlaneVisibility(*s_App)); // empty part → planes shown

    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK(!ui::EffectivePlaneVisibility(*s_App)); // has a body → hidden
  };

  // S1 shadows: toggling the map off/on and sectioning while it renders is
  // exactly where barrier/layout bugs crash the device — assert we survive
  // several frames of each with a body in the scene.
  t = IM_REGISTER_TEST(engine, "view", "shadow_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showShadows = false;
    ctx->Yield(3);
    s_App->showShadows = true;
    ctx->Yield(3);
    ui::AddSection(*s_App, 0); // cut on XY
    ctx->Yield(5);
    ui::ClearSections(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->showShadows);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  // S2 SSAO: same barrier/layout stress as the shadow test — the prepass +
  // AO + blur chain toggled off/on while sectioned, over several frames.
  t = IM_REGISTER_TEST(engine, "view", "ssao_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showAmbientOcclusion = false;
    ctx->Yield(3);
    ui::AddSection(*s_App, 0); // cut on XY
    ctx->Yield(3);
    s_App->showAmbientOcclusion = true;
    ctx->Yield(5);
    ui::ClearSections(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->showAmbientOcclusion);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  // S3 render-quality switches: each change makes the main loop recreate
  // GPU targets behind wait-idle and rebind descriptors — the crash surface
  // is exactly that recreation, so flip everything with frames in between.
  t = IM_REGISTER_TEST(engine, "view", "render_quality_switches_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->ssaoFullRes = true; // half -> full AO targets
    ctx->Yield(3);
    s_App->shadowMapSize = 1024; // shadow map recreation
    ctx->Yield(3);
    s_App->ssaoFullRes = false; // back to defaults, both paths again
    s_App->shadowMapSize = 2048;
    ctx->Yield(3);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  t = IM_REGISTER_TEST(engine, "constraints", "coincident_merges_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    // Inject two lines with near-but-not-equal shared endpoints
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{4.2f, 0.3f}, {4, 3}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Select line1.a and line0.b, apply Coincident
    s_App->sketchEdit.selected = {{l1, 0}, {l0, 1}};
    bool ok = ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Coincident, 0.0);
    IM_CHECK(ok);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    glm::vec2 l0b = std::get<sketch::Line>(sk->entities[0]).b;
    glm::vec2 l1a = std::get<sketch::Line>(sk->entities[1]).a;
    IM_CHECK(glm::length(l0b - l1a) < 1e-3f);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);

    // Undo restores (constraint removed, points back apart)
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "constraints", "horizontal_levels_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 1.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& ln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f);
  };

  // Clicking a line's body (not an endpoint) selects the whole entity, so a
  // whole-line constraint like Horizontal can then be applied.
  t = IM_REGISTER_TEST(engine, "pick", "select_line_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 1}, {2, 1.6f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(0)); // Execute replaced it

    // Pick the segment midpoint (far from both endpoints) → whole-line body.
    glm::vec2 midScreen =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 1.3f));
    ui::SketchPick pick = ui::PickSketchEntity(*s_App, *sk, midScreen, 12.0f);
    IM_CHECK(pick.hit);
    IM_CHECK(pick.body);
    IM_CHECK_EQ(pick.ref.entity, l0);

    // Body selection enables + applies Horizontal.
    s_App->sketchEdit.selected = {pick.ref};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& hln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(hln.a.y - hln.b.y) < 1e-3f);
  };

  // A click near a vertex prefers the endpoint over the body.
  t = IM_REGISTER_TEST(engine, "pick", "prefers_endpoint");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(0)); // Execute replaced it

    glm::vec2 endScreen =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2.0f, 0.0f));
    ui::SketchPick pick = ui::PickSketchEntity(*s_App, *sk, endScreen, 12.0f);
    IM_CHECK(pick.hit);
    IM_CHECK(!pick.body); // endpoint, not body
    IM_CHECK_EQ(pick.ref.entity, l0);
    IM_CHECK_EQ((int)pick.ref.which, 1); // endpoint b
  };

  // Selection precision: a click near a line's endpoint picks the POINT (not
  // the line body, even though it is just as close); a repeated click at the
  // same spot cycles to the line body (the U1 same-spot convention).
  t = IM_REGISTER_TEST(engine, "pick", "endpoint_cycles_to_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    // Off the datum axes so they are not pick candidates at the endpoint.
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0.5f}, {2, 0.5f}});
    sk->constraints.push_back(
        {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    // Pin the camera: the zoom left by earlier tests changes what falls
    // inside the pixel pick tolerances (e.g. the X axis 0.5 UV away).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Select;

    // Click at endpoint b: the point wins over the line body under it.
    glm::vec2 end = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.5f));
    ctx->MouseMoveToPos(ImVec2(end.x, end.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK_EQ(s_App->sketchEdit.selected[0].entity, l0);
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which, 1);
    IM_CHECK(s_App->sketchDrag.mode == ui::SketchDragSession::Mode::Point);

    // Same spot again: cycles to the dimensioned line body, which translates
    // the sketch rather than trying to resize the driving dimension. The body
    // records kWholeEntity, NOT 0 — a body pick and a vertex-0 pick both used
    // to store {id, 0}, so highlighting could not tell them apart and lit the
    // whole line whenever a vertex of it was picked.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK_EQ(s_App->sketchEdit.selected[0].entity, l0);
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which,
                (int)sketch::kWholeEntity);
    IM_CHECK(s_App->sketchDrag.mode ==
             ui::SketchDragSession::Mode::SketchTranslate);

    // Once more wraps back to the endpoint.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which, 1);
    IM_CHECK(s_App->sketchDrag.mode == ui::SketchDragSession::Mode::Point);

    ctx->KeyPress(ImGuiKey_Escape); // clear selection
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Trim tool: clicking the middle span of a crossed line removes just that
  // piece, end to end through the viewport handler.
  t = IM_REGISTER_TEST(engine, "trim", "removes_middle_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t h = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}}); // horizontal
    sk->AddEntity(sketch::Line{{-1, -1}, {-1, 1}}); // crosses at x=-1
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});   // crosses at x=1
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    // Switch to the Trim tool and click the middle of the horizontal line.
    s_App->sketchEdit.tool = ui::SketchTool::Trim;
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    // The middle span [-1,1] is gone; the two outer pieces + 2 crossers remain.
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)4);
    IM_CHECK(sk->IndexOfEntity(h) != SIZE_MAX); // piece 1 kept the id
  };

  // Hovering (no click) with the Trim tool computes the preview span for the
  // segment under the cursor.
  t = IM_REGISTER_TEST(engine, "trim", "preview_highlights_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    sk->AddEntity(sketch::Line{{-1, -1}, {-1, 1}});
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    s_App->sketchEdit.tool = ui::SketchTool::Trim;
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y)); // hover, no click
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.hasTrimPreview);
    // The previewed span is the middle piece [-1,0]..[1,0].
    float lo = std::min(s_App->sketchEdit.trimPreviewA.x,
                        s_App->sketchEdit.trimPreviewB.x);
    float hi = std::max(s_App->sketchEdit.trimPreviewA.x,
                        s_App->sketchEdit.trimPreviewB.x);
    IM_CHECK(std::abs(lo + 1.0f) < 1e-3f);
    IM_CHECK(std::abs(hi - 1.0f) < 1e-3f);
    IM_CHECK_EQ(sk->entities.size(), (size_t)3); // preview didn't mutate
  };

  // Fillet a right-angle corner via the UI path (pick corner → tangent arc).
  t = IM_REGISTER_TEST(engine, "fillet", "fillet_corner_via_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l1 = sk->AddEntity(sketch::Line{{4, 0}, {0, 0}});
    uint32_t l2 = sk->AddEntity(sketch::Line{{0, 0}, {0, 4}});
    sk->constraints.push_back(
        {sketch::ConstraintKind::Coincident, {l1, 1}, {l2, 0}, 0.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    s_App->sketchEdit.tool = ui::SketchTool::Fillet;
    s_App->sketchEdit.filletRadius = 1.0f;
    glm::vec2 corner = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(corner.x, corner.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    int arcs = 0;
    for (const auto& e : sk->entities)
      if (std::holds_alternative<sketch::Arc>(e))
        arcs++;
    // 2 lines + fillet arc + the construction corner-point (direction anchor).
    IM_CHECK_EQ(sk->entities.size(), (size_t)4);
    IM_CHECK_EQ(arcs, 1);
  };

  // A fillet must NEVER rotate a line — even one whose direction is only
  // IMPLICITLY constrained (the owner's staircase bug): L3 is vertical only
  // because its top shares X with the shared corner, via a cross-line
  // point-pair vertical. Filleting the corner used to drag L3 off vertical
  // (and non-adjacent lines too). Drive the real GUI Fillet tool.
  t = IM_REGISTER_TEST(engine, "fillet", "staircase_preserves_directions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 10.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    using K = sketch::ConstraintKind;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t L1 = sk->AddEntity(sketch::Line{{-2, -2}, {-2, 0}}); // vertical
    uint32_t L2 = sk->AddEntity(sketch::Line{{-2, 0}, {0, 0}});   // horizontal
    uint32_t L3 = sk->AddEntity(sketch::Line{{0, 0}, {0, 2}});    // vertical
    uint32_t L4 = sk->AddEntity(sketch::Line{{0, 2}, {2, 2}});    // horizontal
    sk->constraints.push_back({K::Coincident, {L1, 1}, {L2, 0}});
    sk->constraints.push_back({K::Coincident, {L2, 1}, {L3, 0}}); // fillet here
    sk->constraints.push_back({K::Coincident, {L3, 1}, {L4, 0}});
    sk->constraints.push_back({K::Vertical, {L1, 0}, {0, 0}});
    sk->constraints.push_back({K::Fixed, {L1, 0}, {}});
    sk->constraints.push_back({K::Fixed, {L1, 1}, {}});
    sk->constraints.push_back({K::Horizontal, {L2, 1}, {L1, 1}});
    sk->constraints.push_back({K::Vertical, {L2, 1}, {sketch::kOrigin, 0}});
    sk->constraints.push_back({K::Vertical, {L3, 1}, {L2, 1}}); // implicit V
    sk->constraints.push_back({K::Horizontal, {L4, 1}, {L3, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    auto angDeg = [&](uint32_t id) {
      const auto& l =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(id)]);
      return std::atan2(l.b.y - l.a.y, l.b.x - l.a.x) * 180.0 / M_PI;
    };
    const double a1 = angDeg(L1), a2 = angDeg(L2), a3 = angDeg(L3),
                 a4 = angDeg(L4);
    IM_CHECK_LE(std::abs(std::abs(a3) - 90.0), 1e-2); // vertical before

    // Fillet the L2-L3 corner via the real GUI tool.
    s_App->sketchEdit.tool = ui::SketchTool::Fillet;
    s_App->sketchEdit.filletRadius = 0.3f;
    glm::vec2 corner = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(corner.x, corner.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    int arcs = 0;
    for (const auto& e : sk->entities)
      if (std::holds_alternative<sketch::Arc>(e))
        arcs++;
    IM_CHECK_EQ(arcs, 1); // the fillet landed

    // Adjacent AND non-adjacent lines keep their exact directions.
    IM_CHECK_LE(std::abs(angDeg(L1) - a1), 0.1);
    IM_CHECK_LE(std::abs(angDeg(L2) - a2), 0.1);
    IM_CHECK_LE(std::abs(angDeg(L3) - a3), 0.1); // the bug: was ~96-106 deg
    IM_CHECK_LE(std::abs(angDeg(L4) - a4), 0.1);

    ctx->KeyPress(ImGuiKey_Escape); // Fillet -> Select
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // A new sketch defaults to the Select tool; letter keys switch tools (Onshape
  // keymap: g = rectangle, m = trim).
  t = IM_REGISTER_TEST(engine, "keys", "tool_switch");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select); // default
    ctx->KeyPress(ImGuiKey_L);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);
    ctx->KeyPress(ImGuiKey_G);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Rectangle);
    ctx->KeyPress(ImGuiKey_M);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Trim);
  };

  // `h` applies Horizontal to the current selection (constraint keymap).
  t = IM_REGISTER_TEST(engine, "keys", "constraint_on_selection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l =
        sk->AddEntity(sketch::Line{{0, 0}, {4, 0.5f}}); // slightly tilted
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{l, 0}};
    ctx->KeyPress(ImGuiKey_H);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    int nh = 0;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Horizontal)
        nh++;
    IM_CHECK_EQ(nh, 1);
    const auto& ln = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l)]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f); // solved flat
  };

  // Dragging a box over empty space selects the entities it encloses (window).
  t = IM_REGISTER_TEST(engine, "select", "box_selects_enclosed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
    sk->AddEntity(sketch::Line{{0, 1}, {1, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    // Box corners in empty space (well off the geometry) enclosing both lines.
    glm::vec2 a = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    glm::vec2 b = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 2));
    ctx->MouseMoveToPos(ImVec2(a.x, a.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(b.x, b.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2);
  };

  // U8 polish: the Mirror collect phase box-selects like the Select tool,
  // and a tap in empty space advances to the axis pick.
  t = IM_REGISTER_TEST(engine, "mirror", "box_select_and_tap_advances");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
    sk->AddEntity(sketch::Line{{0, 1}, {1, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;

    // Box around both lines (empty-space corners) adds them to the set.
    glm::vec2 a = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    glm::vec2 b = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 2));
    ctx->MouseMoveToPos(ImVec2(a.x, a.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(b.x, b.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2);
    IM_CHECK(!s_App->sketchEdit.mirrorAxisPhase);

    // A tap in empty space continues to the axis step (bottom-left, clear
    // of the top-right session window).
    glm::vec2 t2 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, -2));
    ctx->MouseMoveToPos(ImVec2(t2.x, t2.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.mirrorAxisPhase);
  };

  // U8 polish: a pre-selection whose sole construction line names the mirror
  // axis commits the mirror on tool activation — one gesture.
  t = IM_REGISTER_TEST(engine, "mirror", "preselected_construction_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l1 = sk->AddEntity(sketch::Line{{1, 0}, {2, 1}});
    sketch::Line axisLine{{0, -2}, {0, 2}};
    axisLine.construction = true;
    const uint32_t ax = sk->AddEntity(axisLine);
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const size_t entitiesBefore = sk->entities.size();

    s_App->sketchEdit.selected = {{l1, 0}, {ax, 0}};
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    ui::MirrorToolActivated(*s_App); // the toolbar activation hook
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->entities.size() > entitiesBefore); // mirrored copy exists
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select); // done
    IM_CHECK(s_App->sketchEdit.selected.empty());
  };

  // U8 polish: Pattern selects in-tool (box select), a tap in empty space
  // applies, and Apply hands the tool back to Select.
  t = IM_REGISTER_TEST(engine, "pattern", "in_tool_select_and_tap_applies");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Circle{{0, 0}, 0.5});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Pattern;
    s_App->sketchEdit.patternCircular = false;
    s_App->sketchEdit.patternCount = 3;
    s_App->sketchEdit.patternStep = glm::vec2(2.0f, 0.0f);

    // Box-select the circle while the tool is active.
    glm::vec2 a = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    glm::vec2 b = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1, 1));
    ctx->MouseMoveToPos(ImVec2(a.x, a.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(b.x, b.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);

    // Tap in empty space = Apply; the tool returns to Select. (Bottom-left,
    // clear of the top-right session window.)
    glm::vec2 t2 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, -2));
    ctx->MouseMoveToPos(ImVec2(t2.x, t2.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!sk->derived.empty()); // the pattern op exists
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    IM_CHECK(s_App->sketchEdit.selected.empty());
  };

  // U8 polish: sketch operations (linked patterns/copies/images) list as
  // rows in the Model panel; double-click opens the op's edit session in
  // the Tool window, where params edit live.
  t = IM_REGISTER_TEST(engine, "sketch", "ops_row_opens_pattern_session");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 0.5});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // A LINKED pattern is a derived op (one-shot copies wouldn't list).
    s_App->sketchEdit.selected = {{c1, 0}};
    s_App->sketchEdit.patternCircular = false;
    s_App->sketchEdit.patternCount = 3;
    s_App->sketchEdit.patternStep = glm::vec2(2.0f, 0.0f);
    s_App->sketchEdit.linkGenerative = true;
    IM_CHECK(ui::ApplyPattern(*s_App));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->derived.size(), (size_t)1);

    // Double-click the Operations row → the edit session opens.
    ctx->ItemDoubleClick("//###Model/###op0");
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.editOpIndex, 0);
    IM_CHECK(ctx->GetWindowByRef("//Linear Pattern###ToolWindow") != nullptr);

    // Edit the count in the session; the op regenerates.
    ctx->ItemInputValue("//Linear Pattern###ToolWindow/Count", 5);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->derived[0].count, 5);

    ctx->KeyPress(ImGuiKey_Escape); // closes the session
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.editOpIndex, -1);
  };

  // Escape backs out one level per press: tool → clear selection → leave edit.
  t = IM_REGISTER_TEST(engine, "select", "escape_cascade");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
    s_App->sketchEdit.tool = ui::SketchTool::Circle;
    s_App->sketchEdit.selected = {{l, 0}};
    ctx->KeyPress(ImGuiKey_Escape); // 1: tool → Select
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    IM_CHECK(!s_App->sketchEdit.selected.empty());
    ctx->KeyPress(ImGuiKey_Escape); // 2: clear selection
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.selected.empty());
    IM_CHECK(s_App->sketchEdit.IsEditing());
    ctx->KeyPress(ImGuiKey_Escape); // 3: leave sketch edit
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // Drawing a near-horizontal line auto-adds a Horizontal constraint and snaps
  // it flat.
  t = IM_REGISTER_TEST(engine, "infer", "horizontal_snaps");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, 0));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.06f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    bool hasH = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Horizontal)
        hasH = true;
    IM_CHECK(hasH);
    const auto& ln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f); // snapped flat
  };

  // Ending a line near an existing point snaps coincident to it.
  t = IM_REGISTER_TEST(engine, "infer", "endpoint_coincident");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    // Keep the point well within the sketch's visible UV range (~±2 at the
    // default framing); further out it projects onto a side panel and the click
    // that should land on it misses the viewport.
    uint32_t target = sk->AddEntity(sketch::Point{{1.5f, 0.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    // Let the sketch-entry camera animation settle so the projected click
    // coordinates land on target (see draw_line_undo).
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 = ui::SketchUVToScreen(
        *s_App, sk->plane, glm::vec2(1.5f, 0.5f)); // onto the point
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    bool coincidentToTarget = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Coincident &&
          (c.a.entity == target || c.b.entity == target))
        coincidentToTarget = true;
    IM_CHECK(coincidentToTarget);
  };

  // Ending a line under an existing point (shared X, far in Y) infers a
  // two-point Vertical alignment constraint (Phase N5).
  t = IM_REGISTER_TEST(engine, "infer", "endpoint_aligns_vertical");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t target =
        sk->AddEntity(sketch::Point{{1.5f, 1.8f}}); // shares X below
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    // Line from (-1.5,-1) to (1.5,-1): the end shares X (1.5) with the point
    // but is well away in Y → vertical alignment, not coincident.
    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -1.0f));
    glm::vec2 p1 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, -1.0f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    bool verticalToTarget = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Vertical &&
          (c.a.entity == target || c.b.entity == target))
        verticalToTarget = true;
    IM_CHECK(verticalToTarget);
  };

  // The staged tool prompt reports the right step for each tool/state (N1).
  t = IM_REGISTER_TEST(engine, "prompt", "staged_tool_steps");
  t->TestFunc = [](ImGuiTestContext*) {
    ui::SketchEditState e;
    e.activeSketchId = 1; // in sketch edit

    e.tool = ui::SketchTool::Select;
    IM_CHECK(ui::SketchToolPrompt(e).text.empty()); // Select is quiet

    e.tool = ui::SketchTool::Line;
    e.hasPendingPoint = false;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 1);
    e.hasPendingPoint = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 2);

    ui::SketchEditState m;
    m.tool = ui::SketchTool::Mirror;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 1); // collecting geometry
    IM_CHECK_EQ(ui::SketchToolPrompt(m).stepCount, 2);
    m.mirrorAxisPhase = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 2); // axis-pick step

    ui::SketchEditState el;
    el.tool = ui::SketchTool::Ellipse;
    el.hasPendingPoint = true;
    el.hasPendingPoint2 = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(el).step, 3); // third click
    IM_CHECK_EQ(ui::SketchToolPrompt(el).stepCount, 3);

    ui::SketchEditState sp;
    sp.tool = ui::SketchTool::Spline;
    IM_CHECK(!ui::SketchToolPrompt(sp).text.empty()); // prompts before any pole
    sp.splinePoles = {{0, 0}, {1, 1}};
    IM_CHECK(ui::SketchToolPrompt(sp).text.find("finish") != std::string::npos);
  };

  // Guided mirror (N2): select geometry in-tool, Enter, then click the axis.
  t = IM_REGISTER_TEST(engine, "mirror", "guided_flow");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(
        sketch::Line{{0.5f, 0.6f}, {1.5f, 0.6f}}); // geometry to mirror
    sk->AddEntity(sketch::Line{{0.0f, -1.0f}, {0.0f, 1.0f}}); // vertical axis
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    // Pin the camera: at an inherited far-out zoom, the geometry line's
    // endpoint falls within the (screen-px) point tolerance of the axis
    // click below and would be picked as the mirror axis.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const size_t n0 = sk->entities.size(); // 2
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    s_App->sketchEdit.mirrorAxisPhase = false;
    s_App->sketchEdit.selected.clear();

    // Step 1: click the geometry line's midpoint to select it.
    glm::vec2 g =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.selected.empty());

    // Enter advances to the axis-pick step.
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.mirrorAxisPhase);

    // Step 2: click the axis line → the mirror runs and adds a copy.
    glm::vec2 ax =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 0.0f));
    ctx->MouseMoveToPos(ImVec2(ax.x, ax.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->entities.size() > n0); // a mirrored copy was added
  };

  // Guided mirror across the Y datum axis (NF1): the axis is pickable even
  // though it isn't an entity.
  t = IM_REGISTER_TEST(engine, "mirror", "across_datum_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0.5f, 0.6f}, {1.5f, 0.6f}}); // off the Y axis
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    s_App->sketchEdit.mirrorAxisPhase = false;
    s_App->sketchEdit.selected.clear();

    // Select the line, Enter, then click on the Y axis (x=0, well away from the
    // line) — no entity there, so the datum axis is picked.
    glm::vec2 g =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    glm::vec2 ax =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, -1.2f));
    ctx->MouseMoveToPos(ImVec2(ax.x, ax.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    // v51: the linked mirror is a derived OP recording the datum axis.
    IM_CHECK(!sk->derived.empty());
    IM_CHECK(sk->derived[0].kind == sketch::DerivedKind::Mirror);
    IM_CHECK(sk->derived[0].axisEntity == sketch::kAxisY);
  };

  // With auto-constrain off, no constraints are inferred.
  t = IM_REGISTER_TEST(engine, "infer", "toggle_suppresses");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = false; // suppressed

    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, 0));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.06f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0); // nothing inferred
    s_App->sketchEdit.autoConstrain = true; // restore default for other tests
  };

  // Dragging a line's body translates the whole line and its coincident
  // neighbour, end to end through the viewport drag.
  t = IM_REGISTER_TEST(engine, "drag", "move_whole_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{2, 0}, {2, 3}}); // l1.a ~ l0.b
    sk->constraints.push_back(
        {sketch::ConstraintKind::Coincident, {l0, 1}, {l1, 0}, 0.0});
    sk->constraints.push_back(
        {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Select;

    // Press on the line's midpoint (its body), drag up 1 unit, release.
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    glm::vec2 to = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 1));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& moved =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l0)]);
    // A driving dimension makes this a whole-sketch translation: the line
    // keeps its length and both endpoints of its neighbour move by the same
    // delta, not merely the coincident join.
    IM_CHECK(moved.a.y > 0.5f && moved.b.y > 0.5f);
    IM_CHECK(std::abs(glm::length(moved.b - moved.a) - 4.0f) < 1e-2f);
    const auto& nbr =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l1)]);
    IM_CHECK(glm::length(nbr.a - moved.b) < 1e-2f);
    IM_CHECK(nbr.b.y > 3.5f);
    IM_CHECK(s_App->commands->CanUndo()); // one undoable edit
  };

  // 3D fillet via edge picking (M7/T5): create a box, start a fillet, click a
  // visible B-Rep edge, accept — the body loses volume along that edge.
  t = IM_REGISTER_TEST(engine, "fillet", "fillet_body_edge_via_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    // Frame the box deterministically (same recipe as the face-pick test).
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5f;
    const float radius = glm::length(max - min) * 0.5f;
    // A camera animation left by an earlier flow would override these setters
    // every frame until it finishes — stop it first.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * radius * 5.0f);
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_fillet");
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active()); // the edge slot armed itself (U7)
    auto* fil = dynamic_cast<FilletFeature*>(
        Doc()->FindFeatureById(s_App->slotPick.featureId));
    IM_CHECK(fil != nullptr);
    fil->radius = 0.3;

    // Click the midpoint of the B-Rep edge nearest the camera (a silhouette
    // or front edge from this viewpoint, so the pick can't be occluded).
    const auto polys = Doc()->GetBodies()[0].shape.GenerateEdgePolylines();
    const glm::vec3 eye = s_App->camera->GetPosition();
    glm::vec3 target(0.0f);
    float bestDist = FLT_MAX;
    for (const auto& poly : polys) {
      if (poly.size() < 2)
        continue;
      const glm::vec3 mid = poly[poly.size() / 2];
      const float d = glm::length(mid - eye);
      if (d < bestDist) {
        bestDist = d;
        target = mid;
      }
    }

    ctx->MouseMoveToPos(WorldToWindow(target));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(fil->edges.size(), (size_t)1);

    // Once the first edge is present, the radius handle is visible. A missed
    // handle hit must report false so ViewportInput falls through to the armed
    // blend picker instead of starting a radius drag.
    const double radiusAfterFirstEdge = fil->radius;
    const Ray missRay = s_App->faceSelector->ScreenToWorldRay(
        30.0f, 30.0f, static_cast<int>(s_App->viewportSize.x),
        static_cast<int>(s_App->viewportSize.y), *s_App->camera);
    IM_CHECK(!ui::HandlePreviewHandleInput(*s_App, missRay, /*clicked=*/true,
                                           /*down=*/true, /*released=*/false));
    IM_CHECK(!s_App->handleDrag.active);
    IM_CHECK(std::abs(fil->radius - radiusAfterFirstEdge) < 1e-9);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active()); // picking ends with the preview
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < before - 1e-6);
    IM_CHECK(s_App->commands->CanUndo()); // accept = one undoable action
  };

  // Feature tree affordances: context-menu rename (inline edit) and the
  // draggable rollback bar.
  // A deliberate leak, sitting immediately before a test that reads the model
  // tree. It opens every tool session, turns the terminal on, selects a
  // non-Model dock tab and leaves a preview in the document — then ENDS
  // WITHOUT CLEANING UP, on purpose. If ResetAppForTest does its job the next
  // test never notices; if someone weakens the reset, the next test goes red
  // and this comment says why.
  //
  // This is the check that would have caught all three of the 2026-08-22
  // order-dependent failures, none of which any test was trying to find.
  // Every tool on the model toolbar must be CLICKABLE at the default window
  // size — not merely present in the source. A dogfooding round found the row
  // overflowing at 1280x720 with the last four tools (Simulation, CAM,
  // Section, Measure) culled rather than clipped: nothing could click them
  // until the bar was scrolled, and CAM had no other route at all. The row
  // wraps now, and this is what keeps it true as tools are appended — which
  // is exactly how it broke, since new tools go on the end.
  t = IM_REGISTER_TEST(engine, "toolbar", "every_tool_is_reachable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // A body, so the tools that need one are enabled (a disabled button is
    // still submitted, but this keeps the test honest about real conditions).
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(4);

    static const char* kModelTools[] = {
        "###tb_new_sketch", "###tb_box",         "###tb_sphere",
        "###tb_cylinder",   "###tb_extrude",     "###tb_revolve",
        "###tb_curve3d",    "###tb_sweep",       "###tb_loft",
        "###tb_pipe",       "###tb_datum_plane", "###tb_datum_axis",
        "###tb_boolean",    "###tb_mirror",      "###tb_pattern",
        "###tb_fillet",     "###tb_chamfer",     "###tb_rib",
        "###tb_hole",       "###tb_thread",      "###tb_shell",
        "###tb_draft",      "###tb_split",       "###tb_refine",
        "###tb_fit",        "###tb_planes",      "###tb_simulation",
        "###tb_section",    "###tb_measure",     "###tb_render",
    };
    // THE load-bearing assertion, and it is structural on purpose. Probing
    // each button with ItemExists does NOT work: the test engine SCROLLS to
    // bring an item into view, so every id resolves whether or not it was
    // reachable, and the test passes with the bug present (verified by
    // removing the wrap and watching it stay green). What actually matters is
    // that the row FITS — content no wider than the window means nothing is
    // scrolled out, for a human or for a script.
    // By substring: a child window's real name carries the parent's and a
    // hashed suffix ("##DockHost/##ToolbarRow_1A2B3C4D"), so an exact lookup
    // finds nothing.
    ImGuiWindow* bar = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
      if (std::string(w->Name).find("##ToolbarRow") != std::string::npos) {
        bar = w;
        break;
      }
    IM_CHECK(bar != nullptr);
    ctx->LogInfo("toolbar content %.0f px in %.0f px", bar->ContentSize.x,
                 bar->InnerRect.GetWidth());
    IM_CHECK(bar->ContentSize.x <= bar->InnerRect.GetWidth() + 1.0f);

    // And the ids are all present, which catches a tool dropped entirely.
    ctx->SetRef("//##DockHost");
    for (const char* id : kModelTools)
      IM_CHECK_SILENT(ctx->ItemExists((std::string("**/") + id).c_str()));
#ifdef HAVE_CAM
    IM_CHECK(ctx->ItemExists("**/###tb_cam"));
#endif
  };

  t = IM_REGISTER_TEST(engine, "isolation", "a_messy_test_leaks_nothing");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Mess", glm::vec3(0, 0, 0), 5, 5, 5));
    ctx->Yield(2);

    s_App->sectionToolOpen = true;
    s_App->camToolOpen = true;
    s_App->measure.active = true;
    s_App->showCog = true;
    s_App->perfHudOpen = true;
    s_App->settings.terminalPanel = true; // also forks a shell
    ui::AddSection(*s_App, 0);
    ctx->Yield(6);

    // And take the shared dock tab away from the model tree, which is the
    // specific leak that broke three unrelated tests.
    if (ImGuiWindow* cam = ImGui::FindWindowByName(ui::kPanelCam))
      if (cam->DockNode != nullptr) {
        ctx->ItemClick(cam->TabId);
        ctx->Yield(2);
      }
    IM_CHECK(s_App->camToolOpen); // the mess is real, not a no-op
    // Deliberately no cleanup.
  };

  t = IM_REGISTER_TEST(engine, "tree", "context_rename_and_rollback_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_sphere");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);

    // Rename "Box 1" via right-click > Rename: the row becomes a focused
    // input; typing replaces the name, Enter commits (one undoable edit).
    ctx->ItemClick("//###Model/Box 1##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Rename");
    ctx->KeyCharsReplaceEnter("Chassis");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetFeature(0)->GetName(), std::string("Chassis"));
    IM_CHECK(s_App->commands->CanUndo());

    // Drag the rollback bar (parked at the end) up above the sphere row:
    // regeneration stops at index 1 and the sphere body disappears.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    ctx->ItemDragWithDelta("//###Model/##RollbackBar", ImVec2(0, -30));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->Rollback(), 1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Drag it back past the end: full history replays.
    ctx->ItemDragWithDelta("//###Model/##RollbackBar", ImVec2(0, 60));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->Rollback(), -1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
  };

  // U1a: feature selection is a stable id, not an index — deleting an
  // earlier sibling must not move it (the old int-index selection pointed at
  // whatever feature slid into the vacated slot).
  t = IM_REGISTER_TEST(engine, "selection", "survives_sibling_delete");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_sphere");
    const uint64_t sphereId = Doc()->GetFeature(1)->GetId();

    ctx->ItemClick("//###Model/Sphere 2##1");
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Delete the box above it: the selection stays on the sphere.
    ctx->ItemClick("//###Model/Box 1##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Delete");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Deleting the selected feature itself clears the selection (prune).
    ctx->ItemClick("//###Model/Sphere 2##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Delete");
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), (uint64_t)0);
  };

  // U1b: the unified pick pipeline — face/body cycling on a repeated click,
  // edge picking outside any armed tool, Ctrl+click multi-select, Escape
  // clears.
  t = IM_REGISTER_TEST(engine, "selection", "pick_cycle_multiselect");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");

    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5f;
    const float radius = glm::length(max - min) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(1.0f, 0.6f, 1.0f)) * radius * 5.0f);
    ctx->Yield();

    const auto& items = s_App->selection.items;

    // Click the +X face's center pixel — face interior, no edge in range —
    // → one Face item. (The body-center pixel lines up with corner edges.)
    const glm::vec3 faceCenter(max.x, center.y, center.z);
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Face);

    // Same spot again: cycles to the face's body; once more wraps to face.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Body);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Face);

    // A visible edge midpoint (top edge at z-max): edges outrank faces.
    const glm::vec3 edgeMid((min.x + max.x) * 0.5f, max.y, max.z);
    ctx->MouseMoveToPos(WorldToWindow(edgeMid));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Edge);

    // Ctrl+click back on the face: toggles it IN alongside the edge.
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->Yield();
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)2);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Edge);
    IM_CHECK(items[1].kind == ui::SelectionItem::Kind::Face);

    // Escape clears.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->selection.Empty());
  };

  // U3: dialog pick slots — arm the Boolean's Target/Tool slots and fill
  // them by clicking bodies in the viewport (overriding the defaults, which
  // are the two most-recent producers).
  t = IM_REGISTER_TEST(engine, "slots", "boolean_via_viewport_picks");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    auto* box2 = dynamic_cast<BoxFeature*>(Doc()->GetFeature(1));
    auto* box3 = dynamic_cast<BoxFeature*>(Doc()->GetFeature(2));
    IM_CHECK(box2 != nullptr && box3 != nullptr);
    box2->position = glm::vec3(-3.0f, 0.0f, 0.0f); // overlaps box 1 by 1x2x2
    box3->position = glm::vec3(2.0f, 0.0f, 0.0f);  // far to the right
    Doc()->Regenerate();
    ctx->Yield();

    // Frame all three with the +Z faces toward the camera.
    glm::vec3 min(-4.0f, 0.0f, 0.0f), max(4.0f, 2.0f, 2.0f);
    const glm::vec3 center = (min + max) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(0.3f, 0.5f, 1.0f)) * 20.0f);
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_boolean");
    ctx->Yield();
    IM_CHECK(s_App->pendingFeatureId != 0);
    const uint64_t box1Id = Doc()->GetFeature(0)->GetId();
    const uint64_t box2Id = box2->GetId();

    // Arm Target, click box 1's TOP face; arm Tool, click box 2's —
    // top faces project high on screen, clear of overlapping windows.
    ctx->ItemClick("//Boolean###ToolWindow/###slot1"); // Target
    IM_CHECK(s_App->slotPick.Active());
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(-3.7f, 2.0f, 1.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active()); // single slot disarms on pick
    ctx->ItemClick("//Boolean###ToolWindow/###slot2"); // Tool
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(-1.3f, 2.0f, 1.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    auto* boo = dynamic_cast<BooleanFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(boo != nullptr);
    IM_CHECK_EQ(boo->targetId, box1Id);
    IM_CHECK_EQ(boo->toolId, box2Id);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    // Union of boxes 1+2 (8 + 8 - 1x2x2 overlap = 12) plus untouched box 3.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    bool sawUnion = false;
    for (const Body& b : Doc()->GetBodies())
      if (std::abs(b.shape.Volume() - 12.0) < 1e-3)
        sawUnion = true;
    IM_CHECK(sawUnion);
  };

  // U7b select-first: two pre-selected bodies seed Boolean's target/tool in
  // pick order — the session opens fully satisfied (nothing armed), Accept
  // commits without a single extra pick.
  t = IM_REGISTER_TEST(engine, "slots", "boolean_seeded_from_preselection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "A", glm::vec3(-1.0f, 0.0f, 0.0f), 2.0, 2.0, 2.0));
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "B", glm::vec3(0.0f, 0.0f, 0.0f), 2.0, 2.0, 2.0));
    Doc()->Regenerate();
    ctx->Yield();
    const uint64_t aId = Doc()->GetFeature(0)->GetId();
    const uint64_t bId = Doc()->GetFeature(1)->GetId();
    // Pick order matters: A first (target), B second (tool).
    s_App->selection.Replace({ui::SelectionItem::Kind::Body, aId, {}});
    s_App->selection.Toggle({ui::SelectionItem::Kind::Body, bId, {}});
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_boolean");
    ctx->Yield();
    auto* boo = dynamic_cast<BooleanFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(boo != nullptr);
    IM_CHECK_EQ(boo->targetId, aId);
    IM_CHECK_EQ(boo->toolId, bId);
    IM_CHECK(!s_App->slotPick.Active()); // fully seeded: nothing armed
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Union of two 2x2x2 boxes overlapping 1x2x2: 8 + 8 - 4.
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 12.0) < 1e-3);
  };

  // U7b select-first: pre-selected edges seed the fillet's edge list — ALL
  // of them, not just the first (a regen between batched toggles blended
  // the body mid-seed and stranded the remaining pre-picked names).
  t = IM_REGISTER_TEST(engine, "slots", "fillet_seeded_from_selected_edges");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const Body& body = Doc()->GetBodies()[0];
    const double before = body.shape.Volume();
    // Three distinct edges, Ctrl+click style (ordered toggles).
    for (int e = 0; e < 3; e++) {
      const std::string* edgeName = body.elementMap.EdgeNameOf(e);
      IM_CHECK(edgeName != nullptr);
      s_App->selection.Toggle({ui::SelectionItem::Kind::Edge, 0,
                               ElementRef{body.createdBy, *edgeName}});
    }
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_fillet");
    ctx->Yield();
    auto* fil = dynamic_cast<FilletFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(fil != nullptr);
    IM_CHECK_EQ(fil->edges.size(), (size_t)3);
    fil->radius = 0.3;
    Doc()->Regenerate();
    IM_CHECK(fil->GetError().empty());
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < before - 1e-6);
  };

  // U2: region picking — arm the extrude's Profiles slot and toggle one of
  // a two-region sketch's profiles by clicking inside it in the viewport.
  // U8b select-first in sketch mode: a selected profile seeds the extrude's
  // regions — only that loop fills.
  t = IM_REGISTER_TEST(engine, "sketch", "selection_seeds_extrude_regions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
    Doc()->Regenerate();
    ctx->Yield();

    // Select circle 1, then Extrude from the sketch toolbar.
    s_App->sketchEdit.selected.push_back({c1, 0});
    ToolbarClick(ctx, "###tb_sk_extrude");
    ctx->Yield();
    auto* ext =
        dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)1);
    IM_CHECK(ext->regions[0] == std::vector<uint32_t>{c1});
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    // One cylinder's volume, not two (r=1, default distance 1 → pi).
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - M_PI) < 1e-2);
  };

  t = IM_REGISTER_TEST(engine, "region", "pick_one_of_two_regions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    auto sk =
        std::make_unique<SketchFeature>("Profiles", SketchPlane(PlaneType::XY));
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                       ExtrudeMode::NewBody));
    Doc()->Regenerate();
    const uint64_t extId = Doc()->GetFeature(1)->GetId();
    ui::OpenFeatureEditor(*s_App, extId); // committed feature → Tool window

    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(2.5f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(2.5f, 0.5f, 12.0f));
    ctx->Yield();

    // Arm the Profiles slot (committed feature → Properties panel).
    ctx->ItemClick("//Extrude###ToolWindow/###slot7");
    IM_CHECK(s_App->slotPick.Active());

    // Click inside circle 1's region (the sketch-plane point, not the body).
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(0.0f, 0.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)1);
    // NOLINT: ext is re-fetched after every committed edit below.
    IM_CHECK(ext->regions[0] == std::vector<uint32_t>{c1});
    IM_CHECK(s_App->slotPick.Active()); // multi slot stays armed
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 2.0 * M_PI) <
             1e-2);

    // Clicking the same region again removes it: back to all regions.
    // (Re-fetch: the committed edit replaced the feature object.)
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)0);
    // A multi-region extrude is one body (compound of both cylinders).
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 4.0 * M_PI) <
             1e-2);

    ctx->KeyPress(ImGuiKey_Escape); // disarm the slot
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active());
  };

  // U6: the Ctrl+K command palette — fuzzy find "box", Enter runs the
  // Create Box command (same action the menu calls).
  t = IM_REGISTER_TEST(engine, "palette", "ctrl_k_creates_box");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_K);
    ctx->Yield();
    IM_CHECK(s_App->paletteOpen);
    ctx->KeyChars("box");
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(!s_App->paletteOpen);
    IM_CHECK(s_App->pendingFeatureId != 0); // Box preview began
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
  };

  // Dimension visuals: a new distance dim is born with a non-zero label
  // offset (legible placement), its pill drags to a new spot (writing the
  // serialized labelOffset), and the drag neither moves nor draws geometry
  // underneath (the dimHot gate).
  t = IM_REGISTER_TEST(engine, "dims", "label_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = false;
    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, -0.5f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    const uint32_t lineId = sketch::EntityId(sk->entities[0]);

    // Dimension the two endpoints; the seed pushes the label off the line.
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.selected = {{lineId, 0}, {lineId, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0)); // replaced
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    const glm::vec2 seeded = sk->constraints[0].labelOffset;
    IM_CHECK_GT(glm::length(seeded), 0.1f);

    // The label pill sits at the dimension line's midpoint: mid + offset.
    const glm::vec2 labelUV = glm::vec2(0.0f, -0.5f) + seeded;
    const glm::vec2 labelScr = ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
    const auto lineBefore = std::get<sketch::Line>(sk->entities[0]);
    ctx->MouseMoveToPos(ImVec2(labelScr.x, labelScr.y));
    ctx->Yield(2); // dimHot latches for the canvas gate
    ctx->MouseDown(ImGuiMouseButton_Left);
    // Diagonal drag: only the perpendicular (vertical, for this horizontal
    // line) component may take effect — the axis constraint (Phase 3.5).
    ctx->MouseMoveToPos(ImVec2(labelScr.x + 45.0f, labelScr.y - 45.0f));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // still just the dim
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);    // nothing drawn through
    const glm::vec2 dragged = sk->constraints[0].labelOffset;
    IM_CHECK_GT(glm::length(dragged - seeded), 0.1f); // the label moved
    // The along-span (x) component was killed: the label slid on its axis.
    IM_CHECK_LE(std::abs(dragged.x - seeded.x), 1e-3f);
    const auto lineAfter = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK_LE(glm::length(lineAfter.a - lineBefore.a), 1e-5f);
    IM_CHECK_LE(glm::length(lineAfter.b - lineBefore.b), 1e-5f);

    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit cleanly
    ctx->Yield();
  };

  // The Dimension tool (Decision 1A): D arms it; clicks accumulate targets
  // and the tool infers the dimension kind — two parallel lines = the
  // perpendicular distance, two non-parallel = angle, a circle = radius, a
  // line + empty click = its length. Each creation opens the inline editor.
  t = IM_REGISTER_TEST(engine, "dims", "dimension_tool_infers");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t lA = sk->AddEntity(sketch::Line{{-1.5f, -1}, {1.5f, -1}});
    uint32_t lB = sk->AddEntity(sketch::Line{{-1.5f, 1}, {1.5f, 1}});
    uint32_t lC = sk->AddEntity(sketch::Line{{-1.5f, 0}, {1.5f, 0.8f}});
    uint32_t ci = sk->AddEntity(sketch::Circle{{0.8f, 1.8f}, 0.5});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    // Pin the camera (zoom from earlier tests changes the pick tolerances).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    ctx->KeyPress(ImGuiKey_D); // arm the tool
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    auto closeEditor = [&](size_t expectDims) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(sk->constraints.size(), expectDims);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, (int)expectDims - 1);
      ctx->KeyPress(ImGuiKey_Escape); // closes the inline editor only
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
      IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    };

    // A single line enters LENGTH placement immediately (B); a placement
    // click landing on a PARALLEL line refines it to the perpendicular
    // distance (DistancePL) placement, value = gap.
    clickUV({0, -1});
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK(s_App->sketchEdit.dimPlacing); // length ghost is up right away
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    clickUV({0, 1}); // lands on lB → refine, not place
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(!s_App->sketchEdit.dimPlacePair); // now fixed kind: label only
    clickUV({-0.5f, 0});                       // place the label
    closeEditor(1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::DistancePL);
    IM_CHECK_EQ(sk->constraints[0].a.entity, lA);
    IM_CHECK_EQ(sk->constraints[0].b.entity, lB);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 1e-3);

    // A placement click on a NON-parallel line refines to angle, pinned at
    // the current wedge (immediate — angle placement is deferred).
    clickUV({0, -1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0, 0.4f}); // lands on lC → angle
    IM_CHECK(!s_App->sketchEdit.dimPlacing);
    closeEditor(2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::Angle);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - std::atan2(0.8, 3.0)),
                2e-2);
    {
      const uint32_t ea = sk->constraints[1].a.entity;
      const uint32_t eb = sk->constraints[1].b.entity;
      IM_CHECK((ea == lA && eb == lC) || (ea == lC && eb == lA));
    }

    // A circle rim → radius, with label-only placement.
    clickUV({1.3f, 1.8f});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({2.2f, 2.4f}); // place the label
    closeEditor(3);
    IM_CHECK(sk->constraints[2].kind == sketch::ConstraintKind::Radius);
    IM_CHECK_EQ(sk->constraints[2].a.entity, ci);
    IM_CHECK_LE(std::abs(sk->constraints[2].value - 0.5), 1e-3);

    // A line click → immediate LENGTH placement; an empty-space click
    // PLACES it (the span is axis-parallel, so every zone is aligned).
    // (x off 0 — a click ON the Y axis would refine against the axis.)
    clickUV({0.5f, -1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    clickUV({0.7f, -1.8f}); // empty space: place below the line
    closeEditor(4);
    IM_CHECK(sk->constraints[3].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints[3].a.entity, lA);
    IM_CHECK_EQ(sk->constraints[3].b.entity, lA);
    IM_CHECK_LE(std::abs(sk->constraints[3].value - 3.0), 1e-3);

    // A placement click on a POINT (the circle's centre) refines to the
    // perpendicular point-line distance.
    clickUV({0.5f, -1}); // lA → length placement again
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.8f, 1.8f}); // the circle centre → refine to DistancePL
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(!s_App->sketchEdit.dimPlacePair);
    clickUV({2.0f, -0.2f}); // place the label
    closeEditor(5);
    IM_CHECK(sk->constraints[4].kind == sketch::ConstraintKind::DistancePL);
    IM_CHECK_EQ(sk->constraints[4].a.entity, ci); // the centre point
    IM_CHECK_EQ(sk->constraints[4].b.entity, lA);
    IM_CHECK_LE(std::abs(sk->constraints[4].value - 2.8), 1e-3);

    // No geometry moved (every dimension pinned its current value).
    const auto& la =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lA)]);
    IM_CHECK_LE(std::abs(la.a.y + 1.0f), 1e-3f);

    // Esc with no accumulated targets disarms the tool (staged).
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Placement-driven kind inference (Phase 3.7, the Onshape behavior): a
  // point pair enters a placement stage; the cursor zone picks the kind —
  // above/below the pair = horizontal (DistanceX), sideways = vertical
  // (DistanceY), pulled perpendicular to the span = aligned (Distance) —
  // and the click also places the label. Esc walks the staged ladder.
  t = IM_REGISTER_TEST(engine, "dims", "placement_zones");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const glm::vec2 pa(-1.0f, -0.6f), pb(1.0f, 0.6f); // a diagonal pair
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t p0 = sk->AddEntity(sketch::Point{pa});
    uint32_t p1 = sk->AddEntity(sketch::Point{pb});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    auto closeEditor = [&](size_t expectDims) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(sk->constraints.size(), expectDims);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, (int)expectDims - 1);
      ctx->KeyPress(ImGuiKey_Escape);
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    };

    // The staged Esc ladder: placing → targets kept → targets cleared →
    // tool disarmed.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.dimPlacing);
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2); // kept
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.selected.empty());
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    // Placed ABOVE the pair → horizontal dimension (DistanceX), |dx|.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.0f, 1.5f});
    closeEditor(1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::DistanceX);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 0.03);
    IM_CHECK_EQ(sk->constraints[0].a.entity, p0); // ordered: positive dx
    IM_CHECK_EQ(sk->constraints[0].b.entity, p1);
    // The label landed where clicked: x locked to the span (item 5), y at
    // the cursor's height above the lower point.
    IM_CHECK_LE(std::abs(sk->constraints[0].labelOffset.x), 1e-4f);
    IM_CHECK_LE(std::abs(sk->constraints[0].labelOffset.y - (1.5f + 0.6f)),
                0.05f);

    // Placed to the SIDE → vertical dimension (DistanceY), |dy|.
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({2.2f, 0.0f});
    closeEditor(2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::DistanceY);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 1.2), 0.03);
    IM_CHECK_LE(std::abs(sk->constraints[1].labelOffset.y), 1e-4f);
    IM_CHECK_LE(std::abs(sk->constraints[1].labelOffset.x - (2.2f + 1.0f)),
                0.05f);

    // Placed on the span's PERPENDICULAR → aligned (Distance), full length.
    const glm::vec2 d = glm::normalize(pb - pa);
    const glm::vec2 n(-d.y, d.x);
    const glm::vec2 mid = 0.5f * (pa + pb);
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV(mid + n * 1.2f);
    closeEditor(3);
    IM_CHECK(sk->constraints[2].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_LE(std::abs(sk->constraints[2].value - glm::length(pb - pa)),
                0.03);
    const glm::vec2 off = sk->constraints[2].labelOffset;
    IM_CHECK_LE(std::abs(glm::dot(off, d)), 0.02f);  // ⟂ to the span
    IM_CHECK_LE(glm::length(off - n * 1.2f), 0.05f); // lands where clicked

    ctx->KeyPress(ImGuiKey_Escape); // disarm
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Click-select + Delete for dimensions and constraint glyphs: a single
  // click on a pill (or glyph) selects that constraint — mutually exclusive
  // with entity selection so Delete is never ambiguous — and Delete/
  // Backspace removes it (one undoable edit). Esc deselects first.
  t = IM_REGISTER_TEST(engine, "dims", "select_delete");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5f, -0.5f}, {1.5f, -0.5f}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{1.5f, -0.5f}, {1.5f, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // [0] a distance dim (pinned at the current length), [1] a coincident.
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    s_App->sketchEdit.selected = {{l0, 1}, {l1, 0}};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Coincident, 0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);
    const glm::vec2 seeded = sk->constraints[0].labelOffset; // (0, ±0.8)

    // Single-click the dimension pill: SELECTS (entity selection cleared).
    const glm::vec2 pillUV = glm::vec2(0.0f, -0.5f) + seeded;
    auto clickPill = [&]() {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, pillUV);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->Yield(2); // dimHot latches for the canvas gate
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickPill();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 0);
    IM_CHECK(s_App->sketchEdit.selected.empty());

    // Esc deselects first (the ladder), still editing the sketch.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    IM_CHECK(s_App->sketchEdit.IsEditing());

    // Re-select, Delete removes the dimension; undo restores it.
    clickPill();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 0);
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // the coincident remains
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Coincident);
    IM_CHECK_EQ(sk->entities.size(), (size_t)2); // entities untouched
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);

    // The coincident's GLYPH ("co", anchored at l0's first point + a small
    // offset): clicking inside its rect selects the constraint — the glyph
    // wins over the endpoint underneath. Backspace removes it.
    {
      glm::vec2 s =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
      // Badge rect starts at (s.x + 10, s.y - 16); aim mid-glyph.
      ctx->MouseMoveToPos(ImVec2(s.x + 17.0f, s.y - 10.0f));
      ctx->Yield(2); // glyphHot latches for the canvas gate
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 1);
    IM_CHECK(s_App->sketchEdit.selected.empty()); // no entity grabbed
    ctx->KeyPress(ImGuiKey_Backspace);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->entities.size(), (size_t)2);

    // Entity click clears the constraint selection; Delete then removes the
    // ENTITY (and its dependent dimension), not a constraint by index.
    {
      glm::vec2 mid =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, -0.5f));
      ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
      ctx->Yield(2);
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);    // l0 gone
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0); // its dim dropped with it

    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Negative dimension values flip the dimension's direction (the SW/
  // Onshape convention): signed kinds (DistanceX/Y, Angle) move the free
  // geometry to the other side/wedge and store |v| (literals swap refs;
  // negative-evaluating exprs keep the raw signed value so regeneration
  // stays stable); DistancePL flips its side pin; unsigned kinds (aligned
  // Distance, Radius) silently use |v| with no flip.
  t = IM_REGISTER_TEST(engine, "dims", "negative_flip");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t p0 = sk->AddEntity(sketch::Point{{-1, -0.6f}});
    uint32_t p1 = sk->AddEntity(sketch::Point{{1, 0.6f}});
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 1.6f}, {0, 1.6f}});
    uint32_t pp = sk->AddEntity(sketch::Point{{-1, 0.9f}});
    uint32_t lA = sk->AddEntity(sketch::Line{{-2, -1.5f}, {-0.5f, -1.5f}});
    uint32_t lB = sk->AddEntity(sketch::Line{{-2, -1.5f}, {-0.65f, -0.825f}});
    uint32_t lD = sk->AddEntity(sketch::Line{{2.4f, -1.5f}, {2.4f, 1.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    using CK = sketch::ConstraintKind;
    auto addCon = [&](std::vector<sketch::PointRef> refs, CK kind, double v) {
      s_App->sketchEdit.selected = std::move(refs);
      IM_CHECK(ui::ApplySketchConstraint(*s_App, kind, v));
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    };
    addCon({{p0, 0}}, CK::Fixed, 0.0);                 // [0]
    addCon({{p0, 0}, {p1, 0}}, CK::DistanceX, 2.0);    // [1]
    addCon({{l0, 0}}, CK::Fixed, 0.0);                 // [2]
    addCon({{l0, 1}}, CK::Fixed, 0.0);                 // [3]
    addCon({{pp, 0}, {l0, 0}}, CK::DistancePL, 0.7);   // [4]
    addCon({{lA, 0}}, CK::Fixed, 0.0);                 // [5]
    addCon({{lA, 1}}, CK::Fixed, 0.0);                 // [6]
    addCon({{lB, 0}}, CK::Fixed, 0.0);                 // [7]
    addCon({{lA, 0}, {lB, 0}}, CK::Angle, 0.46364761); // [8]
    addCon({{lD, 0}, {lD, 1}}, CK::Distance, 3.0);     // [9]
    IM_CHECK_EQ(sk->constraints.size(), (size_t)10);

    auto editDim = [&](int idx, const char* text) {
      s_App->sketchEdit.editingDim = idx;
      ctx->Yield(2);
      for (int i = 0; i < 12; i++)
        ctx->KeyPress(ImGuiKey_Backspace);
      ctx->KeyChars(text);
      ctx->KeyPress(ImGuiKey_Enter);
      ctx->Yield(2);
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1); // committed cleanly
    };
    auto pointAt = [&](uint32_t id) {
      return std::get<sketch::Point>(sk->entities[sk->IndexOfEntity(id)]).p;
    };

    // DistanceX, literal "-2": refs swap (stored value stays +2), the free
    // point solves to the OTHER side of the fixed one. Undo restores both.
    editDim(1, "-2");
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 2.0), 1e-6);
    IM_CHECK_EQ(sk->constraints[1].a.entity, p1); // swapped
    IM_CHECK_EQ(sk->constraints[1].b.entity, p0);
    IM_CHECK_LE(std::abs(pointAt(p1).x - (-3.0f)), 1e-3f);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints[1].a.entity, p0); // refs restored
    IM_CHECK_LE(std::abs(pointAt(p1).x - 1.0f), 1e-3f);

    // DistanceX, expression "-4/2": refs stay, the RAW signed value drives
    // (regen re-evaluates the expr, so the sign must carry the direction).
    editDim(1, "-4/2");
    IM_CHECK_LE(std::abs(sk->constraints[1].value - (-2.0)), 1e-6);
    IM_CHECK(sk->constraints[1].valueExpr == "-4/2");
    IM_CHECK_EQ(sk->constraints[1].a.entity, p0);          // NOT swapped
    IM_CHECK_LE(std::abs(pointAt(p1).x - (-3.0f)), 1e-3f); // other side

    // DistancePL, literal "-0.7": the side pin flips — the point crosses to
    // the other side of the line; stored value +0.7.
    editDim(4, "-0.7");
    IM_CHECK_LE(std::abs(sk->constraints[4].value - 0.7), 1e-6);
    IM_CHECK_LE(std::abs(pointAt(pp).y - 2.3f), 1e-3f); // was 0.9 (below)
    // ... and a negative-evaluating expression flips again, keeping the raw
    // value + the expression as typed.
    editDim(4, "0.7-1.4");
    IM_CHECK(sk->constraints[4].valueExpr == "0.7-1.4");
    IM_CHECK_LE(std::abs(sk->constraints[4].value - (-0.7)), 1e-6);
    IM_CHECK_LE(std::abs(pointAt(pp).y - 0.9f), 1e-3f); // back below

    // Angle, literal negative degrees: the mirrored wedge — lB rotates to
    // the other side of lA; stored value stays positive.
    editDim(8, "-26.565051");
    IM_CHECK_LE(std::abs(sk->constraints[8].value - 0.46364761), 1e-4);
    IM_CHECK_EQ(sk->constraints[8].a.entity, lB); // swapped
    {
      const auto& lb =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lB)]);
      const float dir = std::atan2(lb.b.y - lb.a.y, lb.b.x - lb.a.x);
      IM_CHECK_LE(std::abs(dir - (-0.46364761f)), 2e-2f); // below lA now
    }

    // Aligned Distance: no direction — a negative input is just |v|.
    editDim(9, "-2");
    IM_CHECK_LE(std::abs(sk->constraints[9].value - 2.0), 1e-6);
    IM_CHECK_EQ(sk->constraints[9].a.entity, lD); // untouched refs
    IM_CHECK_EQ((int)sk->constraints[9].a.which, 0);
    IM_CHECK_EQ((int)sk->constraints[9].b.which, 1);
    {
      const auto& ld =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lD)]);
      IM_CHECK_LE(std::abs(glm::length(ld.b - ld.a) - 2.0f), 1e-3f);
    }

    // Dimension-on-create with a negative typed value: same rule — the
    // created radius is |v|.
    ctx->KeyPress(ImGuiKey_C);
    ctx->Yield();
    {
      glm::vec2 c =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.3f, -0.4f));
      glm::vec2 r =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.6f, -0.4f));
      ctx->MouseMoveToPos(ImVec2(c.x, c.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(ImVec2(r.x, r.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    ctx->KeyChars("5");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    for (int i = 0; i < 4; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("-2");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints.back().kind == CK::Radius);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 2.0), 1e-6);
    {
      int circles = 0;
      double rad = 0.0;
      for (const auto& e : sk->entities)
        if (const auto* c = std::get_if<sketch::Circle>(&e)) {
          circles++;
          rad = c->radius;
        }
      IM_CHECK_EQ(circles, 1);
      IM_CHECK_LE(std::abs(rad - 2.0), 1e-3);
    }

    s_App->sketchEdit.autoConstrain = true;
    ctx->KeyPress(ImGuiKey_Escape); // Circle → Select
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Inline dimension editor accepts full expressions: "3*4" stores
  // valueExpr and drives value 12; a later plain number clears the expr.
  t = IM_REGISTER_TEST(engine, "dims", "inline_editor_expression");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5f, -0.5f}, {1.5f, -0.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    s_App->sketchEdit.editingDim = 0; // open the inline editor
    ctx->Yield(2);

    // The editor's InputText is auto-focused; replace the value. (Backspace
    // clearing — Ctrl+A select-all is unreliable on a just-refocused field.)
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("3*4");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 12.0), 1e-6);
    IM_CHECK(sk->constraints[0].valueExpr == "3*4");
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    // The dimension drove the geometry: the line is now 12 long.
    {
      const auto& ln = std::get<sketch::Line>(sk->entities[0]);
      IM_CHECK_LE(std::abs(glm::length(ln.b - ln.a) - 12.0f), 1e-2f);
    }

    // A plain number reverts to a literal (expr cleared).
    s_App->sketchEdit.editingDim = 0;
    ctx->Yield(2);
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("5");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 5.0), 1e-6);
    IM_CHECK(sk->constraints[0].valueExpr.empty());

    ctx->KeyPress(ImGuiKey_Escape); // clear selection state / leave edit
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // Dimension-on-create (Phase 3.4): typing a number while a draw tool is
  // live opens a value field that applies a driving dimension to the entity
  // just drawn — circle radius, line length, rectangle width then height
  // (expressions accepted). Esc dismisses without touching the tool's state;
  // digits with no fresh entity (Select tool) do nothing.
  t = IM_REGISTER_TEST(engine, "dims", "dimension_on_create");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };

    // Circle + typed "2" = a radius dimension driving r to 2.
    ctx->KeyPress(ImGuiKey_C);
    clickUV({0, 1.5f});
    clickUV({0.6f, 1.5f});
    ctx->KeyChars("2");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Radius);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 1e-6);
    IM_CHECK_LE(
        std::abs(std::get<sketch::Circle>(sk->entities[0]).radius - 2.0), 1e-3);

    // Line: Esc dismisses the field without a dimension and keeps the
    // tool's chained pending point; a second try applies the length.
    ctx->KeyPress(ImGuiKey_L);
    clickUV({-1, -1});
    clickUV({1, -1});
    IM_CHECK(s_App->sketchEdit.hasPendingPoint); // the line tool chains
    ctx->KeyChars("9");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);
    IM_CHECK(s_App->sketchEdit.hasPendingPoint);    // staged state untouched
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // no dimension created
    ctx->KeyChars("4");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 4.0), 1e-6);
    {
      const auto& ln = std::get<sketch::Line>(sk->entities[1]);
      IM_CHECK_LE(std::abs(glm::length(ln.b - ln.a) - 4.0f), 1e-3f);
    }

    // Rectangle: first number = width, typing again = height (as an
    // expression, which stores valueExpr).
    ctx->KeyPress(ImGuiKey_G);
    clickUV({-1, -0.5f});
    clickUV({0.5f, 0.5f});
    ctx->KeyChars("4");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const uint32_t l0 = sketch::EntityId(sk->entities[2]); // bottom side
    const uint32_t l1 = sketch::EntityId(sk->entities[3]); // right side
    IM_CHECK(sk->constraints.back().kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints.back().a.entity, l0);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 4.0), 1e-6);
    ctx->KeyChars("1");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_End); // collapse any select-all, cursor to end
    ctx->KeyChars("+1");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints.back().kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints.back().a.entity, l1);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 2.0), 1e-6);
    IM_CHECK(sk->constraints.back().valueExpr == "1+1");
    {
      const auto& bottom = std::get<sketch::Line>(sk->entities[2]);
      const auto& right = std::get<sketch::Line>(sk->entities[3]);
      IM_CHECK_LE(std::abs(glm::length(bottom.b - bottom.a) - 4.0f), 1e-3f);
      IM_CHECK_LE(std::abs(glm::length(right.b - right.a) - 2.0f), 1e-3f);
    }

    // Digits while NOT in a draw tool do nothing.
    const size_t nCons = sk->constraints.size();
    ctx->KeyPress(ImGuiKey_Escape); // Rectangle → Select
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    ctx->KeyChars("7");
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK_EQ(sk->constraints.size(), nCons);

    s_App->sketchEdit.autoConstrain = true; // restore the default
    ctx->KeyPress(ImGuiKey_Escape);         // leave sketch edit
    ctx->Yield();
  };

  // Center rectangle (Phase 3.6): Shift+G arms it; centre + corner clicks
  // build 4 lines plus a centre point anchored by a symmetric-about-point
  // constraint — dimensioning the width grows the rectangle symmetrically.
  t = IM_REGISTER_TEST(engine, "sketch", "center_rectangle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;

    ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_G);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::RectangleCenter);

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickUV({0.5f, 0.25f}); // centre
    clickUV({1.5f, 0.75f}); // corner
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)5); // 4 sides + centre point
    // Clicks quantize to pixels (~0.005 UV here): loose bound on placement,
    // then exact bounds against the ACTUAL centre below.
    const glm::vec2 c0 = std::get<sketch::Point>(sk->entities[4]).p;
    IM_CHECK_LE(glm::length(c0 - glm::vec2(0.5f, 0.25f)), 0.05f);
    // The diagonal corners sit symmetric about the centre.
    const auto& bottom0 = std::get<sketch::Line>(sk->entities[0]);
    const auto& top0 = std::get<sketch::Line>(sk->entities[2]);
    IM_CHECK_LE(glm::length(0.5f * (bottom0.a + top0.a) - c0), 1e-3f);
    const uint32_t pointId = sketch::EntityId(sk->entities[4]);
    const uint32_t l0 = sketch::EntityId(sk->entities[0]);
    bool anchored = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Symmetric && c.c.entity == pointId)
        anchored = true;
    IM_CHECK(anchored);

    // Pin the centre, dimension the width: symmetric growth about it.
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.selected = {{pointId, 0}};
    IM_CHECK(
        ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Fixed, 0.0));
    ctx->Yield();
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& bottom = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK_LE(std::abs(glm::length(bottom.b - bottom.a) - 3.0f), 1e-3f);
    IM_CHECK_LE(std::abs(0.5f * (bottom.a.x + bottom.b.x) - c0.x), 1e-3f);
    const auto& centerAfter = std::get<sketch::Point>(sk->entities[4]);
    IM_CHECK_LE(glm::length(centerAfter.p - c0), 1e-4f); // centre stayed put

    s_App->sketchEdit.autoConstrain = true;
    ctx->KeyPress(ImGuiKey_Escape); // clear selection
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Tab-bar folders: foldered parts collapse to one flyout button (assembly
  // import drops 100+ parts in one), the flyout switches tabs, and the
  // folder persists through project save/load.
  t = IM_REGISTER_TEST(engine, "tabs", "folder_flyout");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Imported";
    ui::AddPart(*s_App, "Nut");
    s_App->parts.back().folder = "Imported";
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    // One flyout button for the folder; clicking lists the parts.
    ctx->ItemClick("//##PartTabs/###folderImported");
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###fpart2");
    ctx->Yield();
    IM_CHECK_EQ(s_App->activePart, 2);
    IM_CHECK_EQ(s_App->parts[2].name, "Nut");

    // The folder survives a save/load round-trip.
    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_folders")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield();
    int foldered = 0;
    for (const auto& s : s_App->parts)
      if (s.folder == "Imported")
        foldered++;
    IM_CHECK_EQ(foldered, 2);
    std::filesystem::remove_all(dir);
  };

  // The bottom-bar "+" collapses the three add buttons into one menu.
  // Opening a project must frame what it opened. The fit was guarded on
  // `state.document`, which is null when the active tab is an ASSEMBLY, so a
  // 5 m dock opened with the camera at its default — inside the geometry.
  // It read as an empty scene, and the first thing you did was press F.
  t = IM_REGISTER_TEST(engine, "camera", "opening_an_assembly_frames_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A part far bigger than the default camera distance, in an assembly.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Beam", glm::vec3(0), 4000, 200, 300));
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence occ;
    occ.name = "Beam";
    occ.partId = s_App->parts[0].id;
    occ.fixed = true;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_open_frame")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));

    // Reopen from scratch with the camera somewhere useless.
    ctx->MenuClick("File/New");
    ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetDistance(5.0f); // inside anything of this size
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    ctx->Yield();

    // Framed: the camera stands off by something like the model's size,
    // rather than sitting inside it.
    IM_CHECK(s_App->assembly != nullptr);
    IM_CHECK(s_App->camera->GetDistance() > 1000.0f);
    std::filesystem::remove_all(dir);
  };

  // The camera is global, so arriving at a tab far bigger than the last one
  // used to put you inside it. The first visit to a tab frames it; after
  // that the framing is the user's.
  t = IM_REGISTER_TEST(engine, "camera", "first_visit_to_a_tab_frames_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A small part in tab 0 and a six-metre one in tab 1, saved and reopened
    // so both arrive unvisited — the state a project always opens in.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Chip", glm::vec3(0), 8, 8, 8));
    const size_t big = ui::AddPart(*s_App, "Girder");
    ui::SetActivePart(*s_App, big);
    ctx->Yield();
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Span", glm::vec3(0), 6000, 300, 300));
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_tab_frame")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield(2);
    s_App->camera->FinishAnimation();

    // Tab 0 (the 8 mm chip) was framed by the open: close in.
    const float small = s_App->camera->GetDistance();
    IM_CHECK(small < 500.0f);

    // Switching to the girder frames it rather than leaving us inside.
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    IM_CHECK(s_App->camera->GetDistance() > 1000.0f);

    // Coming back a second time keeps whatever framing the user set.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    s_App->camera->SetDistance(42.0f);
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    IM_CHECK(std::abs(s_App->camera->GetDistance() - 42.0f) < 1.0f);
    std::filesystem::remove_all(dir);
  };

  // The Render modal on an assembly tab. Its own Render button carried a
  // part-only test (state.document && bodies), so on an assembly — where
  // `document` is null — it sat greyed out behind a working Render entry,
  // saying "Add a body to render" about an assembly full of parts.
  t = IM_REGISTER_TEST(engine, "render", "modal_enabled_on_an_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // a part with a body
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly != nullptr);

    // Empty assembly: the TOOLBAR button refuses — there is nothing to trace,
    // and opening a modal to say so would be theatre.
    ctx->SetRef("//##DockHost");
    IM_CHECK((ctx->ItemInfo("**/###tb_asm_render").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);

    // With an occurrence in it, the modal opens AND its own Render button is
    // live. That button used to test `state.document && bodies`, which is
    // false on every assembly tab, so it sat greyed behind a working entry
    // saying "Add a body to render" about an assembly full of parts.
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    occ.fixed = true;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly->OccurrenceCount() > 0);
    ToolbarClick(ctx, "###tb_asm_render");
    ctx->Yield(2);
    IM_CHECK(ctx->UiContext->OpenPopupStack.Size > 0); // the modal is up
    const bool rtReady =
        s_App->rayTracer != nullptr && s_App->rayTracer->IsInitialized();
    if (rtReady) {
      IM_CHECK((ctx->ItemInfo("//Render Still/###render_go").ItemFlags &
                ImGuiItemFlags_Disabled) == 0);
    } else {
      // No ray tracing on this GPU: the modal must blame the GPU, not the
      // model for being empty.
      IM_CHECK(ctx->ItemExists("//Render Still/###render_close"));
    }
    ctx->ItemClick("//Render Still/###render_close");
    ctx->Yield();
  };

  // A tab bar of names alone made a part, its drawing and a folder of them
  // look identical until you clicked. Each tab now carries its kind's glyph,
  // and the ids are "###" so they do not move when the label gains one.
  t = IM_REGISTER_TEST(engine, "tabs", "kind_glyphs_and_stable_ids");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddAssembly(*s_App, "Rig");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Hardware";
    ctx->Yield(2);

    // Addressable by id whatever the label says.
    IM_CHECK(ctx->ItemExists("//##PartTabs/###part0"));
    IM_CHECK(ctx->ItemExists("//##PartTabs/###part1"));
    IM_CHECK(ctx->ItemExists("//##PartTabs/###folderHardware"));

    // And the labels carry the kind glyph — a part is a cube, an assembly is
    // cubes, a folder is a folder.
    ImGuiTestItemInfo part = ctx->ItemInfo("//##PartTabs/###part0");
    ImGuiTestItemInfo asm_ = ctx->ItemInfo("//##PartTabs/###part1");
    ImGuiTestItemInfo folder = ctx->ItemInfo("//##PartTabs/###folderHardware");
    IM_CHECK(strstr(part.DebugLabel, ICON_FA_CUBE) != nullptr);
    IM_CHECK(strstr(asm_.DebugLabel, ICON_FA_CUBES) != nullptr);
    IM_CHECK(strstr(folder.DebugLabel, ICON_FA_FOLDER) != nullptr);
    // The glyphs must be DIFFERENT, or the indicator indicates nothing.
    IM_CHECK(strcmp(part.DebugLabel, asm_.DebugLabel) != 0);
  };

  // File > Open Recent: the app's own history, kept in the state dir. The
  // menu is disabled when empty, lists what was opened, and keeps entries
  // whose file has since gone (disabled, with the reason).
  t = IM_REGISTER_TEST(engine, "file", "open_recent_lists_what_was_opened");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_recent_gui";
    std::filesystem::remove_all(dir);
    RecentProjects::Clear();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "test");
    ctx->Yield();

    // Saving is what puts it there — the same hook as opening.
    const auto recent = RecentProjects::List();
    IM_CHECK_EQ(recent.size(), (size_t)1);
    IM_CHECK(recent[0].find("designer_recent_gui") != std::string::npos);

    // The menu shows it, and clicking it opens that project.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // a fresh document
    ctx->MenuClick("File/Open Recent/###recent0");
    ctx->Yield(2);
    IM_CHECK(Doc()->FeatureCount() > 0); // the box came back
    IM_CHECK_EQ(s_App->projectPath, dir.string());

    std::filesystem::remove_all(dir);
    RecentProjects::Clear();
  };

  t = IM_REGISTER_TEST(engine, "tabs", "add_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Part##addpart");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK(s_App->parts[1].kind == ui::DocKind::Part);
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Assembly##addasm");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)3);
    IM_CHECK(s_App->parts[2].kind == ui::DocKind::Assembly);
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)4);
    IM_CHECK(s_App->parts[3].kind == ui::DocKind::Drawing);
  };

  // "+ > New Folder" creates an EMPTY folder (registry-backed): it renders
  // as a (0) flyout button, survives save/load while empty, and accepts a
  // dropped tab.
  t = IM_REGISTER_TEST(engine, "tabs", "folder_empty_create");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    ctx->Yield();
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Folder##addfolder");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Fasteners");
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)1);
    IM_CHECK_EQ(s_App->tabFolders[0], std::string("Fasteners"));
    IM_CHECK(ui::ProjectDirty(*s_App));

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_empty_folder")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)0);
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)1);
    IM_CHECK_EQ(s_App->tabFolders[0], std::string("Fasteners"));

    // The empty folder is a live drop target.
    ctx->ItemDragAndDrop("//##PartTabs/###part1",
                         "//##PartTabs/###folderFasteners");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[1].folder, std::string("Fasteners"));
    std::filesystem::remove_all(dir);
  };

  // Folder management (Onshape-style tab folders): rename via right-click,
  // move docs in/out via the tab context menus, and drag-drop between the
  // bar and folder buttons. Folder edits mark the project dirty even though
  // no document version changes (they live only in project.json).
  t = IM_REGISTER_TEST(engine, "tabs", "folder_manage");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Imported";
    ui::AddPart(*s_App, "Nut");
    s_App->parts.back().folder = "Imported";
    ui::SetActivePart(*s_App, 0);
    s_App->projectMetaDirty = false; // direct setup writes above don't count
    ctx->Yield(2);

    // Rename the folder via right-click > Rename.
    ctx->ItemClick("//##PartTabs/###folderImported", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Rename...");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Hardware");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[1].folder, std::string("Hardware"));
    IM_CHECK_EQ(s_App->parts[2].folder, std::string("Hardware"));
    IM_CHECK(ui::ProjectDirty(*s_App));

    // Move the top-level part into a NEW folder via its context menu.
    ctx->ItemClick("//##PartTabs/###part0", ImGuiMouseButton_Right);
    ctx->MenuClick("//$FOCUSED/Move to folder/New folder...##mvnew");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Misc");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string("Misc"));

    // ...and back out of it via the flyout entry's context menu.
    ctx->ItemClick("//##PartTabs/###folderMisc");
    ctx->ItemClick("//$FOCUSED/###fpart0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Move out of folder");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string(""));

    // Drag the top-level tab onto the folder button to file it.
    ctx->ItemDragAndDrop("//##PartTabs/###part0",
                         "//##PartTabs/###folderHardware");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string("Hardware"));

    // Drag "Nut" out of the open flyout onto the bar's empty tail.
    // Hand-rolled with raw positions: ItemDragAndDrop refocuses the target's
    // window to resolve it, which closes the flyout mid-drag.
    const ImVec2 tailPos =
        ctx->ItemInfo("//##PartTabs/##tabsdroptail").RectFull.GetCenter();
    ctx->ItemClick("//##PartTabs/###folderHardware");
    const ImVec2 nutPos =
        ctx->ItemInfo("//$FOCUSED/###fpart2").RectFull.GetCenter();
    ctx->MouseMoveToPos(nutPos);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(tailPos);
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[2].folder, std::string(""));

    // Dissolve the folder: contents move to the top level, nothing deleted.
    ctx->ItemClick("//##PartTabs/###folderHardware", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Dissolve folder");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)3);
    for (const auto& s : s_App->parts)
      IM_CHECK_EQ(s.folder, std::string(""));
  };

  // Mate-tool hover feedback uses the same fresh face inference as a click:
  // before committing either connector it identifies the candidate occurrence,
  // then clears the transient marker as soon as the live mate preview starts.
  t = IM_REGISTER_TEST(engine, "assembly", "mate_hover_marker");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const uint64_t partSlotId = s_App->parts[0].id;

    ui::AddAssembly(*s_App, "Asm");
    ctx->Yield();
    Occurrence first;
    first.partId = partSlotId;
    first.fixed = true;
    const uint64_t firstId = s_App->assembly->AddOccurrence(first);
    Occurrence second;
    second.partId = partSlotId;
    second.position = glm::vec3(10.0f, 0.0f, 0.0f);
    const uint64_t secondId = s_App->assembly->AddOccurrence(second);
    s_App->assembly->BumpVersion();
    ctx->Yield(3);

    const glm::vec3 center = (min + max) * 0.5f;
    const glm::vec3 mid = center + glm::vec3(5.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(mid +
                               glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)) *
                                   glm::length(max - min) * 6.0f);
    ui::StartMateTool(*s_App, MateType::Fastened);
    ctx->Yield();

    // The ray through the empty gap between occurrences clears any candidate.
    ctx->MouseMoveToPos(WorldToWindow(mid));
    ctx->Yield();
    IM_CHECK(!s_App->mateTool.hasHover);

    const glm::vec3 topFirst(center.x, max.y, center.z);
    const glm::vec3 topSecond = topFirst + glm::vec3(10.0f, 0.0f, 0.0f);
    const glm::vec3 edgeMidpoint(center.x, max.y, max.z);
    ctx->MouseMoveToPos(WorldToWindow(edgeMidpoint));
    ctx->Yield();
    IM_CHECK(s_App->mateTool.hasHover);
    IM_CHECK(s_App->mateTool.hover.anchor.snapEdge.IsSet());
    IM_CHECK(s_App->mateTool.hoverSnapIsMidpoint);

    // Moving to the middle of the face prefers its centroid over all edges.
    ctx->MouseMoveToPos(WorldToWindow(topFirst));
    ctx->Yield();
    IM_CHECK(s_App->mateTool.hasHover);
    IM_CHECK_EQ(s_App->mateTool.hover.occurrenceId, firstId);
    IM_CHECK(!s_App->mateTool.hover.anchor.snapEdge.IsSet());

    // Click the midpoint candidate and verify the connector persists the same
    // stable edge reference that hover advertised.
    ctx->MouseMoveToPos(WorldToWindow(edgeMidpoint));
    ctx->Yield();
    const ElementRef hoveredMidpoint = s_App->mateTool.hover.anchor.snapEdge;
    IM_CHECK(hoveredMidpoint.IsSet());
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->mateTool.haveFirst);
    IM_CHECK_EQ(s_App->mateTool.first.occurrenceId, firstId);
    IM_CHECK_EQ(s_App->mateTool.first.anchor.snapEdge.name,
                hoveredMidpoint.name);

    ctx->MouseMoveToPos(WorldToWindow(topSecond));
    ctx->Yield();
    IM_CHECK(s_App->mateTool.hasHover);
    IM_CHECK_EQ(s_App->mateTool.hover.occurrenceId, secondId);
    IM_CHECK(!s_App->mateTool.hover.anchor.snapEdge.IsSet());
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_NE(s_App->mateTool.previewMateId, uint64_t{0});
    IM_CHECK(!s_App->mateTool.hasHover);

    ui::CancelMateTool(*s_App);
    IM_CHECK(!s_App->mateTool.active);
  };

  // U7c: an existing mate's connector can be re-picked — StartMateEdit opens
  // the session, clicking the B row arms it, and a viewport pick rewrites
  // that side (one undo step) and re-solves.
  t = IM_REGISTER_TEST(engine, "assembly", "mate_edit_repick_connector");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const uint64_t partSlotId = s_App->parts[0].id;

    ui::AddAssembly(*s_App, "Asm");
    ctx->Yield();
    Occurrence first;
    first.partId = partSlotId;
    first.fixed = true;
    s_App->assembly->AddOccurrence(first);
    Occurrence second;
    second.partId = partSlotId;
    second.position = glm::vec3(10.0f, 0.0f, 0.0f);
    const uint64_t secondId = s_App->assembly->AddOccurrence(second);
    s_App->assembly->BumpVersion();
    ctx->Yield(3);

    const glm::vec3 center = (min + max) * 0.5f;
    const glm::vec3 mid = center + glm::vec3(5.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(mid +
                               glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)) *
                                   glm::length(max - min) * 6.0f);
    ctx->Yield();

    // Create a fastened mate via the session (top faces of both boxes).
    ui::StartMateTool(*s_App, MateType::Fastened);
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active()); // connector A armed itself
    const glm::vec3 topFirst(center.x, max.y, center.z);
    const glm::vec3 topSecond = topFirst + glm::vec3(10.0f, 0.0f, 0.0f);
    ctx->MouseMoveToPos(WorldToWindow(topFirst));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->mateTool.haveFirst);
    IM_CHECK(s_App->slotPick.Active()); // auto-advanced to connector B
    ctx->MouseMoveToPos(WorldToWindow(topSecond));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_NE(s_App->mateTool.previewMateId, uint64_t{0});
    ui::AcceptMateTool(*s_App);
    ctx->Yield();
    const uint64_t mateId = ui::SelectedMateId(*s_App);
    IM_CHECK_NE(mateId, uint64_t{0});
    const std::string oldFaceA =
        s_App->assembly->FindMateById(mateId)->a.anchor.face.name;

    // Re-pick connector A onto the FIXED box's front face (occurrence 2
    // moved when the fastened mate solved; occurrence 1 stays at the
    // origin, so its face positions are known). Frame it center-screen —
    // the session window floats at the viewport's top right, and a pick
    // under it would hit the window instead.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(center +
                               glm::normalize(glm::vec3(0.15f, 0.35f, 1.0f)) *
                                   glm::length(max - min) * 5.0f);
    ctx->Yield();
    ui::StartMateEdit(*s_App, mateId);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active()); // rows filled: nothing self-arms
    ctx->ItemClick("//Edit Mate###ToolWindow/###slot12");
    IM_CHECK(s_App->slotPick.Active());
    const glm::vec3 frontFirst(center.x, center.y, max.z);
    ctx->MouseMoveToPos(WorldToWindow(frontFirst));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    Mate* m = s_App->assembly->FindMateById(mateId);
    IM_CHECK(m != nullptr);
    IM_CHECK(m->a.anchor.face.name != oldFaceA);
    IM_CHECK(!s_App->slotPick.Active()); // one re-pick per row click

    ctx->ItemClick("//Edit Mate###ToolWindow/Done");
    ctx->Yield();
    IM_CHECK(!s_App->mateTool.active);
  };

  // Assembly measure: two occurrences of a 2mm box, 10mm apart — measuring
  // a top face on each reads the 8mm gap in world space; re-clicking one
  // deselects it (occurrence is part of the element's identity).
  t = IM_REGISTER_TEST(engine, "measure", "assembly_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const uint64_t partSlotId = s_App->parts[0].id;

    ui::AddAssembly(*s_App, "Asm");
    ctx->Yield();
    IM_CHECK(s_App->assembly != nullptr);
    Occurrence o1;
    o1.partId = partSlotId;
    o1.fixed = true;
    const uint64_t occ1 = s_App->assembly->AddOccurrence(o1);
    Occurrence o2;
    o2.partId = partSlotId;
    o2.position = glm::vec3(10.0f, 0.0f, 0.0f);
    const uint64_t occ2 = s_App->assembly->AddOccurrence(o2);
    s_App->assembly->BumpVersion();
    ctx->Yield(3); // resync builds the occurrence meshes

    // Slightly-elevated view centered between the boxes; top faces clear.
    const glm::vec3 cA = (min + max) * 0.5f;
    const glm::vec3 mid = cA + glm::vec3(5.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(mid +
                               glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)) *
                                   glm::length(max - min) * 6.0f);
    ctx->Yield();

    s_App->measure = ui::MeasureState{};
    s_App->measure.active = true;
    ctx->Yield();

    // Top-face centers of each occurrence (world space).
    const glm::vec3 topA(cA.x, max.y, cA.z);
    const glm::vec3 topB = topA + glm::vec3(10.0f, 0.0f, 0.0f);
    ctx->MouseMoveToPos(WorldToWindow(topA));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);
    IM_CHECK_EQ(s_App->measure.picked[0].occurrenceId, occ1);

    ctx->MouseMoveToPos(WorldToWindow(topB));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)2);
    IM_CHECK_EQ(s_App->measure.picked[1].occurrenceId, occ2);
    IM_CHECK(s_App->measure.hasDistanceLine);
    const float gap = glm::length(s_App->measure.distB - s_App->measure.distA);
    IM_CHECK_LE(std::abs(gap - (10.0f - (max.x - min.x))), 1e-3f);

    // Same face, same occurrence → toggles OFF (identity includes the
    // occurrence, so this must not add a third element).
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
  };

  // Turning the terminal preference off and on again must give a working
  // shell. ShutdownTerminal terminated the PTY but kept the TerminalState,
  // whose `everStarted` latch (there to stop the shell respawning after the
  // user presses Stop) then blocked every future start — a panel that could
  // never open a shell again until the app restarted. The same one-shot trap
  // the chooser this panel replaced used to have.
  t = IM_REGISTER_TEST(engine, "terminal", "reopens_after_being_turned_off");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const bool had = s_App->settings.terminalPanel;
    s_App->settings.terminalPanel = true;
    ctx->Yield(8);
    IM_CHECK(ui::TerminalRunning(*s_App));

    s_App->settings.terminalPanel = false;
    ctx->Yield(4);
    IM_CHECK(!ui::TerminalRunning(*s_App)); // ended, not hidden

    // And again — this is what used to be impossible.
    s_App->settings.terminalPanel = true;
    ctx->Yield(8);
    IM_CHECK(ui::TerminalRunning(*s_App));

    s_App->settings.terminalPanel = had;
    ctx->Yield(4);
  };

  // F10: the other half. Autosave writing the right documents is only useful
  // if recovery puts them back — and the old path opened the sidecar AS the
  // project, which on a multi-document project would have replaced it with
  // one part.
  t = IM_REGISTER_TEST(engine, "autosave", "recovers_every_document");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_recover.dsgn";
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir.string() + ".autosave.d");

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Dirty the part that is NOT the open tab.
    const auto slotOf = [](const char* name) -> size_t {
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].name == name)
          return i;
      return 0;
    };
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 44.0f);
    ctx->Yield();
    ui::SetActivePart(*s_App, slotOf("Second")); // and look away from it
    ctx->Yield(2);

    Autosaver autosaver(0.001);
    autosaver.Prime(0.0);
    IM_CHECK(ui::AutosaveProject(*s_App, autosaver, 1.0) >= 1);
    IM_CHECK(!Autosaver::RecoverableDocuments(dir.string()).empty());

    // The crash: reopen from disk, which has the SAVED width, not 44.
    IM_CHECK(ui::OpenProject(*s_App, dir.string(), &err));
    ui::MarkProjectOpened(*s_App, dir.string());
    ctx->Yield(2);
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0); // the saved value — the edit is gone

    // Recovery puts it back, into the right document.
    IM_CHECK(ui::RecoverProject(*s_App, dir.string(), &err));
    ctx->Yield(3);
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 44.0);
    // And it is not pretending to be saved.
    IM_CHECK(ui::ProjectDirty(*s_App));
  };

  // A clean project must stop costing anything. The conflicts scan walks the
  // whole project (for the duplicate ids git cannot report) AND spawns
  // `git` — it used to do both once a second, forever, to answer "no".
  t = IM_REGISTER_TEST(engine, "conflicts", "a_clean_project_stops_scanning");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_scan_cost.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(5);

    const uint64_t before = s_App->conflictScans;
    ctx->Yield(60); // a second of frames, and then some
    const uint64_t during = s_App->conflictScans - before;
    IM_CHECK_LE(during, (uint64_t)1); // at most the one the settle allowed
    ctx->LogInfo("scans over 60 clean frames: %d", (int)during);

    // But an actual change still gets noticed — the point is to stop asking,
    // not to stop answering.
    {
      std::ofstream(dir / "NOTES.txt") << "someone else\n";
    }
    s_App->diskCheckStamp = -1.0;
    ctx->Yield(5);
    IM_CHECK(s_App->conflictScans > before + during);
  };

  // F8: the project moved on disk while the app held it in memory. This is
  // the terminal-panel scenario — the panel ships inside the app and starts
  // in the project's directory, so a `git checkout` there is a normal thing
  // to do. Before this, the next Ctrl+S replaced it with no message.
  t = IM_REGISTER_TEST(engine, "disk", "a_moved_project_refuses_to_be_saved");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_disk_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);
    // A project we just wrote has not moved, and saving again is fine — the
    // app's own writes must never read as somebody else's.
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Somebody else writes.
    {
      std::ofstream(dir / "NOTES.txt") << "not this app\n";
    }
    IM_CHECK(ui::ProjectChangedOnDisk(*s_App));

    // The save refuses and raises the modal rather than overwriting.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->diskChangeBlockedSave);
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));

    // Cancel leaves the project moved: dismissing a save does not un-move it.
    ctx->SetRef("//Project changed on disk");
    ctx->ItemClick("Cancel###disk_cancel");
    ctx->Yield(3);
    IM_CHECK(!s_App->diskChangeBlockedSave);
    IM_CHECK(s_App->diskChanged);
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));

    // Save anyway goes through, and re-stamps — the same change must not be
    // reported twice.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->diskChangeBlockedSave);
    ctx->SetRef("//Project changed on disk");
    ctx->ItemClick("Save anyway###disk_overwrite");
    ctx->Yield(3);
    IM_CHECK(!s_App->diskChangeBlockedSave);
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
    // A save is not a delete: an unrelated external file is still there.
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));
  };

  // The banner is the half that makes the refusal rarely the first news. It
  // shows on any saved project, git or not — "something else wrote to your
  // project" is true either way.
  t = IM_REGISTER_TEST(engine, "disk", "the_history_panel_says_it_moved");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_disk_banner.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    // Nothing has happened: no banner.
    IM_CHECK(!s_App->diskChanged);

    // Go back to the model, which is where a person actually is when
    // somebody else writes to their project.
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(model != nullptr);
    ctx->ItemClick(model->TabId);
    ctx->Yield(2);

    {
      std::ofstream(dir / "NOTES.txt") << "not this app\n";
    }
    // The poll is deliberately slow (a directory walk), so drive it rather
    // than yielding for three seconds of test time.
    s_App->diskCheckStamp = -1.0;
    ctx->Yield(3);
    // Noticed with the History panel BEHIND another tab: the poll runs from
    // the top of the frame, not from inside the panel, which would only ever
    // have caught changes made while you were looking at the History tab.
    IM_CHECK(s_App->diskChanged);
    // The menu-bar notice this flag drives (MainMenuBar's ###disk_notice) is
    // NOT asserted here: a disabled MenuItem in the main menu bar does not
    // resolve through the test engine's item paths, and three ways of asking
    // failed. What matters and IS covered is the flag being true while
    // another tab owns the dock node — the notice is fifteen lines of ImGui
    // reading it.

    // Now go read it.
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("Reload###hist_disk_reload");
    ctx->Yield(3);

    // Reloading agrees with the disk again, so the banner goes.
    IM_CHECK(!s_App->diskChanged);
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
  };

  // F11 G2: clicking a version shows what has changed since it. The point is
  // that it reads as the model, not as files — "Box \"Box 1\" — width 2 to 9",
  // not "features/1-box.json | M".
  t = IM_REGISTER_TEST(engine, "history", "a_version_shows_what_changed_since");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_diff_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(3);

    // Change the model AFTER that version, and write it — the diff is against
    // what is ON DISK, which is what "since that version" means. Saved but
    // not committed, so this is the everyday question: what have I done since
    // the last version?
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 9.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ctx->Yield(2);

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("**/###hist_row0"); // the version, not a button
    ctx->Yield(3);

    IM_CHECK(!s_App->diffFrom.empty());
    IM_CHECK_EQ(s_App->diffChanges.size(), (size_t)1);
    const ProjectMerge::ItemChange& c = s_App->diffChanges[0];
    IM_CHECK_STR_EQ(c.itemKind.c_str(), "Box");
    IM_CHECK(c.summary.find("width 2 to 9") != std::string::npos);

    // Back returns to the list, and the versions are still there.
    ctx->ItemClick("Back###hist_diff_back");
    ctx->Yield(2);
    IM_CHECK(s_App->diffFrom.empty());
    IM_CHECK(ctx->ItemExists("**/###hist_row0"));
  };

  // Variables and Configurations: two v1-complete workstreams with, until
  // now, ZERO gui-test contact of any kind. That was not an oversight — the
  // panels could not be clicked. Every row sat in an ImGui::PushID(int)
  // scope, which leaves a widget reachable only by numeric id
  // (docs/ui/gui-coverage.md), so no test and no agent could name a cell.
  //
  // Behind that, the flow was broken end to end: the Variables panel creates
  // every variable at PROJECT scope, and the Configurations panel listed
  // only PART-scoped ones — so the first thing anyone would try (add a
  // variable, make a configuration of it) answered "No variables to
  // override" immediately after they had added one. Document::Regenerate had
  // always handled the project case; only the panel did not show it.
  //
  // This walks the whole parametric-family story by clicking: a variable, a
  // dimension driven by it, a configuration that overrides it, and the
  // geometry following in both directions.
  t = IM_REGISTER_TEST(engine, "configs", "a_parametric_family_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    // A 4 x 4 profile extruded 10: something a variable can actually drive.
    // (Box/Sphere/Cylinder cannot — the primitives carry no expression
    // fields at all, so a part built from them can never be configured.)
    AddRectSketch(ctx, 1, 2.0f);
    ToolbarCreateAccept(ctx, "###tb_extrude");
    ctx->Yield(2);
    // A 4 x 4 profile: every volume below is that area times the length.
    constexpr double kArea = 4.0 * 4.0;
    const double base = Doc()->GetBodies().at(0).shape.Volume();
    IM_CHECK(base > 0.0);

    // These panels share a dock node with Model, so bring each to the front
    // before clicking in it — and put Model back at the end, or the next
    // test starts on the wrong tab (docs/ui/gui-coverage.md).
    const auto showPanel = [&](const char* title) {
      ImGuiWindow* w = ImGui::FindWindowByName(title);
      IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
      ctx->ItemClick(w->TabId);
      ctx->Yield(2);
    };

    // A variable, by clicking. It lands in PROJECT scope — that is what the
    // panel's only button does.
    showPanel(ui::kPanelVariables);
    ctx->ItemClick("//###Variables/+ Add variable");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Variables/###vars_table/###var_name_project_var1",
                        "len");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Variables/###vars_table/###var_expr_project_len",
                        "30");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->projectParameters.size(), (size_t)1);
    IM_CHECK_STR_EQ(s_App->projectParameters[0].name.c_str(), "len");

    // Drive the extrude from it: fx turns the dimension into an expression.
    // Fully qualified, never "**": a wildcard crawl through this many items
    // runs for minutes and trips the engine watchdog.
    showPanel(ui::kPanelModel);
    ctx->ItemDoubleClick("//###Model/Extrude 2##1");
    ctx->Yield(2);
    ctx->ItemClick("//###ToolWindow/Distance/fx");
    ctx->Yield(2);
    ctx->ItemInputValue("//###ToolWindow/Distance/##expr", "len");
    ctx->Yield(3);
    ctx->ItemClick("//###ToolWindow/Close");
    ctx->Yield(2);
    const double driven = Doc()->GetBodies().at(0).shape.Volume();
    ctx->LogInfo("base %.1f -> driven by len=30: %.1f", base, driven);
    IM_CHECK(std::abs(driven - kArea * 30.0) < 1e-3);

    // A configuration that overrides it. This is the row that used to read
    // "No variables to override".
    showPanel(ui::kPanelConfigurations);
    ctx->ItemClick("//###Configurations/+ Add configuration");
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetConfigurations().size(), (size_t)1);
    const uint64_t cfgId = Doc()->GetConfigurations()[0].id;
    ctx->ItemInputValue("//###Configurations/###cfg_name", "Short");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Configurations/cfg_overrides/###cfg_override_len",
                        "10");
    ctx->Yield(3);
    const double shortVol = Doc()->GetBodies().at(0).shape.Volume();
    ctx->LogInfo("Short (len=10): %.1f", shortVol);
    IM_CHECK(std::abs(shortVol - kArea * 10.0) < 1e-3);

    // ...and back. A configuration that cannot be left is not a
    // configuration, it is an edit.
    ctx->ItemClick("//###Configurations/Base (as authored)");
    ctx->Yield(3);
    IM_CHECK(std::abs(Doc()->GetBodies().at(0).shape.Volume() - driven) < 1e-3);
    const std::string row =
        "//###Configurations/Short###cfg_row_" + std::to_string(cfgId);
    ctx->ItemClick(row.c_str());
    ctx->Yield(3);
    IM_CHECK(std::abs(Doc()->GetBodies().at(0).shape.Volume() - shortVol) <
             1e-3);
    showPanel(ui::kPanelModel); // leave the dock as the next test expects it
  };

  // Two ways to do one thing, and nothing checking they agree. add_drawing_view
  // dropped every view at the sheet centre at 1:1 while the Add view button
  // fitted a standard scale and placed it clear of the others — so a scripted
  // sheet came out as a pile and a clicked one came out as a drawing. Nobody
  // noticed because each path had its own test.
  //
  // The bar is the document state, not the code path: do it by clicking, do
  // it by RPC, and the two documents have to match. Add a pair here whenever
  // an action grows a second way to reach it.
  t = IM_REGISTER_TEST(engine, "parity", "clicking_and_scripting_agree");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // The part both sheets will show: three different dimensions, so a
    // difference in orientation or scale cannot hide.
    const auto buildPart = [&] {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New");
      ctx->Yield();
      ToolbarCreateAccept(ctx, "###tb_box");
      ctx->ItemDoubleClick("//###Model/Box 1##0");
      ctx->Yield();
      ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 80.0f);
      ctx->ItemInputValue("//Box###ToolWindow/Height/##Height", 8.0f);
      ctx->ItemInputValue("//Box###ToolWindow/Depth/##Depth", 60.0f);
      ctx->Yield(2);
      ctx->ItemClick("//##PartTabs/+##addmenu");
      ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
      ctx->Yield(3);
    };
    // What a sheet IS, for comparison: every view's orientation, scale and
    // where it sits. Ids differ between the two runs and are left out.
    const auto sheetState = [&] {
      std::vector<std::string> rows;
      for (const DrawingView& v : s_App->drawing->GetViews()) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s scale=%.4f at (%.3f, %.3f)",
                 ViewOrientationToString(v.orientation), v.scale, v.position.x,
                 v.position.y);
        rows.push_back(buf);
      }
      return rows;
    };

    buildPart();
    ctx->SetRef("//###Model");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose one for me
    for (const char* orient : {"front", "top", "right"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    const std::vector<std::string> clicked = sheetState();
    IM_CHECK_EQ(clicked.size(), (size_t)3);

    // The same three views, asked for the way a script asks: no position and
    // no scale, because that is what "just put a front view on it" means.
    buildPart();
    for (const char* orient : {"front", "top", "right"}) {
      const nlohmann::json params = {{"source", "Part 1"},
                                     {"orientation", orient}};
      remote::RpcParams rp(params);
      const auto reply =
          remote::HandleDrawingRpc(*s_App, "add_drawing_view", rp);
      IM_CHECK(reply.has_value());
      ctx->Yield(2);
    }
    const std::vector<std::string> scripted = sheetState();

    IM_CHECK_EQ(scripted.size(), clicked.size());
    for (size_t i = 0; i < clicked.size() && i < scripted.size(); i++) {
      ctx->LogInfo("view %d: clicked [%s] scripted [%s]", (int)i,
                   clicked[i].c_str(), scripted[i].c_str());
      IM_CHECK_STR_EQ(scripted[i].c_str(), clicked[i].c_str());
    }
  };

  // Drawings had ONE gui test and all it asserted was that the tab appeared.
  // The workspace behind it — pick a source, pick an orientation, add a view
  // — had never been clicked, which is how a Z-up orientation table survived
  // in a Y-up app: every "front" view was really the top. It could not have
  // been clicked either. A bare ImGui::Combo gathers with an EMPTY label, so
  // no ref could reach the two combos that drive the panel; they carry
  // ###ids now.
  //
  // The plate has three different dimensions on purpose. Against a cube, or
  // even a square plate, a swapped axis looks exactly like a correct one.
  t = IM_REGISTER_TEST(engine, "drawing", "build_a_drawing_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 80.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Height/##Height", 8.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Depth/##Depth", 60.0f);
    ctx->Yield(2); // editing a committed feature commits as you type

    // A drawing tab, from the tab bar's + menu.
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield(3);
    IM_CHECK(s_App->drawing != nullptr);

    // Two views, by clicking: the orientation combo then Add view.
    // ComboClick wants a ref RELATIVE to the window (docs/ui/gui-coverage.md).
    ctx->SetRef("//###Model");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose a scale for me
    for (const char* orient : {"front", "top"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    const auto& views = s_App->drawing->GetViews();
    IM_CHECK_EQ(views.size(), (size_t)2);

    // What each named view actually SHOWS. Up is +Y here, so the front of a
    // plate 80 wide and 8 thick is 80 x 8 — not the 80 x 60 a Z-up table
    // produced, which is the top view wearing the wrong name.
    // No IM_CHECK in here: its failure path returns void, which makes the
    // lambda's return type ambiguous. An unprojected view comes back as
    // zero and fails the size checks below just as loudly.
    const auto extent = [&](size_t i) -> glm::dvec2 {
      const auto it = s_App->drawingRenders.find(views[i].id);
      if (it == s_App->drawingRenders.end() || !it->second.hasGeometry)
        return glm::dvec2(0.0);
      return it->second.bboxMax - it->second.bboxMin;
    };
    const glm::dvec2 front = extent(0), top = extent(1);
    ctx->LogInfo("front %.1f x %.1f, top %.1f x %.1f", front.x, front.y, top.x,
                 top.y);
    IM_CHECK(std::abs(front.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(front.y - 8.0) < 1e-6);
    IM_CHECK(std::abs(top.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(top.y - 60.0) < 1e-6);

    // Two views of one part share a scale — a top view at 2:1 beside a front
    // view at 1:1 is a drafting error, and it is what fitting each view to
    // the sheet on its own produced.
    IM_CHECK_EQ(views[0].scale, views[1].scale);

    // ...and they do not land on top of each other. The sheet validator is
    // the same one the panel shows, so this is the user-visible verdict.
    const std::vector<drawing::DrawingIssue> issues = drawing::ValidateDrawing(
        *s_App->drawing, s_App->drawingRenders, {}, {});
    for (const drawing::DrawingIssue& issue : issues)
      ctx->LogWarning("%s", issue.detail.c_str());
    IM_CHECK(issues.empty());
  };

  // F11 layer 3: what the GEOMETRY did between a version and now. The
  // numbers have to be real — a box grown from 20 to 30 mm on one axis adds
  // exactly the slab that appeared, and nothing is removed.
  t = IM_REGISTER_TEST(engine, "history", "compare_shape_against_a_version");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_geomdiff.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box"); // 2 x 2 x 2 by default
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);
    const std::string version = GitVault::Log(dir.string())[0].hash;

    // Grow one axis: 2x2x2 -> 6x2x2. That is +16 mm3 and nothing lost.
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 6.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ctx->Yield(2);

    const ui::GeometryChange g =
        ui::CompareVersionGeometry(*s_App, version, "parts/Part 1");
    ctx->LogInfo("geometry diff: %s (old %.1f, new %.1f)", g.summary.c_str(),
                 g.oldVolume, g.newVolume);
    IM_CHECK(g.valid);
    IM_CHECK(std::abs(g.oldVolume - 8.0) < 1e-3);
    IM_CHECK(std::abs(g.newVolume - 24.0) < 1e-3);
    IM_CHECK(std::abs(g.added - 16.0) < 1e-3); // the slab that appeared
    IM_CHECK(g.removed < 1e-6);                // nothing was cut away
    IM_CHECK(g.summary.find("added") != std::string::npos);
    IM_CHECK(g.summary.find("removed") == std::string::npos);

    // The diff is also ON the model now: the slab that appeared is a green
    // overlay solid, drawn over the part. A volume answers "how much"; only
    // the shape answers "where", which is the question people actually ask.
    ctx->Yield(2); // the resync that turns the shapes into instances
    IM_CHECK(s_App->HasGeometryDelta());
    IM_CHECK(s_App->deltaExtraInstance != 0);
    IM_CHECK(s_App->deltaSummary.find("added") != std::string::npos);

    // And a version compared against itself is "same shape", not noise.
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);
    const std::string now = GitVault::Log(dir.string())[0].hash;
    const ui::GeometryChange same =
        ui::CompareVersionGeometry(*s_App, now, "parts/Part 1");
    IM_CHECK(same.valid);
    IM_CHECK_STR_EQ(same.summary.c_str(), "same shape");
    ctx->Yield(2);
    IM_CHECK(!s_App->HasGeometryDelta()); // nothing to show, nothing shown

    // The overlay is true only of the shape it was measured on. Edit the
    // part and it has to go: a stale red/green picture of a part that has
    // moved on is worse than no picture at all.
    ui::CompareVersionGeometry(*s_App, version, "parts/Part 1");
    ctx->Yield(2);
    IM_CHECK(s_App->HasGeometryDelta());
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 7.0f);
    ctx->Yield(3);
    IM_CHECK(!s_App->HasGeometryDelta());
    IM_CHECK_EQ(s_App->deltaExtraInstance, 0u);
  };

  // Clicking a diff row goes and finds the thing. The case that earns it is a
  // change in a document that is NOT the open tab — on a 48-part project,
  // "Extrude \"Boss\" in part Stand" is otherwise a sentence you have to act
  // on yourself.
  t = IM_REGISTER_TEST(engine, "history", "clicking_a_change_selects_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_reveal.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);

    // Change the FIRST part, then go and stand on the second one.
    const auto slotOf = [](const char* name) -> size_t {
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].name == name)
          return i;
      return 0;
    };
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    const uint64_t changedId = Doc()->GetFeature(0)->GetId();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 7.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::SetActivePart(*s_App, slotOf("Second"));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->activePart, (int)slotOf("Second"));

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("**/###hist_row0");
    ctx->Yield(3);
    // project.json changes too, because the active tab is stored in it —
    // so assert the row that matters rather than the count.
    int boxRow = -1;
    for (int r = 0; r < (int)s_App->diffChanges.size(); r++)
      if (s_App->diffChanges[r].itemKind == "Box")
        boxRow = r;
    IM_CHECK(boxRow >= 0);
    IM_CHECK(s_App->diffChanges[boxRow].summary.find("width 2 to 7") !=
             std::string::npos);

    // The click: it switches tab AND selects, so the tree row highlights.
    ctx->ItemClick(("**/###diff_row" + std::to_string(boxRow)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->activePart, (int)slotOf("Part 1"));
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), changedId);

    if (ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel)) {
      ctx->ItemClick(model->TabId);
      ctx->Yield(2);
    }
  };

  // Conflicts (F7): two lines of work that changed the same thing, brought
  // together through the panel. The whole point is that the row reads as a
  // FEATURE — "Box 'Box 1' ... width 5 vs 9" — and not as a path.
  t = IM_REGISTER_TEST(engine, "conflicts", "resolve_a_merge_in_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_conflicts_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK(ui::ProjectHasHistory(*s_App));

    // A second line of work, started the way a user does: the History
    // panel's own controls. Without a verb like this the app could not
    // produce a conflict at all.
    ImGuiWindow* history = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(history != nullptr && history->DockNode != nullptr);
    ctx->ItemClick(history->TabId);
    ctx->Yield(2);
    // Whatever git called the first branch — "master" on older installs,
    // "main" on newer ones. Never assume it.
    const std::string firstLine = GitVault::CurrentBranch(s_App->projectPath);
    IM_CHECK(!firstLine.empty());

    ctx->SetRef("//###History");
    ctx->ItemOpen("Branches");
    ctx->Yield();
    // A TreeNode pushes an id scope, so its contents are addressed THROUGH
    // it — "//###History/###hist_branch" finds nothing.
    ctx->ItemClick("Branches/###hist_branch");
    ctx->KeyCharsReplace("alt");
    ctx->ItemClick("Branches/Create###hist_branch_go");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(GitVault::CurrentBranch(s_App->projectPath).c_str(), "alt");

    // The History panel shares its dock node with Model, so Model is BEHIND
    // it now — the feature tree has to be brought forward before its rows can
    // be clicked, exactly as a user would.
    const auto showModel = [&ctx]() {
      ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
      IM_CHECK(model != nullptr && model->DockNode != nullptr);
      ctx->ItemClick(model->TabId);
      ctx->Yield(2);
    };

    // Their width, saved on the branch.
    showModel();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 9.0f);
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Back to the first line, and a different width there.
    IM_CHECK(ui::SwitchToBranch(*s_App, firstLine, &err));
    ctx->Yield(2);
    showModel();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 5.0f);
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Bring the branch in — a conflict, which is an outcome and not a
    // failure.
    IM_CHECK(ui::MergeBranch(*s_App, "alt", &err));
    ui::InvalidateConflicts(*s_App);
    ctx->Yield(3);

    // The panel exists ONLY now: a permanent tab saying "no conflicts" every
    // day would be noise, so its appearance is the signal.
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelConflicts);
    IM_CHECK(panel != nullptr);
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 1);
    const ProjectMerge::Conflict& c = s_App->conflicts.conflicts[0];
    // The row is a feature, not a file.
    IM_CHECK_STR_EQ(c.itemKind.c_str(), "Box");
    IM_CHECK(c.summary.find("width") != std::string::npos);
    IM_CHECK(c.summary.find("5 vs 9") != std::string::npos);

    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###Conflicts");

    // Finishing while something is undecided must do nothing: a half-resolved
    // tree committed as if it were finished is the silent loss F7 exists to
    // stop.
    ctx->ItemClick("Finish###cf_finish");
    ctx->Yield(2);
    IM_CHECK(GitVault::InMerge(s_App->projectPath));
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 1);

    ctx->ItemClick("Keep theirs###cf_theirs0");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 0);

    ctx->ItemClick("Finish###cf_finish");
    ctx->Yield(3);

    // Concluded: the merge is committed, the project reopened, and the panel
    // is gone because there is nothing left to say.
    IM_CHECK(!GitVault::InMerge(s_App->projectPath));
    IM_CHECK(s_App->conflicts.Empty());
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 9.0); // theirs, as chosen
  };

  // The bar is "the resolved project loads and regenerates", never "git
  // exited 0". A resolution can be perfectly valid JSON and still leave the
  // document broken.
  t = IM_REGISTER_TEST(engine, "conflicts", "finish_refuses_a_broken_project");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_conflicts_bad.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Break the saved project the way a bad resolution does: a file that is
    // valid on disk but not loadable.
    for (const auto& entry :
         std::filesystem::directory_iterator(dir / "features")) {
      std::ofstream(entry.path(), std::ios::trunc)
          << "<<<<<<< ours\n{}\n=======\n{}\n>>>>>>> theirs\n";
      break;
    }

    s_App->conflictsBreakage.clear();
    IM_CHECK(!ui::FinishMerge(*s_App, &err));
    // It names the document and what broke, rather than leaving a project
    // that opens to an error tomorrow.
    IM_CHECK_EQ(s_App->conflictsBreakage.size(), (size_t)1);
    IM_CHECK(s_App->conflictsBreakage[0].reason.find("conflict markers") !=
             std::string::npos);
  };

  // Version history (F6): the panel is the one home for versions, it reads
  // the whole PROJECT (so it works from any tab kind), naming a state adds a
  // version, and restoring is refused while there are unsaved changes.
  t = IM_REGISTER_TEST(engine, "history", "name_and_restore_a_version");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_history_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    // Before: Model owns the shared node. Without this the check below is a
    // false positive — the default layout docks History last, and the last
    // window docked is the one the node selects.
    //
    // SELECTED, not merely asserted: this used to read the incoming state and
    // check it, which held only because no earlier test had left another tab
    // of this node in front. The disk/* cases do exactly that, and the
    // precondition then failed in whichever shard ran them first.
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(model != nullptr && model->DockNode != nullptr);
    ctx->ItemClick(model->TabId);
    ctx->Yield(2);
    IM_CHECK_EQ(model->DockNode->SelectedTabId, model->TabId);

    // Reach the panel the way a user does: click its dock tab. (A tab item's
    // id IS the window's TabId, so it addresses directly — ImGui::FocusWindow
    // deliberately does not select dock tabs, so neither a menu item nor the
    // test engine's WindowFocus can stand in for the click.)
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    IM_CHECK_EQ(panel->DockNode->SelectedTabId, panel->TabId);

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    // The panel caches the log (reading it spawns `git`) and refreshes on a
    // ~1 s timer; a test's frames are far quicker than that, so a save made
    // around the panel rather than through it has to invalidate the cache.
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK(ui::ProjectHasHistory(*s_App));

    // Name the state we just saved. It is unchanged, so this only works
    // because a name gets a version of its own.
    ctx->SetRef("//###History");
    ctx->ItemClick("###hist_name");
    ctx->KeyCharsReplace("before the lid");
    ctx->ItemClick("###hist_name_go");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->historyLog.size(), (size_t)2);
    IM_CHECK_EQ(s_App->historyLog[0].subject, std::string("before the lid"));
    IM_CHECK(s_App->historyLog[0].named);
    IM_CHECK(!s_App->historyLog[1].named); // the plain save under it

    // A second part, saved: the version above predates it.
    ui::AddPart(*s_App, "Lid");
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK(s_App->historyLog.size() >= 3);

    // Unsaved changes: restoring would discard them, so it is refused.
    ui::AddPart(*s_App, "Scratch");
    ctx->Yield(2);
    IM_CHECK(ui::ProjectDirty(*s_App));
    IM_CHECK((ctx->ItemInfo("//###History/**/###hist_restore1").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);
    ctx->ItemClick("//###History/###hist_save");
    ctx->Yield(2);
    IM_CHECK(!ui::ProjectDirty(*s_App));

    // Restore the named version: the parts added after it are gone, and the
    // restore is itself a version (nothing is lost).
    const size_t before = s_App->historyLog.size();
    int named = -1;
    for (int i = 0; i < static_cast<int>(s_App->historyLog.size()); i++)
      if (s_App->historyLog[i].named)
        named = i;
    IM_CHECK(named >= 0);
    ctx->ItemClick(
        ("//###History/**/###hist_restore" + std::to_string(named)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->parts.size(), (size_t)1);
    IM_CHECK(s_App->historyLog.size() > before);

    std::filesystem::remove_all(dir);
  };

  // The panel's rows must stay INSIDE the panel. The first version reserved
  // room for its buttons with pixel literals (70 for "Name", 58 for the
  // Restore column); the style scales with the display's content scale and
  // the font-size setting, so on a HiDPI screen those literals were about
  // half a button short — the buttons hung off the right edge and the panel
  // scrolled sideways. The trigger is FONT SIZE, not window width, so that
  // is what this drives.
  t = IM_REGISTER_TEST(engine, "history", "rows_fit_at_a_large_font");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_history_fit.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;

    // Font size is a PREFERENCE now (Edit > Preferences); the View > Font Size
    // submenu this used to click is gone. Set it directly — driving the
    // Preferences window would be testing that window, not these widths.
    s_App->settings.uiFontScale = 1.4f;
    ui::MarkSettingsChanged(*s_App);
    ctx->Yield(4); // the atlas rebuild happens between frames

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId); // the tab, as a user would

    // Wait for the state the assertions need rather than guessing a frame
    // count — the row list is only read once the panel is actually up, and a
    // fixed Yield(4) passed alone while failing in the full suite.
    for (int i = 0; i < 90; i++) {
      if (panel->DockNode->SelectedTabId == panel->TabId &&
          !s_App->historyLog.empty())
        break;
      ctx->Yield();
    }
    IM_CHECK(!s_App->historyLog.empty());
    const float rightEdge = panel->InnerRect.Max.x;
    // 1 px of slack: rounding, not a missing button.
    IM_CHECK_LE(ctx->ItemInfo("//###History/###hist_name_go").RectFull.Max.x,
                rightEdge + 1.0f);
    IM_CHECK_LE(
        ctx->ItemInfo("//###History/**/###hist_restore0").RectFull.Max.x,
        rightEdge + 1.0f);
    // And nothing else pushed the content wider than the panel, which is
    // what the sideways scroll was.
    IM_CHECK_LE(panel->ContentSize.x, panel->InnerRect.GetWidth() + 1.0f);

    // Put the font back. ResetAppForTest restores the preference for the next
    // test, but the ATLAS is rebuilt by the main loop, so leaving it large
    // would follow this test into the next one.
    s_App->settings.uiFontScale = 1.0f;
    ui::MarkSettingsChanged(*s_App);
    ctx->Yield(4);
    std::filesystem::remove_all(dir);
  };

  // Preferences (docs/ui/settings.md): one window over one Settings struct,
  // every control addressable, and a change reaching the renderer's own
  // fields — the "it reverted when I opened a second project" class of bug
  // starts with two copies of a preference.
  t = IM_REGISTER_TEST(engine, "settings", "a_change_reaches_the_renderer");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/###edit_preferences");
    ctx->Yield();
    IM_CHECK(s_App->settingsWindowOpen);

    // Shadows are on by default and the renderer agrees.
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows);

    ctx->ItemClick("//###SettingsWindow/###set_shadows");
    ctx->Yield(2);
    IM_CHECK(!s_App->settings.shadows);
    IM_CHECK(!s_App->showShadows); // ApplySettings pushed it through

    // Back on, from the same control: the View menu's duplicate is gone —
    // render quality was session state there, resetting every launch, and two
    // controls over one preference is how the ends drift apart.
    ctx->ItemClick("//###SettingsWindow/###set_shadows");
    ctx->Yield(2);
    IM_CHECK(s_App->settings.shadows);
    IM_CHECK(s_App->showShadows);
    // And the View menu really has no duplicate left.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuAction(ImGuiTestAction_Open, "View");
    ctx->Yield();
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Shadows"));
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Render Quality"));
    IM_CHECK(!ctx->ItemExists("//$FOCUSED/Font Size"));
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();

    // An enum row is a combo, and its id is stable.
    ctx->ItemClick("//###SettingsWindow/###set_ssao");
    ctx->ItemClick("//$FOCUSED/full");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settings.ssao, std::string("full"));
    IM_CHECK(s_App->showAmbientOcclusion);
    IM_CHECK(s_App->ssaoFullRes);

    // Reset puts everything back, so a broken config never needs the JSON.
    ctx->ItemClick("//###SettingsWindow/###set_reset_all");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settings.ssao, std::string("half"));
    IM_CHECK(s_App->settings.shadows);
  };

  // A restart-tier row must SAY so rather than pretending to apply: the
  // swapchain and every renderer are built once, at startup.
  t = IM_REGISTER_TEST(engine, "settings", "a_restart_row_says_restart");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->settingsWindowOpen = true;
    ctx->Yield(2);
    bool sawGpuRow = false;
    bool saidRestart = false;
    for (const settings::Field& f : settings::Fields()) {
      if (std::string(f.key) != "gpu_device")
        continue;
      sawGpuRow = true;
      // The TIER is the contract: the window turns it into the "(needs a
      // restart)" note. The blurb deliberately does NOT repeat the word —
      // that read as "...the viewport needs a restart. (needs a restart)".
      saidRestart = f.tier == settings::Tier::Restart;
      // It does carry the asymmetry the note cannot: simulation builds its
      // device per run, so the same setting lands immediately there.
      IM_CHECK(strstr(f.blurb, "next run") != nullptr);
    }
    IM_CHECK(sawGpuRow);
    IM_CHECK(saidRestart);
    IM_CHECK(ctx->ItemExists("//###SettingsWindow/###set_gpu_device"));
  };

  // Viewport handles (docs/ui/viewport-handles.md): the three owners publish
  // into one list, so hover can say WHICH arrow is under the cursor — before
  // this, the hit test ran only on click and nothing knew until you pressed.
  t = IM_REGISTER_TEST(engine, "handles", "hover_highlights_the_arrow");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A pending FILLET has the direct-manipulation handle (its radius); a
    // pending box does not — ComputeHandleSpec only serves features with a
    // headline scalar (extrude distance, blend radius, revolve angle).
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.edgeNames.empty());
    const ElementRef ref{body.createdBy,
                         body.elementMap.edgeNames.begin()->second};
    ui::BeginPreviewFeature(*s_App,
                            std::make_unique<FilletFeature>(
                                "Fillet", std::vector<ElementRef>{ref}, 0.4));
    ctx->Yield(2);
    IM_CHECK(ui::HasViewportHandle(*s_App));
    IM_CHECK(!s_App->viewportHandles.empty());
    const ui::ViewportHandle h = s_App->viewportHandles[0];
    IM_CHECK_EQ(ui::OwnerOf(h.id), ui::HandleOwner::Feature);

    // Nothing under the cursor to start with (the mouse is not on the arrow).
    ctx->MouseMoveToPos(WorldToWindow(h.origin - h.dir * (h.length * 6.0f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, (uint64_t)0);

    // On the shaft: hovered, and it is THIS handle, not merely "something".
    ctx->MouseMoveToPos(WorldToWindow(h.origin + h.dir * (h.length * 0.6f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, h.id);

    // Moving off clears it — a highlight that never turns off is worse than
    // none, because it stops meaning "you can grab this".
    ctx->MouseMoveToPos(WorldToWindow(h.origin - h.dir * (h.length * 6.0f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, (uint64_t)0);
    ui::CancelPendingFeature(*s_App);
  };

  // Per-axis, which is the whole reason the shared handle carries its own
  // colour: hovering the gizmo's X arrow must identify X.
  t = IM_REGISTER_TEST(engine, "handles", "gizmo_hover_names_the_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::AddAssembly(*s_App, "Rig");
    ctx->Yield();
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    ui::SelectOccurrence(*s_App, s_App->assembly->GetOccurrences()[0].id);
    ctx->Yield(2);

    // Three translate arrows (and their arcs) are published, one per axis.
    IM_CHECK(s_App->viewportHandles.size() >= 3);
    int axesSeen = 0;
    for (const ui::ViewportHandle& h : s_App->viewportHandles)
      if (ui::OwnerOf(h.id) == ui::HandleOwner::Gizmo &&
          h.kind == ui::HandleKind::Linear)
        axesSeen++;
    IM_CHECK_EQ(axesSeen, 3);

    for (const ui::ViewportHandle& h : s_App->viewportHandles) {
      if (ui::OwnerOf(h.id) != ui::HandleOwner::Gizmo ||
          h.kind != ui::HandleKind::Linear)
        continue;
      ctx->MouseMoveToPos(WorldToWindow(h.origin + h.dir * (h.length * 0.6f)));
      ctx->Yield(2);
      // The axis under the cursor is the one reported — not the first one.
      IM_CHECK_EQ(ui::GizmoAxisOf(ui::SubOf(s_App->hoveredHandle)),
                  ui::GizmoAxisOf(ui::SubOf(h.id)));
    }

    // And the highlight reaches the DRAWN output, not just the state: the
    // same overlay, built with and without a hover, must differ — and only
    // in the hovered axis's own lines.
    const uint64_t xId =
        ui::HandleId(ui::HandleOwner::Gizmo, ui::GizmoSub(0, /*rotate=*/false));
    const auto overlayWith = [&](uint64_t hovered) {
      s_App->hoveredHandle = hovered;
      std::vector<LineVertex3D> lines;
      s_App->viewportHandles.clear(); // rebuilt by the call below
      ui::BuildGizmoOverlay(*s_App, lines);
      return lines;
    };
    const std::vector<LineVertex3D> plain = overlayWith(0);
    const std::vector<LineVertex3D> hoveredX = overlayWith(xId);
    IM_CHECK_EQ(plain.size(), hoveredX.size()); // geometry is untouched
    int brighter = 0;
    for (size_t i = 0; i < plain.size(); i++)
      if (plain[i].color != hoveredX[i].color) {
        brighter++;
        // Brighter, and still recognisably the axis's own colour: X stays
        // red rather than turning into one shared accent.
        IM_CHECK(hoveredX[i].color.r > plain[i].color.r);
        IM_CHECK(hoveredX[i].color.r > hoveredX[i].color.g);
      }
    IM_CHECK(brighter > 0);
  };

  // Reported: clicking a POINT in a sketch highlighted the point and one of
  // the lines next to it. A body pick and a vertex pick both stored {id, 0},
  // so the drawing code — which could only compare the entity — lit the whole
  // line whenever one of its vertices was selected.
  t = IM_REGISTER_TEST(engine, "sketch",
                       "picking_a_point_does_not_light_its_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    size_t index = 0;
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App, &index);
    IM_CHECK(sk != nullptr);
    // A rectangle: its corners are shared by two lines, which is where the
    // report came from.
    const auto ids = sk->AddRectangle({0.0f, 0.0f}, {3.0f, 2.0f});
    IM_CHECK(ids.size() >= 4);
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    ctx->Yield(2);

    // params.y is the primitive's shape: 0 = line, 1 = dot. Counting them
    // separately is the report in one assertion — "it highlights both the
    // point and one of the lines next to it".
    const glm::vec4 sel(1.00f, 0.62f, 0.20f, 1.0f);
    const auto litCounts = [&]() {
      const std::vector<PrimVertex> prims = ui::BuildActiveSketchPrimitives(
          *s_App, s_App->sketchEdit.activeSketchId);
      int lines = 0, dots = 0;
      for (const PrimVertex& v : prims) {
        if (glm::length(v.color - sel) > 0.01f)
          continue;
        (v.params.y > 0.5f ? dots : lines)++;
      }
      return std::pair<int, int>(lines, dots);
    };
    IM_CHECK_EQ(litCounts().first, 0); // nothing selected yet
    IM_CHECK_EQ(litCounts().second, 0);

    // The LINE BODY selected: its shaft is lit.
    s_App->sketchEdit.selected = {{ids[0], sketch::kWholeEntity}};
    ctx->Yield();
    IM_CHECK(litCounts().first > 0);

    // One of that line's ENDPOINTS instead: the dot lights, the line does
    // NOT. That second half is the bug.
    s_App->sketchEdit.selected = {{ids[0], 1}};
    ctx->Yield();
    IM_CHECK(litCounts().second > 0);  // the point itself is highlighted
    IM_CHECK_EQ(litCounts().first, 0); // and no line is
    s_App->sketchEdit.selected.clear();
  };

  // Wrap every registered test so ResetAppForTest() runs before it — this is
  // what makes the suite order-independent. Done once here rather than per
  // test.
  ImVector<ImGuiTest*> allTests;
  ImGuiTestEngine_GetTestList(engine, &allTests);
  for (ImGuiTest* test : allTests) {
    if (!test->TestFunc)
      continue;
    g_origTestFunc[test] = test->TestFunc;
    test->TestFunc = &RunTestWithReset;
  }
}
