#include "app/guitests/GuiTestsCommon.h"
#include "core/AssemblySolve.h"
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/FlexibleActions.h"
#include "ui/MotionActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/ViewActions.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>

// Flexible sub-assemblies in the GUI (docs/assemblies/README.md
// #flexible-sub-assemblies): everything a person does with one, done the way
// they do it -- the checkbox and the picker in the occurrence's editor, the
// rows under it in the tree, the study's driver combo, a drag in the
// viewport -- and each undone with Ctrl+Z.

namespace {

// A crank in a housing, placed in an engine. The housing is FIXED in the
// sub-assembly and the crank turns on a revolute at its origin, seated
// above it by the part's own height so the two never touch. The sub-assembly
// has a configuration "Held" that holds the crank at 90 degrees. A second
// unit, "Loose", has nothing fixed: the one a Flexible tick must refuse.
struct Rig {
  uint64_t unitOcc = 0, looseOcc = 0, frameOcc = 0;
  uint64_t housingOcc = 0, crankOcc = 0, crankMate = 0, heldCfg = 0;
  size_t unitIdx = 0, engineIdx = 0;
  float height = 0.0f;
};

Rig BuildRig(ImGuiTestContext* ctx) {
  Rig r;
  ctx->SetRef("//##MainMenuBar");
  ToolbarCreateAccept(ctx, "###tb_box");
  ctx->Yield();
  const uint64_t partId = s_App->parts[0].id;
  glm::vec3 lo, hi;
  ui::PartDocById(*s_App, partId)->GetBodies()[0].shape.GetBoundingBox(lo, hi);
  const glm::vec3 size = hi - lo;
  r.height = size.z;

  r.unitIdx = ui::AddAssembly(*s_App, "Crank unit");
  ui::SetActivePart(*s_App, r.unitIdx);
  {
    Occurrence h;
    h.name = "Housing";
    h.partId = partId;
    h.fixed = true;
    r.housingOcc = s_App->assembly->AddOccurrence(h);
    Occurrence c;
    c.name = "Crank";
    c.partId = partId;
    r.crankOcc = s_App->assembly->AddOccurrence(c);
    Mate m;
    m.type = MateType::Revolute;
    m.name = "crank";
    m.a.occurrenceId = r.housingOcc;
    m.a.anchor.origin = true;
    m.b.occurrenceId = r.crankOcc;
    m.b.anchor.origin = true;
    m.flipped = false;
    m.offset = size.z + 0.5f * size.z;
    r.crankMate = s_App->assembly->AddMate(m);
    Configuration& held = s_App->assembly->AddConfiguration("Held");
    held.mateOverrides.push_back(
        {r.crankMate, glm::half_pi<float>(), std::nullopt});
    r.heldCfg = held.id;
    ui::SolveActiveAssembly(*s_App);
  }
  const uint64_t unitSlot = s_App->parts[r.unitIdx].id;

  const size_t looseIdx = ui::AddAssembly(*s_App, "Loose unit");
  ui::SetActivePart(*s_App, looseIdx);
  {
    Occurrence a;
    a.name = "Left";
    a.partId = partId;
    const uint64_t left = s_App->assembly->AddOccurrence(a);
    Occurrence b;
    b.name = "Right";
    b.partId = partId;
    b.position = glm::vec3(2.0f * size.x, 0.0f, 0.0f);
    const uint64_t right = s_App->assembly->AddOccurrence(b);
    Mate m;
    m.type = MateType::Revolute;
    m.a.occurrenceId = left;
    m.a.anchor.origin = true;
    m.b.occurrenceId = right;
    m.b.anchor.origin = true;
    m.flipped = false;
    s_App->assembly->AddMate(m);
  }
  const uint64_t looseSlot = s_App->parts[looseIdx].id;

  r.engineIdx = ui::AddAssembly(*s_App, "Engine");
  ui::SetActivePart(*s_App, r.engineIdx);
  Occurrence frame;
  frame.name = "Frame";
  frame.partId = partId;
  frame.fixed = true;
  r.frameOcc = s_App->assembly->AddOccurrence(frame);
  Occurrence unit;
  unit.name = "Pump";
  unit.partId = unitSlot;
  unit.refKind = OccurrenceRef::Assembly;
  unit.fixed = true;
  unit.position = glm::vec3(4.0f * size.x, 0.0f, 0.0f);
  r.unitOcc = s_App->assembly->AddOccurrence(unit);
  Occurrence loose;
  loose.name = "Loose";
  loose.partId = looseSlot;
  loose.refKind = OccurrenceRef::Assembly;
  loose.fixed = true;
  loose.position = glm::vec3(-6.0f * size.x, 0.0f, 0.0f);
  r.looseOcc = s_App->assembly->AddOccurrence(loose);
  ui::SolveActiveAssembly(*s_App);
  ctx->Yield(3);
  return r;
}

const Occurrence& Unit(const Rig& r) {
  return *s_App->assembly->FindById(r.unitOcc);
}

// The crank's angle as this placement has it posed.
double CrankDeg(const Rig& r) {
  return glm::degrees(assembly::MateDofAt(*s_App->assembly, {r.unitOcc},
                                          r.crankMate, MateDof::Rotation,
                                          ui::AssemblyResolverFor(*s_App))
                          .value_or(-1.0e9));
}

// The engine's drive and an idler, each on a revolute on the frame, chained
// 1:1 by a relation "belt".
struct DriveChain {
  uint64_t drive = 0, idler = 0, relation = 0;
};
DriveChain AddDriveChain(const Rig& r) {
  const uint64_t partId = s_App->parts[0].id;
  uint64_t mates[2] = {0, 0};
  for (int i = 0; i < 2; i++) {
    Occurrence o;
    o.name = i ? "Idler" : "Drive";
    o.partId = partId;
    const uint64_t id = s_App->assembly->AddOccurrence(o);
    Mate m;
    m.type = MateType::Revolute;
    m.name = i ? "idler" : "drive";
    m.a.occurrenceId = r.frameOcc;
    m.a.anchor.origin = true;
    m.b.occurrenceId = id;
    m.b.anchor.origin = true;
    m.flipped = false;
    m.offset = (2.0f + 2.0f * i) * r.height;
    mates[i] = s_App->assembly->AddMate(m);
  }
  Relation chain;
  chain.name = "belt";
  chain.kind = RelationKind::Chain;
  chain.mateA = mates[0];
  chain.mateB = mates[1];
  chain.ratio = 1.0;
  const uint64_t relId = s_App->assembly->AddRelation(chain);
  return {mates[0], mates[1], relId};
}

void Undo(ImGuiTestContext* ctx) {
  ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
  ctx->Yield(2);
}

// Tick Flexible in the Pump's editor.
void TickFlexible(ImGuiTestContext* ctx, const Rig& r) {
  ctx->ItemDoubleClick(
      ("//###Model/###occ_" + std::to_string(r.unitOcc)).c_str());
  ctx->Yield(2);
  IM_CHECK_EQ(s_App->asmEditOccurrence, r.unitOcc);
  ctx->ItemClick("//###ToolWindow/###occ_flexible");
  ctx->Yield(2);
}

} // namespace

void RegisterFlexibleGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;

  // The occurrence's editor: Flexible and the configuration picker, each one
  // undo step through the actions the RPC uses, and a refusal shown where
  // the click was.
  t = IM_REGISTER_TEST(engine, "flexible", "tick_flexible_and_configure");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const Rig r = BuildRig(ctx);
    IM_CHECK(!Unit(r).flexible);
    const size_t undo0 = s_App->assemblyHistory.undo.size();

    // A unit with nothing fixed is refused, in place, and nothing changes.
    ctx->ItemDoubleClick(
        ("//###Model/###occ_" + std::to_string(r.looseOcc)).c_str());
    ctx->Yield(2);
    ctx->ItemClick("//###ToolWindow/###occ_flexible");
    ctx->Yield(2);
    IM_CHECK(!s_App->assembly->FindById(r.looseOcc)->flexible);
    IM_CHECK_EQ(s_App->asmEditRefusalFor, r.looseOcc);
    ctx->LogInfo("refusal: %s", s_App->asmEditRefusal.c_str());
    IM_CHECK(s_App->asmEditRefusal.find("fixed") != std::string::npos);
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0);
    ctx->ItemClick("//###ToolWindow/###occ_edit_done");
    ctx->Yield();

    // The Pump: tick Flexible. Its crank now moves, and the tree shows it.
    TickFlexible(ctx, r);
    IM_CHECK(Unit(r).flexible);
    IM_CHECK(s_App->asmEditRefusal.empty());
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0 + 1);
    IM_CHECK(Unit(r).FindInnerPose(r.crankOcc) != nullptr);
    IM_CHECK(Unit(r).FindInnerPose(r.housingOcc) == nullptr); // folds in
    const std::string partRow = "//###Model/###occ_in_" +
                                std::to_string(r.unitOcc) + "_" +
                                std::to_string(r.crankOcc);
    const std::string mateRow = "//###Model/###occ_inmate_" +
                                std::to_string(r.unitOcc) + "_" +
                                std::to_string(r.crankMate);
    IM_CHECK(ctx->ItemExists(partRow.c_str()));
    IM_CHECK(ctx->ItemExists(mateRow.c_str()));

    // Pick "Held": the crank goes to the 90 degrees that configuration
    // holds, for this placement only -- the shared sub-assembly is not
    // written.
    const float authored =
        ui::AssemblyDocById(*s_App, s_App->parts[r.unitIdx].id)
            ->FindMateById(r.crankMate)
            ->rotation;
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick(
        ("###occ_config/###occ_config_" + std::to_string(r.heldCfg)).c_str());
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(2);
    IM_CHECK_EQ(Unit(r).configurationId, r.heldCfg);
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0 + 2);
    // A hold poses the part without rewriting the value it holds, as a
    // top-level configuration's does: read where the crank STANDS.
    const Occurrence* crank =
        ui::AssemblyDocById(*s_App, s_App->parts[r.unitIdx].id)
            ->FindById(r.crankOcc);
    const glm::mat4 pose = assembly::InnerPlacement(Unit(r), *crank);
    const double standsDeg = glm::degrees(std::atan2(pose[0][1], pose[0][0]));
    ctx->LogInfo("held crank stands at %.3f deg", standsDeg);
    IM_CHECK(std::abs(std::abs(standsDeg) - 90.0) < 1e-2);
    IM_CHECK(ui::AssemblyDocById(*s_App, s_App->parts[r.unitIdx].id)
                 ->FindMateById(r.crankMate)
                 ->rotation == authored);

    // Clicking the crank's row selects that PART, not the unit.
    ctx->ItemClick(partRow.c_str());
    ctx->Yield(2);
    const auto [unitSel, partSel] = ui::SelectedInnerOccurrence(*s_App);
    IM_CHECK_EQ(unitSel, r.unitOcc);
    IM_CHECK_EQ(partSel, r.crankOcc);
    IM_CHECK_EQ(ui::SelectedOccurrenceId(*s_App), (uint64_t)0);

    // Undo each: the configuration, then Flexible.
    s_App->asmEditOccurrence = 0;
    Undo(ctx);
    IM_CHECK_EQ(Unit(r).configurationId, (uint64_t)0);
    IM_CHECK(Unit(r).flexible);
    Undo(ctx);
    IM_CHECK(!Unit(r).flexible);
    IM_CHECK(!ctx->ItemExists(partRow.c_str()));
    // The selected part went with it.
    IM_CHECK_EQ(ui::SelectedInnerOccurrence(*s_App).first, (uint64_t)0);
  };

  // The relation editor names an inner mate as a side: an engine's drive
  // chained to the unit's crank, picked in the Driven combo as "Pump /
  // crank", and turning the drive then turns the crank.
  t = IM_REGISTER_TEST(engine, "flexible", "a_relation_side_is_an_inner_mate");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const Rig r = BuildRig(ctx);
    const DriveChain dc = AddDriveChain(r);
    const uint64_t relId = dc.relation;
    const uint64_t mates[2] = {dc.drive, dc.idler};
    ui::SolveActiveAssembly(*s_App);
    ctx->Yield(2);
    TickFlexible(ctx, r);
    ctx->ItemClick("//###ToolWindow/###occ_edit_done");
    ctx->Yield();

    ctx->ItemDoubleClick(
        ("//###Model/###rel_row_" + std::to_string(relId)).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->asmEditRelation, relId);
    const size_t undo0 = s_App->assemblyHistory.undo.size();
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick(("###rel_side_b/###rel_side_b_" +
                     std::to_string(r.unitOcc) + "_" +
                     std::to_string(r.crankMate))
                        .c_str());
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(2);
    const auto rel = [&]() { return s_App->assembly->FindRelationById(relId); };
    IM_CHECK_EQ(rel()->mateB, r.crankMate);
    IM_CHECK(rel()->mateBPath == std::vector<uint64_t>{r.unitOcc});
    IM_CHECK(rel()->mateAPath.empty());
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0 + 1);
    ctx->LogInfo("relation error: '%s'", rel()->error.c_str());
    IM_CHECK(rel()->error.empty());

    // Turn the drive: the chain turns the unit's crank with it.
    s_App->assembly->FindMateById(mates[0])->rotation = glm::radians(40.0f);
    ui::SolveActiveAssemblyDriven(*s_App, mates[0], MateDof::Rotation);
    ctx->Yield();
    ctx->LogInfo("drive at 40 deg, crank at %.3f deg", CrankDeg(r));
    IM_CHECK(std::abs(CrankDeg(r) - 40.0) < 1e-2);

    ctx->ItemClick("//###ToolWindow/###rel_edit_done");
    ctx->Yield();
    Undo(ctx); // the drive turn was not an undo step; the side pick was
    IM_CHECK_EQ(rel()->mateB, mates[1]);
    IM_CHECK(rel()->mateBPath.empty());
  };

  // A study driven by the unit's own crank, picked in the Motion window's
  // driver combo and run.
  t = IM_REGISTER_TEST(engine, "flexible", "an_inner_mate_drives_a_study");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const Rig r = BuildRig(ctx);
    TickFlexible(ctx, r);
    IM_CHECK(Unit(r).flexible);
    ctx->ItemClick("//###ToolWindow/###occ_edit_done");
    ctx->Yield();

    ToolbarClick(ctx, "###tb_asm_motion");
    ctx->Yield(2);
    IM_CHECK(s_App->motionToolOpen);
    // The engine has no mate of its own that moves: Add takes the unit's.
    ctx->ItemClick("//Motion###ToolWindow/###motion_add");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->assembly->GetMotionStudies().size(), (size_t)1);
    const uint64_t studyId = s_App->assembly->GetMotionStudies()[0].id;
    const auto study = [&]() {
      return s_App->assembly->FindMotionStudyById(studyId);
    };
    IM_CHECK(study()->driverPath == std::vector<uint64_t>{r.unitOcc});
    IM_CHECK_EQ(study()->driverMate, r.crankMate);

    // Point it somewhere else and back through the combo, as a person picks
    // it: clear the path by hand, then choose "Pump / crank".
    study()->driverPath.clear();
    study()->driverMate = 0;
    const size_t undo0 = s_App->assemblyHistory.undo.size();
    ctx->SetRef("//Motion###ToolWindow");
    ctx->ComboClick(("###motion_driver/###motion_driver_" +
                     std::to_string(r.unitOcc) + "_" +
                     std::to_string(r.crankMate))
                        .c_str());
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(2);
    IM_CHECK(study()->driverPath == std::vector<uint64_t>{r.unitOcc});
    IM_CHECK_EQ(study()->driverMate, r.crankMate);
    IM_CHECK(study()->driverDof == MateDof::Rotation);
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0 + 1);

    // Preview it: the crank turns through the run, pose by pose.
    ctx->ItemClick("//Motion###ToolWindow/###motion_preview");
    for (int i = 0; i < 600 && ui::MotionResultFor(*s_App, studyId) == nullptr;
         i++)
      ctx->Yield();
    const motion::Result* run = ui::MotionResultFor(*s_App, studyId);
    IM_CHECK(run != nullptr);
    IM_CHECK(run->samples.size() > 2);
    const motion::Verdict v = motion::Summarize(*run, *study(), false);
    ctx->LogInfo("verdict: %s", v.text.c_str());
    IM_CHECK(v.level != motion::Verdict::Level::Bad);
    // Scrubbing to the middle poses this placement's crank at half a turn.
    ctx->ItemInputValue("//Motion###ToolWindow/###motion_t", 0.5f);
    ctx->Yield(2);
    ctx->LogInfo("scrubbed crank to %.3f deg", CrankDeg(r));
    IM_CHECK(std::abs(std::abs(CrankDeg(r)) - 180.0) < 1e-2);

    // Undo the driver pick: the study goes back to naming nothing.
    ctx->ItemClick("//Motion###ToolWindow/###motion_close");
    ctx->Yield();
    Undo(ctx);
    IM_CHECK(study() != nullptr);
    IM_CHECK(study()->driverPath.empty());
  };

  // Dragging the crank in the viewport: a click on it selects the PART (not
  // the unit), its ring sits on its own mate, and a turn of the ring turns
  // the crank within it -- this placement's pose, one undo step.
  t = IM_REGISTER_TEST(engine, "flexible", "drag_an_inner_part_in_its_mate");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const Rig r = BuildRig(ctx);
    TickFlexible(ctx, r);
    IM_CHECK(Unit(r).flexible);
    ctx->ItemClick("//###ToolWindow/###occ_edit_done");
    ctx->Yield(2);

    // Aim at the crank's middle from above and click it.
    const Occurrence* crank =
        ui::AssemblyDocById(*s_App, s_App->parts[r.unitIdx].id)
            ->FindById(r.crankOcc);
    const glm::mat4 world = ui::InnerDisplayMatrix(*s_App, Unit(r), *crank);
    glm::vec3 lo, hi;
    ui::PartDocById(*s_App, crank->partId)
        ->GetBodies()[0]
        .shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = glm::vec3(world * glm::vec4((lo + hi) * 0.5f, 1.0f));
    const glm::vec3 top =
        glm::vec3(world * glm::vec4((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                    hi.z, 1.0f));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(mid + glm::vec3(0.0f, 0.0f, 1.0f) *
                                         glm::length(hi - lo) * 6.0f *
                                         LensScale());
    ctx->Yield(3);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const auto [unitSel, partSel] = ui::SelectedInnerOccurrence(*s_App);
    IM_CHECK_EQ(unitSel, r.unitOcc);
    IM_CHECK_EQ(partSel, r.crankOcc);

    // Its ring: on the crank's revolute, about the unit's Z.
    ctx->Yield(2);
    const auto ring = [&]() -> ui::ViewportHandle {
      for (const ui::ViewportHandle& h : s_App->viewportHandles)
        if (h.kind == ui::HandleKind::Rotate)
          return h;
      return {};
    };
    IM_CHECK(ring().length > 0.0f);
    int linear = 0;
    for (const ui::ViewportHandle& h : s_App->viewportHandles)
      linear += h.kind == ui::HandleKind::Linear ? 1 : 0;
    IM_CHECK_EQ(linear, 0); // a revolute turns; nothing slides
    const glm::vec3 centre = ring().origin;
    const glm::vec3 axis = ring().dir;
    IM_CHECK(std::abs(std::abs(axis.z) - 1.0f) < 1e-4f);
    const glm::vec3 b1 = ui::PerpTo(axis);
    const glm::vec3 b2 = glm::cross(axis, b1);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(centre);
    s_App->camera->SetUp(b2);
    s_App->camera->SetPosition(centre + axis * (40.0f * LensScale()) +
                               b1 * 3.0f);
    ctx->Yield(3);
    const float radius = ring().length;
    IM_CHECK(radius > 0.1f);
    const auto at = [&](float deg) {
      const float a = glm::radians(deg);
      return centre + (std::cos(a) * b1 + std::sin(a) * b2) * radius;
    };
    const double before = CrankDeg(r);
    const size_t undo0 = s_App->assemblyHistory.undo.size();
    ctx->MouseMoveToPos(WorldToWindow(at(0.0f)));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    for (int step = 1; step <= 3; step++) {
      ctx->MouseMoveToPos(WorldToWindow(at(30.0f * step)));
      ctx->Yield();
    }
    IM_CHECK(s_App->gizmoDrag.active && s_App->gizmoDrag.rotate);
    IM_CHECK_EQ(s_App->gizmoDrag.innerOcc, r.crankOcc);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const double turned = CrankDeg(r) - before;
    ctx->LogInfo("a 90 degree ring drag turned the crank %.2f degrees", turned);
    IM_CHECK(std::abs(std::abs(turned) - 90.0) < 1e-2); // snapped
    IM_CHECK_EQ(s_App->assemblyHistory.undo.size(), undo0 + 1);
    // The part is DRAWN where it turned to: its placement pose moved.
    const glm::mat4 after = assembly::InnerPlacement(Unit(r), *crank);
    IM_CHECK(std::abs(glm::degrees(std::atan2(after[0][1], after[0][0])) -
                      glm::degrees(std::atan2(0.0f, 1.0f))) > 1.0f);
    // The shared sub-assembly was not written.
    IM_CHECK(ui::AssemblyDocById(*s_App, s_App->parts[r.unitIdx].id)
                 ->FindMateById(r.crankMate)
                 ->rotation == 0.0f);

    Undo(ctx);
    IM_CHECK(std::abs(CrankDeg(r) - before) < 1e-3);
  };

  // An exploded view moves the crank on its own, by path (v130), on top of
  // the unit's step; the auto-explode stays top-level.
  t = IM_REGISTER_TEST(engine, "flexible", "explode_an_inner_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const Rig r = BuildRig(ctx);
    TickFlexible(ctx, r);
    ctx->ItemClick("//###ToolWindow/###occ_edit_done");
    ctx->Yield(2);
    ExplodedView v;
    v.name = "Apart";
    ui::SnapshotAssembly(*s_App);
    const uint64_t viewId = s_App->assembly->AddExplodedView(std::move(v));
    s_App->activeExplodedView = viewId;
    s_App->explodeFactor = 1.0f;
    ui::SelectInnerOccurrence(*s_App, r.unitOcc, r.crankOcc);
    ctx->Yield(3);

    // The handles are the crank's: world arrows at where it is drawn.
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK_EQ(s_App->viewportHandles.size(), (size_t)3);
    const glm::vec3 origin = s_App->viewportHandles[0].origin;
    const float L = ui::GizmoScale(*s_App);
    Ray grab;
    grab.origin = origin + glm::vec3(0.0f, 0.0f, 0.5f * L) +
                  glm::vec3(100.0f, 0.0f, 0.0f);
    grab.direction = {-1.0f, 0.0f, 0.0f};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    IM_CHECK(s_App->gizmoDrag.active);
    IM_CHECK_EQ(s_App->gizmoDrag.axis, 2);
    Ray moved = grab;
    moved.origin.z += 3.0f * r.height;
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    ctx->Yield(2);
    const ExplodedView* view = s_App->assembly->FindExplodedViewById(viewId);
    IM_CHECK_EQ(view->steps.size(), (size_t)1);
    IM_CHECK(view->steps[0].path == std::vector<uint64_t>{r.unitOcc});
    IM_CHECK_EQ(view->steps[0].occurrenceId, r.crankOcc);
    IM_CHECK(std::abs(view->steps[0].offset.z - 3.0f * r.height) < 1e-3f);
    // Only the crank's meshes moved; the housing's did not.
    int movedMeshes = 0, stillMeshes = 0;
    for (const ui::DocumentRenderMesh& m : s_App->documentMeshes) {
      if (m.occurrenceId != r.unitOcc)
        continue;
      const auto xf = s_App->meshRenderer->GetInstanceTransform(m.meshId);
      IM_CHECK_SILENT(xf.has_value());
      const glm::mat4 expect = ui::DisplayMatrixOf(*s_App, Unit(r)) * m.localXf;
      const float dz = (*xf)[3].z - expect[3].z;
      if (!m.innerPath.empty() && m.innerPath[0] == r.crankOcc) {
        IM_CHECK(std::abs(dz - 3.0f * r.height) < 1e-3f);
        movedMeshes++;
      } else {
        IM_CHECK(std::abs(dz) < 1e-4f);
        stillMeshes++;
      }
    }
    IM_CHECK(movedMeshes > 0 && stillMeshes > 0);

    Undo(ctx);
    IM_CHECK(s_App->assembly->FindExplodedViewById(viewId)->steps.empty());
    s_App->activeExplodedView = 0;
  };
}
