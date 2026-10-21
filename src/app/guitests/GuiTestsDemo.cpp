// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/GuiTests.h"
#include "app/guitests/GuiTestsCommon.h"

#include "cad/CADMaterials.h"
#include "core/Literals.h"
#include "core/io/GitVault.h"
#include "imgui_te_utils.h" // ImOsPOpen
#include "ui/AssemblyActions.h"
#include "ui/Commands.h"
#include "ui/DocumentSlots.h"
#include "ui/MotionActions.h" // FfmpegPath
#include "ui/ProjectActions.h"
#include "ui/SequencePlayback.h"
#include "ui/SketchInteraction.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"
#ifdef HAVE_CAM
#include "ui/CamPlayback.h"
#endif

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

// On Windows, OpenCASCADE's Standard_Mutex.hxx includes windows.h, whose
// winbase.h defines `Yield()` as a macro and breaks every ctx->Yield(n)
// below. Only the Windows build sees it (docs/packaging/README.md#windows).
#ifdef Yield
#undef Yield
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
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
  // The whole window as it is: without these the capture tool hides and
  // relocates windows while it records, and a click on a tool window's
  // button found nothing under the mouse.
  args->InFlags |=
      ImGuiCaptureFlags_IncludeOtherWindows | ImGuiCaptureFlags_IncludePopups;
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
// The pointer's glide while a scene records, against the engine's own. A
// move shorter than one second of travel is already stretched to 0.5-1 s, so
// a faster base leaves short, deliberate moves as they were and cuts only the
// long crossings of the screen, which show nothing on the way.
static constexpr float kSceneMouseSpeed = 2.5f;
static float s_MouseSpeedBefore = 0.0f;

// The assembly scene's pace, eye and distance: a ratio of the sequence's
// own pace; the direction the camera looks from; times the engine's
// diagonal.
static constexpr float kAssemblySceneSpeed = 1.5f;
static const glm::vec3 kAssemblySceneEye(0.35f, -1.0f, 0.4f);
static constexpr float kAssemblySceneDistance = 1.6f;

static void BeginScene(ImGuiTestContext* ctx, const char* name) {
  s_SceneRecording = false;
  if (GuiTestCaptureDir().empty())
    return;
  const std::filesystem::path clip =
      std::filesystem::path(GuiTestCaptureDir()) / (std::string(name) + ".mp4");
  // The app as its user sees it at their desk, not the suite's defaults
  // (half-resolution occlusion); the reset before the next test puts the
  // defaults back.
  s_App->settings.ssao = "full";
  ui::ApplySettings(*s_App);
  ctx->Yield();
  s_SceneRecording = BeginRecording(ctx, clip.string());
  IM_CHECK_NO_RET(s_SceneRecording);
  s_MouseSpeedBefore = ctx->EngineIO->MouseSpeed;
  ctx->EngineIO->MouseSpeed *= kSceneMouseSpeed;
}
static void EndScene(ImGuiTestContext* ctx) {
  if (s_SceneRecording) {
    IM_CHECK_NO_RET(EndRecording(ctx));
    ctx->EngineIO->MouseSpeed = s_MouseSpeedBefore;
  }
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
// The camera turned `degrees` about the world's vertical over `seconds`, a
// turntable round the target: each frame's angle is an exact fraction of the
// whole, so a full turn ends where it began. The horizon is held level every
// frame: moving the eye alone keeps the camera's previous up, which is not
// vertical when it looks down, and over a turn that tilt became a tumble.
// (Camera::Orbit is a trackball in mouse pixels, about the camera's own up --
// neither degrees nor a turntable.)
static void OrbitFor(ImGuiTestContext* ctx, double degrees, double seconds) {
  const int n = SceneFrames(ctx, seconds);
  const glm::vec3 target = s_App->camera->GetTarget();
  const glm::vec3 eye0 = s_App->camera->GetPosition() - target;
  for (int i = 1; i <= n; i++) {
    const float a = static_cast<float>(glm::radians(degrees) * i / n);
    const float c = std::cos(a), sn = std::sin(a);
    const glm::vec3 eye(c * eye0.x - sn * eye0.y, sn * eye0.x + c * eye0.y,
                        eye0.z);
    s_App->camera->SetPosition(target + eye);
    s_App->camera->SetUp(conv::kWorldUpF);
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

// FrameBodies, flown to over `seconds` (instantly in the suite), from
// `distance` times the bodies' diagonal -- more than FrameBodies' 1.6 when
// the bodies are about to grow.
static void FlyToBodies(ImGuiTestContext* ctx, Document* doc, double seconds,
                        float distance) {
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
  s_App->camera->AnimateToPosition(
      mid + glm::normalize(glm::vec3(1.0f, -1.3f, 0.9f)) * (distance * size),
      mid, static_cast<float>(std::max(seconds, 0.05)));
  ctx->Yield();
  if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
    s_App->camera->FinishAnimation();
  Hold(ctx, seconds);
}

// After FrameBodies: slide the camera sideways (target and eye together) so
// the bodies' centre lands in the middle of the viewport left of `window` --
// a floating tool window that would otherwise cover part of the model.
static void CentreLeftOf(ImGuiTestContext* ctx, const glm::dvec3& lo,
                         const glm::dvec3& hi, const char* window) {
  ImGuiWindow* w = ctx->WindowInfo(window, ImGuiTestOpFlags_NoError).Window;
  if (w == nullptr)
    return;
  const glm::vec3 mid((lo + hi) * 0.5);
  const float left = s_App->viewportPos.x;
  const float right = std::min(w->Pos.x, left + s_App->viewportSize.x);
  const float want = 0.5f * (left + right);
  // Pixels per world unit along the camera's right, measured.
  const glm::vec3 eye = s_App->camera->GetPosition();
  const glm::vec3 fwd = glm::normalize(mid - eye);
  const glm::vec3 rightDir =
      glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
  const float px = WorldToWindow(mid + rightDir).x - WorldToWindow(mid).x;
  if (std::abs(px) < 1e-3f)
    return;
  const glm::vec3 shift = rightDir * ((WorldToWindow(mid).x - want) / px);
  s_App->camera->SetTarget(s_App->camera->GetTarget() + shift);
  s_App->camera->SetPosition(eye + shift);
  ctx->Yield(2);
}
static void CentreLeftOf(ImGuiTestContext* ctx, Document* doc,
                         const char* window) {
  glm::dvec3 lo(1e300), hi(-1e300);
  for (const Body& b : doc->GetBodies()) {
    glm::dvec3 l, h;
    b.shape.GetBoundingBox(l, h);
    lo = glm::min(lo, l);
    hi = glm::max(hi, h);
  }
  CentreLeftOf(ctx, lo, hi, window);
}

// The active assembly's box: every shown leaf's bodies, placed.
static bool AssemblyBox(glm::dvec3& lo, glm::dvec3& hi) {
  lo = glm::dvec3(1e300);
  hi = glm::dvec3(-1e300);
  for (const assembly::FlatInstance& inst : ui::FlattenActiveAssembly(*s_App)) {
    if (!inst.visible || inst.part == nullptr)
      continue;
    for (const Body& b : inst.part->GetBodies()) {
      if (b.reference)
        continue;
      glm::dvec3 l, h;
      b.shape.GetBoundingBox(l, h);
      for (int c = 0; c < 8; c++) {
        const glm::dvec3 p = glm::dvec3(
            inst.world * glm::dvec4((c & 1) ? h.x : l.x, (c & 2) ? h.y : l.y,
                                    (c & 4) ? h.z : l.z, 1.0));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
      }
    }
  }
  return lo.x <= hi.x;
}

static void ShowPanel(ImGuiTestContext* ctx, const char* title) {
  ImGuiWindow* w = ImGui::FindWindowByName(title);
  IM_CHECK_SILENT_RETV(w != nullptr && w->DockNode != nullptr, );
  ctx->ItemClick(w->TabId);
  ctx->Yield(2);
}

// A shape's largest flat face that nothing of the shape can hide -- the
// outermost along its own normal -- with its centre on the face. 0 when
// there is none.
static void BestFlatFace(const TopoDS_Shape& shape, double& best,
                         gp_Pnt& centre, gp_Dir& normal,
                         TopoDS_Face* bestFace = nullptr) {
  best = 0.0;
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid())
    return;
  double xmin, ymin, zmin, xmax, ymax, zmax;
  box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    if (props.Mass() <= best)
      continue;
    gp_Dir nf = surf.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED)
      nf.Reverse();
    const gp_Pnt c = props.CentreOfMass();
    double outermost = -1e300;
    for (double x : {xmin, xmax})
      for (double y : {ymin, ymax})
        for (double z : {zmin, zmax})
          outermost =
              std::max(outermost, gp_Vec(gp_Pnt(x, y, z).XYZ()).Dot(nf));
    if (gp_Vec(c.XYZ()).Dot(nf) <
        outermost - 1e-3 * (1.0 + std::abs(outermost)))
      continue;
    if (BRepClass_FaceClassifier(f, c, 1e-6).State() != TopAbs_IN)
      continue;
    best = props.Mass();
    centre = c;
    normal = nf;
    if (bestFace != nullptr)
      *bestFace = f;
  }
}

// The point of a flat face farthest from every one of its edges, and that
// distance: where a click picks the face rather than a nearby edge, and the
// room a boss drawn there has. A grid over the face's extent, kept to the
// points on it.
static gp_Pnt RoomiestPoint(const TopoDS_Face& face, double& clearance) {
  clearance = 0.0;
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  TopoDS_Compound edges;
  BRep_Builder builder;
  builder.MakeCompound(edges);
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
    builder.Add(edges, ex.Current());
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
  gp_Pnt best;
  constexpr int kN = 24;
  for (int i = 0; i <= kN; i++)
    for (int j = 0; j <= kN; j++)
      for (int k = 0; k <= kN; k++) {
        const gp_Pnt guess(x0 + (x1 - x0) * i / kN, y0 + (y1 - y0) * j / kN,
                           z0 + (z1 - z0) * k / kN);
        // Onto the face's plane; only one layer survives a flat face.
        GeomAPI_ProjectPointOnSurf onto(guess, surf);
        if (onto.NbPoints() == 0 || onto.LowerDistance() > 1e-3)
          continue;
        const gp_Pnt p = onto.NearestPoint();
        if (BRepClass_FaceClassifier(face, p, 1e-6).State() != TopAbs_IN)
          continue;
        BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(),
                                     edges);
        if (d.IsDone() && d.Value() > clearance) {
          clearance = d.Value();
          best = p;
        }
      }
  return best;
}

static void RegisterDemoScenes(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;

  // A variable made in the Variables panel; a rectangle drawn on Top and
  // dimensioned; extruded with the variable typed as its distance; the
  // variable changed -- and the block follows.
  t = IM_REGISTER_TEST(engine, "demo", "parametric");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    s_App->camera->FinishAnimation();
    BeginScene(ctx, "parametric");

    // The variable, first, where a user makes one.
    ShowPanel(ctx, ui::kPanelVariables);
    Hold(ctx, 0.4);
    ctx->ItemClick("//###Variables/+ Add variable");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Variables/###vars_table/###var_name_project_var1",
                        "height");
    ctx->Yield(2);
    const char* field =
        "//###Variables/###vars_table/###var_expr_project_height";
    ctx->ItemInputValue(field, "10");
    ctx->Yield(2);
    Hold(ctx, 0.6);
    IM_CHECK_SILENT(ui::FindProjectParameter(*s_App, "height") != nullptr);
    ShowPanel(ctx, ui::kPanelModel);

    // The rectangle, drawn with the tool, then a dimension on two sides.
    EnterSketchOnPlane(ctx, 0); // Top
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    Hold(ctx, 0.3);
    ToolbarClick(ctx, "###tb_sk_rectgrp");
    const auto* sk = dynamic_cast<const SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK_SILENT(sk != nullptr);
    const SketchPlane plane = sk->plane;
    const auto at = [&](glm::vec2 uv) {
      const glm::vec2 p = ui::SketchUVToScreen(*s_App, plane, uv);
      return ImVec2(p.x, p.y);
    };
    // Zoomed so a 40 mm block fills about half the view: an empty
    // document's sketch view is a few millimetres across.
    for (int i = 0; i < 4; i++) {
      const float px = at({20, 0}).x - at({-20, 0}).x;
      if (px <= 1.0f)
        break;
      s_App->camera->SetDistance(s_App->camera->GetDistance() * px /
                                 (0.45f * s_App->viewportSize.x));
      ctx->Yield();
    }
    // Drawn near 40 x 25, so the typed dimensions visibly settle it.
    ctx->MouseMoveToPos(at({-18.0f, -11.0f}));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(at({17.0f, 11.5f}));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    Hold(ctx, 0.3);
    // The rectangle's box, in sketch units, read again after each change.
    glm::vec2 lo, hi;
    const auto box = [&]() {
      const auto* now = dynamic_cast<const SketchFeature*>(
          Doc()->GetFeature(Doc()->FeatureCount() - 1));
      glm::dvec2 dlo(1e300), dhi(-1e300);
      for (const auto& e : now->entities)
        if (const auto* ln = std::get_if<sketch::Line>(&e)) {
          dlo = glm::min(dlo, glm::min(glm::dvec2(ln->a), glm::dvec2(ln->b)));
          dhi = glm::max(dhi, glm::max(glm::dvec2(ln->a), glm::dvec2(ln->b)));
        }
      lo = glm::vec2(dlo);
      hi = glm::vec2(dhi);
      return now->constraints.size();
    };
    const size_t constraintsBefore = box();
    // The Dimension tool: click the edge, click where the label goes, type.
    // Each label is placed off the sketch's axes: the rectangle is centred
    // on the origin, and a placement click on an axis REFINES the dimension
    // into an angle against it rather than placing the length.
    // The tool stays armed after a dimension, so it is picked once; the
    // value goes in over the measured one the editor opens with.
    ToolbarClick(ctx, "###tb_sk_dimension");
    ctx->Yield();
    const auto dimension = [&](glm::vec2 onEdge, glm::vec2 place,
                               const char* value) {
      ctx->MouseMoveToPos(at(onEdge));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
      ctx->MouseMoveToPos(at(place));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      IM_CHECK_SILENT(s_App->sketchEdit.editingDim >= 0);
      ctx->KeyCharsReplaceEnter(value);
      ctx->Yield(2);
      Hold(ctx, 0.4);
    };
    dimension({lo.x + 0.3f * (hi.x - lo.x), lo.y},
              {lo.x + 0.3f * (hi.x - lo.x), lo.y - 6.0f}, "40");
    box();
    dimension({hi.x, lo.y + 0.3f * (hi.y - lo.y)},
              {hi.x + 6.0f, lo.y + 0.3f * (hi.y - lo.y)}, "25");
    IM_CHECK_GE(box(), constraintsBefore + 2);
    IM_CHECK_LT(std::abs((hi.x - lo.x) - 40.0f), 1e-3f);
    IM_CHECK_LT(std::abs((hi.y - lo.y) - 25.0f), 1e-3f);
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();
    ShowView(ctx, "iso", 0.8);

    // Extrude, its distance the variable, typed where a value would go.
    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield(2);
    Hold(ctx, 0.5);
    ctx->ItemClick("//###ToolWindow/Distance/fx");
    ctx->Yield(2);
    ctx->ItemInput("//###ToolWindow/Distance/##expr");
    ctx->Yield();
    ctx->KeyChars("hei");
    ctx->Yield(2);
    Hold(ctx, 0.3);
    ctx->KeyPress(ImGuiKey_Tab); // completes it to "height"
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    Hold(ctx, 0.5);
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK_SILENT(!Doc()->GetBodies().empty());
    const double v0 = Doc()->GetBodies().at(0).shape.Volume();
    // Back far enough for the block three times as tall.
    FlyToBodies(ctx, Doc(), 1.0, 2.6f);

    // And the variable drives it.
    ShowPanel(ctx, ui::kPanelVariables);
    ctx->ItemInputValue(field, "30");
    ctx->Yield(2);
    Hold(ctx, 1.0);
    const double v1 = Doc()->GetBodies().at(0).shape.Volume();
    ctx->ItemInputValue(field, "15");
    ctx->Yield(2);
    Hold(ctx, 1.0);
    const double v2 = Doc()->GetBodies().at(0).shape.Volume();

    // The renderer's turn: the block in a few materials, from the body's own
    // menu, turning a little under each. Seen from 55 degrees up first: a
    // polished flat face is a mirror, and from lower down the block's top
    // reflects only the dull part of the studio, so a metal looked matte.
    {
      const glm::vec3 target = s_App->camera->GetTarget();
      const glm::vec3 eye = s_App->camera->GetPosition() - target;
      const float dist = glm::length(eye);
      const glm::vec2 flat = glm::normalize(glm::vec2(eye.x, eye.y));
      const float el = glm::radians(55.0f);
      s_App->camera->AnimateToPosition(
          target + dist * glm::vec3(flat * std::cos(el), std::sin(el)), target,
          0.8f);
      ctx->Yield();
      if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
        s_App->camera->FinishAnimation();
      Hold(ctx, 0.8);
      s_App->camera->SetUp(conv::kWorldUpF);
    }
    ShowPanel(ctx, ui::kPanelModel);
    const std::string bodyRow =
        "//###Model/**/" + Doc()->GetBodies().at(0).name + "##body";
    for (const char* material : {"Copper", "Walnut", "Carbon Fibre"}) {
      ctx->ItemClick(bodyRow.c_str(), ImGuiMouseButton_Right);
      ctx->Yield();
      ctx->MenuClick((std::string("//$FOCUSED/Material/") + material).c_str());
      ctx->Yield(2);
      IM_CHECK_STR_EQ(
          CADMaterials::MaterialLibrary::GetMaterialOrDefault(
              Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetMaterial())
              .name.c_str(),
          material);
      OrbitFor(ctx, 25.0, 1.2);
    }
    EndScene(ctx);
    // 40 x 25 x 10 = 10000 mm3; three times as tall, then one and a half.
    IM_CHECK_LT(std::abs(v0 - 10000.0), 1e-3);
    IM_CHECK_LT(std::abs(v1 / v0 - 3.0), 1e-6);
    IM_CHECK_LT(std::abs(v2 / v0 - 1.5), 1e-6);
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
    // Far enough back for the block and the cylinder about to stand on it.
    FlyToBodies(ctx, Doc(), 0.0, 3.2f);
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
    // The change: a cylinder stood on the block's top face and added to it
    // -- one part, so the restore visibly takes it back off.
    const size_t features = Doc()->FeatureCount();
    glm::dvec3 lo, hi;
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 top(0.5 * (lo.x + hi.x), 0.5 * (lo.y + hi.y), hi.z);
    ToolbarClick(ctx, "###tb_cylinder");
    ctx->Yield();
    IM_CHECK_SILENT(s_App->sketchEdit.pickingPlane);
    Hold(ctx, 0.3);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    Hold(ctx, 0.5);
    ctx->ItemClick("//###ToolWindow/###Mode");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ctx->ItemClick((std::string("//") +
                    ctx->UiContext->OpenPopupStack.back().Window->Name +
                    "/**/Add")
                       .c_str());
    ctx->Yield();
    IM_CHECK_SILENT(s_App->primitive.spec.mode == ExtrudeMode::Add);
    // Its size is the tool's default, derived from the face it stands on.
    ctx->Yield(2);
    Hold(ctx, 0.6);
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK_GT(Doc()->FeatureCount(), features);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1); // one part
    FlyToBodies(ctx, Doc(), 0.8, 2.6f);
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
    const std::string restore = "**/###hist_restore" + std::to_string(named);
    ctx->MouseMove(restore.c_str()); // its tooltip says what it will do
    Hold(ctx, 1.5);
    ctx->ItemClick(restore.c_str());
    ctx->Yield(3);
    Hold(ctx, 1.5);
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
    FrameBodies(ctx, Doc());
    CentreLeftOf(ctx, Doc(), "//###ToolWindow");
    BeginScene(ctx, "fem");
    Hold(ctx, 0.5);
    ctx->ItemClick("//###ToolWindow/###study_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    ctx->ItemClick("//###ToolWindow/**/###field_von_mises");
    ctx->Yield(2);
    Hold(ctx, 0.5);
    // The deflection, magnified until it shows: 0.3 mm on a 200 mm beam is
    // invisible at 1x.
    ctx->ItemInputValue("//###ToolWindow/**/###deform", 100.0f);
    ctx->Yield(2);
    Hold(ctx, 0.5);
    OrbitFor(ctx, 30.0, 2.0);
    ctx->ItemClick("//###ToolWindow/**/###field_displacement");
    ctx->Yield(2);
    Hold(ctx, 0.5);
    OrbitFor(ctx, -30.0, 2.0);
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

  // The OM615 with its injection pump: the assembly turned once round, cut
  // open, run through two crank revolutions in section; then one of its
  // parts opened from the tree and a boss sketched and extruded on it.
  // Registered only when LOBOLE_DEMO_OM615 names the project (a generated
  // model, in scratch/, which CI does not have): tools/demo/record.sh sets it
  // to a copy, so the developer's own is never opened for writing.
  if (const char* om615 = std::getenv("LOBOLE_DEMO_OM615")) {
    static std::string s_Om615;
    s_Om615 = om615;
    t = IM_REGISTER_TEST(engine, "demo", "om615");
    t->TestFunc = [](ImGuiTestContext* ctx) {
      std::string err;
      const bool opened = ui::OpenProject(*s_App, s_Om615, &err);
      if (!opened)
        ctx->LogError("open %s: %s", s_Om615.c_str(), err.c_str());
      IM_CHECK_SILENT(opened);
      ui::MarkProjectOpened(*s_App, s_Om615);
      IM_CHECK(WaitForOpen(ctx));
      size_t engineIdx = SIZE_MAX;
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].name == "engine")
          engineIdx = i;
      IM_CHECK_SILENT(engineIdx != SIZE_MAX);
      ui::SetActivePart(*s_App, engineIdx);
      ctx->Yield(5);
      IM_CHECK_SILENT(s_App->assembly != nullptr);
      ShowPanel(ctx, ui::kPanelModel);
      ShowView(ctx, "iso", 0.0);
      ui::RunCommand(*s_App, "view.fit");
      ctx->Yield(2);
      s_App->camera->FinishAnimation();
      // Lowered from the fit's iso to a little above the engine's middle,
      // level: the turn then walks round it rather
      // than looking down on its rocker cover.
      {
        const glm::vec3 target = s_App->camera->GetTarget();
        const glm::vec3 eye = s_App->camera->GetPosition() - target;
        // Closer than the fit, which leaves room for the iso's corners.
        const float dist = 0.75f * glm::length(eye);
        glm::vec2 flat(eye.x, eye.y);
        flat = glm::length(flat) > 1e-6f ? glm::normalize(flat)
                                         : glm::vec2(1.0f, -1.0f);
        const float elev = glm::radians(12.0f);
        s_App->camera->SetPosition(
            target + dist * glm::vec3(std::cos(elev) * flat.x,
                                      std::cos(elev) * flat.y, std::sin(elev)));
        s_App->camera->SetUp(conv::kWorldUpF);
      }
      ctx->Yield(3);

      // The motion's poses, ready before the camera rolls: the stored run
      // when it is still the study's, else a collision-free run of it.
      IM_CHECK_SILENT(!s_App->assembly->GetMotionStudies().empty());
      const uint64_t studyId = s_App->assembly->GetMotionStudies()[0].id;
      const auto poses = ui::PosesToPlay(*s_App, studyId, &err);
      if (!poses)
        ctx->LogError("poses: %s", err.c_str());
      IM_CHECK_SILENT(poses != nullptr);
      IM_CHECK(WaitFor(
          ctx, "the motion's poses", [&] { return poses->done.load(); },
          280.0));
      if (!poses->error.empty())
        ctx->LogError("poses: %s", poses->error.c_str());
      IM_CHECK_SILENT(poses->result != nullptr && poses->error.empty());
      const size_t samples = poses->result->samples.size();
      IM_CHECK_SILENT(samples >= 2);

      // The part to open: the one with the biggest clean flat face to build
      // on, not the block or the head (whose rebuild would hold the video
      // up), nothing sheet-thin (a gasket's face is all bolt holes), and one
      // the camera can see at the point it will be right-clicked -- checked
      // here, by a click, before anything records.
      struct Candidate {
        uint64_t occ;
        std::string name;
        double area;
        glm::dvec3 world;
      };
      std::vector<Candidate> candidates;
      std::set<uint64_t> seen;
      for (const Occurrence& o : s_App->assembly->GetOccurrences()) {
        if (o.refKind != OccurrenceRef::Part || !seen.insert(o.partId).second)
          continue;
        const Document* pd = ui::OccurrencePartDoc(*s_App, o);
        if (pd == nullptr || pd->GetBodies().size() != 1)
          continue;
        const std::string& nm = o.name;
        if (nm.rfind("block", 0) == 0 || nm.rfind("cylinder head", 0) == 0)
          continue;
        glm::dvec3 plo, phi;
        pd->GetBodies()[0].shape.GetBoundingBox(plo, phi);
        const glm::dvec3 ext = phi - plo;
        if (std::min({ext.x, ext.y, ext.z}) < 8.0)
          continue;
        double area = 0.0;
        gp_Pnt c;
        gp_Dir n(0, 0, 1);
        BestFlatFace(pd->GetBodies()[0].shape.GetShape(), area, c, n);
        if (area > 0.0)
          candidates.push_back(
              {o.id, nm, area,
               o.position + o.orientation * glm::dvec3(c.X(), c.Y(), c.Z())});
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const Candidate& a, const Candidate& b) {
                  return a.area > b.area;
                });
      ImVec2 pick(0, 0);
      std::string chosen;
      uint64_t chosenOcc = 0;
      glm::dvec3 chosenWorld(0.0);
      for (const Candidate& c : candidates) {
        const ImVec2 at = WorldToWindow(glm::vec3(c.world));
        if (at.x < s_App->viewportPos.x || at.y < s_App->viewportPos.y ||
            at.x > s_App->viewportPos.x + s_App->viewportSize.x ||
            at.y > s_App->viewportPos.y + s_App->viewportSize.y)
          continue;
        s_App->selection.Clear();
        ctx->MouseMoveToPos(at);
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        const bool hit = ui::IsOccurrenceSelected(*s_App, c.occ);
        s_App->selection.Clear();
        ctx->Yield();
        if (hit) {
          pick = at;
          chosen = c.name;
          chosenOcc = c.occ;
          chosenWorld = c.world;
          break;
        }
      }
      IM_CHECK_SILENT(!chosen.empty());
      ctx->LogInfo("will open %s", chosen.c_str());
      ctx->MouseMoveToPos(
          ImVec2(s_App->viewportPos.x + 10.0f, s_App->viewportPos.y + 10.0f));

      BeginScene(ctx, "om615");
      Hold(ctx, 0.6);
      OrbitFor(ctx, 360.0, 12.0); // once round, level
      Hold(ctx, 0.4);
      // The pose the part was found from, to come back to after the cut.
      const glm::vec3 homeEye = s_App->camera->GetPosition();
      const glm::vec3 homeTarget = s_App->camera->GetTarget();
      ToolbarClick(ctx, "###tb_section");
      ctx->Yield(3);
      IM_CHECK(!s_App->sections.empty());
      // Down the crank's axis (Y), through the bores: the YZ plane at x 0,
      // so the pistons and rods show as they move.
      s_App->sections[0].basePlane = 2;
      s_App->sections[0].offset = 0.0f;
      // Looked at square from +X, a little above, and the +X half -- the
      // camera's -- cut away (with `reverse` the cut keeps the camera's half).
      s_App->sections[0].reverse = false;
      ctx->Yield(3);
      Hold(ctx, 0.6);
      {
        const glm::vec3 target = s_App->camera->GetTarget();
        const float dist = glm::length(s_App->camera->GetPosition() - target);
        s_App->camera->AnimateToPosition(
            target + glm::normalize(glm::vec3(1.0f, -0.25f, 0.3f)) * dist,
            target, 1.2f);
        ctx->Yield();
        if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
          s_App->camera->FinishAnimation();
        Hold(ctx, 1.4);
      }
      // Two crank revolutions, in section, a pose a frame.
      const int n = SceneFrames(ctx, 7.0);
      for (int i = 0; i <= n; i++) {
        ui::ShowMotionPoses(*s_App, studyId, poses->result,
                            double(samples - 1) * i / std::max(n, 1));
        ctx->Yield();
      }
      Hold(ctx, 0.6);
      ui::StopMotionPlayback(*s_App);
      ui::ClearSections(*s_App);
      ctx->Yield(2);
      s_App->camera->AnimateToPosition(homeEye, homeTarget, 1.2f);
      ctx->Yield();
      if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast)
        s_App->camera->FinishAnimation();
      Hold(ctx, 1.3);

      // A part of it, right-clicked in the viewport and opened -- at the
      // point found above, projected again: the orbit, cut and motion leave
      // the camera a hair from where it was, enough to land on the block
      // behind a bracket.
      pick = WorldToWindow(glm::vec3(chosenWorld));
      ctx->MouseMoveToPos(pick);
      Hold(ctx, 0.4);
      ctx->MouseClick(ImGuiMouseButton_Right);
      ctx->Yield(2);
      IM_CHECK_SILENT(ui::IsOccurrenceSelected(*s_App, chosenOcc));
      IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
      ImGuiWindow* menu = ctx->UiContext->OpenPopupStack.back().Window;
      IM_CHECK_SILENT(menu != nullptr);
      Hold(ctx, 0.6);
      ctx->ItemClick((std::string("//") + menu->Name + "/###occ_open").c_str());
      ctx->Yield(3);
      IM_CHECK_SILENT(s_App->assembly == nullptr); // a part tab
      IM_CHECK_SILENT(!Doc()->GetBodies().empty());
      FlyToBodies(ctx, Doc(), 0.8, 1.6f);

      // The face found above, on the part as it opened: looked at square.
      double best = 0.0;
      gp_Pnt centre;
      gp_Dir normal(0, 0, 1);
      TopoDS_Face face;
      BestFlatFace(Doc()->GetBodies()[0].shape.GetShape(), best, centre, normal,
                   &face);
      IM_CHECK_SILENT(best > 0.0);
      double clearance = 0.0;
      centre = RoomiestPoint(face, clearance);
      IM_CHECK_SILENT(clearance > 1.0);
      const glm::vec3 fc(centre.X(), centre.Y(), centre.Z());
      const glm::vec3 fn(normal.X(), normal.Y(), normal.Z());
      s_App->camera->AlignToFace(fc, fn, 0.0f);
      for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
        ctx->Yield();
      Hold(ctx, 0.5);

      // A boss on it: New Sketch, the face clicked where nothing but the
      // face is near, a circle, then an extrude of just the circle.
      const size_t features = Doc()->FeatureCount();
      const double volumeBefore = Doc()->GetBodies()[0].shape.Volume();
      ToolbarClick(ctx, "###tb_new_sketch");
      ctx->Yield();
      IM_CHECK_SILENT(s_App->sketchEdit.pickingPlane);
      Hold(ctx, 0.3);
      ctx->MouseMoveToPos(WorldToWindow(fc));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
        ctx->Yield();
      IM_CHECK_SILENT(s_App->sketchEdit.IsEditing());
      Hold(ctx, 0.4);
      ToolbarClick(ctx, "###tb_sk_circlegrp");
      const float radius = 0.6f * static_cast<float>(clearance);
      const glm::vec3 inPlane = glm::normalize(glm::cross(
          fn, std::abs(fn.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
      ctx->MouseMoveToPos(WorldToWindow(fc));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(WorldToWindow(fc + inPlane * radius));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      Hold(ctx, 0.4);
      // The circle, selected, and Extrude from the sketch's own toolbar: the
      // extrude takes the selection as its region.
      ctx->KeyPress(ImGuiKey_Escape); // back to selecting
      ctx->Yield();
      IM_CHECK_SILENT(s_App->sketchEdit.tool == ui::SketchTool::Select);
      ctx->MouseMoveToPos(WorldToWindow(fc + inPlane * radius));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
      IM_CHECK_SILENT(!s_App->sketchEdit.selected.empty());
      Hold(ctx, 0.4);
      ToolbarClick(ctx, "###tb_sk_extrude");
      ctx->Yield(2);
      Hold(ctx, 0.5);
      IM_CHECK_SILENT(dynamic_cast<const ExtrudeFeature*>(
                          ui::ResolvePendingFeature(*s_App)) != nullptr);
      Hold(ctx, 0.4);
      ShowView(ctx, "iso", 0.8);
      ctx->ItemInputValue("//###ToolWindow/Distance/##Distance",
                          static_cast<float>(std::round(radius)));
      ctx->Yield(2);
      Hold(ctx, 0.6);
      ctx->ItemClick("//###ToolWindow/###tw_accept");
      ctx->Yield(2);
      FlyToBodies(ctx, Doc(), 0.8, 1.6f);
      Hold(ctx, 1.2);

      // Nothing is final: the boss suppressed and back from the feature
      // tree, then the rollback bar dragged above it and back to the end.
      IM_CHECK_SILENT(Doc()->FeatureCount() == features + 2);
      const auto row = [&](size_t i) {
        return "//###Model/" + Doc()->GetFeature(i)->GetName() + "##" +
               std::to_string(i);
      };
      const std::string boss = row(features + 1);
      for (const char* verb : {"Suppress", "Unsuppress"}) {
        ctx->ItemClick(boss.c_str(), ImGuiMouseButton_Right);
        ctx->Yield();
        Hold(ctx, 0.4);
        ctx->ItemClick((std::string("//$FOCUSED/") + verb).c_str());
        ctx->Yield(2);
        IM_CHECK_SILENT(Doc()->GetFeature(features + 1)->IsSuppressed() ==
                        (verb[0] == 'S'));
        Hold(ctx, 1.0);
      }
      const ImRect sketchRow = ctx->ItemInfo(row(features).c_str()).RectFull;
      const ImRect bossRow = ctx->ItemInfo(boss.c_str()).RectFull;
      ctx->MouseMove("//###Model/##RollbackBar");
      ctx->MouseDown(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(ImVec2(sketchRow.GetCenter().x, sketchRow.Min.y));
      ctx->MouseUp(ImGuiMouseButton_Left);
      ctx->Yield(2);
      IM_CHECK_SILENT(Doc()->Rollback() == static_cast<int>(features));
      Hold(ctx, 1.2);
      ctx->MouseMove("//###Model/##RollbackBar");
      ctx->MouseDown(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(
          ImVec2(bossRow.GetCenter().x, bossRow.Max.y + bossRow.GetHeight()));
      ctx->MouseUp(ImGuiMouseButton_Left);
      ctx->Yield(2);
      IM_CHECK_SILENT(Doc()->Rollback() == -1);
      Hold(ctx, 1.5);
      EndScene(ctx);
      IM_CHECK_GT(Doc()->FeatureCount(), features + 1); // sketch + extrude
      // What was added is the boss -- the circle, extruded -- and not the
      // face it stands on: pi r^2 d, to the circle's facets.
      const double depth = std::round(radius);
      const double added = Doc()->GetBodies()[0].shape.Volume() - volumeBefore;
      ctx->LogInfo("boss: %.0f mm3 added, a disc of r %.1f d %.0f is %.0f",
                   added, radius, depth, M_PI * radius * radius * depth);
      IM_CHECK_LT(std::abs(added - M_PI * radius * radius * depth),
                  0.05 * M_PI * radius * radius * depth);
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New"); // leave the big model behind
      ctx->Yield(2);
    };
  }

  // The printed engine (generators/engine/) put together: cut down the
  // cylinders, its assembly sequence played in the Motion window from the
  // block alone to the flywheel, each step's parts lit as they go home.
  // Registered only when LOBOLE_DEMO_ENGINE names the project (generated,
  // in scratch/, which CI does not have): tools/demo/record.sh sets it to
  // a copy.
  if (const char* enginePath = std::getenv("LOBOLE_DEMO_ENGINE")) {
    static std::string s_Engine;
    s_Engine = enginePath;
    t = IM_REGISTER_TEST(engine, "demo", "assembly");
    t->TestFunc = [](ImGuiTestContext* ctx) {
      std::string err;
      const bool opened = ui::OpenProject(*s_App, s_Engine, &err);
      if (!opened)
        ctx->LogError("open %s: %s", s_Engine.c_str(), err.c_str());
      IM_CHECK_SILENT(opened);
      ui::MarkProjectOpened(*s_App, s_Engine);
      IM_CHECK(WaitForOpen(ctx));
      if (s_App->assembly == nullptr)
        for (size_t i = 0; i < s_App->parts.size(); i++)
          if (s_App->parts[i].kind == ui::DocKind::Assembly) {
            ui::SetActivePart(*s_App, i);
            break;
          }
      ctx->Yield(5);
      IM_CHECK_SILENT(s_App->assembly != nullptr);
      IM_CHECK_SILENT(!s_App->assembly->GetSequence().Empty());

      // The sequence checked before the camera rolls (a minute and more):
      // Play then shows the timeline it laid out.
      std::string why;
      if (!ui::StartSequencePlayback(*s_App, &why))
        ctx->LogError("play: %s", why.c_str());
      IM_CHECK_SILENT(why.empty());
      IM_CHECK(WaitFor(
          ctx, "the sequence checked",
          [] {
            const auto& sh = s_App->sequenceShow;
            return sh && (!sh->poses || sh->poses->done.load());
          },
          280.0));
      IM_CHECK_SILENT(s_App->sequenceShow->poses &&
                      s_App->sequenceShow->poses->error.empty());
      ui::StopSequencePlayback(*s_App);
      ctx->Yield(2);

      // Cut down the cylinders: the XZ plane through the bores and the
      // crank, the camera's half (-Y) cut away: `reverse` keeps y >= 0.
      ToolbarClick(ctx, "###tb_section");
      ctx->Yield(3);
      IM_CHECK_SILENT(!s_App->sections.empty());
      s_App->sections[0].basePlane = 1;
      s_App->sections[0].offset = 0.0f;
      s_App->sections[0].reverse = true;
      s_App->sectionToolOpen = false; // the cut outlives its tool window
      ctx->Yield(3);
      // The Motion window, its pace and its highlight.
      ToolbarClick(ctx, "###tb_asm_motion");
      ctx->Yield(2);
      IM_CHECK_SILENT(s_App->motionToolOpen);
      ctx->ItemInputValue("//Motion###ToolWindow/###sequence_speed",
                          kAssemblySceneSpeed);
      IM_CHECK_SILENT(s_App->sequenceShow->playSpeed > 1.0);
      IM_CHECK_SILENT(s_App->sequenceShow->playHighlight);

      // Looked at square to the cut from a little in front and above, the
      // engine left of the Motion window.
      glm::dvec3 lo, hi;
      IM_CHECK_SILENT(AssemblyBox(lo, hi));
      {
        const glm::vec3 mid((lo + hi) * 0.5);
        const float size = static_cast<float>(glm::length(hi - lo));
        // Whatever the open and the tab switch flew to, landed first.
        for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
          ctx->Yield();
        s_App->camera->StopAnimation();
        s_App->camera->SetTarget(mid);
        s_App->camera->SetPosition(mid + glm::normalize(kAssemblySceneEye) *
                                             (kAssemblySceneDistance * size));
        s_App->camera->SetUp(conv::kWorldUpF); // level: after the eye
        ctx->Yield(2);
      }
      CentreLeftOf(ctx, lo, hi, "Motion###ToolWindow");
      s_App->camera->SetUp(conv::kWorldUpF);
      ctx->Yield(2);
      ctx->MouseMoveToPos(
          ImVec2(s_App->viewportPos.x + 10.0f, s_App->viewportPos.y + 10.0f));
      ctx->Yield(3);

      BeginScene(ctx, "assembly");
      Hold(ctx, 1.2);
      ctx->ItemClick("//Motion###ToolWindow/###sequence_play");
      ctx->Yield();
      const sequence::Timeline* tl = ui::ShownSequence(*s_App);
      IM_CHECK_SILENT(tl != nullptr);
      const double seconds = tl->seconds;
      // Out of the way while it plays: on the viewport's empty corner, so
      // nothing it shows is hovered.
      ctx->MouseMoveToPos(
          ImVec2(s_App->viewportPos.x + 10.0f, s_App->viewportPos.y + 10.0f));
      if (ctx->EngineIO->ConfigRunSpeed == ImGuiTestRunSpeed_Fast) {
        // The suite: to the end at once.
        ctx->ItemInputValue("//Motion###ToolWindow/###sequence_t",
                            static_cast<float>(seconds));
      } else {
        // The interface's clock, which is the clip's: a 60th of a second
        // a frame, however long a frame takes to draw.
        const int most = SceneFrames(ctx, 2.0 * seconds) + 600;
        for (int i = 0; i < most && s_App->sequenceShow->playing; i++)
          ctx->Yield();
      }
      ctx->Yield(2);
      IM_CHECK(!s_App->sequenceShow->playing);
      IM_CHECK(ui::ShownSequence(*s_App) != nullptr); // put together
      IM_CHECK(ui::SequenceHighlighted(*s_App).empty());
      Hold(ctx, 2.0);
      EndScene(ctx);

      ui::StopSequencePlayback(*s_App);
      ui::ClearSections(*s_App);
      s_App->motionToolOpen = false;
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New"); // leave the big model behind
      ctx->Yield(2);
    };
  }
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
