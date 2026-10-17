// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/GuiTests.h"
#include "app/guitests/GuiTestsCommon.h"

#include "core/Literals.h"
#include "core/io/GitVault.h"
#include "imgui_te_utils.h" // ImOsPOpen
#include "ui/Commands.h"
#include "ui/MotionActions.h" // FfmpegPath
#include "ui/ProjectActions.h"
#include "ui/SketchInteraction.h"
#include "ui/VariableActions.h"
#ifdef HAVE_CAM
#include "ui/CamPlayback.h"
#endif

#include <cstdio>
#include <filesystem>
#include <vector>

// Recording (docs/ui/testing.md#recording-a-scene): the engine's video
// capture, fed by main.cpp's CaptureForTestEngine. A demo scene records only
// when the run was given --capture-dir; in the ordinary suite it runs the
// same steps and records nothing, so a UI change that breaks a scene fails
// the suite the day it lands.

namespace {
std::string s_CaptureDir;

// The frames of an encoded clip, decoded back to RGB8 by ffmpeg.
std::vector<std::vector<uint8_t>> DecodeClip(const std::string& path, int w,
                                             int h) {
  std::vector<std::vector<uint8_t>> frames;
  const std::string cmd = "\"" + ui::FfmpegPath() +
                          "\" -nostdin -v error -i \"" + path +
                          "\" -f rawvideo -pix_fmt rgb24 -";
  FILE* pipe = ImOsPOpen(cmd.c_str(), "r");
  if (pipe == nullptr)
    return frames;
  const size_t bytes = size_t(w) * h * 3;
  for (;;) {
    std::vector<uint8_t> f(bytes);
    if (std::fread(f.data(), 1, bytes, pipe) != bytes)
      break;
    frames.push_back(std::move(f));
  }
  ImOsPClose(pipe);
  return frames;
}

double MeanLuma(const std::vector<uint8_t>& rgb) {
  double sum = 0;
  for (size_t i = 0; i + 2 < rgb.size(); i += 3)
    sum += 0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2];
  return sum / double(rgb.size() / 3);
}
} // namespace

void SetGuiTestCaptureDir(const std::string& dir) { s_CaptureDir = dir; }
const std::string& GuiTestCaptureDir() { return s_CaptureDir; }

// Starts recording the whole window to `path` at 60 fps -- one video frame
// per frame of the suite's fixed 1/60 s clock, however long each took.
static bool BeginRecording(ImGuiTestContext* ctx, const std::string& path) {
  ctx->CaptureReset();
  ImGuiCaptureArgs* args = ctx->CaptureArgs;
  std::snprintf(args->InOutputFile, sizeof(args->InOutputFile), "%s",
                path.c_str());
  args->InRecordFPSTarget = 60;
  args->InPadding = 0.0f;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  args->InCaptureRect =
      ImRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
  // The encoder wants even sides, so the engine rounds the size up to 2 --
  // and then down by 2 when it reaches the viewport's edge, which a window
  // already even does exactly (`>=` where `>` was meant): 1280x720 came out
  // 1278x718. An even window needs no rounding, so it is asked for none.
  if (int(vp->Size.x) % 2 == 0 && int(vp->Size.y) % 2 == 0)
    args->InSizeAlign = 1;
  return ctx->CaptureBeginVideo();
}

// Ends the recording and puts the suite's clock back: ending a video sets
// the engine's ConfigFixedDeltaTime to 0 (wall-clock time), and every test
// after it would then judge double-clicks by how loaded the machine is
// (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count).
static bool EndRecording(ImGuiTestContext* ctx) {
  const float fixedDt = ctx->EngineIO->ConfigFixedDeltaTime;
  const bool ok = ctx->CaptureEndVideo();
  ctx->EngineIO->ConfigFixedDeltaTime = fixedDt;
  return ok;
}

// --- the demo scenes (docs/plans/demo-video.md) ---------------------------
//
// Each scene sets itself up off camera, records only what the video shows,
// and checks its outcome like any other test. Run in the suite (Fast, no
// --capture-dir) it records nothing and its holds and sweeps collapse to a
// frame or two; recorded (tools/demo/record.sh) the engine runs it at
// Cinematic speed and every hold is real time on the fixed 60 fps clock.

// Recording a scene, when the run was given --capture-dir.
static bool s_SceneRecording = false;
static void BeginScene(ImGuiTestContext* ctx, const char* name) {
  s_SceneRecording = false;
  if (GuiTestCaptureDir().empty())
    return;
  const std::filesystem::path clip =
      std::filesystem::path(GuiTestCaptureDir()) / (std::string(name) + ".mp4");
  s_SceneRecording = BeginRecording(ctx, clip.string());
  IM_CHECK_NO_RET(s_SceneRecording);
}
static void EndScene(ImGuiTestContext* ctx) {
  if (s_SceneRecording)
    IM_CHECK_NO_RET(EndRecording(ctx));
  s_SceneRecording = false;
}

// Frames for `seconds` of the video -- or one, when nothing records and the
// engine runs Fast.
static int SceneFrames(ImGuiTestContext* ctx, double seconds) {
  if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
    return 1;
  return std::max(1, static_cast<int>(seconds * 60.0));
}
static void Hold(ImGuiTestContext* ctx, double seconds) {
  ctx->Yield(SceneFrames(ctx, seconds));
}
// The camera turned `degrees` about the view's vertical over `seconds`.
static void OrbitFor(ImGuiTestContext* ctx, double degrees, double seconds) {
  const int n = SceneFrames(ctx, seconds);
  for (int i = 0; i < n; i++) {
    s_App->camera->Orbit(degrees / n, 0.0);
    ctx->Yield();
  }
}
// A named view, animated when recorded and settled at once when not.
static void ShowView(ImGuiTestContext* ctx, const char* view, double seconds) {
  ui::RunCommand(*s_App, std::string("view.") + view);
  ctx->Yield();
  if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
    s_App->camera->FinishAnimation();
  Hold(ctx, seconds);
}
// The camera on `doc`'s bodies from the front-right and above, far enough to
// see them all: set from their box rather than by view.fit, which does not
// frame bodies a scene adds to the document directly.
static void FrameBodies(ImGuiTestContext* ctx, Document* doc) {
  glm::dvec3 lo(1e300), hi(-1e300);
  for (const Body& b : doc->GetBodies()) {
    glm::dvec3 l, h;
    b.shape.GetBoundingBox(l, h);
    lo = glm::min(lo, l);
    hi = glm::max(hi, h);
  }
  IM_CHECK_SILENT_RETV(lo.x <= hi.x, );
  const glm::vec3 mid((lo + hi) * 0.5);
  const float size = static_cast<float>(glm::length(hi - lo));
  s_App->camera->StopAnimation();
  s_App->camera->SetTarget(mid);
  s_App->camera->SetUp(glm::vec3(0, 0, 1));
  s_App->camera->SetPosition(
      mid + glm::normalize(glm::vec3(1.0f, -1.3f, 0.9f)) * (1.6f * size));
  ctx->Yield(2);
}

static void ShowPanel(ImGuiTestContext* ctx, const char* title) {
  ImGuiWindow* w = ImGui::FindWindowByName(title);
  IM_CHECK_SILENT_RETV(w != nullptr && w->DockNode != nullptr, );
  ctx->ItemClick(w->TabId);
  ctx->Yield(2);
}

static void RegisterDemoScenes(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;

  // A sketch, drawn; extruded; its height a variable, changed in the
  // Variables panel -- and the part follows.
  t = IM_REGISTER_TEST(engine, "demo", "parametric");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    BeginScene(ctx, "parametric");
    EnterSketchOnPlane(ctx, 0); // Top
    Hold(ctx, 0.5);
    ToolbarClick(ctx, "###tb_sk_rectgrp");
    const auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK_SILENT(sk != nullptr);
    const glm::vec2 c = ui::SketchUVToScreen(*s_App, sk->plane, {0, 0});
    const float half = 0.18f * s_App->viewportSize.y;
    ctx->MouseMoveToPos(ImVec2(c.x - 1.4f * half, c.y - half));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(c.x + 1.4f * half, c.y + half));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    Hold(ctx, 0.4);
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_extrude");
    ctx->Yield(2);
    IM_CHECK_SILENT(!Doc()->GetBodies().empty());
    ShowView(ctx, "iso", 1.0);

    // The height becomes a variable (off the record's clicks: binding is its
    // own scene), and the Variables panel drives it.
    const size_t index = Doc()->FeatureCount() - 1;
    Feature* extrude = Doc()->GetFeature(index);
    IM_CHECK_SILENT(extrude != nullptr && extrude->TypeName() == "Extrude");
    nlohmann::json before = DocumentIO::FeatureToJson(*extrude);
    const double h0 = before.value("distance", 10.0);
    IM_CHECK_SILENT(ui::SetProjectVariable(
        *s_App, "height", literals::ExactNumber(h0), "mm, the block"));
    nlohmann::json after = before;
    after["distance_expr"] = "height";
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
    ctx->Yield(2);
    const double v0 = Doc()->GetBodies().at(0).shape.Volume();
    ShowPanel(ctx, ui::kPanelVariables);
    Hold(ctx, 0.5);
    const char* field =
        "//###Variables/###vars_table/###var_expr_project_height";
    ctx->ItemInputValue(field, literals::ExactNumber(h0 * 3).c_str());
    ctx->Yield(2);
    Hold(ctx, 1.0);
    const double v1 = Doc()->GetBodies().at(0).shape.Volume();
    ctx->ItemInputValue(field, literals::ExactNumber(h0 * 0.5).c_str());
    ctx->Yield(2);
    Hold(ctx, 1.0);
    const double v2 = Doc()->GetBodies().at(0).shape.Volume();
    EndScene(ctx);
    // The block followed its variable: three times as tall, then half.
    IM_CHECK_LT(std::abs(v1 / v0 - 3.0), 1e-6);
    IM_CHECK_LT(std::abs(v2 / v0 - 0.5), 1e-6);
    ShowPanel(ctx, ui::kPanelModel);
  };

  // A version named, the part changed and saved, the named version restored:
  // the change is gone and nothing was lost.
  t = IM_REGISTER_TEST(engine, "demo", "history");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH -- skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "lobole_demo_history.dsgn";
    std::filesystem::remove_all(dir);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    ShowView(ctx, "iso", 0.0);
    std::string err;
    IM_CHECK_SILENT(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0; // the panel's log cache, refreshed now
    ShowPanel(ctx, ui::kPanelHistory);

    BeginScene(ctx, "history");
    ctx->SetRef("//###History");
    ctx->ItemClick("###hist_name");
    ctx->KeyCharsReplace("the plain block");
    ctx->ItemClick("###hist_name_go");
    ctx->Yield(2);
    Hold(ctx, 0.8);
    const size_t features = Doc()->FeatureCount();
    AddFixtureCylinder(ctx);
    ctx->Yield(2);
    IM_CHECK_GT(Doc()->FeatureCount(), features);
    Hold(ctx, 0.8);
    ctx->SetRef("//###History");
    ctx->ItemClick("###hist_save");
    ctx->Yield(2);
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    Hold(ctx, 0.8);
    int named = -1;
    for (int i = 0; i < static_cast<int>(s_App->historyLog.size()); i++)
      if (s_App->historyLog[i].named)
        named = i;
    IM_CHECK_SILENT(named >= 0);
    ctx->ItemClick(("**/###hist_restore" + std::to_string(named)).c_str());
    ctx->Yield(3);
    Hold(ctx, 1.2);
    EndScene(ctx);
    IM_CHECK_EQ(Doc()->FeatureCount(), features); // the cylinder is gone
    std::filesystem::remove_all(dir);
    ShowPanel(ctx, ui::kPanelModel);
  };

#ifdef HAVE_NETGEN
  // A cantilever's static study run, and its stress shown.
  t = IM_REGISTER_TEST(engine, "demo", "fem");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ShowView(ctx, "iso", 0.0);
    BeginScene(ctx, "fem");
    Hold(ctx, 0.5);
    ctx->ItemClick("//###ToolWindow/###study_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    ctx->ItemClick("//###ToolWindow/**/###field_von_mises");
    ctx->Yield(2);
    Hold(ctx, 0.5);
    OrbitFor(ctx, 60.0, 3.0);
    EndScene(ctx);
    const fem::StudyRuntime* rt = Doc()->StudyResults(studyId);
    IM_CHECK_SILENT(rt != nullptr);
    IM_CHECK_GT(rt->maxVonMises, 0.0);
  };
#endif

#ifdef HAVE_CAM
  // A plate, an operation generated, and the cut simulated end to end.
  t = IM_REGISTER_TEST(engine, "demo", "cam");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    setup.safeZ = 20.0;
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    ShowPanel(ctx, ui::kPanelCam);
    ctx->ItemClick(
        ("//CAM###CamPanel/**/###cam_setup_" + std::to_string(setup.id))
            .c_str(),
        ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_add_op");
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(2);
    s_App->camGenerateOpen = false;
    ctx->Yield(2);
    IM_CHECK_SILENT(!s_App->camToolpaths.empty());
    FrameBodies(ctx, CamPart());

    BeginScene(ctx, "cam");
    ToolbarClick(ctx, "###tb_job_simulate");
    ctx->Yield(3);
    IM_CHECK_SILENT(s_App->camPlaybackOpen);
    const int n = SceneFrames(ctx, 5.0);
    for (int i = 1; i <= n; i++) {
      ui::SeekCamPlayback(*s_App,
                          s_App->camTimeline.CursorAtDistance(
                              s_App->camTimeline.totalDistance * i / n));
      ctx->Yield();
    }
    Hold(ctx, 1.0);
    EndScene(ctx);
    IM_CHECK_GT(s_App->camStockSim.RemovedVolume(), 100.0);
    ui::EndCamPlayback(*s_App);
    ctx->Yield(2);
  };
#endif

  // The print check on a part with a piece held in the air: it finds the
  // piece that starts off the bed, and Show points at it.
  t = IM_REGISTER_TEST(engine, "demo", "print");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Standing", glm::vec3(0), glm::vec3(0, 0, 1), 6.0, 12.0));
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Held", glm::vec3(20, 0, 3), glm::vec3(0, 0, 1), 4.0, 8.0));
    ctx->Yield(2);
    Doc()->SetPrintProcess(printfit::Process{"fdm", "pla", std::nullopt});
    s_App->printCheck.reset();
    FrameBodies(ctx, Doc());

    BeginScene(ctx, "print");
    ShowPanel(ctx, ui::kPanelVariables);
    if (ctx->ItemInfo("//###Variables/###print_process",
                      ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Print process");
      ctx->Yield(2);
    }
    ctx->SetRef("//###Variables");
    ctx->ItemClick("###print_check");
    ctx->Yield(2);
    Hold(ctx, 1.0);
    IM_CHECK_SILENT(s_App->printCheck.has_value());
    s_App->selection.Clear();
    ctx->ItemClick("###print_check_stacking");
    ctx->Yield(2);
    Hold(ctx, 1.5);
    EndScene(ctx);
    IM_CHECK(s_App->printCheck->report.stacking.status ==
             printcheck::Status::Fail);
    IM_CHECK(s_App->selection.First(ui::SelectionItem::Kind::Face) != nullptr);
    Doc()->SetPrintProcess(std::nullopt);
    s_App->printCheck.reset();
    ShowPanel(ctx, ui::kPanelModel);
  };
}

void RegisterDemoGuiTests(ImGuiTestEngine* engine) {
  RegisterDemoScenes(engine);
  ImGuiTest* t = nullptr;

  // The recording itself, end to end: two seconds of an orbiting camera,
  // encoded by ffmpeg and decoded back. Every frame arrives (the clock is
  // fixed, so 120 frames are two seconds), at the window's size, showing the
  // window -- the interface as well as the scene -- and moving.
  t = IM_REGISTER_TEST(engine, "capture", "records_a_clip");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (ui::FfmpegPath().empty()) {
      IM_ERRORF("%s", "ffmpeg is not on PATH; the clip cannot be encoded");
      return;
    }
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);

    const std::filesystem::path clip =
        std::filesystem::temp_directory_path() /
        ("lobole_capture_" + std::to_string(ImGui::GetFrameCount()) + ".mp4");
    std::filesystem::remove(clip);
    IM_CHECK(BeginRecording(ctx, clip.string()));

    // The camera turned a step a frame, not by mouse: a recording runs at
    // Cinematic speed, where each mouse move glides and then pauses, so 120
    // moves were two minutes of video rather than two seconds.
    constexpr int kFrames = 120;
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    for (int i = 0; i < kFrames; i++) {
      s_App->camera->Orbit(4.0, 0.0);
      ctx->Yield();
    }
    IM_CHECK(EndRecording(ctx));

    const int w = static_cast<int>(ctx->CaptureArgs->OutImageSize.x);
    const int h = static_cast<int>(ctx->CaptureArgs->OutImageSize.y);
    ctx->LogInfo("clip %dx%d at %s", w, h, clip.string().c_str());
    IM_CHECK(std::filesystem::exists(clip));
    // The whole window (to the encoder's even alignment), not a crop.
    IM_CHECK_LE(std::abs(w - int(size.x)), 1);
    IM_CHECK_LE(std::abs(h - int(size.y)), 1);

    const auto frames = DecodeClip(clip.string(), w, h);
    std::filesystem::remove(clip);
    ctx->LogInfo("decoded %d frames", int(frames.size()));
    IM_CHECK_GE(int(frames.size()), kFrames - 4);
    IM_CHECK_LE(int(frames.size()), kFrames + 8);
    IM_CHECK(frames.size() >= 2);
    // Not black: the scene and the panels are drawn.
    IM_CHECK_GT(MeanLuma(frames.back()), 10.0);
    // Moving: the camera turned between the first frame and the last.
    double diff = 0;
    for (size_t i = 0; i < frames.front().size(); i++)
      diff += std::abs(int(frames.front()[i]) - int(frames.back()[i]));
    IM_CHECK_GT(diff / double(frames.front().size()), 0.5);
  };
}
