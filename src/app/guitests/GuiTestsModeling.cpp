// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/guitests/GuiTestsCommon.h"
#include "cad/NonDestructiveBoolean.h"
#include "common/ChildProcess.h" // CurrentPid
#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/HeavyBuild.h"
#include "core/Measure.h"
#include "core/Primitives.h"
#include "core/PrintAllowance.h"
#include "core/SheetMetal.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CradleFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DeleteFaceFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/HemFeature.h"
#include "core/features/JogFeature.h"
#include "core/features/MoveFaceFeature.h"
#include "core/features/OffsetFaceFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/ReplaceFaceFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/io/ReadContext.h" // kMaxPartFeatures
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AlignTool.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/FeatureBuildJob.h"
#include "ui/FeatureEditor.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h" // ExportActiveDocument
#include "ui/MeasureActions.h"
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h"
#include "ui/PrimitiveTool.h"
#include "ui/PrintProfileActions.h"
#include "ui/ProjectActions.h" // ProjectDirty
#include "ui/SceneOverlays.h"
#include "ui/SettingsWindow.h" // OpenPreferences
#include "ui/TemplateActions.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"
#include "ui/ViewportHandle.h"
#include <chrono>
#include <filesystem>
#include <fstream>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Compound.hxx>
#include <cfloat>
#include <format>

#include <unistd.h> // getpid: a scratch file per shard

// On Windows, OpenCASCADE's Standard_Mutex.hxx (through
// cad/NonDestructiveBoolean.h) includes windows.h, whose winbase.h defines
// `Yield()` as a macro and breaks every ctx->Yield(n) below. Only the Windows
// build sees it (docs/packaging/README.md#windows).
#ifdef Yield
#undef Yield
#endif

// Modeling GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
namespace {

// The primitive/* base: a 10 x 6 x 2 plate on the Top plane, through the core
// tool (so its faces are named), as one undo step.
void PrimitiveTestPlate() {
  primitive::Add(*Doc(), *s_App->commands,
                 primitive::SpecFromJson({{"type", "Box"},
                                          {"on", "Top"},
                                          {"width", 10.0},
                                          {"depth", 6.0},
                                          {"height", 2.0}}));
}

// The middle of the plate's top face.
glm::vec3 PlateTop() {
  glm::dvec3 lo(0.0), hi(0.0);
  Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
  const glm::dvec3 mid = (lo + hi) * 0.5;
  const glm::dvec3 up = conv::kWorldUp;
  return glm::vec3(mid + up * glm::dot(hi - mid, up));
}

// The camera straight down the world's up axis at `p`, so a point on a
// horizontal face lands where WorldToWindow says.
void LookDownAt(ImGuiTestContext* ctx, const glm::vec3& p, float distance) {
  s_App->camera->StopAnimation();
  s_App->camera->SetTarget(p);
  s_App->camera->SetUp(-glm::vec3(conv::kWorldFront));
  s_App->camera->SetPosition(p + distance * LensScale() * conv::kWorldUpF);
  ctx->Yield(2);
}

// --- defaults/scale: where a tool's preview lands ----------------------------
//
// A default is judged by what it does on the part in front of it
// (docs/ui/tool-sessions.md#defaults-and-refusals): the preview must be in
// the view fitted to that part, and must not be a sliver of it.
struct WorldBox {
  glm::dvec3 lo{DBL_MAX}, hi{-DBL_MAX};
  bool Empty() const { return lo.x > hi.x; }
  void Add(const glm::dvec3& p) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  void Add(const WorldBox& b) {
    if (!b.Empty()) {
      Add(b.lo);
      Add(b.hi);
    }
  }
  glm::dvec3 Extent() const { return Empty() ? glm::dvec3(0.0) : hi - lo; }
};

WorldBox BoxOf(const TopoDS_Shape& shape) {
  WorldBox out;
  if (shape.IsNull())
    return out;
  Bnd_Box bb;
  BRepBndLib::AddOptimal(shape, bb, /*useTriangulation=*/false);
  if (bb.IsVoid())
    return out;
  double x0, y0, z0, x1, y1, z1;
  bb.Get(x0, y0, z0, x1, y1, z1);
  out.Add(glm::dvec3(x0, y0, z0));
  out.Add(glm::dvec3(x1, y1, z1));
  return out;
}

TopoDS_Compound CompoundOf(const std::vector<const Body*>& bodies) {
  BRep_Builder builder;
  TopoDS_Compound c;
  builder.MakeCompound(c);
  for (const Body* b : bodies)
    if (b->shape.IsValid())
      builder.Add(c, b->shape.GetShape());
  return c;
}

// The material `a` has that `b` has not, as a box (empty when none).
WorldBox CutBox(const TopoDS_Compound& a, const TopoDS_Compound& b) {
  BRepAlgoAPI_Cut cut;
  if (!cad::BuildNonDestructive(cut, a, b))
    return BoxOf(a);
  return BoxOf(cut.Shape());
}

// What the reference part is: its bodies and its sketches, the thing the
// view is fitted to before the tool opens.
WorldBox ScenePartBox() {
  WorldBox box;
  for (const Body& b : Doc()->GetBodies()) {
    glm::dvec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi, /*tight=*/true);
    box.Add(lo);
    box.Add(hi);
  }
  for (size_t i = 0; i < Doc()->FeatureCount(); i++)
    if (auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(i)))
      for (const sketch::Entity& e : sk->entities)
        for (const glm::dvec2& uv : sketch::TessellateEntity(e))
          box.Add(glm::dvec3(sk->plane.PlaneToWorld(uv)));
  return box;
}

// The pending preview's geometry: the material it adds or takes away (the
// bodies it made, where the material is only re-cut), and the reference
// geometry drawn for it. `solid` says which kind the box came from.
struct PreviewExtent {
  WorldBox box;
  bool solid = false;
};
PreviewExtent PendingPreviewExtent() {
  PreviewExtent out;
  size_t idx = 0;
  if (ui::ResolvePendingFeature(*s_App, &idx) == nullptr)
    return out;
  const std::vector<Body>* after = Doc()->CachedBodiesAfter(idx);
  const std::vector<Body>* before =
      idx > 0 ? Doc()->CachedBodiesAfter(idx - 1) : nullptr;
  std::vector<const Body*> added, gone;
  const auto among = [](const Body& b, const std::vector<Body>* list) {
    if (list != nullptr)
      for (const Body& o : *list)
        if (o.shape.SameGeometryAs(b.shape))
          return true;
    return false;
  };
  if (after != nullptr)
    for (const Body& b : *after)
      if (!among(b, before))
        added.push_back(&b);
  if (before != nullptr)
    for (const Body& b : *before)
      if (!among(b, after))
        gone.push_back(&b);
  if (!added.empty() || !gone.empty()) {
    const TopoDS_Compound a = CompoundOf(added), g = CompoundOf(gone);
    WorldBox change;
    change.Add(gone.empty() ? BoxOf(a) : CutBox(a, g));
    if (!gone.empty() && !added.empty())
      change.Add(CutBox(g, a));
    // Re-cut but not moved (a split): the pieces it made are the preview.
    if (change.Empty())
      change = BoxOf(a);
    out.box = change;
    out.solid = !change.Empty();
  }
  std::vector<LineVertex3D> drawn;
  ui::AppendDatumPlaneQuads(*s_App, drawn);
  ui::AppendDatumAxisLines(*s_App, drawn);
  ui::AppendFrameLines(*s_App, drawn);
  ui::AppendCurve3DLines(*s_App, drawn);
  for (const LineVertex3D& v : drawn)
    out.box.Add(glm::dvec3(v.position));
  return out;
}

// How far out of the fitted view a box reaches, in view half-widths at the
// part's depth: 1 is the edge of the picture, 2 one more half-width past it.
// Measured at the depth the view was fitted at rather than through the
// perspective, which is closest to an object the fit puts the eye beside
// and reads anything wider than that distance as off to infinity.
double ViewReach(const WorldBox& box) {
  const Camera& cam = *s_App->camera;
  const glm::mat4 view = cam.GetViewMatrix();
  const double halfV =
      cam.GetDistance() * std::tan(0.5 * glm::radians(double(cam.GetFOV())));
  const double halfH = halfV * std::max(1e-3f, cam.GetAspectRatio());
  const glm::vec3 pivot = glm::vec3(view * glm::vec4(cam.GetTarget(), 1.0f));
  double reach = 0.0;
  for (int c = 0; c < 8; c++) {
    const glm::dvec3 p((c & 1) ? box.hi.x : box.lo.x,
                       (c & 2) ? box.hi.y : box.lo.y,
                       (c & 4) ? box.hi.z : box.lo.z);
    const glm::vec3 v = glm::vec3(view * glm::vec4(glm::vec3(p), 1.0f)) - pivot;
    reach = std::max(
        {reach, std::abs(double(v.x)) / halfH, std::abs(double(v.y)) / halfV});
  }
  return reach;
}

} // namespace

void RegisterModelingGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "document", "parametric_box_undo_redo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(Doc() != nullptr);
    // TWO baselines. A datum plane or a sketch is a feature that produces no
    // body, so measuring bodies against the FEATURE count only holds for a
    // document containing nothing but solids — found by --gui-test-shuffle
    // putting a datum test in front of this one.
    const size_t before = Doc()->FeatureCount();
    const size_t bodiesBefore = Doc()->GetBodies().size();

    // The toolbar's Box: a sketch on the plane picked, and its extrude --
    // two features, one body, ONE undo step.
    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 1);

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 2);
  };

  // A part holds at most kMaxPartFeatures, so the app never writes one it
  // cannot open (docs/versioning/README.md#what-a-load-refuses): a tool
  // that would take it past says so, and Accept does nothing.
  t = IM_REGISTER_TEST(engine, "document",
                       "accept_refused_past_the_feature_limit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    {
      Document::BulkEdit bulk(*Doc());
      while (Doc()->FeatureCount() < readctx::kMaxPartFeatures)
        Doc()->AddFeature(std::make_unique<DatumPlaneFeature>(
            "P" + std::to_string(Doc()->FeatureCount() + 1)));
    }
    ctx->Yield();
    const size_t undo = s_App->commands->UndoDepth();
    ToolbarClick(ctx, "###tb_datum_plane");
    ctx->Yield(2);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept", ImGuiTestOpFlags_NoError);
    IM_CHECK(accept.ID != 0);
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) != 0);
    ui::AcceptPendingFeature(*s_App); // the keyboard's path, past the button
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undo);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), readctx::kMaxPartFeatures);

    // Nor does a sketch begin past it: it committed its feature without
    // asking. Refused where the plane pick is answered, which stays armed
    // and says why.
    IM_CHECK_EQ(ui::BeginSketchOnPlane(*s_App, ui::DefaultPlane(0), "S"),
                (uint64_t)0);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), readctx::kMaxPartFeatures);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undo);
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.actionMsg.find("at most 50 000 features") !=
             std::string::npos);
    s_App->sketchEdit = ui::SketchEditState{};
  };

  t = IM_REGISTER_TEST(engine, "document", "create_sphere_and_cylinder");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const size_t before = Doc()->FeatureCount();
    const size_t bodiesBefore = Doc()->GetBodies().size(); // see above

    PrimitiveOnBasePlane(ctx, "###tb_sphere", 0);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    PrimitiveOnBasePlane(ctx, "###tb_cylinder", 0);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 4); // a sketch + a solid each
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 2);
    // A sphere is a revolved half disc; a cylinder an extruded circle.
    IM_CHECK(Doc()->GetFeature(before + 1)->TypeName() == "Revolve");
    IM_CHECK(Doc()->GetFeature(before + 3)->TypeName() == "Extrude");

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
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);    // Box 1    -> index 0
    AddFixtureSphere(ctx); // Sphere 1 -> index 1
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
    AddFixtureBox(ctx);

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

  // Expression entry (docs/ui/numeric-input.md#the-widgets). Ctrl+click a
  // dimension and type an expression — the whole point of ui::ExprInput,
  // offered on every ExprDrag* field since the variables workstream, and never
  // once tested. The sketch expression tests go through the inline dimension
  // editor's own InputText and the assembly one through "##expr" after fx; this
  // path had no coverage at all, which is how the drop below survived.
  //
  // ItemInputValue IS the gesture: MouseMove, Ctrl down, click, type, Enter
  // (imgui_te_context.cpp, ImGuiTestAction_Input).
  t = IM_REGISTER_TEST(engine, "properties",
                       "dimension_field_takes_an_expression");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    // Each case starts from a LITERAL: an expression now switches the row to
    // its fx form (typed_expression_keeps_its_text), so "##Width" is only
    // there while the field holds a number and "##expr" is where the text
    // lives afterwards.
    constexpr const char* kWidth = "//Box###ToolWindow/Width/##Width";
    constexpr const char* kExpr = "//Box###ToolWindow/Width/##expr";

    // Arithmetic whose leading literal differs from what is in the field.
    // This is the one case that works today: sscanf reads 5, 5 != 2 moves the
    // value, ImGui marks the item edited, and only then does
    // ExprCommitOnLastItem get to look at the text.
    ctx->ItemInputValue(kWidth, "5*8");
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 40.0);

    // A function call. sscanf cannot parse it, so the value does not move, so
    // TempInputScalar never calls MarkItemEdited, so IsItemDeactivatedAfterEdit
    // is false and the text is discarded without a word. Same for every
    // expression that names a variable.
    ctx->ItemInputValue(kExpr, "40"); // back to a literal
    ctx->Yield(2);
    ctx->ItemInputValue(kWidth, "sqrt(100)");
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 10.0);

    // And arithmetic whose leading literal happens to EQUAL the current value
    // is dropped for the same reason — the number did not move.
    ctx->ItemInputValue(kExpr, "10"); // back to a literal, same value
    ctx->Yield(2);
    ctx->ItemInputValue(kWidth, "10/2");
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 5.0);
  };

  // Same widget, second defect: the Ctrl+click field borrows ImGui's temp
  // input, whose buffer is a 32-byte stack array (TempInputScalar). Past 31
  // characters the keystrokes are refused (imgui_widgets.cpp, BufCapacity)
  // and what commits is a PREFIX of what was typed — a different number, no
  // warning. 33 chars here; truncated it evaluates to 16 rather than 17.
  t = IM_REGISTER_TEST(engine, "properties",
                       "dimension_expression_is_not_truncated");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    std::string expr = "1";
    for (int i = 0; i < 16; i++)
      expr += "+1";
    IM_CHECK_EQ((int)expr.size(), 33);
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", expr.c_str());
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 17.0);
  };

  // The human path, which ItemInputValue does NOT exercise: it clears the
  // field with Ctrl+A first, so it would pass even if the value arrived
  // unselected and the typed text appended to it ("2" + "30" = 230).
  t = IM_REGISTER_TEST(engine, "properties", "dimension_field_opens_selected");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    ctx->ItemInput("//Box###ToolWindow/Width/##Width"); // Ctrl+click, no more
    ctx->Yield(2);
    ctx->KeyChars("30");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 30.0);
  };

  // ImGui's drag opens text entry on THREE gestures, and
  // ImGuiSliderFlags_NoInput turns off all three: Ctrl+click, double-click,
  // and Enter on a nav-focused field. Double-click is the one people find
  // without being told, and rebuilding only Ctrl+click made every numeric
  // field in the app harder to type into than before U14.
  t = IM_REGISTER_TEST(engine, "properties",
                       "dimension_field_opens_on_double_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    ctx->ItemDoubleClick("//Box###ToolWindow/Width/##Width"); // no modifier
    ctx->Yield(2);
    ctx->KeyChars("7*3");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 21.0);
  };

  // U14 phase 2: the xyz rows are our own layout now rather than ImGui's
  // DragScalarN, so each component is a full expression field — and the row
  // has to stay grouped, or IsItemActivated/IsItemDeactivatedAfterEdit stop
  // reaching HandleEdit and the edit silently stops being undoable.
  t = IM_REGISTER_TEST(engine, "properties", "vector_row_takes_an_expression");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    const float y0 = box->position.y;

    ctx->ItemInputValue("//Box###ToolWindow/##Position/##y", "3*4");
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->position.y, 12.0f);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->position.y, y0);
  };

  // U14 phase 3: the point of owning the buffer. Type part of a variable's
  // name into any numeric field and the document's variables are offered
  // with their values; Tab takes the highlighted one.
  t = IM_REGISTER_TEST(engine, "properties",
                       "dimension_field_completes_a_variable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(Doc()->SetParameter("plate_width", "42.5"));
    Doc()->Regenerate();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    // Ctrl+click, type a prefix, Tab. No completion popup existed before
    // the field owned its own text buffer — ImGui's could not be written to.
    ctx->ItemInput("//Box###ToolWindow/Width/##Width");
    ctx->Yield(2);
    ctx->KeyChars("plate_w");
    ctx->Yield(2);
    IM_CHECK(ImGui::FindWindowByName("##expr_complete") != nullptr);
    ctx->KeyPress(ImGuiKey_Tab);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 42.5);

    // And it completes INSIDE an expression, not just as the whole of one.
    // (Through ##expr: the row is in its fx form now, holding the text.)
    ctx->ItemInputValue("//Box###ToolWindow/Width/##expr", "12");
    ctx->Yield(2);
    ctx->ItemInput("//Box###ToolWindow/Width/##Width");
    ctx->Yield(2);
    ctx->KeyChars("2*plate_w");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Tab);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 85.0);
  };

  // A dimension typed as an expression KEEPS ITS TEXT. Evaluating and
  // discarding was defensible while nothing could type a variable into these
  // fields; once completion offers them, a field that collapses "plate_width"
  // to the 42.5 it evaluated to is silently dropping the link — and the next
  // edit edits 42.5.
  t = IM_REGISTER_TEST(engine, "properties", "typed_expression_keeps_its_text");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(Doc()->SetParameter("plate_width", "42.5"));
    Doc()->Regenerate();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", "plate_width*2");
    ctx->Yield(3);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_STR_EQ(box->widthExpr.c_str(), "plate_width*2");
    IM_CHECK_EQ(box->width, 85.0);

    // ...so the variable still drives it.
    IM_CHECK(Doc()->SetParameter("plate_width", "10"));
    Doc()->Regenerate();
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 20.0);

    // A plain number stays a literal — expressions are still opt-in per edit.
    ctx->ItemInputValue("//Box###ToolWindow/Width/##expr", "7");
    ctx->Yield(3);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK(box->widthExpr.empty());
    IM_CHECK_EQ(box->width, 7.0);
  };

  // ...and the row is CLICKABLE, which is the part that looked impossible.
  // ImGui deactivates an active InputText on ANY outside click, in the same
  // frame and unconditionally, so the click that picks a row is also the
  // click that ends the edit. Since the buffer is ours that is recoverable:
  // the popup is drawn before the field and reports the pick first, so the
  // deactivation can be read as "not a commit" and the field re-focused.
  t = IM_REGISTER_TEST(engine, "properties", "completion_row_can_be_clicked");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(Doc()->SetParameter("plate_width", "42.5"));
    Doc()->Regenerate();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();

    ctx->ItemInput("//Box###ToolWindow/Width/##Width");
    ctx->Yield(2);
    ctx->KeyChars("plate_w");
    ctx->Yield(2);
    ctx->ItemClick("//##expr_complete/plate_width##0");
    ctx->Yield(3);
    // The field is still open — a pick is not a commit — so Enter is what
    // applies it.
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 42.5);
  };

  // The same completion in the fx field, which STORES the text rather than
  // its value — a different widget with its own commit rules, sharing the
  // popup. Its trap: a pick deactivates and re-focuses the field, and the
  // re-focus must not read as the user entering it (which would re-seed the
  // buffer from the model and throw the completion away).
  t = IM_REGISTER_TEST(engine, "properties", "fx_field_completes_a_variable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(Doc()->SetParameter("plate_width", "42.5"));
    Doc()->Regenerate();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemClick("//Box###ToolWindow/Width/fx");
    ctx->Yield(2);

    ctx->ItemInput("//Box###ToolWindow/Width/##expr");
    ctx->Yield(2);
    ctx->KeyChars("plate_w");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Tab);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(3);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_STR_EQ(box->widthExpr.c_str(), "plate_width");
    IM_CHECK_EQ(box->width, 42.5);
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_accept_commits_once");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
    // Pending: the sketch and its extrude are in the doc + rendered, but NOT
    // on the undo stack yet
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(!s_App->commands->CanUndo());

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(s_App->commands->CanUndo()); // now one undoable action

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // both, in one step
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_cancel_discards");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
    IM_CHECK(ui::HasPendingFeature(*s_App));

    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // the sketch went too
    IM_CHECK(!s_App->commands->CanUndo());         // stack untouched
  };

  t = IM_REGISTER_TEST(engine, "preview", "ctrl_z_cancels_pending");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
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
  t = IM_REGISTER_TEST(engine, "preview", "tab_switch_discards_pending");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2); // the preview, both halves

    ui::AddPart(*s_App, "Part 2"); // stashes the first tab
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));

    ui::SetActivePart(*s_App, 0); // back to the tab that had the preview
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // not stranded
    IM_CHECK(!s_App->commands->CanUndo());         // and never committed
  };

  // The primitive tools (docs/ui/tool-sessions.md#primitives): a Box is a
  // sketch on what it stands on plus its extrude, and the toolbar's way to
  // one is pick-the-face-first. The base is made through the core tool, so
  // its faces are named and its top face has a known outline: 10 x 6, 2
  // thick, centred on the origin on the Top plane.
  // Feature templates (docs/modeling/templates.md#in-the-gui): the plate's
  // own sketch and extrude saved through the tree's Save as template window,
  // placed by the Insert template tool on the plate's top face (a viewport
  // pick), the row marked when the template changes and brought up to date
  // from its menu, then unlinked into plain features with the same geometry
  // -- and the unlink undone.
  t = IM_REGISTER_TEST(engine, "templates", "save_insert_update_unlink");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    PrimitiveTestPlate(); // a sketch on Top and its extrude, 10 x 6 x 2
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    const auto row = [&](size_t i) {
      return "//###Model/" + Doc()->GetFeature(i)->GetName() + "##" +
             std::to_string(i);
    };
    // Both rows, then the context menu's Save as template.
    ctx->ItemClick(row(0).c_str());
    ctx->KeyDown(ImGuiMod_Shift);
    ctx->ItemClick(row(1).c_str());
    ctx->KeyUp(ImGuiMod_Shift);
    ctx->Yield();
    ctx->ItemClick(row(1).c_str(), ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_save_template");
    ctx->Yield(2);
    auto& save = s_App->saveTemplate;
    IM_CHECK(save.open);
    IM_CHECK_EQ(save.featureIds.size(), (size_t)2);
    // The extrude's height is a literal: offered, and taken as `height`.
    bool offered = false;
    for (auto& r : save.rows)
      if (r.candidate.featureId == Doc()->GetFeature(1)->GetId() &&
          r.candidate.path == "distance") {
        r.use = true;
        r.param = "height";
        offered = true;
      }
    IM_CHECK(offered);
    save.name = "post";
    ctx->ItemClick("//Save as template###SaveTemplate/###tpl_save");
    ctx->Yield();
    IM_CHECK_STR_EQ(save.error.c_str(), "");
    const tmpl::Template* t = tmpl::Find(s_App->projectTemplates, "post");
    IM_CHECK(t != nullptr);
    IM_CHECK(t->FindParam("height") != nullptr);
    IM_CHECK(t->FindParam("on") != nullptr);
    ctx->ItemClick("//Save as template###SaveTemplate/###tpl_close");
    ctx->Yield();
    IM_CHECK(!save.open);

    // Insert: the pick, then the plate's top face in the viewport.
    const glm::vec3 top = PlateTop();
    LookDownAt(ctx, top, 20.0f);
    const double plate = Doc()->GetBodies()[0].shape.Volume();
    ToolbarClick(ctx, "###tb_template");
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Template);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    size_t idx = 0;
    auto* pending =
        dynamic_cast<TemplateFeature*>(ui::ResolvePendingFeature(*s_App, &idx));
    IM_CHECK(pending != nullptr);
    IM_CHECK(pending->GetError().empty());
    IM_CHECK(pending->values.at("on").ref.contains("element"));
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)3);
    auto* instance = dynamic_cast<TemplateFeature*>(Doc()->GetFeature(2));
    IM_CHECK(instance != nullptr);
    IM_CHECK(instance->GetError().empty());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2); // a second post
    const uint64_t instanceId = instance->GetId();
    IM_CHECK(!ui::TemplateChanged(*s_App, *instance).value_or(true));

    // The template changes (saved again, 1 mm taller): the row says so and
    // nothing moves until its menu's Update from template.
    const double before = Doc()->GetBodies()[1].shape.Volume();
    {
      tmpl::SaveSpec spec;
      spec.name = "post";
      spec.featureIds = {Doc()->GetFeature(0)->GetId(),
                         Doc()->GetFeature(1)->GetId()};
      tmpl::Template next = tmpl::FromFeatures(*Doc(), spec);
      // The source as it is, with its height one more: a revision.
      next.features[1]["distance"] = 3.0;
      next.revision = t->revision + 1;
      for (tmpl::Template& x : s_App->projectTemplates)
        if (x.name == "post")
          x = next;
    }
    ctx->Yield();
    instance = dynamic_cast<TemplateFeature*>(Doc()->GetFeature(2));
    IM_CHECK(ui::TemplateChanged(*s_App, *instance).value_or(false));
    IM_CHECK(std::abs(Doc()->GetBodies()[1].shape.Volume() - before) < 1e-9);
    ctx->ItemClick(row(2).c_str(), ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_template_update");
    ctx->Yield(2);
    instance = dynamic_cast<TemplateFeature*>(Doc()->GetFeature(2));
    IM_CHECK(instance != nullptr);
    IM_CHECK(!ui::TemplateChanged(*s_App, *instance).value_or(true));
    IM_CHECK(instance->GetError().empty());
    const double taller = Doc()->GetBodies()[1].shape.Volume();
    IM_CHECK(std::abs(taller - before * 1.5) < 1e-6);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - plate) < 1e-9);

    // Unlink: plain features, the same geometry; one Ctrl+Z gives the
    // instance back.
    const std::vector<uint64_t> owned = {instance->IdOf(1), instance->IdOf(2)};
    ctx->ItemClick(row(2).c_str(), ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_template_unlink");
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)4);
    IM_CHECK(Doc()->FindFeatureById(instanceId) == nullptr);
    IM_CHECK_EQ(Doc()->GetFeature(2)->GetId(), owned[0]);
    IM_CHECK_EQ(Doc()->GetFeature(3)->GetId(), owned[1]);
    IM_CHECK(std::abs(Doc()->GetBodies()[1].shape.Volume() - taller) < 1e-9);
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)3);
    IM_CHECK(Doc()->FindFeatureById(instanceId) != nullptr);
  };

  // With nothing in the library the tool still opens its pick -- the window
  // is where the refusal is read -- and a pick places nothing.
  t = IM_REGISTER_TEST(engine, "templates",
                       "insert_refuses_with_an_empty_library");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(s_App->projectTemplates.empty());
    const size_t count = Doc()->FeatureCount();
    ToolbarClick(ctx, "###tb_template");
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.actionMsg.find("no templates") !=
             std::string::npos);
    IM_CHECK(ctx->ItemExists("//###ToolWindow"));
    ctx->ItemClick((std::string("//##DockHost/**/") +
                    SketchPlane::StandardPlaneName(PlaneType::XY))
                       .c_str());
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.pickingPlane); // still armed, still saying why
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), count);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
  };

  t = IM_REGISTER_TEST(engine, "primitive", "face_pick_drag_snap_accept");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    PrimitiveTestPlate();
    const size_t base = Doc()->FeatureCount();
    const glm::vec3 top = PlateTop();
    // Looking down at a point to the right of and above the plate, so the
    // plate sits low on the left: the drags below must stay clear of the tool
    // window, which opens top-right.
    LookDownAt(ctx, top, 20.0f);
    const glm::vec3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f);
    const glm::vec3 right =
        WorldToWindow(top + x).x > WorldToWindow(top).x ? x : -x;
    const glm::vec3 up =
        WorldToWindow(top + y).y < WorldToWindow(top).y ? y : -y;
    LookDownAt(ctx, top + 4.0f * right + 2.0f * up, 20.0f);

    ToolbarClick(ctx, "###tb_box");
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Primitive);
    // Off-centre on the face: where on it is clicked is not where it goes.
    ctx->MouseMoveToPos(WorldToWindow(top + glm::vec3(2.5f, 1.5f, 0.0f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);

    // The ghost is on the face: a pending extrude of a face-attached sketch,
    // sized from the face (half its smaller span, 6 -> 3), at its centre.
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK(ui::PrimitiveSessionActive(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), base + 2);
    const auto& session = s_App->primitive;
    IM_CHECK(session.spec.on.kind == primitive::OnKind::Face);
    IM_CHECK(session.placement.hasExtent);
    IM_CHECK(std::abs(session.spec.width.value - 3.0) < 1e-9);
    IM_CHECK(std::abs(session.spec.height.value - 3.0) < 1e-9);
    IM_CHECK_EQ(session.spec.offsetU.value, 0.0);
    IM_CHECK_EQ(session.spec.offsetV.value, 0.0);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(base));
    IM_CHECK(sk != nullptr);
    IM_CHECK(sk->faceRef.IsSet());

    // Drag it in the face's frame. The cursor positions are points on the
    // face's plane, so the offset they ask for is exact up to the pick.
    const SketchPlane plane = session.placement.plane;
    const glm::dvec2 hi = session.placement.hi;
    const auto at = [&](double u, double v) {
      return WorldToWindow(glm::vec3(plane.PlaneToWorld(glm::dvec2(u, v))));
    };
    // Grabbed off its middle: looking straight down, the middle is where
    // its height arrow stands, and the arrow wins a tie.
    const glm::dvec2 grab(1.0, -1.0);
    // The offset follows the cursor from where it was grabbed.
    const auto to = [&](double u, double v) {
      return at(grab.x + u, grab.y + v);
    };
    ctx->MouseMoveToPos(at(grab.x, grab.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(session.dragging);
    // Somewhere free: neither axis near the centre or an edge line.
    ctx->MouseMoveToPos(to(0.8, 0.7));
    ctx->Yield(2);
    IM_CHECK(session.snapU.empty() && session.snapV.empty());
    IM_CHECK(std::abs(session.spec.offsetU.value - 0.8) < 0.15);
    IM_CHECK(std::abs(session.spec.offsetV.value - 0.7) < 0.15);
    // Its side near the face's +u edge: flush, exactly.
    ctx->MouseMoveToPos(to(hi.x - 1.5 - 0.05, 0.7));
    ctx->Yield(2);
    IM_CHECK_STR_EQ(session.snapU.c_str(), "flush");
    IM_CHECK(std::abs(session.spec.offsetU.value - (hi.x - 1.5)) < 1e-9);
    // Near the middle: the face's centre, exactly, on both axes.
    ctx->MouseMoveToPos(to(0.06, -0.05));
    ctx->Yield(2);
    IM_CHECK_STR_EQ(session.snapU.c_str(), "centre");
    IM_CHECK_STR_EQ(session.snapV.c_str(), "centre");
    IM_CHECK_EQ(session.spec.offsetU.value, 0.0);
    IM_CHECK_EQ(session.spec.offsetV.value, 0.0);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!session.dragging);

    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), base + 2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(base));
    IM_CHECK(sk != nullptr);
    IM_CHECK(sk->faceRef.IsSet());
    IM_CHECK(sk->GetError().empty());
    sketch::Solver solver;
    solver.Load(*sk);
    const sketch::SolveResult r = solver.Solve();
    IM_CHECK(r.ok);
    IM_CHECK_EQ(r.dof, 0);
    Feature* solid = Doc()->GetFeature(base + 1);
    IM_CHECK(solid->TypeName() == "Extrude");
    IM_CHECK(solid->GetError().empty());
    double made = 0.0;
    for (const Body& b : Doc()->GetBodies())
      if (b.createdBy == solid->GetId())
        made = b.shape.Volume();
    IM_CHECK(std::abs(made - 27.0) < 1e-6); // 3 x 3 x 3

    ctx->MenuClick("Edit/Undo"); // one step takes both
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), base);
  };

  // Escape during the preview: nothing was made, nothing is on the stack.
  t = IM_REGISTER_TEST(engine, "primitive", "escape_leaves_document_unchanged");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    PrimitiveTestPlate();
    const ui::DocumentFingerprint before = ui::FingerprintOf(*Doc());
    const size_t undoDepth = s_App->commands->UndoDepth();
    const glm::vec3 top = PlateTop();
    LookDownAt(ctx, top, 16.0f);

    ToolbarClick(ctx, "###tb_cylinder");
    ctx->MouseMoveToPos(WorldToWindow(top + glm::vec3(1.0f, 1.0f, 0.0f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(3);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK(!(ui::FingerprintOf(*Doc()) == before)); // the ghost is there

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK(ui::FingerprintOf(*Doc()) == before);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoDepth);
  };

  // A curved face cannot carry a sketch: refused where the user is looking,
  // and the tool stays armed for a flat one.
  t = IM_REGISTER_TEST(engine, "primitive", "curved_face_refused");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureCylinder(ctx); // upright at x = -4, r 0.8, spanning -1..1
    ctx->Yield();
    const size_t base = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_box");
    const glm::vec3 axis(-4.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(axis);
    s_App->camera->SetUp(conv::kWorldUpF);
    s_App->camera->SetPosition(axis + 10.0f * glm::vec3(conv::kWorldFront));
    ctx->Yield(2);
    ctx->MouseMoveToPos(
        WorldToWindow(axis + 0.8f * glm::vec3(conv::kWorldFront)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane); // still armed
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Primitive);
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), base);       // nothing created
    IM_CHECK(!s_App->sketchEdit.actionMsg.empty()); // and it says why
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(!s_App->sketchEdit.pickingPlane);
  };

  // Select-first: with a flat face already picked, the tool skips the pick.
  t = IM_REGISTER_TEST(engine, "primitive", "selected_face_is_the_reference");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    PrimitiveTestPlate();
    const glm::vec3 top = PlateTop();
    LookDownAt(ctx, top, 16.0f);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->selection.First(ui::SelectionItem::Kind::Face) != nullptr);

    ToolbarClick(ctx, "###tb_sphere");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane); // no second pick asked for
    IM_CHECK(ui::PrimitiveSessionActive(*s_App));
    IM_CHECK(s_App->primitive.spec.on.kind == primitive::OnKind::Face);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // The window's fields rewrite the preview: a typed width is the sketch's
  // dimension and the solid's size, before anything is accepted.
  t = IM_REGISTER_TEST(engine, "primitive", "fields_rewrite_the_preview");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    PrimitiveOnBasePlane(ctx, "###tb_box", 0);
    IM_CHECK(ui::PrimitiveSessionActive(*s_App));
    const double d = s_App->primitive.spec.depth.value;
    const double h = s_App->primitive.spec.height.value;
    ctx->ItemInputValue("//###ToolWindow/Width/##Width", 7.0f);
    ctx->Yield(2);
    IM_CHECK(std::abs(s_App->primitive.spec.width.value - 7.0) < 1e-9);
    IM_CHECK(Doc()->GetBodies().size() == 1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 7.0 * d * h) <
             1e-6 * 7.0 * d * h);
    // An expression keeps its text and drives the value.
    ctx->ItemInputValue("//###ToolWindow/Height/##Height", "2*3");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(s_App->primitive.spec.height.expr.c_str(), "2*3");
    IM_CHECK(std::abs(s_App->primitive.spec.height.value - 6.0) < 1e-9);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(Doc()->GetFeature(1));
    IM_CHECK(ext != nullptr);
    IM_CHECK_STR_EQ(ext->distanceExpr.c_str(), "2*3");
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 7.0 * d * 6.0) <
             1e-6 * 7.0 * d * 6.0);
  };

  // U9: the Hole tool. A hole is NOT sugar for an extruded cut (its tool is
  // built from primitives, so counterbore/countersink are impossible as a
  // cut), and before this it could only be created over the script API.
  t = IM_REGISTER_TEST(engine, "hole", "sk_hole_button_on_existing_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // something to drill into
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
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);
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
    AddFixtureCylinder(ctx);
    ctx->Yield();
    const size_t base = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_hole");
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    IM_CHECK(s_App->sketchEdit.purpose == ui::SketchPurpose::Hole);

    // The default cylinder stands along world up at x = -4 (NextSpot):
    // radius 0.8, spanning -1..1 along up. Look at it from the front so the
    // lateral face fills the middle of the view (a model face always
    // outranks the base planes).
    const glm::vec3 axis(-4.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(axis);
    s_App->camera->SetUp(conv::kWorldUpF);
    s_App->camera->SetPosition(axis + 10.0f * glm::vec3(conv::kWorldFront));
    ctx->Yield(2);
    ctx->MouseMoveToPos(
        WorldToWindow(axis + 0.8f * glm::vec3(conv::kWorldFront)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.pickingPlane); // still armed
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(Doc()->FeatureCount(), base);       // nothing created
    IM_CHECK(!s_App->sketchEdit.actionMsg.empty()); // and it says why

    // The flat top cap is accepted.
    s_App->camera->SetTarget(axis + conv::kWorldUpF);
    s_App->camera->SetUp(-glm::vec3(conv::kWorldFront));
    s_App->camera->SetPosition(axis + 11.0f * conv::kWorldUpF);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(axis + conv::kWorldUpF));
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
    AddFixtureBox(ctx);
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    // Straight down at the middle of the box's top face. Primitives are
    // placed by their min corner, so this must come from the bounding box —
    // a guessed centre lands on a corner edge, which outranks the face.
    glm::dvec3 lo(0.0), hi(0.0);
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
    AddFixtureBox(ctx);
    ctx->Yield();

    glm::dvec3 lo(0.0), hi(0.0);
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
      if (tc->expect == Expect::PickThenChanges) {
        // It asks for a face/plane first -- and given one (the Top plane of
        // an EMPTY part, which has nothing of its own to measure), its
        // defaults make something: the size comes from the scene's reach,
        // not a literal.
        IM_CHECK(s_App->sketchEdit.pickingPlane);
        IM_CHECK(!ui::HasPendingFeature(*s_App));
        ctx->ItemClick((std::string("//##DockHost/**/") +
                        SketchPlane::StandardPlaneName(PlaneType::XY))
                           .c_str());
        ctx->Yield(2);
        IM_CHECK(!s_App->sketchEdit.pickingPlane);
      }

      // Whatever else it is, the click must have DONE something. A toolbar
      // button that is enabled and silently does nothing is the same bug in
      // its earliest form. A session that adds its feature only on Accept
      // (the Fit tool) is open instead.
      IM_CHECK(ui::HasPendingFeature(*s_App) || s_App->fitTool.open);

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
      if (tc->expect == Expect::Changes ||
          tc->expect == Expect::PickThenChanges)
        IM_CHECK(!(ui::FingerprintOf(*Doc()) == before));
    };
  }

  // Every creation tool that opens a preview on its own defaults, on the
  // same setup at two sizes -- a 2 mm part and a 300 mm one. With the view
  // fitted to the part, the preview must land in it (within half a picture
  // past its edges: a pattern's copies beside the part are fine, a helix
  // three times its size or an axis off through the origin is not), and
  // must not be a sliver of it (no thinner than a hundredth of the part).
  // Both are measured, and logged, at both sizes before the test fails.
  // A literal default passes the defaults/* gate at any one size and fails
  // here at the other (docs/ui/tool-sessions.md#defaults-and-refusals).
  for (const ToolCase& tc : kToolCases) {
    if (tc.expect == Expect::ArmsAPick ||
        tc.expect == Expect::RefusesUntilPicked)
      continue;
    t = IM_REGISTER_TEST(engine, "defaults_scale", tc.name);
    t->TestFunc = [](ImGuiTestContext* ctx) {
      const ToolCase* tc = FindToolCase(ctx->Test->Name);
      IM_CHECK_SILENT(tc != nullptr);
      // The fitted view and three quarters of a picture past either edge: a
      // pattern's third copy reaches 2.05 of these.
      constexpr double kOutOfView = 2.5;
      std::vector<std::string> misses;
      for (const float k : {1.0f, 150.0f}) {
        const double size = 2.0 * k; // the fixture box's, and the setup's
        ctx->SetRef("//##MainMenuBar");
        ctx->MenuClick("File/New");
        ctx->Yield();
        SetupFor(ctx, tc->needs, k);
        if (tc->needs == Needs::Nothing)
          AddFixtureBox(ctx, k); // something to be the size of
        const WorldBox part = ScenePartBox();
        IM_CHECK_SILENT(!part.Empty());
        ui::FitCameraToWorldBox(*s_App, glm::vec3(part.lo), glm::vec3(part.hi));
        s_App->camera->FinishAnimation();
        ctx->Yield(2);

        ToolbarClick(ctx, tc->id);
        ctx->Yield();
        if (tc->expect == Expect::PickThenChanges) {
          IM_CHECK(s_App->sketchEdit.pickingPlane);
          ctx->ItemClick((std::string("//##DockHost/**/") +
                          SketchPlane::StandardPlaneName(PlaneType::XY))
                             .c_str());
          ctx->Yield(2);
        }
        IM_CHECK(ui::HasPendingFeature(*s_App));
        Feature* pending = ui::ResolvePendingFeature(*s_App);
        IM_CHECK(pending != nullptr);
        IM_CHECK_STR_EQ(pending->GetError().c_str(), "");
        const PreviewExtent preview = PendingPreviewExtent();
        if (tc->expect != Expect::HarmlessNoOp) {
          IM_CHECK(!preview.box.Empty());
          const glm::dvec3 ext = preview.box.Extent();
          // A solid is a sliver by its thinnest side; reference geometry (a
          // line, a plane, a curve) by its longest.
          const double measure = preview.solid
                                     ? std::min({ext.x, ext.y, ext.z})
                                     : std::max({ext.x, ext.y, ext.z});
          const double reach = ViewReach(preview.box);
          ctx->LogInfo("%s at %g: preview %g x %g x %g, %s %g, view reach %.2f",
                       tc->name, size, ext.x, ext.y, ext.z,
                       preview.solid ? "thinnest" : "longest", measure, reach);
          if (measure < size / 100.0)
            misses.push_back(std::format("at {:g}: a sliver ({:g} < {:g})",
                                         size, measure, size / 100.0));
          if (reach > kOutOfView)
            misses.push_back(std::format(
                "at {:g}: out of the fitted view ({:.2f} half-widths)", size,
                reach));
        }
        ui::CancelPendingFeature(*s_App);
        ctx->Yield();
      }
      for (const std::string& m : misses)
        ctx->LogError("%s %s", tc->name, m.c_str());
      IM_CHECK(misses.empty());
    };
  }

  // A symmetric extrude is the tool's End choice, not a key of its own
  // (docs/modeling/README.md#extrude): from its defaults, End = Symmetric
  // in the tool window sweeps half the distance each way from the sketch
  // plane, and accepts.
  t = IM_REGISTER_TEST(engine, "defaults", "extrude_symmetric_from_the_tool");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddRectSketch(ctx, 1, 1.0f, /*centreU=*/3.0f);
    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###End/Symmetric");
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(2);
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(ext != nullptr);
    IM_CHECK(ext->endMode == ExtrudeEnd::Symmetric);
    IM_CHECK(ext->GetError().empty());
    IM_CHECK(ext->distance > 0.0);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // The sketch plane runs through the origin: along its normal the body
    // reaches distance/2 either side of it. (Along the normal, not "on one
    // axis": the distance opens at the profile's own width, so a square
    // profile spans it in the plane too.)
    auto* sk =
        dynamic_cast<SketchFeature*>(Doc()->FindFeatureById(ext->sketchId));
    IM_CHECK(sk != nullptr);
    const glm::dvec3 n = sk->plane.GetNormal();
    int a = 0;
    for (int i = 1; i < 3; i++)
      if (std::abs(n[i]) > std::abs(n[a]))
        a = i;
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    IM_CHECK(std::abs(hi[a] - lo[a] - ext->distance) < 1e-6);
    IM_CHECK(std::abs(hi[a] - ext->distance * 0.5) < 1e-6);
    IM_CHECK(std::abs(lo[a] + ext->distance * 0.5) < 1e-6);
  };

  // U10: a tool that runs cleanly and changes nothing says so — and is still
  // acceptable, because Refine on an already-clean body is a fair answer.
  t = IM_REGISTER_TEST(engine, "defaults", "no_op_warns_without_blocking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // a clean box: nothing to merge
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
    AddFixtureBox(ctx);
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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
  t = IM_REGISTER_TEST(engine, "shell", "open_face_pick_hollows_the_box");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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

  // Fillet, Chamfer and Shell open at a size taken from the body (a tenth of
  // its thinnest extent, the Shell of its thinnest wall -- on this box the
  // same -- rounded to 1/2/5), not a literal: 0.5 mm was a near-invisible
  // fillet on a 300 mm part and too big for a small one.
  t = IM_REGISTER_TEST(engine, "defaults", "blend_and_wall_scale_with_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    struct Case {
      double x, y, z, expect;
    };
    for (const Case c : {Case{300, 200, 50, 5.0}, Case{2, 2, 2, 0.2}}) {
      ResetAppForTest();
      ctx->Yield();
      s_App->commands->Execute(
          std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>(
              "Box", glm::vec3(0.0f), c.x, c.y, c.z)),
          *Doc());
      ctx->Yield(2);
      for (const char* tb : {"###tb_fillet", "###tb_chamfer"}) {
        ToolbarClick(ctx, tb);
        ctx->Yield();
        auto* fil = dynamic_cast<FilletFeature*>(
            Doc()->FindFeatureById(s_App->pendingFeatureId));
        IM_CHECK(fil != nullptr);
        IM_CHECK(std::abs(fil->radius - c.expect) < 1e-9);
        ui::CancelPendingFeature(*s_App);
        ctx->Yield();
      }
      ToolbarClick(ctx, "###tb_shell");
      ctx->Yield();
      auto* sh = dynamic_cast<ShellFeature*>(
          Doc()->FindFeatureById(s_App->pendingFeatureId));
      IM_CHECK(sh != nullptr);
      IM_CHECK(std::abs(sh->thickness - c.expect) < 1e-9);
      ui::CancelPendingFeature(*s_App);
      ctx->Yield();
    }
  };

  // The same sizes on a body no "producer" made -- here the only body is a
  // sweep's. The tools measured nothing there and opened at literals (a
  // 0.5 fillet, a 0.25 wall, a 1.0 offset, a 1.0 rib on a 300 mm part).
  t = IM_REGISTER_TEST(engine, "defaults", "blend_and_wall_on_a_swept_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    SetupFor(ctx, Needs::ProfileAndPath, 150.0f); // 300 profile, 900 path
    ToolbarCreateAccept(ctx, "###tb_sweep");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    const double expect = 20.0; // a tenth of 300, rounded down to 1/2/5
    for (const char* tb : {"###tb_fillet", "###tb_chamfer"}) {
      ToolbarClick(ctx, tb);
      ctx->Yield();
      auto* fil = dynamic_cast<FilletFeature*>(
          Doc()->FindFeatureById(s_App->pendingFeatureId));
      IM_CHECK(fil != nullptr);
      IM_CHECK_EQ(fil->radius, expect);
      ui::CancelPendingFeature(*s_App);
      ctx->Yield();
    }
    ToolbarClick(ctx, "###tb_shell");
    ctx->Yield();
    auto* sh = dynamic_cast<ShellFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sh != nullptr);
    IM_CHECK_EQ(sh->bodyId, Doc()->GetBodies()[0].createdBy);
    IM_CHECK_EQ(sh->thickness, expect);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    ToolbarClick(ctx, "###tb_offset_face");
    ctx->Yield();
    auto* off = dynamic_cast<OffsetFaceFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(off != nullptr);
    IM_CHECK_EQ(off->distance, expect);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // A cut opens THROUGH the part: switching an extrude to Cut in the tool
  // takes the part's depth along the sketch's normal, where the profile's
  // width (the Add default) or a fixed millimetre left a skin or a scratch.
  t = IM_REGISTER_TEST(engine, "defaults", "extrude_cut_goes_through_the_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx, 150.0f); // a 300 mm cube
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi, /*tight=*/true);
    const double solid = Doc()->GetBodies()[0].shape.Volume();
    // On the bottom face, looking up into the part: a 60 mm square.
    const glm::dvec3 c = 0.5 * (lo + hi);
    ui::BeginSketchOnPlane(*s_App,
                           SketchPlane(glm::dvec3(c.x, c.y, lo.z),
                                       glm::dvec3(0, 0, 1),
                                       glm::dvec3(1, 0, 0)),
                           "Pocket");
    ctx->Yield();
    if (s_App->camera)
      s_App->camera->FinishAnimation();
    auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(sk != nullptr);
    const size_t index = Doc()->FeatureCount() - 1;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const float h = 30.0f;
    sk->AddEntity(sketch::Line{{-h, -h}, {h, -h}});
    sk->AddEntity(sketch::Line{{h, -h}, {h, h}});
    sk->AddEntity(sketch::Line{{h, h}, {-h, h}});
    sk->AddEntity(sketch::Line{{-h, h}, {-h, -h}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
    ctx->Yield();
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();

    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(ext != nullptr);
    IM_CHECK(ext->mode == ExtrudeMode::Add);
    IM_CHECK_EQ(ext->distance, 50.0); // the 60 mm profile, rounded down
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###Mode/Cut");
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(2);
    IM_CHECK(ext->mode == ExtrudeMode::Cut);
    IM_CHECK(std::abs(ext->distance - (hi.z - lo.z)) < 1e-6);
    IM_CHECK(ext->GetError().empty());
    size_t idx = 0;
    IM_CHECK(ui::ResolvePendingFeature(*s_App, &idx) != nullptr);
    const std::vector<Body>* cut = Doc()->CachedBodiesAfter(idx);
    IM_CHECK(cut != nullptr && cut->size() == 1);
    const double hole = 60.0 * 60.0 * (hi.z - lo.z);
    IM_CHECK(std::abs((*cut)[0].shape.Volume() - (solid - hole)) <
             1e-6 * solid);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // A datum axis stands in the part it is for, not at the world origin
  // where a part built anywhere else is not.
  t = IM_REGISTER_TEST(engine, "defaults", "datum_axis_through_the_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx, 150.0f); // 300 mm, at x = -600..-300
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi, /*tight=*/true);
    ToolbarClick(ctx, "###tb_datum_axis");
    ctx->Yield();
    auto* ax = dynamic_cast<DatumAxisFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(ax != nullptr);
    IM_CHECK(ax->GetError().empty());
    const glm::dvec3 o = ax->Origin();
    for (int a = 0; a < 3; a++) {
      IM_CHECK_GE(o[a], lo[a]);
      IM_CHECK_LE(o[a], hi[a]);
    }
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // Picking the face sizes a thread: the metric size that face carries
  // (a boss's major diameter), not the M8 the tool opened with, which fits
  // only an M8 and refused on anything else.
  t = IM_REGISTER_TEST(engine, "defaults", "thread_takes_the_faces_size");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    struct Case {
      double radius;
      const char* expect;
    };
    for (const Case c : {Case{0.8, "M1.6"}, Case{10.0, "M20"}}) {
      ctx->MenuClick("File/New");
      ctx->Yield();
      s_App->commands->Execute(
          std::make_unique<AddFeatureCommand>(std::make_unique<CylinderFeature>(
              "Boss", glm::vec3(0.0f), conv::kWorldUpF, c.radius,
              4.0 * c.radius)),
          *Doc());
      ctx->Yield(2);
      const Body& body = Doc()->GetBodies()[0];
      std::string side;
      for (const auto& [i, name] : body.elementMap.names) {
        const auto el =
            Measure::Describe(Doc()->GetBodies(), {body.createdBy, name});
        if (el && el->shape == "cylinder")
          side = name;
      }
      IM_CHECK(!side.empty());
      ToolbarClick(ctx, "###tb_thread");
      ctx->Yield();
      auto* th = dynamic_cast<ThreadFeature*>(
          Doc()->FindFeatureById(s_App->pendingFeatureId));
      IM_CHECK(th != nullptr);
      ui::SlotPickClicked(*s_App, {ui::SelectionItem::Kind::Face, 0,
                                   ElementRef{body.createdBy, side}});
      ctx->Yield(2);
      IM_CHECK_STR_EQ(th->designation.c_str(), c.expect);
      IM_CHECK_STR_EQ(th->GetError().c_str(), "");
      ui::CancelPendingFeature(*s_App);
      ctx->Yield();
    }
  };

  // Without stock declared, a flange, a hem and a jog have no length to
  // derive: each opens refused, naming the rules (as a base flange does),
  // offers no made-up length, and leaves the document as it was.
  t = IM_REGISTER_TEST(engine, "defaults", "sheet_tools_without_a_gauge");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    IM_CHECK(!Doc()->SheetMetal());
    const ui::DocumentFingerprint before = ui::FingerprintOf(*Doc());
    for (const char* tb : {"###tb_edge_flange", "###tb_hem", "###tb_jog"}) {
      ToolbarClick(ctx, tb);
      ctx->Yield(2);
      Feature* f = Doc()->FindFeatureById(s_App->pendingFeatureId);
      IM_CHECK(f != nullptr);
      IM_CHECK(f->GetError().find("sheet-metal rules") != std::string::npos);
      IM_CHECK((ctx->ItemInfo("//###ToolWindow/###tw_accept").ItemFlags &
                ImGuiItemFlags_Disabled) != 0);
      double length = -1.0;
      if (auto* ef = dynamic_cast<EdgeFlangeFeature*>(f))
        length = ef->length;
      else if (auto* hem = dynamic_cast<HemFeature*>(f))
        length = hem->length;
      else if (auto* jog = dynamic_cast<JogFeature*>(f))
        length = jog->height;
      IM_CHECK_EQ(length, 0.0);
      ui::CancelPendingFeature(*s_App);
      ctx->Yield();
      IM_CHECK(ui::FingerprintOf(*Doc()) == before);
    }
  };

  // A pattern whose source has no body has nothing to repeat: it opens
  // naming the source (not a spacing it could not derive, which it leaves
  // at 0), and cancelling changes nothing.
  t = IM_REGISTER_TEST(engine, "defaults", "pattern_without_a_source_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::make_unique<BoxFeature>(
            "Flat", glm::vec3(0.0f), 2.0, 0.0, 2.0)), // fails: no height
        *Doc());
    ctx->Yield(2);
    IM_CHECK(!Doc()->GetFeature(0)->GetError().empty());
    const ui::DocumentFingerprint before = ui::FingerprintOf(*Doc());
    ToolbarClick(ctx, "###tb_pattern");
    ctx->Yield(2);
    auto* pat = dynamic_cast<PatternFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(pat != nullptr);
    IM_CHECK_EQ(pat->spacing, 0.0);
    // Shown in the window; Accept stays offered, as for any feature whose
    // error is an earlier one's consequence (ErrorAfterEarlierFailure).
    IM_CHECK(pat->GetError().find("source body") != std::string::npos);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(ui::FingerprintOf(*Doc()) == before);
  };

  // Draft has no body slot, and nothing in the UI ever wrote bodyId — Execute
  // rejects faces whose ref does not match it, so the pick must set it.
  t = IM_REGISTER_TEST(engine, "draft", "face_pick_names_the_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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

  // Direct edit (docs/modeling/README.md#direct-edit). The fixture box with
  // a hole in it, drilled from the top: through, or `depth` deep.
  struct Drilled {
    uint64_t body = 0; // the Boolean's id: the drilled body's createdBy
    glm::dvec3 lo{0.0}, hi{0.0};
    double box = 0.0; // the undrilled volume
  };
  static const auto drill = [](ImGuiTestContext* ctx, double depth) {
    AddFixtureBox(ctx);
    ctx->Yield();
    Drilled d;
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    Doc()->GetBodies()[0].shape.GetBoundingBox(d.lo, d.hi, true);
    d.box = Doc()->GetBodies()[0].shape.Volume();
    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    const double h = d.hi.z - d.lo.z;
    const double through = depth <= 0.0;
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::make_unique<CylinderFeature>(
            "Bore",
            glm::dvec3(c.x, c.y, through ? d.lo.z - 0.5 : d.hi.z - depth),
            glm::dvec3(0, 0, 1), 0.3 * h, through ? h + 1.0 : depth + 0.5)),
        *Doc());
    const uint64_t cylId = Doc()->GetFeature(1)->GetId();
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::make_unique<BooleanFeature>(
            "Drill", boxId, cylId, BooleanOp::Subtract)),
        *Doc());
    ctx->Yield(2);
    d.body = Doc()->GetFeature(2)->GetId();
    return d;
  };
  // The name of the drilled body's face nearest `p`.
  static const auto faceAt = [](const Drilled& d, const glm::dvec3& p) {
    for (const Body& b : Doc()->GetBodies()) {
      if (b.createdBy != d.body)
        continue;
      std::string best;
      double bestD = 1e9;
      for (const auto& [i, n] : b.elementMap.names)
        if (const auto dist = Measure::DistanceToElementAt(b, true, i, p, 1e-3);
            dist && *dist < bestD) {
          bestD = *dist;
          best = n;
        }
      return ElementRef{d.body, best};
    }
    return ElementRef{};
  };
  static const auto bodyVolume = [](uint64_t id) {
    for (const Body& b : Doc()->GetBodies())
      if (b.createdBy == id)
        return b.shape.Volume();
    return 0.0;
  };

  // A preselected bore, deleted and accepted: the block is whole again.
  t = IM_REGISTER_TEST(engine, "direct", "delete_preselected_bore");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = drill(ctx, 0.0);
    IM_CHECK(bodyVolume(d.body) < d.box - 1e-3);
    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    const double r = 0.3 * (d.hi.z - d.lo.z);
    const ElementRef bore = faceAt(d, {c.x + r, c.y, c.z});
    IM_CHECK(!bore.name.empty());
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, bore});
    ToolbarClick(ctx, "###tb_delete_face");
    ctx->Yield();
    auto* del = dynamic_cast<DeleteFaceFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(del != nullptr);
    IM_CHECK_EQ(del->faces.size(), (size_t)1); // seeded from the selection
    IM_CHECK_EQ(del->bodyId, d.body);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(std::abs(bodyVolume(d.body) - d.box) < 1e-9 * d.box);
  };

  // "Add the feature" grows a blind hole's bore to its floor, shown in the
  // list before anything is accepted.
  t = IM_REGISTER_TEST(engine, "direct", "add_the_feature_takes_the_floor");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = drill(ctx, 1.0);
    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    const double r = 0.3 * (d.hi.z - d.lo.z);
    const ElementRef bore = faceAt(d, {c.x + r, c.y, d.hi.z - 0.5});
    const ElementRef floor = faceAt(d, {c.x, c.y, d.hi.z - 1.0});
    IM_CHECK(!bore.name.empty() && !floor.name.empty() &&
             bore.name != floor.name);
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, bore});
    ToolbarClick(ctx, "###tb_delete_face");
    ctx->Yield();
    auto* del = dynamic_cast<DeleteFaceFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(del != nullptr);
    IM_CHECK_EQ(del->faces.size(), (size_t)1);
    ctx->ItemClick("//###ToolWindow/**/###add_feature");
    ctx->Yield();
    del = dynamic_cast<DeleteFaceFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK_EQ(del->faces.size(), (size_t)2);
    IM_CHECK(
        std::any_of(del->faces.begin(), del->faces.end(),
                    [&](const ElementRef& r) { return r.name == floor.name; }));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(std::abs(bodyVolume(d.body) - d.box) < 1e-9 * d.box);
  };

  // A delete the faces around cannot heal: Accept is disabled and the
  // window names the face.
  t = IM_REGISTER_TEST(engine, "direct", "refused_delete_names_the_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = drill(ctx, 0.0);
    const ElementRef top = faceAt(d, {d.lo.x + 0.1, d.lo.y + 0.1, d.hi.z});
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, top});
    ToolbarClick(ctx, "###tb_delete_face");
    ctx->Yield(2);
    Feature* f = Doc()->FindFeatureById(s_App->pendingFeatureId);
    IM_CHECK(f != nullptr);
    IM_CHECK(f->GetError().find("`" + top.name + "`") != std::string::npos);
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) != 0);
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // Pick faces… on a direct edit whose face is gone: the editor opens with
  // the slot armed, the dead ref goes with its "x", and a click in the
  // viewport picks its successor.
  t = IM_REGISTER_TEST(engine, "direct", "pick_faces_repairs_a_dangling_ref");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = drill(ctx, 0.0);
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::make_unique<DeleteFaceFeature>(
            "Fill", d.body,
            std::vector<ElementRef>{
                ElementRef{d.body, std::to_string(d.body) + "/face99"}})),
        *Doc());
    ctx->Yield(2);
    const size_t idx = Doc()->FeatureCount() - 1;
    IM_CHECK(Doc()->GetFeature(idx)->GetError().find("no longer exists") !=
             std::string::npos);

    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(c));
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    // From above and to the -X side: the bore's +X wall faces the camera
    // through the opening.
    s_App->camera->SetPosition(
        glm::vec3(c) + glm::normalize(glm::vec3(-1.0f, 0.0f, 2.0f)) * 12.0f);
    ctx->Yield(2);

    ctx->ItemClick(("//###Model/Fill##" + std::to_string(idx)).c_str(),
                   ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###pick_faces");
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active());
    ctx->ItemClick("//###ToolWindow/**/x");
    ctx->Yield();
    auto* del = dynamic_cast<DeleteFaceFeature*>(Doc()->GetFeature(idx));
    IM_CHECK(del != nullptr && del->faces.empty());
    // A point on the bore's far (+X) wall, just below the rim.
    const double r = 0.3 * (d.hi.z - d.lo.z);
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(c.x + r, c.y, d.hi.z - 0.2)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    del = dynamic_cast<DeleteFaceFeature*>(Doc()->GetFeature(idx));
    IM_CHECK_EQ(del->faces.size(), (size_t)1);
    IM_CHECK(del->GetError().empty());
    IM_CHECK(std::abs(bodyVolume(d.body) - d.box) < 1e-9 * d.box);
    s_App->editFeatureId = 0;
    s_App->slotPick = {};
  };

  // The Cradle tool (docs/modeling/README.md#cradle): a block and a ball on
  // a printed part; the toolbar opens it on the two, its defaults take the
  // print process's gap, the accept is one undo step, it builds off the
  // frame as a `feature` job, and the block comes back a closed mesh body.
  t = IM_REGISTER_TEST(engine, "cradle", "a_block_cut_to_a_ball_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Blank", glm::dvec3(-15, -15, -15), 30.0, 30.0, 19.0));
    Doc()->AddFeature(
        std::make_unique<SphereFeature>("Ball", glm::dvec3(0.0), 10.0));
    Doc()->SetPrintProcess(printfit::Process{"fdm", "petg", std::nullopt});
    Doc()->Regenerate();
    ctx->Yield();
    const uint64_t blockId = Doc()->GetFeature(0)->GetId();
    const uint64_t ballId = Doc()->GetFeature(1)->GetId();
    const size_t depth = s_App->commands->UndoDepth();
    ToolbarClick(ctx, "###tb_cradle");
    ctx->Yield();
    auto* cr = dynamic_cast<CradleFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(cr != nullptr);
    IM_CHECK_EQ(cr->targetId, blockId);
    IM_CHECK_EQ(cr->toolId, ballId);
    IM_CHECK(!cr->hasClearance); // the print process's gap
    // The preview builds as a job too; wait for it before accepting.
    IM_CHECK(WaitFor(
        ctx, "the preview to build", [&] { return !Doc()->HasUnbuilt(); },
        60.0));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth + 1);
    IM_CHECK(WaitFor(
        ctx, "the cradle to build", [&] { return !Doc()->HasUnbuilt(); },
        60.0));
    ctx->Yield(2);
    const Feature* made = Doc()->GetFeature(Doc()->FeatureCount() - 1);
    IM_CHECK(made->GetError().empty());
    const Body* block = nullptr;
    for (const Body& b : Doc()->GetBodies())
      if (b.createdBy == blockId)
        block = &b;
    IM_CHECK(block != nullptr && block->IsMesh());
    IM_CHECK(block->mesh->Closed());
    // PETG's gap is 0.6 on the diameter: 0.3 a side.
    const auto* built = dynamic_cast<const CradleFeature*>(made);
    IM_CHECK(std::abs(built->ReportJson().value("clearance_mm", 0.0) - 0.3) <
             1e-12);
    // Undo gives the block back as a B-Rep.
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    for (const Body& b : Doc()->GetBodies())
      if (b.createdBy == blockId)
        IM_CHECK(!b.IsMesh());
  };

  // The Align tool (docs/interchange/README.md#placing-an-import): a 40 x
  // 20 x 10 box read as a mesh body, standing on its 40 x 20. The toolbar
  // opens it on the import waiting for a pick, Accept disabled; a click on
  // the +X end previews that end on the floor, the 40 mm standing up;
  // Accept is one undo step and Undo puts it back; a second preview is put
  // back by Cancel without touching the undo stack.
  t = IM_REGISTER_TEST(engine, "align",
                       "a_floor_picked_on_a_scan_is_one_undo_step");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    const auto stl =
        std::filesystem::temp_directory_path() /
        ("lobole_gui_align_" + std::to_string(proc::CurrentPid()) + ".stl");
    {
      std::vector<glm::vec3> corners;
      for (int i = 0; i < 8; i++)
        corners.emplace_back((i & 1) ? 20.0f : -20.0f, (i & 2) ? 10.0f : -10.0f,
                             (i & 4) ? 5.0f : -5.0f);
      const int quads[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                               {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
      std::ofstream out(stl, std::ios::binary);
      const char header[80] = {};
      out.write(header, 80);
      const uint32_t count = 12;
      out.write(reinterpret_cast<const char*>(&count), 4);
      for (const auto& q : quads)
        for (const std::array<int, 3> tri :
             {std::array<int, 3>{q[0], q[1], q[2]},
              std::array<int, 3>{q[0], q[2], q[3]}}) {
          glm::vec3 a = corners[tri[0]], b = corners[tri[1]],
                    c = corners[tri[2]];
          if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.0f)
            std::swap(b, c); // wound outward
          const float n[3] = {0, 0, 0};
          out.write(reinterpret_cast<const char*>(n), 12);
          for (const glm::vec3& p : {a, b, c})
            out.write(reinterpret_cast<const char*>(&p), 12);
          const char attr[2] = {};
          out.write(attr, 2);
        }
    }
    auto imp = std::make_unique<ImportFeature>("scan", stl.string());
    imp->upAxis = interchange::UpAxis::None;
    imp->representation = mesh::Representation::Mesh;
    Doc()->AddFeature(std::move(imp));
    Doc()->Regenerate();
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), size_t{1});
    IM_CHECK(Doc()->GetBodies()[0].IsMesh());
    const uint64_t id = Doc()->GetFeature(0)->GetId();
    const auto zSpan = [] {
      glm::dvec3 lo, hi;
      BodyBounds(Doc()->GetBodies()[0], lo, hi);
      return std::pair<double, double>{lo.z, hi.z};
    };
    const auto import = [&] {
      return dynamic_cast<const ImportFeature*>(Doc()->FindFeatureById(id));
    };
    const size_t depth = s_App->commands->UndoDepth();

    ToolbarClick(ctx, "###tb_align");
    ctx->Yield();
    IM_CHECK(s_App->alignTool.open);
    IM_CHECK_EQ(s_App->alignTool.featureId, id);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kAlignToolSlot);
    IM_CHECK((ctx->ItemInfo("//###ToolWindow/###tw_accept").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);

    // The +X end, from straight out along +X.
    const auto lookAtEnd = [&] {
      const glm::vec3 end(20.0f, 0.0f, 0.0f);
      s_App->camera->StopAnimation();
      s_App->camera->SetTarget(end);
      s_App->camera->SetUp(conv::kWorldUpF);
      s_App->camera->SetPosition(end + glm::vec3(80.0f, 0.0f, 0.0f));
      ctx->Yield(2);
      ctx->MouseMoveToPos(WorldToWindow(end + glm::vec3(0.0f, 3.0f, 1.0f)));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
    };
    lookAtEnd();
    IM_CHECK(s_App->alignTool.preview.has_value());
    IM_CHECK(s_App->alignTool.error.empty());
    IM_CHECK(std::abs(zSpan().first) < 1e-4);         // on the floor
    IM_CHECK(std::abs(zSpan().second - 40.0) < 1e-4); // standing on its end
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth); // a preview, unrecorded
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK(!s_App->alignTool.open);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth + 1);
    IM_CHECK(!import()->placement.IsIdentity());
    IM_CHECK(std::abs(zSpan().second - 40.0) < 1e-4);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    IM_CHECK(import()->placement.IsIdentity());
    IM_CHECK(std::abs(zSpan().second - zSpan().first - 10.0) < 1e-4);

    // Previewed again, then cancelled: back where it was, nothing recorded.
    const size_t undone = s_App->commands->UndoDepth();
    ToolbarClick(ctx, "###tb_align");
    ctx->Yield();
    lookAtEnd();
    IM_CHECK(s_App->alignTool.preview.has_value());
    ctx->ItemClick("//###ToolWindow/###tw_cancel");
    ctx->Yield(2);
    IM_CHECK(!s_App->alignTool.open);
    IM_CHECK(import()->placement.IsIdentity());
    IM_CHECK(std::abs(zSpan().second - zSpan().first - 10.0) < 1e-4);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undone);
    std::filesystem::remove(stl);
  };

  // The Fit tool (docs/modeling/README.md#datums-on-a-scan): a 40 x 20 x 10
  // box read as a mesh body. The toolbar arms a point pick with Accept
  // disabled; a click on the +X end fits (Auto) that end's plane, and Accept
  // adds a DatumPlane from_fit standing on it as one undo step, which Undo
  // takes away; Escape closes a second session with nothing recorded.
  t = IM_REGISTER_TEST(engine, "fit",
                       "a_plane_picked_on_a_body_is_one_undo_step");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    const auto stl =
        std::filesystem::temp_directory_path() /
        ("lobole_gui_fit_" + std::to_string(proc::CurrentPid()) + ".stl");
    {
      std::vector<glm::vec3> corners;
      for (int i = 0; i < 8; i++)
        corners.emplace_back((i & 1) ? 20.0f : -20.0f, (i & 2) ? 10.0f : -10.0f,
                             (i & 4) ? 5.0f : -5.0f);
      const int quads[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                               {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
      std::ofstream out(stl, std::ios::binary);
      const char header[80] = {};
      out.write(header, 80);
      const uint32_t count = 12;
      out.write(reinterpret_cast<const char*>(&count), 4);
      for (const auto& q : quads)
        for (const std::array<int, 3> tri :
             {std::array<int, 3>{q[0], q[1], q[2]},
              std::array<int, 3>{q[0], q[2], q[3]}}) {
          glm::vec3 a = corners[tri[0]], b = corners[tri[1]],
                    c = corners[tri[2]];
          if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.0f)
            std::swap(b, c); // wound outward
          const float n[3] = {0, 0, 0};
          out.write(reinterpret_cast<const char*>(n), 12);
          for (const glm::vec3& p : {a, b, c})
            out.write(reinterpret_cast<const char*>(&p), 12);
          const char attr[2] = {};
          out.write(attr, 2);
        }
    }
    auto imp = std::make_unique<ImportFeature>("scan", stl.string());
    imp->upAxis = interchange::UpAxis::None;
    imp->representation = mesh::Representation::Mesh;
    Doc()->AddFeature(std::move(imp));
    Doc()->Regenerate();
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), size_t{1});
    const uint64_t id = Doc()->GetFeature(0)->GetId();
    const size_t depth = s_App->commands->UndoDepth();
    const size_t features = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_fit_primitive");
    ctx->Yield();
    IM_CHECK(s_App->fitTool.open);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kFitToolSlot);
    IM_CHECK((ctx->ItemInfo("//###ToolWindow/###tw_accept").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);
    const auto clickEnd = [&] {
      const glm::vec3 end(20.0f, 0.0f, 0.0f);
      s_App->camera->StopAnimation();
      s_App->camera->SetTarget(end);
      s_App->camera->SetUp(conv::kWorldUpF);
      s_App->camera->SetPosition(end + glm::vec3(80.0f, 0.0f, 0.0f));
      ctx->Yield(2);
      ctx->MouseMoveToPos(WorldToWindow(end + glm::vec3(0.0f, 3.0f, 1.0f)));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield(2);
    };
    clickEnd();
    IM_CHECK_EQ(s_App->fitTool.bodyFeatureId, id);
    IM_CHECK_EQ(s_App->fitTool.fittedKind, int(mesh::Kind::Plane));
    IM_CHECK(s_App->fitTool.error.empty());
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth); // nothing yet
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK(!s_App->fitTool.open);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth + 1);
    IM_CHECK_EQ(Doc()->FeatureCount(), features + 1);
    const auto* dp =
        dynamic_cast<const DatumPlaneFeature*>(Doc()->GetFeature(features));
    IM_CHECK(dp != nullptr);
    if (dp != nullptr) {
      IM_CHECK(dp->kind == DatumPlaneKind::FromFit);
      IM_CHECK(dp->fit.kind == mesh::Kind::Plane); // never auto
      IM_CHECK(dp->GetError().empty());
      const SketchPlane& p = dp->OutputPlane();
      IM_CHECK(glm::length(p.GetNormal() - glm::dvec3(1, 0, 0)) < 1e-6);
      IM_CHECK(std::abs(p.GetOrigin().x - 20.0) < 1e-4);
    }

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), features);

    ToolbarClick(ctx, "###tb_fit_primitive");
    ctx->Yield();
    clickEnd();
    IM_CHECK(s_App->fitTool.open);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(!s_App->fitTool.open);
    IM_CHECK_EQ(Doc()->FeatureCount(), features);
    std::filesystem::remove(stl);
  };

  // Heavy features build off the frame
  // (docs/ui/long-operations.md#heavy-features). The knob makes this Delete
  // Face one (a bore is two faces, far under the estimate that sends the fan
  // grill's 916 to a job) and holds the worker a second and a half before it
  // heals, so the build spans frames whatever the machine.
  static const auto heavyDelete = [](ImGuiTestContext* ctx, int delayMs) {
    const Drilled d = drill(ctx, 0.0);
    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    const double r = 0.3 * (d.hi.z - d.lo.z);
    const ElementRef bore = faceAt(d, {c.x + r, c.y, c.z});
    heavy::SetTestOptions({true, delayMs});
    heavy::ForgetAll();
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, bore});
    ToolbarClick(ctx, "###tb_delete_face");
    ctx->Yield();
    return d;
  };
  static const auto saved = [](const char* name) -> std::string {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("lobole-heavy-" + std::to_string(getpid()) + "-" + name + ".json"))
            .string();
    if (!DocumentIO::Save(*Doc(), path).has_value())
      return "(not saved)";
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    std::filesystem::remove(path);
    return bytes;
  };

  // The frame keeps running while it builds, and none of those frames is
  // held: the worker heals, the frame draws.
  t = IM_REGISTER_TEST(engine, "direct",
                       "a_heavy_delete_face_builds_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = heavyDelete(ctx, 1500);
    const size_t depth = s_App->commands->UndoDepth();
    const double a0 = MainThreadCpuMs();
    ui::AcceptPendingFeature(*s_App);
    const double acceptMs = MainThreadCpuMs() - a0;
    ctx->LogInfo("the accept cost %.1f ms of main-thread time", acceptMs);
    ctx->Yield();
    const uint64_t slot = ui::FeatureBuildSlotOf(*s_App, Doc());
    const Feature* del = Doc()->GetFeature(Doc()->FeatureCount() - 1);
    IM_CHECK(Doc()->BuildStateOf(del->GetId()) ==
             Document::BuildState::Building);
    IM_CHECK(ui::FeatureBuildRunning(*s_App, slot));
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth + 1); // one undo step
    // While it builds the part shows what it was, and says so.
    IM_CHECK(bodyVolume(d.body) < d.box - 1e-3);
    int frames = 0;
    double worstMs = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    while (ui::FeatureBuildRunning(*s_App, slot)) {
      const double f0 = MainThreadCpuMs();
      ctx->Yield();
      worstMs = std::max(worstMs, MainThreadCpuMs() - f0);
      frames++;
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60))
        break;
    }
    ctx->Yield(2);
    ctx->LogInfo("%d frames while it built, worst %.1f ms", frames, worstMs);
    IM_CHECK(!ui::FeatureBuildRunning(*s_App, slot));
    IM_CHECK(frames >= 10);
    // A frame's share is the poll: never the heal (1.5 s here). Judged
    // against the build itself so a loaded machine does not fail it.
    IM_CHECK(worstMs < 750.0);
    IM_CHECK(Doc()->BuildStateOf(del->GetId()) == Document::BuildState::Built);
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(std::abs(bodyVolume(d.body) - d.box) < 1e-9 * d.box);
  };

  // Cancelling the build of an edit undoes the edit: the document saves
  // byte for byte as it did before, and no undo step is left.
  t = IM_REGISTER_TEST(engine, "direct", "cancelling_a_heavy_delete_undoes_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // The preview is not the edit: the document as it stood before the tool.
    const Drilled d = drill(ctx, 0.0);
    const std::string before = saved("before");
    const size_t features = Doc()->FeatureCount();
    const size_t depth = s_App->commands->UndoDepth();
    {
      const glm::dvec3 c = (d.lo + d.hi) * 0.5;
      const double r = 0.3 * (d.hi.z - d.lo.z);
      const ElementRef bore = faceAt(d, {c.x + r, c.y, c.z});
      heavy::SetTestOptions({true, 60000});
      heavy::ForgetAll();
      s_App->selection.Clear();
      s_App->selection.items.push_back(
          {ui::SelectionItem::Kind::Face, 0, bore});
      ToolbarClick(ctx, "###tb_delete_face");
      ctx->Yield();
    }
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    const uint64_t slot = ui::FeatureBuildSlotOf(*s_App, Doc());
    IM_CHECK(ui::FeatureBuildRunning(*s_App, slot));
    // The Model panel's bar, and its cancel.
    ctx->ItemClick("//###Model/**/###feature_build_cancel");
    IM_CHECK(WaitFor(
        ctx, "the build to stop",
        [&] { return !ui::FeatureBuildRunning(*s_App, slot); }, 30.0));
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), features);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth);
    IM_CHECK(!s_App->commands->CanRedo());
    IM_CHECK(!Doc()->HasUnbuilt());
    IM_CHECK(saved("after") == before);
  };

  // Cancelling the build of an EXISTING feature -- an open that rebuilds,
  // here a regeneration with every cache dropped -- leaves it not built
  // (cancelled), what follows it waiting, until the tree's Build.
  t = IM_REGISTER_TEST(engine, "direct",
                       "cancelling_a_rebuild_leaves_it_not_built");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = heavyDelete(ctx, 0);
    ui::AcceptPendingFeature(*s_App);
    const uint64_t slot = ui::FeatureBuildSlotOf(*s_App, Doc());
    IM_CHECK(WaitFor(
        ctx, "the first build",
        [&] {
          return !ui::FeatureBuildRunning(*s_App, slot) && !Doc()->HasUnbuilt();
        },
        30.0));
    const size_t index = Doc()->FeatureCount() - 1;
    const Feature* del = Doc()->GetFeature(index);
    const size_t depth = s_App->commands->UndoDepth();
    // Rebuilt from nothing, slowly: the build of a feature nobody just edited.
    heavy::SetTestOptions({true, 60000});
    heavy::ForgetAll();
    Doc()->ClearRegenCache();
    Doc()->Regenerate();
    ctx->Yield(2);
    IM_CHECK(ui::FeatureBuildRunning(*s_App, slot));
    const std::string row = del->GetName() + "##" + std::to_string(index);
    ctx->ItemClick(("//###Model/" + row).c_str(), ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_cancel_build");
    IM_CHECK(WaitFor(
        ctx, "the build to stop",
        [&] { return !ui::FeatureBuildRunning(*s_App, slot); }, 30.0));
    ctx->Yield(2);
    IM_CHECK(Doc()->BuildStateOf(del->GetId()) ==
             Document::BuildState::Cancelled);
    IM_CHECK_EQ(Doc()->FeatureCount(), index + 1); // still there
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth);
    IM_CHECK(bodyVolume(d.body) < d.box - 1e-3); // not built
    // Asked for, it builds.
    heavy::SetTestOptions({true, 0});
    ctx->ItemClick(("//###Model/" + row).c_str(), ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_build");
    IM_CHECK(WaitFor(
        ctx, "the rebuild",
        [&] {
          return !Doc()->HasUnbuilt() && !ui::FeatureBuildRunning(*s_App, slot);
        },
        30.0));
    IM_CHECK(std::abs(bodyVolume(d.body) - d.box) < 1e-9 * d.box);
  };

  // Move Face with its derived default: a preselected bore moves in the
  // plane of the face it sits in, the volume kept.
  t = IM_REGISTER_TEST(engine, "direct", "move_preselected_bore");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const Drilled d = drill(ctx, 0.0);
    const double before = bodyVolume(d.body);
    const glm::dvec3 c = (d.lo + d.hi) * 0.5;
    const double r = 0.3 * (d.hi.z - d.lo.z);
    const ElementRef bore = faceAt(d, {c.x + r, c.y, c.z});
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, bore});
    ToolbarClick(ctx, "###tb_move_face");
    ctx->Yield();
    auto* mv =
        dynamic_cast<MoveFaceFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(mv != nullptr);
    IM_CHECK_EQ(mv->faces.size(), (size_t)1);
    // In the top's plane: no Z, and some X or Y.
    IM_CHECK(std::abs(mv->translate.z) < 1e-12);
    IM_CHECK(glm::length(mv->translate) > 0.0);
    const glm::dvec3 shift = mv->translate;
    const uint64_t id = mv->GetId();
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(std::abs(bodyVolume(d.body) - before) < 1e-9 * before);
    // The bore, by its moved name, where it was moved to.
    for (const Body& b : Doc()->GetBodies()) {
      if (b.createdBy != d.body)
        continue;
      const auto at =
          b.elementMap.Resolve(bore.name + ";M" + std::to_string(id));
      IM_CHECK(at.has_value());
      const auto info = Measure::DescribeFaceAt(b, *at);
      IM_CHECK(info.has_value());
      IM_CHECK(glm::length(glm::dvec3(info->centroid.x, info->centroid.y, 0) -
                           glm::dvec3(c.x + shift.x, c.y + shift.y, 0)) < 1e-6);
    }
  };

  // Replace Face's target from the tree: the top put onto a datum plane's
  // row, half a unit above it.
  t = IM_REGISTER_TEST(engine, "direct", "replace_top_onto_a_datum_row");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    glm::dvec3 lo, hi;
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi, true);
    auto dp =
        std::make_unique<DatumPlaneFeature>("Ceiling", DatumPlaneKind::Offset);
    dp->basePlane = PlaneType::XY;
    dp->offset = hi.z + 0.5;
    s_App->commands->Execute(std::make_unique<AddFeatureCommand>(std::move(dp)),
                             *Doc());
    ctx->Yield(2);
    const Drilled box{boxId, lo, hi, 0.0};
    const ElementRef top =
        faceAt(box, {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, hi.z});
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, top});
    ToolbarClick(ctx, "###tb_replace_face");
    ctx->Yield();
    auto* rp =
        dynamic_cast<ReplaceFaceFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(rp != nullptr);
    IM_CHECK_STR_EQ(rp->face.name.c_str(), top.name.c_str());
    // The face seeded, the target is what the window waits for.
    IM_CHECK(s_App->slotPick.Active());
    IM_CHECK_EQ(s_App->slotPick.slot,
                static_cast<int>(ui::PickSlot::ReplaceFaceTarget));
    ctx->ItemClick("//###Model/Ceiling##1");
    ctx->Yield(2);
    rp = dynamic_cast<ReplaceFaceFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(rp != nullptr);
    IM_CHECK(rp->targetKind == ReplaceTargetKind::Plane);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    const glm::dvec3 e = hi - lo;
    IM_CHECK(std::abs(bodyVolume(boxId) - e.x * e.y * (e.z + 0.5)) <
             1e-9 * e.x * e.y * e.z);
  };

  // Offset Face's handle: an arrow on the face, along its normal, that
  // drags the distance.
  t = IM_REGISTER_TEST(engine, "direct", "offset_handle_drags_the_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    glm::dvec3 lo, hi;
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi, true);
    const Drilled box{boxId, lo, hi, 0.0};
    const ElementRef side =
        faceAt(box, {hi.x, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2});
    s_App->selection.Clear();
    s_App->selection.items.push_back({ui::SelectionItem::Kind::Face, 0, side});
    ToolbarClick(ctx, "###tb_offset_face");
    ctx->Yield();
    auto* off =
        dynamic_cast<OffsetFaceFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(off != nullptr);
    IM_CHECK_EQ(off->faces.size(), (size_t)1);
    IM_CHECK(off->distance > 0.0); // derived from the face, outward

    const glm::vec3 N(1.0f, 0.0f, 0.0f);
    const glm::vec3 C((float)hi.x, (float)((lo.y + hi.y) / 2),
                      (float)((lo.z + hi.z) / 2));
    // Looking at the arrow from the side and above, the arrow mid-screen:
    // clear of the tool window, and nowhere near parallel to the view.
    const double d0 = off->distance;
    const glm::vec3 look = C + N * static_cast<float>(d0 + 0.6);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(look);
    s_App->camera->SetUp(glm::vec3(0.0f, 0.0f, 1.0f));
    s_App->camera->SetPosition(
        look + glm::normalize(glm::vec3(0.3f, -1.0f, 0.5f)) * 12.0f);
    ctx->Yield(2);
    // The arrow as the frame published it: grab it mid-shaft.
    IM_CHECK_EQ(s_App->viewportHandles.size(), (size_t)1);
    const ui::ViewportHandle h = s_App->viewportHandles.front();
    IM_CHECK(glm::length(h.dir - N) < 1e-4f);
    IM_CHECK(glm::length(h.origin - (C + N * static_cast<float>(d0))) < 1e-3f);
    const glm::vec3 grab = h.origin + N * (0.45f * h.length);
    ctx->MouseMoveToPos(WorldToWindow(grab));
    ctx->Yield(2);
    IM_CHECK(s_App->hoveredHandle == h.id); // it highlights before the grab
    ctx->MouseDown(ImGuiMouseButton_Left);
    // 0.9 out from 0.1: the drag lands on 1.0, which a length snaps to.
    ctx->MouseMoveToPos(WorldToWindow(grab + N * static_cast<float>(1.0 - d0)));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    off = dynamic_cast<OffsetFaceFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(off != nullptr);
    IM_CHECK(std::abs(off->distance - 1.0) < 0.05);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
  };

  // Split's default knife is the XY plane at the body's mid-height, so the
  // preview shows two pieces before the user touches anything.
  t = IM_REGISTER_TEST(engine, "split", "default_plane_cuts_the_body_in_two");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);
    ctx->Yield();
    // A datum plane on the box's top face — the worst case, and the one hit
    // in practice: tangent, so any offset in the wrong direction misses.
    ToolbarClick(ctx, "###tb_datum_plane");
    ctx->Yield();
    auto* dp = dynamic_cast<DatumPlaneFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(dp != nullptr);
    glm::dvec3 bmin, bmax;
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
  t = IM_REGISTER_TEST(engine, "refine", "merges_the_seam_left_by_a_union");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    AddFixtureBox(ctx);
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

  // Direct-manipulation preview handles (docs/ui/viewport-handles.md):
  // drag the viewport arrow to set the pending extrude's distance.
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

  // The editor's Modeled checkbox and Hand combo reach the feature: ticking
  // it cuts the real groove (the body loses volume, the overlay stops being
  // drawn over it), and Left is what the saved feature holds.
  t = IM_REGISTER_TEST(engine, "thread", "modeled_checkbox_cuts_the_groove");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A short M8 boss: 8 turns, so the preview's cut is quick.
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 10.0));
    ctx->Yield();
    const double blank = Doc()->GetBodies()[0].shape.Volume();

    ToolbarClick(ctx, "###tb_thread");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 5));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0, 0, 5) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   30.0f);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(
        glm::normalize(glm::vec3(1, 1, 0)) * 4.0f + glm::vec3(0, 0, 5)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    auto* th = dynamic_cast<ThreadFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(th != nullptr);
    IM_CHECK(!th->face.name.empty());
    IM_CHECK(!th->modeled);

    ctx->SetRef("//Thread###ToolWindow");
    ctx->ItemClick("Modeled");
    ctx->ComboClick("###Hand/Left");
    ctx->Yield(3);
    th = dynamic_cast<ThreadFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(th != nullptr);
    IM_CHECK(th->modeled);
    IM_CHECK(!th->rightHanded);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    const auto* kept = dynamic_cast<const ThreadFeature*>(Doc()->GetFeature(1));
    IM_CHECK(kept != nullptr);
    IM_CHECK_STR_EQ(kept->GetError().c_str(), "");
    IM_CHECK(kept->overlay.cut);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < blank - 1.0);
  };

  // The worm editor (docs/modeling/worm-gears.md): the blank is picked in
  // the viewport, its module derived from it, and the hand and the starts
  // set in the window reach the feature that is kept — which cuts the worm
  // and reads back its lead angle.
  t = IM_REGISTER_TEST(engine, "worm", "editor_picks_the_blank_and_sets_hand");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Blank", glm::vec3(0.0f), glm::vec3(0, 0, 1), 12.0, 40.0));
    ctx->Yield();
    const double blank = Doc()->GetBodies()[0].shape.Volume();
    ToolbarClick(ctx, "###tb_worm");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 20));
    s_App->camera->SetUp(glm::vec3(0, 0, 1));
    s_App->camera->SetPosition(glm::vec3(0, 0, 20) +
                               glm::normalize(glm::vec3(1, 1, 0.3f)) * 90.0f);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(
        glm::normalize(glm::vec3(1, 1, 0)) * 12.0f + glm::vec3(0, 0, 20)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    auto* wo = dynamic_cast<WormFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(wo != nullptr);
    IM_CHECK(!wo->face.name.empty());
    IM_CHECK(wo->rightHanded);
    ctx->SetRef("//Worm###ToolWindow");
    ctx->ComboClick("###Hand/Left");
    ctx->ItemInputValue("##Starts", 2);
    ctx->Yield(3);
    wo = dynamic_cast<WormFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(wo != nullptr);
    IM_CHECK(!wo->rightHanded);
    IM_CHECK_EQ(wo->starts, 2);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    const auto* kept = dynamic_cast<const WormFeature*>(Doc()->GetFeature(1));
    IM_CHECK(kept != nullptr);
    IM_CHECK_STR_EQ(kept->GetError().c_str(), "");
    IM_CHECK(kept->report.valid);
    // Module 2 from the 24 mm blank, two starts on a 20 mm pitch diameter:
    // tan(gamma) = 2 * 2 / 20.
    IM_CHECK(std::abs(kept->report.spec.module - 2.0) < 1e-9);
    IM_CHECK(std::abs(std::tan(kept->report.sizes.leadAngle) - 0.2) < 1e-9);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < blank - 100.0);
  };

  // A cam on a picked hub (docs/modeling/cam-profiles.md): nothing typed,
  // every number from the hub, and the follower changed from the window.
  t = IM_REGISTER_TEST(engine, "cam_profile",
                       "tool_cuts_the_default_lobe_on_a_picked_hub");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Hub", glm::vec3(0.0f), glm::vec3(0, 0, 1), 12.0, 40.0));
    ctx->Yield();
    ToolbarClick(ctx, "###tb_cam_profile");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 20));
    s_App->camera->SetUp(glm::vec3(0, 0, 1));
    s_App->camera->SetPosition(glm::vec3(0, 0, 20) +
                               glm::normalize(glm::vec3(1, 1, 0.3f)) * 90.0f);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(
        glm::normalize(glm::vec3(1, 1, 0)) * 12.0f + glm::vec3(0, 0, 20)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(3);
    auto* cp =
        dynamic_cast<CamProfileFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(cp != nullptr);
    IM_CHECK(!cp->face.name.empty());
    IM_CHECK_STR_EQ(cp->GetError().c_str(), "");
    IM_CHECK(cp->report.valid);
    ctx->SetRef("//Cam###ToolWindow");
    ctx->ComboClick("###Follower/Knife");
    ctx->Yield(3);
    cp = dynamic_cast<CamProfileFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(cp != nullptr);
    IM_CHECK(cp->followerSet);
    IM_CHECK(cp->follower.kind == cammech::FollowerKind::Knife);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    const auto* kept =
        dynamic_cast<const CamProfileFeature*>(Doc()->GetFeature(1));
    IM_CHECK(kept != nullptr);
    IM_CHECK_STR_EQ(kept->GetError().c_str(), "");
    IM_CHECK(kept->report.valid);
    // The hub's radius and length, and a lobe rising half the radius.
    IM_CHECK(std::abs(kept->report.baseRadius - 12.0) < 1e-9);
    IM_CHECK(std::abs(kept->report.width - 40.0) < 1e-9);
    IM_CHECK(std::abs(kept->report.profile.maxRadiusMm - 18.0) < 1e-6);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
  };

  // The wheel editor: picked the same way, its rows show what the hob made
  // (the centre distance, the ratio, the throat) once it has regenerated.
  t = IM_REGISTER_TEST(engine, "worm", "wheel_editor_reports_the_pair");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Blank", glm::vec3(0.0f), glm::vec3(0, 0, 1), 43.0, 13.0));
    ctx->Yield();
    ToolbarClick(ctx, "###tb_worm_wheel");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 6.5f));
    s_App->camera->SetUp(glm::vec3(0, 0, 1));
    s_App->camera->SetPosition(glm::vec3(0, 0, 6.5f) +
                               glm::normalize(glm::vec3(1, 1, 0.3f)) * 250.0f);
    ctx->Yield(2);
    ctx->MouseMoveToPos(WorldToWindow(
        glm::normalize(glm::vec3(1, 1, 0)) * 43.0f + glm::vec3(0, 0, 6.5f)));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(3);
    auto* ww =
        dynamic_cast<WormWheelFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ww != nullptr);
    IM_CHECK(!ww->face.name.empty());
    IM_CHECK_STR_EQ(ww->GetError().c_str(), "");
    IM_CHECK(ww->report.valid);
    ctx->SetRef("//Worm wheel###ToolWindow");
    IM_CHECK(ctx->ItemExists("##Teeth"));
    IM_CHECK(ctx->ItemExists("###Hand"));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    const auto* kept =
        dynamic_cast<const WormWheelFeature*>(Doc()->GetFeature(1));
    IM_CHECK(kept != nullptr);
    IM_CHECK_STR_EQ(kept->GetError().c_str(), "");
    IM_CHECK(std::abs(kept->report.sizes.centreDistance - 50.0) < 1e-9);
  };

  // Repro for the reported circular-pattern segfault (dogfooding backlog):
  // preview a pattern on a body, switch it to Circular, and work the axis/
  // count/angle widgets across frames — the crash lived in this flow.
  t = IM_REGISTER_TEST(engine, "pattern",
                       "circular_preview_param_edits_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
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

  // R14: a cut leaves no body to click, so the pattern's Source slot takes
  // the CUT'S OWN TREE ROW. Without this the engine can pattern a hole but
  // the GUI cannot ask for one.
  t = IM_REGISTER_TEST(engine, "pattern", "cut_row_is_the_pattern_source");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0), 60.0, 20.0, 5.0));
    auto sk =
        std::make_unique<SketchFeature>("HoleSk", SketchPlane(PlaneType::XY));
    sk->AddEntity(sketch::Circle{{10, 10}, 3.0});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(1)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>(
        "Hole", skId, 5.0, ExtrudeMode::Cut, false));
    const uint64_t cutId = Doc()->GetFeature(2)->GetId();
    ctx->Yield();
    const double drilled = Doc()->GetBodies()[0].shape.Volume();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_pattern");
    ctx->Yield();
    auto* pat = dynamic_cast<PatternFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(pat != nullptr);
    IM_CHECK(pat->sourceId != cutId); // defaults to the body producer

    ctx->ItemClick("//###ToolWindow/###slot3"); // arm Source
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active());
    ctx->ItemClick("//##DockHost/**/Hole##2"); // the CUT's row
    ctx->Yield();
    IM_CHECK_EQ(pat->sourceId, cutId);

    pat->axis.kind = AxisRef::Kind::X;
    pat->count = 3;
    pat->spacing = 15.0;
    Doc()->Regenerate();
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    IM_CHECK(Doc()->GetFeature(3)->GetError().empty());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Two more holes than the drilled plate had.
    const double hole = 3.14159265358979 * 9.0 * 5.0;
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() -
                      (drilled - 2.0 * hole)) < 1e-3);
  };

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
      return angles::ToDegrees(std::atan2(l.b.y - l.a.y, l.b.x - l.a.x));
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

  // The gate for the two-table split (docs/ui/keybindings.md): the
  // metadata lives in core (keymap::Defaults) and the behaviour in ui
  // (ui::Commands), keyed by the same id. A row in one and not the other is a
  // command with no key, or a key that runs nothing — both silent.
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
  t = IM_REGISTER_TEST(engine, "fillet", "fillet_body_edge_via_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    // Frame the box deterministically (same recipe as the face-pick test).
    glm::dvec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5;
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

  // Sheet metal: the whole Tier 1 flow through the GUI — declare the stock,
  // thicken a profile into a blank, click an edge of it and get a wall, then
  // lay it flat again. Clicking the edge is the point: which of a rim's two
  // edges is picked is the only control there is over which way the wall
  // folds, and nothing but a real viewport click exercises it.
  t = IM_REGISTER_TEST(engine, "sheetmetal", "edge_pick_bends_a_wall");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A fresh document first: this test declares sheet-metal stock, and a
    // document carried in from a predecessor would arrive already holding
    // somebody else's geometry.
    ctx->MenuClick("File/New");
    ctx->Yield();
    SetupFor(ctx, Needs::SheetBlank);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    const double blank = Doc()->GetBodies()[0].shape.Volume();
    IM_CHECK(Doc()->GetBodies()[0].bends.empty());

    glm::dvec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5;
    const float radius = glm::length(max - min) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * radius * 5.0f);
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_edge_flange");
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active()); // the edge slot armed itself
    auto* flange = dynamic_cast<EdgeFlangeFeature*>(
        Doc()->FindFeatureById(s_App->slotPick.featureId));
    IM_CHECK(flange != nullptr);
    // The default wall came from the STOCK (5t), not from a literal.
    IM_CHECK(std::abs(flange->length - 5.0 * 1.5) < 1e-9);

    // Click the nearest LONG edge: a silhouette or front edge from this
    // viewpoint, so the pick cannot be occluded, and longer than the sheet is
    // thick — the short edges at the corners, where two rims meet, are not
    // panel edges, and this test once passed by bending a wall off one.
    const auto polys = Doc()->GetBodies()[0].shape.GenerateEdgePolylines();
    const glm::vec3 eye = s_App->camera->GetPosition();
    glm::vec3 target(0.0f);
    float bestDist = FLT_MAX;
    for (const auto& poly : polys) {
      if (poly.size() < 2 || glm::length(poly.back() - poly.front()) < 3.0f)
        continue;
      // The ARC midpoint, not poly[size / 2]: a straight edge tessellates to
      // its two endpoints, so the middle ELEMENT is a corner vertex, and the
      // click lands on whichever edge meets there.
      const glm::vec3 mid = topo::PolylineArcMidpoint(poly);
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
    IM_CHECK_EQ(flange->edges.size(), (size_t)1);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() > blank + 1e-6);
    // And the bend was RECORDED, which is what the flat pattern runs on.
    IM_CHECK_EQ(Doc()->GetBodies()[0].bends.size(), (size_t)1);

    // Lay it flat: one gauge thick everywhere, which no wrong fold survives.
    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_flat_pattern");
    ctx->Yield();
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetError().empty());
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 span = max - min;
    const float thin = std::min({span.x, span.y, span.z});
    IM_CHECK(std::abs(thin - 1.5f) < 1e-3f);
  };

  // The Sheet Metal panel: declaring the stock is a real undoable edit, and
  // the numbers it shows are the DERIVED ones a flange will use rather than
  // the zeros that are stored.
  t = IM_REGISTER_TEST(engine, "sheetmetal", "panel_declares_the_stock");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield(2);
    IM_CHECK(!Doc()->SheetMetal().has_value());

    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    // A CollapsingHeader's open state lives in ImGui's ini and outlives the
    // per-test reset, so open it only when the button is not already there.
    if (ctx->ItemInfo("//###Variables/###sm_make", ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Sheet metal");
      ctx->Yield(2);
    }
    ctx->ItemClick("//###Variables/###sm_make");
    ctx->Yield(2);

    IM_CHECK(Doc()->SheetMetal().has_value());
    const sheet::SheetMetalRules& rules = *Doc()->SheetMetal();
    IM_CHECK(!rules.material.empty());
    IM_CHECK(rules.thickness > 0.0);
    // Stored unset, shown DERIVED: the radius a flange will use is the
    // material's own minimum, not a zero and not a literal.
    IM_CHECK(rules.bendRadius == 0.0);
    IM_CHECK(std::abs(rules.EffectiveBendRadius() -
                      sheet::MinBendRadius(rules.material, rules.thickness)) <
             1e-9);
    // One undoable step, like every other document-level statement.
    IM_CHECK(s_App->commands->CanUndo());
    s_App->commands->Undo(*Doc());
    ctx->Yield();
    IM_CHECK(!Doc()->SheetMetal().has_value());

    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // The flat pattern tool keeps the part FOLDED in the base configuration.
  // A flat pattern replaces the folded shape, so it is authored suppressed
  // and a `Flat` configuration shows it; the tool used to add it live, which
  // left the part flat in every configuration with no way back. Accepting
  // now leaves the blank on screen through that configuration, and the Sheet
  // Metal panel's "Show the blank" switches between the two.
  t = IM_REGISTER_TEST(engine, "sheetmetal", "flat_pattern_keeps_the_fold");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield(2);
    SetupFor(ctx, Needs::SheetFlanged);
    Document* part = Doc();
    // The smallest side of the box: the sheet's thickness when it is flat,
    // the flange's height when it is folded.
    const auto thinnest = [&] {
      double lo = 1e300;
      for (const Body& b : part->GetBodies()) {
        glm::dvec3 mn, mx;
        b.shape.GetBoundingBox(mn, mx, /*tight=*/true);
        const glm::vec3 size = mx - mn;
        lo = std::min({lo, double(size.x), double(size.y), double(size.z)});
      }
      return lo;
    };
    const double t0 = Doc()->SheetMetal()->thickness;
    IM_CHECK(thinnest() > 2.0 * t0); // folded
    const size_t undoBefore = s_App->commands->UndoDepth();

    ToolbarCreateAccept(ctx, "###tb_flat_pattern");
    ctx->Yield(2);
    const Feature* flat = part->GetFeature(part->FeatureCount() - 1);
    IM_CHECK_EQ(flat->TypeName(), std::string("FlatPattern"));
    IM_CHECK(flat->IsSuppressed()); // off in the base configuration
    IM_CHECK(part->ActiveConfiguration() != 0);
    IM_CHECK(thinnest() < 1.5 * t0); // the blank, as previewed
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoBefore + 1); // one step

    // Unticking "Show the blank" is the folded part again.
    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    if (ctx->ItemInfo("//###Variables/###sm_flat", ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Sheet metal");
      ctx->Yield(2);
    }
    ctx->ItemClick("//###Variables/###sm_flat");
    ctx->Yield(3);
    IM_CHECK(part->ActiveConfiguration() == 0);
    IM_CHECK(thinnest() > 2.0 * t0);
    ctx->ItemClick("//###Variables/###sm_flat");
    ctx->Yield(3);
    IM_CHECK(thinnest() < 1.5 * t0);

    // Undo takes the feature and its configuration away together. (The
    // panel's toggle is a view switch, not an undo step.)
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
    const size_t features = part->FeatureCount();
    const size_t configs = part->GetConfigurations().size();
    s_App->commands->Undo(*part);
    ctx->Yield(2);
    IM_CHECK_EQ(part->FeatureCount(), features - 1);
    IM_CHECK_EQ(part->GetConfigurations().size(), configs - 1);
    IM_CHECK(part->ActiveConfiguration() == 0);
    IM_CHECK(thinnest() > 2.0 * t0);
  };

  // A flat pattern on a drawing, by clicking (S11): the Add-view form's
  // configuration combo draws the Flat blank beside the folded part — with
  // its crease as a bend line — and the view's Bend table button lists it.
  t = IM_REGISTER_TEST(engine, "sheetmetal", "flat_view_on_a_sheet");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield(2);
    SetupFor(ctx, Needs::SheetFlanged);
    Document* part = Doc();
    const uint64_t base = part->GetBodies()[0].createdBy;
    // The Flat configuration, as the Sheet Metal panel's "Show the blank"
    // makes it: a suppressed flat pattern that the configuration turns on.
    part->AddFeature(
        std::make_unique<FlatPatternFeature>("Flat pattern", base));
    const uint64_t flatFeature =
        part->GetFeature(part->FeatureCount() - 1)->GetId();
    part->GetFeature(part->FeatureCount() - 1)->SetSuppressed(true);
    part->Regenerate();
    Configuration& cfg = part->AddConfiguration("Flat");
    cfg.unsuppressedFeatures = {flatFeature};
    const uint64_t cfgId = cfg.id;
    // The orientation that looks square onto the blank.
    const glm::dvec3 normal(ui::DefaultPlane(1).GetNormal());
    const char* orient = nullptr;
    const char* names[] = {"front", "back", "left", "right", "top", "bottom"};
    for (int i = 0; i < 6 && !orient; ++i) {
      glm::dvec3 dir, up;
      OrientationAxes(static_cast<ViewOrientation>(i), dir, up);
      if (std::abs(glm::dot(glm::normalize(dir), normal)) > 1.0 - 1e-9)
        orient = names[i];
    }
    IM_CHECK_SILENT(orient != nullptr);

    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield(3);
    IM_CHECK_SILENT(s_App->drawing != nullptr);
    // Add view is a tool window, opened from the toolbar
    // (docs/ui/panels.md#drawing-tab-the-drawing-panel).
    ToolbarClick(ctx, "###tb_dwg_add_view");
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ItemInputValue("###dwg_scale", 0.0f);
    ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
    ctx->ComboClick(
        ("###dwg_configuration/Flat##cfg" + std::to_string(cfgId)).c_str());
    ctx->Yield();
    ctx->ItemClick("###dwg_add_view");
    ctx->Yield(3);
    ctx->ItemClick(
        "###dwg_add_view_close"); // a floating window a test opens, it closes
    ctx->Yield();

    const auto& views = s_App->drawing->GetViews();
    IM_CHECK_EQ_NO_RET(views.size(), (size_t)1);
    if (views.size() != 1)
      return;
    const uint64_t viewId = views[0].id;
    IM_CHECK_EQ(views[0].configuration, cfgId);
    const auto it = s_App->drawingRenders.find(viewId);
    IM_CHECK_SILENT(it != s_App->drawingRenders.end());
    size_t bends = 0;
    for (const drawing::SheetCurve& c : it->second.curves)
      bends += c.kind == drawing::CurveKind::Bend ? 1 : 0;
    IM_CHECK_EQ(bends, (size_t)1);
    // The part's own tab never left its authored configuration.
    IM_CHECK_EQ(part->ActiveConfiguration(), (uint64_t)0);

    // A bend table is in the view row's menu
    // (docs/ui/panels.md#drawing-tab-the-drawing-panel).
    ctx->ItemClick(
        ("//###Model/**/###dwg_view_" + std::to_string(viewId)).c_str(),
        ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###dwg_bend_table");
    ctx->Yield(2);
    const auto& notes = s_App->drawing->GetNotes();
    IM_CHECK_EQ_NO_RET(notes.size(), (size_t)1);
    if (notes.size() != 1)
      return;
    IM_CHECK(notes[0].table == NoteTable::Bends);
    const drawing::ResolvedNote rn = ui::ResolveDrawingNote(*s_App, notes[0]);
    IM_CHECK(rn.resolved);
    IM_CHECK(rn.text.rfind("BEND TABLE\nB1  ", 0) == 0);
    // Placed clear of the view it tabulates.
    glm::dvec2 mn, mx;
    drawing::LabelBox(rn.pos, rn.text, mn, mx);
    IM_CHECK(mn.x > it->second.bboxMax.x);
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
  t = IM_REGISTER_TEST(engine, "tree", "context_rename_and_rollback_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    AddFixtureSphere(ctx);
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

  // ---- Tolerances (docs/tolerances/README.md#gui) --------------------------
  //
  // The editor lives in the Measure tool window, because measuring a face and
  // saying what it may vary by are the same gesture on the same pick. These
  // drive the picks through MeasureElementClicked rather than the viewport:
  // the pick pipeline has its own tests, and what is under test here is that
  // the tolerance UI turns a pick into document data.
  // A face picked in the viewport says which feature MADE it — the stable
  // name it carries is minted by that feature — so the tree can mark the row
  // instead of leaving you to hunt it in a fifty-feature list.
  t = IM_REGISTER_TEST(engine, "tree",
                       "a_picked_face_names_the_feature_that_made_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield(2);
    IM_CHECK(ui::FeaturesThatMadeThePick(*s_App).empty()); // nothing picked
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();

    // Straight down at the middle of the top face, off the bounding box: a
    // guessed centre lands on a corner edge, which outranks the face.
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 top((lo.x + hi.x) * 0.5f, hi.y, (lo.z + hi.z) * 0.5f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(top);
    s_App->camera->SetUp(glm::vec3(0, 0, -1));
    s_App->camera->SetPosition(top + glm::vec3(0, 10, 0));
    ctx->Yield(3);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);

    const std::vector<uint64_t> made = ui::FeaturesThatMadeThePick(*s_App);
    IM_CHECK_EQ(made.size(), (size_t)1);
    IM_CHECK_EQ(made[0], boxId);
  };

  t = IM_REGISTER_TEST(engine, "tolerance", "a_measured_bore_gets_a_class");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);

    // The cylindrical face, taken off the body's own element map so the test
    // never hard-codes a toponame.
    const Body& body = Doc()->GetBodies()[0];
    std::string round;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto face = Measure::DescribeFaceAt(body, index);
      if (face && face->kind == "cylinder") {
        round = name;
        break;
      }
    }
    IM_CHECK(!round.empty());

    s_App->measure.active = true;
    ctx->Yield(2);
    ui::MeasureElementClicked(*s_App, ui::MeasureState::Element::OfFace(
                                          ElementRef{body.createdBy, round}));
    ctx->Yield(2);

    // With no class chosen the editor refuses BY NAME rather than writing a
    // plausible one: there is no honest default for a face nobody has said is
    // a bore or a shaft.
    ctx->ItemClick("//###ToolWindow/###tol_set");
    ctx->Yield(2);
    IM_CHECK(Doc()->GetTolerances().empty());
    IM_CHECK(!s_App->toleranceMsg.empty());

    // Pretend the project was just saved, so the dirty check below means
    // something (docs/versioning/README.md#unsaved-changes).
    s_App->lastSavedVersion = Doc()->SaveStamp();
    IM_CHECK(!ui::ProjectDirty(*s_App));

    // ComboClick resolves relative to the current ref, not from the root.
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###tol_letter/H  bore");
    ctx->Yield();
    ctx->ComboClick("###tol_grade/IT7");
    ctx->Yield();
    ctx->ItemClick("//###ToolWindow/###tol_set");
    ctx->Yield(2);

    IM_CHECK_EQ(Doc()->GetTolerances().size(), (size_t)1);
    // Saved with the part and regenerating nothing — and still an unsaved
    // change, or closing the project would drop it without asking.
    IM_CHECK(ui::ProjectDirty(*s_App));
    IM_CHECK_STR_EQ(Doc()->GetTolerances()[0].isoClass.c_str(), "H7");
    IM_CHECK_STR_EQ(Doc()->GetTolerances()[0].ref.name.c_str(), round.c_str());
    IM_CHECK(!Doc()->GetTolerances()[0].IsDistance());

    // A tolerance moves no surface. This is the claim the whole design rests
    // on, asserted where a person would actually break it.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);

    // ...and it undoes.
    IM_CHECK(s_App->commands->CanUndo());
    s_App->commands->Undo(*Doc());
    ctx->Yield();
    IM_CHECK(Doc()->GetTolerances().empty());

    s_App->measure = ui::MeasureState{};
  };

  // A plane has no size of its own — and the refusal says what to do about
  // it, because a second pick turns the same gesture into a length.
  t = IM_REGISTER_TEST(engine, "tolerance", "a_plane_is_told_what_to_do");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureBox(ctx);
    const Body& body = Doc()->GetBodies()[0];
    std::vector<std::string> planes;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto face = Measure::DescribeFaceAt(body, index);
      if (face && face->kind == "plane")
        planes.push_back(name);
    }
    IM_CHECK(planes.size() >= 2);

    s_App->measure.active = true;
    ctx->Yield(2);
    ui::MeasureElementClicked(*s_App,
                              ui::MeasureState::Element::OfFace(
                                  ElementRef{body.createdBy, planes[0]}));
    ctx->Yield(2);
    // No Set button at all here: there is nothing to tolerance yet, and the
    // window says so instead of offering a button that would refuse.
    IM_CHECK(
        ctx->ItemInfo("//###ToolWindow/###tol_set", ImGuiTestOpFlags_NoError)
            .ID == 0);

    // The second face makes it a length, and now it commits.
    ui::MeasureElementClicked(*s_App,
                              ui::MeasureState::Element::OfFace(
                                  ElementRef{body.createdBy, planes[1]}));
    ctx->Yield(2);
    s_App->toleranceLower = -0.1;
    s_App->toleranceUpper = 0.1;
    ctx->ItemClick("//###ToolWindow/###tol_set");
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetTolerances().size(), (size_t)1);
    IM_CHECK(Doc()->GetTolerances()[0].IsDistance());
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);

    s_App->measure = ui::MeasureState{};
  };

  // The Variables panel lists what the part carries, and clears a row.
  t = IM_REGISTER_TEST(engine, "tolerance", "the_panel_lists_and_clears");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    const Body& body = Doc()->GetBodies()[0];
    std::string round;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto face = Measure::DescribeFaceAt(body, index);
      if (face && face->kind == "cylinder") {
        round = name;
        break;
      }
    }
    IM_CHECK(!round.empty());
    Tolerance tol;
    tol.ref = ElementRef{body.createdBy, round};
    tol.isoClass = "g6";
    const uint64_t id = Doc()->SetTolerance(tol).id;
    ctx->Yield(2);

    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    // Toggle the section only if the row is not already reachable. A
    // CollapsingHeader's open state lives in ImGui's ini and outlives
    // ResetAppForTest, so ItemOpen here was order-dependent: it passed
    // serially and failed under --shuffle in the shard where the section was
    // already open, which is the whole reason the suite runs shuffled.
    const std::string del = "//###Variables/###tol_del_" + std::to_string(id);
    if (ctx->ItemInfo(del.c_str(), ImGuiTestOpFlags_NoError).ID == 0) {
      ctx->ItemClick("//###Variables/Tolerances");
      ctx->Yield(2);
    }
    ctx->ItemClick(del.c_str());
    ctx->Yield(2);
    IM_CHECK(Doc()->GetTolerances().empty());
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // The print process is picked in the Variables panel beside the
  // tolerances it qualifies (docs/tolerances/README.md#printed-parts): only
  // the carried pairs are offered, choosing one rebuilds nothing, and "Not
  // printed" takes it off again.
  t = IM_REGISTER_TEST(engine, "tolerance", "print_process_from_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    ctx->Yield(2);
    IM_CHECK(!Doc()->PrintProcess().has_value());
    const uint64_t version = Doc()->Version();

    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    // Open the section only if it is closed: its state lives in the ini and
    // outlives the reset between tests (see the_panel_lists_and_clears).
    if (ctx->ItemInfo("//###Variables/###print_process",
                      ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Print process");
      ctx->Yield(2);
    }
    // As if just saved: the edit below must make the project unsaved.
    s_App->lastSavedVersion = Doc()->SaveStamp();
    IM_CHECK(!ui::ProjectDirty(*s_App));
    // ComboClick resolves relative to the current ref, not from the root.
    ctx->SetRef("//###Variables");
    ctx->ComboClick("###print_process/###print_fdm_petg");
    ctx->Yield(2);
    IM_CHECK(Doc()->PrintProcess().has_value());
    IM_CHECK_EQ(Doc()->PrintProcess()->material, std::string("petg"));
    IM_CHECK_EQ(Doc()->Version(), version); // drives no geometry
    IM_CHECK(ui::ProjectDirty(*s_App));     // ...and is saved all the same

    ctx->ComboClick("###print_process/###print_none");
    ctx->Yield(2);
    IM_CHECK(!Doc()->PrintProcess().has_value());
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // The slicer settings under the print process
  // (docs/tolerances/README.md#slicer-settings): each field stated or left
  // to the preset, the pattern from the accepted list, and a combination
  // the slicer refuses -- grid at 100 % -- said and not stored.
  t = IM_REGISTER_TEST(engine, "tolerance", "slicer_settings_from_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    ctx->Yield(2);
    Doc()->SetPrintProcess(printfit::Process{"fdm", "pla", std::nullopt});
    IM_CHECK(Doc()->PrintSettings().Empty());
    const uint64_t version = Doc()->Version();
    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    if (ctx->ItemInfo("//###Variables/###print_process",
                      ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Print process");
      ctx->Yield(2);
    }
    ctx->SetRef("//###Variables");
    ctx->ItemClick("###slicer_on_walls");
    ctx->Yield(2);
    IM_CHECK(Doc()->PrintSettings().part.walls.has_value());
    ctx->ItemClick("###slicer_on_infill");
    ctx->Yield(2);
    IM_CHECK(Doc()->PrintSettings().part.infillPct.has_value());
    ctx->ComboClick("###slicer_pattern/###slicer_pattern_grid");
    ctx->Yield(2);
    IM_CHECK(Doc()->PrintSettings().part.infill == printset::Infill::Grid);
    // Grid cannot print 100 %: refused, the stored settings unchanged.
    const double before = *Doc()->PrintSettings().part.infillPct;
    ctx->ItemInputValue("###slicer_infill", "100");
    ctx->Yield(2);
    IM_CHECK_EQ(*Doc()->PrintSettings().part.infillPct, before);
    ctx->ComboClick("###slicer_pattern/###slicer_pattern_rectilinear");
    ctx->Yield(2);
    ctx->ItemInputValue("###slicer_infill", "100");
    ctx->Yield(2);
    IM_CHECK_EQ(*Doc()->PrintSettings().part.infillPct, 100.0);
    IM_CHECK(Doc()->PrintSettings().part.infill ==
             printset::Infill::Rectilinear);
    IM_CHECK_EQ(Doc()->Version(), version); // drives no geometry
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // Check print (docs/tolerances/README.md#checking-a-print): the button in
  // the Print process section runs ui::RunPrintCheck on the part and lists
  // the seven items; a ceiling's Show selects the face it is about. A
  // cylinder standing at z = 0 and a second one held 3 above the bed: the
  // part is on the bed, two pieces, and the upper one's underside is a
  // ceiling as wide as it is -- and a piece that starts in the air, which
  // the stacking line fails and its Show points at.
  t = IM_REGISTER_TEST(engine, "tolerance", "print_check_from_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Standing", glm::vec3(0), glm::vec3(0, 0, 1), 2.0, 4.0));
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Held", glm::vec3(10, 0, 3), glm::vec3(0, 0, 1), 1.0, 2.0));
    ctx->Yield(2);
    Doc()->SetPrintProcess(printfit::Process{"fdm", "pla", std::nullopt});
    s_App->printCheck.reset();
    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    if (ctx->ItemInfo("//###Variables/###print_process",
                      ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Print process");
      ctx->Yield(2);
    }
    ctx->SetRef("//###Variables");
    ctx->ItemClick("###print_check");
    ctx->Yield(2);
    IM_CHECK_SILENT(s_App->printCheck.has_value());
    if (!s_App->printCheck)
      return;
    const printcheck::Report& r = s_App->printCheck->report;
    IM_CHECK(r.bed.status == printcheck::Status::Pass);
    IM_CHECK(r.shells.status == printcheck::Status::Pass);
    IM_CHECK_EQ(r.shells.pieces, 2);
    IM_CHECK(r.chords.status == printcheck::Status::Pass);
    IM_CHECK(r.overhang.limitDeg == 45.0);
    IM_CHECK_SILENT(r.bridges.faces.size() == 1);
    IM_CHECK(std::abs(r.bridges.worstSpanMm - 2.0) < 0.01);
    IM_CHECK(std::abs(r.bridges.faces[0].heightMm - 3.0) < 1e-6);
    s_App->selection.Clear();
    ctx->ItemClick("###print_check_bridge");
    ctx->Yield(2);
    IM_CHECK(s_App->selection.First(ui::SelectionItem::Kind::Face) != nullptr);
    IM_CHECK(r.stacking.status == printcheck::Status::Fail);
    IM_CHECK(std::abs(r.stacking.limitMm - 0.6) < 1e-9);
    IM_CHECK_SILENT(r.stacking.lifted.size() == 1);
    IM_CHECK(std::abs(r.stacking.lifted[0].heightMm - 3.0) < 1e-6);
    s_App->selection.Clear();
    ctx->ItemClick("###print_check_stacking");
    ctx->Yield(2);
    IM_CHECK(s_App->selection.First(ui::SelectionItem::Kind::Face) != nullptr);
    Doc()->SetPrintProcess(std::nullopt);
    s_App->printCheck.reset();
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // Preferences > Print profiles is the store
  // (docs/tolerances/README.md#print-profiles): New writes the built-in
  // table as a file, Duplicate copies it, a rename moves it (refused onto a
  // name that is taken), a row edit rewrites it, Delete removes it. The GUI
  // runner points XDG_CONFIG_HOME at a scratch dir; the test still cleans up
  // by name.
  t = IM_REGISTER_TEST(engine, "tolerance",
                       "print_profiles_are_kept_in_preferences");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    for (const char* stale : {"New profile", "New profile 2", "Test P1S"})
      (void)ui::DeletePrintProfile(*s_App, stale);
    const auto find = [](const char* name) {
      return printfit::FindProfile(s_App->printProfiles, name);
    };
    const size_t before = s_App->printProfiles.size();
    ui::OpenPreferences(*s_App, "Print profiles");
    ctx->Yield(2);
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemClick("###prof_new");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->printProfiles.size(), before + 1);
    const printfit::Profile* made = find("New profile");
    IM_CHECK_SILENT(made != nullptr);
    if (made == nullptr)
      return;
    // "The defaults, corrected": it starts as a built-in table.
    IM_CHECK(made->table ==
             *printfit::FindBuiltin(made->table.process, made->table.material));
    const std::filesystem::path dir(ui::PrintProfileDir());
    IM_CHECK(std::filesystem::exists(dir / "new-profile.json"));

    ctx->ItemClick("###prof_dup");
    ctx->Yield(2);
    IM_CHECK(find("New profile 2") != nullptr);
    // The duplicate is selected; renamed onto a taken name, it is refused.
    ctx->ItemInputValue("###prof_name", "New profile");
    ctx->Yield(3);
    IM_CHECK(find("New profile 2") != nullptr);
    IM_CHECK_EQ(s_App->printProfiles.size(), before + 2);
    ctx->ItemInputValue("###prof_name", "Test P1S");
    ctx->Yield(3);
    IM_CHECK(find("Test P1S") != nullptr);
    IM_CHECK(find("New profile 2") == nullptr);
    IM_CHECK(std::filesystem::exists(dir / "test-p1s.json"));
    IM_CHECK(!std::filesystem::exists(dir / "new-profile-2.json"));

    // The gap row, typed as a coupon's answer, read back from DISK.
    int gap = -1;
    const printfit::Profile* mine = find("Test P1S");
    for (size_t i = 0; mine != nullptr && i < mine->table.rows.size(); i++)
      if (mine->table.rows[i].kind == printfit::Printed::Gap)
        gap = static_cast<int>(i);
    IM_CHECK_SILENT(gap >= 0);
    if (gap < 0)
      return;
    ctx->ItemInputValue(
        ("###prof_rows/###prof_row_allow_" + std::to_string(gap)).c_str(),
        "0.2");
    ctx->Yield(3);
    const auto fromDisk = DocumentIO::LoadPrintProfiles(ui::PrintProfileDir());
    const printfit::Profile* saved =
        printfit::FindProfile(fromDisk, "Test P1S");
    IM_CHECK_SILENT(saved != nullptr);
    if (saved == nullptr)
      return;
    const printfit::Row& row = saved->table.rows[static_cast<size_t>(gap)];
    IM_CHECK(std::abs(row.allowanceMm - 0.2) < 1e-9);
    IM_CHECK(row.source.rfind("coupon ", 0) == 0);

    ctx->ItemClick("###prof_delete");
    ctx->Yield(2);
    IM_CHECK(find("Test P1S") == nullptr);
    IM_CHECK(!std::filesystem::exists(dir / "test-p1s.json"));
    (void)ui::DeletePrintProfile(*s_App, "New profile");
    IM_CHECK_EQ(s_App->printProfiles.size(), before);
    s_App->settingsWindowOpen = false;
  };

  // A part picks one of the user's profiles in the Print process combo and
  // EMBEDS a copy; when the store's copy changes, the section says so and
  // offers both directions -- the CAM setup's machine block, for a print
  // (docs/tolerances/README.md#print-profiles).
  t = IM_REGISTER_TEST(engine, "tolerance", "print_profile_picked_and_matched");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    ctx->Yield(2);
    const char* kName = "Test pick P1S";
    (void)ui::DeletePrintProfile(*s_App, kName);
    auto mine = printfit::ProfileFromBuiltin(kName, "fdm", "pla");
    IM_CHECK_SILENT(mine.has_value());
    if (!mine)
      return;
    IM_CHECK(ui::SavePrintProfile(*s_App, *mine));
    const auto match = [] {
      return printfit::MatchProfile(s_App->printProfiles,
                                    *Doc()->PrintProcess());
    };

    ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
    ctx->ItemClick(w->TabId);
    ctx->Yield(2);
    if (ctx->ItemInfo("//###Variables/###print_process",
                      ImGuiTestOpFlags_NoError)
            .ID == 0) {
      ctx->ItemClick("//###Variables/Print process");
      ctx->Yield(2);
    }
    ctx->SetRef("//###Variables");
    ctx->ComboClick(
        (std::string("###print_process/###print_prof_") + kName).c_str());
    ctx->Yield(2);
    IM_CHECK(Doc()->PrintProcess().has_value());
    IM_CHECK(Doc()->PrintProcess()->profile.has_value());
    IM_CHECK(match() == printfit::ProfileMatch::Same);

    // The user re-measures: the store's gap is 0.1 a side now.
    printfit::Profile remeasured = *mine;
    for (printfit::Row& r : remeasured.table.rows)
      if (r.kind == printfit::Printed::Gap)
        r.allowanceMm = 0.2;
    IM_CHECK(ui::SavePrintProfile(*s_App, remeasured));
    ctx->Yield(2);
    IM_CHECK(match() == printfit::ProfileMatch::Differs);
    ctx->ItemClick("###print_use_mine");
    ctx->Yield(2);
    IM_CHECK(*Doc()->PrintProcess()->profile == remeasured);
    IM_CHECK(match() == printfit::ProfileMatch::Same);
    // Undoable: the part's old copy comes back, and differs again.
    s_App->commands->Undo(*Doc());
    ctx->Yield(2);
    IM_CHECK(*Doc()->PrintProcess()->profile == *mine);
    IM_CHECK(match() == printfit::ProfileMatch::Differs);
    // "Save as mine" goes the other way: the store takes the part's copy.
    ctx->ItemClick("###print_save_mine");
    ctx->Yield(2);
    IM_CHECK(match() == printfit::ProfileMatch::Same);
    IM_CHECK(*printfit::FindProfile(s_App->printProfiles, kName) == *mine);
    // Gone from the store: "missing", and the part still reads its copy.
    IM_CHECK(ui::DeletePrintProfile(*s_App, kName));
    ctx->Yield(2);
    IM_CHECK(match() == printfit::ProfileMatch::Missing);
    IM_CHECK(ctx->ItemExists("###print_save_mine"));
    ctx->ComboClick("###print_process/###print_none");
    ctx->Yield(2);
    IM_CHECK(!Doc()->PrintProcess().has_value());
    ctx->ItemClick(ImGui::FindWindowByName(ui::kPanelModel)->TabId);
    ctx->Yield(2);
  };

  // File > Export of a printed part's STL meshes at the profile's chords and
  // says so in the viewport
  // (docs/tolerances/README.md#exporting-a-printed-part).
  t = IM_REGISTER_TEST(engine, "tolerance",
                       "printed_stl_export_says_its_chords");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    AddFixtureCylinder(ctx);
    ctx->Yield(2);
    auto k5 = printfit::ProfileFromBuiltin("Test export P1S", "fdm", "pla");
    IM_CHECK_SILENT(k5.has_value());
    if (!k5)
      return;
    k5->stlDeflectionMm = 0.01;
    Doc()->SetPrintProcess(printfit::FromProfile(*k5));
    const auto path = std::filesystem::temp_directory_path() /
                      ("designer_gui_export_" +
                       std::to_string(static_cast<long>(::getpid())) + ".stl");
    std::string error;
    IM_CHECK(ui::ExportActiveDocument(*s_App, path.string(), &error));
    ctx->Yield(2);
    ctx->LogInfo("note: %s", s_App->exportNote.c_str());
    IM_CHECK(s_App->exportNote.find("chords 0.01 mm") != std::string::npos);
    IM_CHECK(!s_App->exportNoteFailed);
    IM_CHECK(ImGui::GetTime() < s_App->exportNoteUntil);
    std::filesystem::remove(path);
    // A failure says so in the same place, as a warning.
    IM_CHECK(
        !ui::ExportActiveDocument(*s_App, "/nonexistent-dir/x.stl", &error));
    IM_CHECK(s_App->exportNoteFailed);
    Doc()->SetPrintProcess(std::nullopt);
    s_App->exportNoteUntil = 0.0;
  };
}
