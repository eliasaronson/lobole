#include "app/guitests/GuiTestsCommon.h"

#include "core/SheetMetal.h"
#include "core/features/CornerReliefFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/UnfoldFeature.h"

#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/JobActions.h"
#include "ui/OpenJob.h"
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h" // SelectFeature (the fixtures)
#include "ui/ViewActions.h"

#include <chrono>

#if defined(__linux__)
#include <pthread.h>
#include <time.h>
#endif

// Tests run inside the live application and reach it through the AppState.
ui::AppState* s_App = nullptr;

Document* Doc() { return s_App->document.get(); }

cam::JobDocument* Job() { return ui::ActiveJob(*s_App); }

cam::JobDocument* MakeJob() {
  if (s_App->job)
    return s_App->job.get();
  if (s_App->parts.empty())
    return nullptr;
  ui::CreateJobForPart(*s_App, s_App->parts[s_App->activePart].id);
  return s_App->job.get();
}

Document* CamPart() {
  if (s_App->document)
    return s_App->document.get();
  if (const cam::JobDocument* job = ui::ActiveJob(*s_App))
    for (const cam::JobPart& part : job->GetParts())
      if (Document* d = ui::JobSourceDocument(*s_App, part.sourceDocId))
        return d;
  return nullptr;
}

// Between-test reset. The ImGui Test Engine runs every test back to back in one
// process, so state a test leaves behind leaks into the next one — most harmful
// is the camera zoom: a viewport-click test that passes alone fails mid-suite
// when a predecessor left the camera zoomed in and its UV click coordinates
// then project off-screen. Restoring the app's initial interaction state before
// each test makes full-suite runs behave like isolated ones. Registered as a
// TestFunc wrapper below, so individual tests don't have to opt in.
void ResetAppForTest() {
  // FIRST, before the camera: a CAM test leaves a JOB tab behind (v73: setups
  // live on a job, not on the part), and the next test then opens on it,
  // where `document` is null. Dropping them switches tabs, and a tab switch
  // can FIT the camera — so doing it after the reset below left the next
  // test's screen coordinates pointing somewhere else, which is how a sketch
  // trim two tests later failed with a perfectly good preview of the wrong
  // span.
  for (size_t i = s_App->parts.size(); i-- > 0;)
    if (s_App->parts[i].kind == ui::DocKind::Job)
      ui::DeletePart(*s_App, i);
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
  // Project state no document owns: a predecessor's requirements would put
  // the Requirements panel in the left dock in front of the next test's.
  s_App->requirements.clear();
  s_App->requirementsEdits++;
  // Every other floating tool window, for the same reason: one left open by
  // a test that stopped early covers the viewport, and the next test's clicks
  // land on it (docs/ui/testing.md#independence-any-partition-is-valid).
  s_App->simulationToolOpen = false;
  s_App->bomWindowOpen = false;
  s_App->camGenerateOpen = false;
  s_App->camPlaybackOpen = false;
  s_App->drawingAddViewOpen = false;
  s_App->paletteOpen = false;
  s_App->asmEditOccurrence = 0;
  s_App->asmEditRelation = 0;
  // A pending bring-this-tab-forward request (EnterSimulation, focus_panel)
  // outlives a short test and then fights the NEXT test's item clicks: the
  // engine locates "**/" items by focusing windows, and a competing
  // SetWindowFocus every frame can make a click land on nothing — silently.
  s_App->focusPanel.clear();
  s_App->focusPanelFrames = 0;
  // A force-enabled idle pacer left armed makes every later frame a 100 ms
  // event-wait — the whole suite crawls if the pacer test aborts mid-way.
  s_App->pacerForceEnable = false;
  // Toolpaths generate INLINE in this suite: thirty tests read
  // `camToolpaths` right after a pick, and the real (deferred) path is the
  // subject of exactly one of them, `cam/adaptive_clears_off_the_frame`,
  // which turns it back on for itself
  // (docs/ui/long-operations.md#cam-toolpath-passes).
  s_App->camDeferPasses = false;
  s_App->backgroundJobs.clear(); // joins whatever a previous test left
  s_App->camAdaptiveQueue.clear();
#ifdef HAVE_CAM
  s_App->camAdaptiveCache.Clear();
#endif
  s_App->camAdaptiveInFlight = 0;
  s_App->camPassBatchDone = 0;
  s_App->camPassBatchTotal = 0;
  s_App->camVerifyWhenSettled = false;
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
  s_App->drawingNoteTool = {};
  s_App->detailTool = {};
  s_App->drawingMessage.clear();
  s_App->inContext = {};
  s_App->showCog = false;
  s_App->perfHudOpen = false;
  // CAM is a tool session plus a pile of cached results, none of which the
  // app clears on its own (a job outlives the window that made it, by
  // design). Its overlays draw over the viewport the next test clicks in.
  s_App->camToolOpen = false;
  // The assembly's viewport modes are tool windows over the viewport the next
  // test clicks in — the same leak as the CAM one below, and it showed up the
  // same way: a rim pick and a section-arrow drag that hit a window instead.
  s_App->explodeToolOpen = false;
  s_App->motionToolOpen = false;
  s_App->camVerb = ui::AppState::CamVerb::None;
  s_App->camVerbOp = 0;
  s_App->camVerbTool = 0;
  s_App->camVerbFinishTool = 0;
  s_App->camGhostOp = 0;
  s_App->camActiveSetup = 0;
  s_App->camRefusal.clear();
  s_App->camNote.clear();
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
  // Unconditional on purpose
  // (docs/ui/testing.md#independence-any-partition-is-valid): tracking whether
  // a test "touched the layout" would have read clean in exactly the case that
  // mattered, because a window docking itself is not an action any test
  // performs.
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
  ui::ResetJobEditAudit();
  const auto it = g_origTestFunc.find(ctx->Test);
  if (it != g_origTestFunc.end() && it->second)
    it->second(ctx);
  // Every test is also an audit of the job-undo rule (DocumentSlots.h): an
  // edit to a job that did not call MarkJobEdited would not undo, and the
  // guards counted it while this test ran.
  if (ui::JobEditAuditMisses() > 0)
    ctx->LogError("a job edit did not call MarkJobEdited: %s",
                  ui::JobEditAuditLastMiss().c_str());
  IM_CHECK_EQ(ui::JobEditAuditMisses(), 0);
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
  // The sheet-metal tools live in a flyout group: whichever was used last is
  // on the bar and the rest are behind its caret. Every one still answers to
  // its own "###tb_*" id — this opens the group when the one asked for is
  // not the one showing, so a caller names the TOOL, not where it sits.
  if (ctx->ItemInfo((std::string("**/") + tbId).c_str(),
                    ImGuiTestOpFlags_NoError)
          .ID == 0) {
    ctx->ItemClick("**/sheet/###more");
    ctx->Yield();
  }
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

namespace {
// The old toolbar's placement ladder and names, kept for the fixture.
glm::vec3 FixtureSpot() {
  const int n = static_cast<int>(Doc()->FeatureCount());
  return glm::vec3(-4.0f + n * 2.5f, 0.0f, 0.0f);
}
std::string FixtureName(const char* base) {
  return std::string(base) + " " + std::to_string(Doc()->FeatureCount() + 1);
}
void AddFixture(ImGuiTestContext* ctx, std::unique_ptr<Feature> f) {
  s_App->commands->Execute(std::make_unique<AddFeatureCommand>(std::move(f)),
                           *Doc());
  ui::SelectFeature(*s_App,
                    Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetId());
  ctx->Yield(2); // the frames the toolbar's click + accept used to take
}
} // namespace

void AddFixtureBox(ImGuiTestContext* ctx) {
  AddFixture(ctx, std::make_unique<BoxFeature>(FixtureName("Box"),
                                               FixtureSpot(), 2.0, 2.0, 2.0));
}

void AddFixtureSphere(ImGuiTestContext* ctx) {
  AddFixture(ctx, std::make_unique<SphereFeature>(FixtureName("Sphere"),
                                                  FixtureSpot(), 1.0));
}

void AddFixtureCylinder(ImGuiTestContext* ctx) {
  AddFixture(ctx, std::make_unique<CylinderFeature>(
                      FixtureName("Cylinder"), FixtureSpot() - conv::kWorldUpF,
                      conv::kWorldUpF, 0.8, 2.0));
}

void PrimitiveOnBasePlane(ImGuiTestContext* ctx, const char* tbId,
                          int planeIdx) {
  ToolbarClick(ctx, tbId);
  ctx->Yield();
  IM_CHECK_SILENT(s_App->sketchEdit.pickingPlane);
  ctx->ItemClick(
      (std::string("//##DockHost/**/") +
       SketchPlane::StandardPlaneName(static_cast<PlaneType>(planeIdx)))
          .c_str());
  ctx->Yield(2);
  ctx->SetRef("//##MainMenuBar");
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
  AddFixtureBox(ctx);
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
      ("//###ToolWindow/###material_" + std::to_string(createdBy)).c_str());
  ctx->Yield();
  IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
  ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
  IM_CHECK_SILENT(popup != nullptr);
  ctx->ItemClick((std::string("//") + popup->Name + "/**/Steel").c_str());
  ctx->Yield(2);

  glm::dvec3 lo(0.0), hi(0.0);
  Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
  const glm::vec3 mid = (lo + hi) * 0.5;
  s_App->camera->StopAnimation();
  s_App->camera->SetTarget(mid);
  s_App->camera->SetUp(glm::vec3(0, 1, 0));

  // Fixed on the x=0 end face, looking straight at it.
  s_App->camera->SetPosition(mid - glm::vec3(400.0f, 0.0f, 0.0f));
  ctx->Yield(3);
  ctx->ItemClick("//###ToolWindow/###sim_bc_fixed");
  ctx->Yield();
  ctx->MouseMoveToPos(WorldToWindow(glm::vec3(lo.x, mid.y, mid.z)));
  ctx->Yield();
  ctx->MouseClick(ImGuiMouseButton_Left);
  ctx->Yield(2);

  // 100 N spread over the top face, from above.
  ctx->ItemClick("//###ToolWindow/###sim_bc_force");
  ctx->Yield();
  ctx->ItemInputValue("//###ToolWindow/###sim_magnitude", "100");
  ctx->Yield();
  s_App->camera->SetUp(glm::vec3(0, 0, -1));
  s_App->camera->SetPosition(mid + glm::vec3(0.0f, 400.0f, 0.0f));
  ctx->Yield(3);
  ctx->MouseMoveToPos(WorldToWindow(glm::vec3(mid.x, hi.y, mid.z)));
  ctx->Yield();
  ctx->MouseClick(ImGuiMouseButton_Left);
  ctx->Yield(2);

  const fem::Study* st = Doc()->FindStudy(s_App->simulation.activeStudyId);
  IM_CHECK_SILENT(st != nullptr);
  IM_CHECK_EQ(st->loads.size(), (size_t)2);
  // The default direction is into the picked face: -Y, from the top.
  const fem::Load& force = st->loads[1];
  IM_CHECK(force.type == fem::LoadType::Force);
  IM_CHECK(std::abs(force.direction.y + 1.0) < 1e-6);
  IM_CHECK(std::abs(force.magnitude - 100.0) < 1e-9);
}

bool WaitFor(ImGuiTestContext* ctx, const char* what,
             const std::function<bool()>& done, double seconds) {
  const auto t0 = std::chrono::steady_clock::now();
  int frames = 0;
  while (!done()) {
    const double waited =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (ctx->IsError())
      return false;
    if (waited > seconds) {
      IM_ERRORF("waited %.0f s (%d frames) for %s", waited, frames, what);
      return false;
    }
    ctx->Yield();
    frames++;
  }
  return true;
}

bool WaitForJobs(ImGuiTestContext* ctx, const char* kind) {
  const std::string what = std::string("the '") + kind + "' jobs";
  return WaitFor(ctx, what.c_str(),
                 [kind] { return !ui::BackgroundJobRunning(*s_App, kind); });
}

#if defined(__linux__)
namespace {
pthread_t g_mainThread{};
bool g_haveMainThread = false;
} // namespace
void NoteMainThread() {
  g_mainThread = pthread_self();
  g_haveMainThread = true;
}
double MainThreadCpuMs() {
  clockid_t clock{};
  timespec ts{};
  if (g_haveMainThread && pthread_getcpuclockid(g_mainThread, &clock) == 0 &&
      clock_gettime(clock, &ts) == 0)
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#else
void NoteMainThread() {}
double MainThreadCpuMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif

bool WaitForOpen(ImGuiTestContext* ctx) {
  return WaitFor(ctx, "the project to open",
                 [] { return !ui::OpenRunning(*s_App); });
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
  WaitFor(
      ctx, "a study result",
      [studyId] {
        const fem::Study* s = Doc()->FindStudy(studyId);
        return Doc()->StudyResults(studyId) != nullptr ||
               (s != nullptr && !s->error.empty());
      },
      120.0);
  if (Doc()->StudyResults(studyId))
    return true;
  // An IM_CHECK here would return from THIS function and let the test walk on
  // into a null result, which is a segfault rather than a failure report.
  const fem::Study* st = Doc()->FindStudy(studyId);
  ctx->LogError("no result for study %llu: %s", (unsigned long long)studyId,
                st && !st->error.empty() ? st->error.c_str() : "(no error)");
  return false;
}

const ToolCase kToolCases[37] = {
    // The primitives stand on something: each arms a face/plane pick, and
    // the Top plane of an empty part is enough for its defaults.
    {"box", "###tb_box", Needs::Nothing, Expect::PickThenChanges},
    // A fresh project has one part and nothing to derive from: the window
    // says so and Accept stays disabled until a source exists and is picked.
    {"derive", "###tb_derive", Needs::Nothing, Expect::RefusesUntilPicked},
    {"sphere", "###tb_sphere", Needs::Nothing, Expect::PickThenChanges},
    {"cylinder", "###tb_cylinder", Needs::Nothing, Expect::PickThenChanges},
    {"curve3d", "###tb_curve3d", Needs::Nothing, Expect::Changes},
    {"datum_plane", "###tb_datum_plane", Needs::Nothing, Expect::Changes},
    {"datum_axis", "###tb_datum_axis", Needs::Nothing, Expect::Changes},
    {"frame", "###tb_frame", Needs::Nothing, Expect::Changes},
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
    {"worm", "###tb_worm", Needs::Body, Expect::RefusesUntilPicked},
    {"worm_wheel", "###tb_worm_wheel", Needs::Body, Expect::RefusesUntilPicked},
    {"rib", "###tb_rib", Needs::BodyAndOpenSketch, Expect::Changes},
    {"hole", "###tb_hole", Needs::Body, Expect::ArmsAPick},
    {"new_sketch", "###tb_new_sketch", Needs::Nothing, Expect::ArmsAPick},
    // Sheet metal. The base flange thickens the profile by the document's
    // gauge, so with stock declared it changes the model on its defaults.
    {"base_flange", "###tb_base_flange", Needs::SheetRules, Expect::Changes},
    // A flange has to be told WHICH edge: which of a rim's two edges is
    // picked is the whole of the direction control, so there is nothing
    // sensible to default to.
    {"edge_flange", "###tb_edge_flange", Needs::SheetBlank,
     Expect::RefusesUntilPicked},
    // A blank with no bends flattens to itself. No change is the honest
    // answer, and the feature says so in a warning.
    {"flat_pattern", "###tb_flat_pattern", Needs::SheetBlank,
     Expect::HarmlessNoOp},
    // Every bend of the newest body, by default: with a wall up, that lays
    // it down.
    {"unfold", "###tb_unfold", Needs::SheetFlanged, Expect::Changes},
    // The one Unfold still open is the only sensible thing to fold back.
    {"refold", "###tb_refold", Needs::SheetUnfolded, Expect::Changes},
    // A hem has to be told which edge, as a flange does.
    {"hem", "###tb_hem", Needs::SheetBlank, Expect::RefusesUntilPicked},
    // With no one-line sketch to step along, the jog says so and waits.
    {"jog", "###tb_jog", Needs::SheetBlank, Expect::RefusesUntilPicked},
    // Every corner of the newest body where two bends cross.
    {"corner_relief", "###tb_corner_relief", Needs::SheetCornered,
     Expect::Changes},
    // The newest closed sketch is the blank's own outline; a normal cut asks
    // which profile rather than cutting the whole part away.
    {"normal_cut", "###tb_normal_cut", Needs::SheetBlank,
     Expect::RefusesUntilPicked},
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
    AddFixtureBox(ctx);
    break;
  case Needs::TwoBodies:
    AddFixtureBox(ctx);
    AddFixtureBox(ctx);
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
  case Needs::SheetRules:
  case Needs::SheetBlank:
  case Needs::SheetFlanged:
  case Needs::SheetUnfolded:
  case Needs::SheetCornered: {
    // The stock, set straight on the document: declaring it is setup, not
    // something the tool under test is supposed to do for itself.
    sheet::SheetMetalRules rules;
    rules.material = "aluminum";
    rules.gauge = "1.5mm";
    rules.thickness = 1.5;
    Doc()->SetSheetMetal(rules);
    AddRectSketch(ctx, 1, 8.0f);
    if (needs == Needs::SheetRules)
      break;
    ToolbarCreateAccept(ctx, "###tb_base_flange");
    if (needs == Needs::SheetBlank)
      break;
    if (needs == Needs::SheetCornered) {
      // Two walls on two straight edges that meet at a corner: the first
      // pair that flanges cleanly AND gives the relief a corner to cut.
      const Body& body = Doc()->GetBodies()[0];
      const uint64_t base = body.createdBy;
      std::vector<std::string> names;
      for (const auto& [idx, name] : body.elementMap.edgeNames)
        names.push_back(name);
      bool made = false;
      for (size_t i = 0; i < names.size() && !made; ++i) {
        for (size_t j = i + 1; j < names.size() && !made; ++j) {
          auto walls = std::make_unique<EdgeFlangeFeature>(
              "Walls",
              std::vector<ElementRef>{ElementRef{base, names[i]},
                                      ElementRef{base, names[j]}},
              5.0, 90.0);
          walls->position = FlangePosition::BendOutside;
          Doc()->AddFeature(std::move(walls));
          const bool ok =
              Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty();
          if (ok) {
            Doc()->AddFeature(
                std::make_unique<CornerReliefFeature>("Probe", base));
            const bool corner = Doc()
                                    ->GetFeature(Doc()->FeatureCount() - 1)
                                    ->GetError()
                                    .empty();
            Doc()->RemoveFeature(Doc()->FeatureCount() - 1);
            if (corner) {
              made = true;
              break;
            }
          }
          Doc()->RemoveFeature(Doc()->FeatureCount() - 1);
        }
      }
      IM_CHECK_SILENT(made);
      break;
    }
    // A wall on the first straight edge that takes one. Straight onto the
    // document: which edge is picked is not what these tools are about, and
    // the edge-click itself has its own test.
    {
      const Body& body = Doc()->GetBodies()[0];
      const uint64_t base = body.createdBy;
      std::vector<std::string> names;
      for (const auto& [idx, name] : body.elementMap.edgeNames)
        names.push_back(name);
      bool made = false;
      for (const std::string& name : names) {
        Doc()->AddFeature(std::make_unique<EdgeFlangeFeature>(
            "Wall", std::vector<ElementRef>{ElementRef{base, name}}, 5.0,
            90.0));
        if (Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty()) {
          made = true;
          break;
        }
        Doc()->RemoveFeature(Doc()->FeatureCount() - 1);
      }
      IM_CHECK_SILENT(made);
    }
    if (needs == Needs::SheetFlanged)
      break;
    Doc()->AddFeature(std::make_unique<UnfoldFeature>(
        "Unfold", Doc()->GetBodies()[0].createdBy));
    IM_CHECK_SILENT(
        Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    break;
  }
  case Needs::BodyAndOpenSketch: {
    // A rib grows a thin web from an open profile down onto a body, so the
    // gate needs both: the default box, and a line hanging above it.
    AddFixtureBox(ctx);
    glm::dvec3 lo(0.0), hi(0.0);
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
