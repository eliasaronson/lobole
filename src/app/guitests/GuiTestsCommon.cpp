#include "app/guitests/GuiTestsCommon.h"

// Tests run inside the live application and reach it through the AppState.
ui::AppState* s_App = nullptr;

Document* Doc() { return s_App->document.get(); }

// Between-test reset. The ImGui Test Engine runs every test back to back in one
// process, so state a test leaves behind leaks into the next one — most harmful
// is the camera zoom: a viewport-click test that passes alone fails mid-suite
// when a predecessor left the camera zoomed in and its UV click coordinates
// then project off-screen. Restoring the app's initial interaction state before
// each test makes full-suite runs behave like isolated ones. Registered as a
// TestFunc wrapper below, so individual tests don't have to opt in.
void ResetAppForTest() {
  if (s_App->camera) {
    s_App->camera->StopAnimation();
    s_App->camera->SetPosition(
        glm::vec3(8.0f, 6.0f, 8.0f)); // main.cpp defaults
    s_App->camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
  }
  s_App->showShortcuts = false; // a stray window eats later viewport clicks
  // An armed key capture would swallow every key press of the next test, and
  // it is session state rather than a preference, so the settings reset below
  // does not cover it.
  s_App->keyCaptureId.clear();
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
  // A pending bring-this-tab-forward request (EnterSimulation, focus_panel)
  // outlives a short test and then fights the NEXT test's item clicks: the
  // engine locates "**/" items by focusing windows, and a competing
  // SetWindowFocus every frame can make a click land on nothing — silently.
  s_App->focusPanel.clear();
  s_App->focusPanelFrames = 0;
  // A force-enabled idle pacer left armed makes every later frame a 100 ms
  // event-wait — the whole suite crawls if the pacer test aborts mid-way.
  s_App->pacerForceEnable = false;
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
std::unordered_map<ImGuiTest*, void (*)(ImGuiTestContext*)> g_origTestFunc;

void RunTestWithReset(ImGuiTestContext* ctx) {
  ResetAppForTest();
  const auto it = g_origTestFunc.find(ctx->Test);
  if (it != g_origTestFunc.end() && it->second)
    it->second(ctx);
}

// Window position of a world point, mapped through the central-node viewport
// rect (where the 3D scene actually renders), matching the picking math.
float LensScale() {
  return std::tan(glm::radians(22.5f)) /
         std::tan(glm::radians(0.5f * s_App->camera->GetFOV()));
}

ImVec2 WorldToWindow(const glm::vec3& p) {
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
void ToolbarClick(ImGuiTestContext* ctx, const char* tbId) {
  ctx->SetRef("//##DockHost");
  ctx->ItemClick((std::string("**/") + tbId).c_str());
  ctx->Yield();
  // Leave the ref on the menu bar, matching the old Create-menu helper: several
  // callers follow up with MenuClick("Edit/Undo") etc. and rely on that ref.
  ctx->SetRef("//##MainMenuBar");
}

// Most tests want a committed feature: create from the toolbar, then accept the
// preview (exercises the real accept path).
void ToolbarCreateAccept(ImGuiTestContext* ctx, const char* tbId) {
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
void BuildCantileverStudy(ImGuiTestContext* ctx, double height) {
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
//
// A FAILED run is not a hang and must not be waited out. Both prepare paths
// (PrepareStudy / PrepareTopology) clear study.error synchronously before
// their worker exists, and the poll paths set it only on a terminal failure
// — so a non-empty error inside this loop is a finished, failed run, and
// there is nothing left to wait for. Without this, a build without Netgen
// (CI: run_study errors in the first frame) spun the full 6000 frames on
// software Vulkan, turning an instant failure into ~8 minutes per test and
// reporting it as the engine's 300 s watchdog abort rather than as the
// missing mesher.
bool WaitForStudyResult(ImGuiTestContext* ctx, uint64_t studyId) {
  for (int i = 0; i < 6000 && !Doc()->StudyResults(studyId); ++i) {
    const fem::Study* s = Doc()->FindStudy(studyId);
    if (s && !s->error.empty())
      break;
    ctx->Yield();
  }
  if (Doc()->StudyResults(studyId))
    return true;
  // An IM_CHECK here would return from THIS function and let the test walk on
  // into a null result, which is a segfault rather than a failure report.
  const fem::Study* st = Doc()->FindStudy(studyId);
  ctx->LogError("no result for study %llu: %s", (unsigned long long)studyId,
                st && !st->error.empty() ? st->error.c_str() : "(no error)");
  return false;
}

const ToolCase kToolCases[24] = {
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

const ToolCase* FindToolCase(const char* testName) {
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
void EnterHoleSketchOnPlane(ImGuiTestContext* ctx, int planeIdx) {
  ui::BeginSketchOnPlane(*s_App, ui::DefaultPlane(planeIdx), "Hole points",
                         ui::SketchPurpose::Hole);
  ctx->Yield();
  if (s_App->camera)
    s_App->camera->FinishAnimation();
  ctx->Yield();
}

void EnterSketchOnPlane(ImGuiTestContext* ctx, int planeIdx) {
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
void AddRectSketch(ImGuiTestContext* ctx, int planeIdx, float half,
                   float centreU) {
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

void AddPathSketch(ImGuiTestContext* ctx, int planeIdx) {
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

void SetupFor(ImGuiTestContext* ctx, Needs needs) {
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
