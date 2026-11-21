// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/GuiTests.h" // GuiTestCaptureDir
#include "app/guitests/GuiTestsCommon.h"
#include "core/Measure.h"
#include "core/fem/SimDocument.h"
#include "remote/RemoteSimulation.h"
#include "remote/RpcParams.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h" // the mesh view's style
#include "render/MeshRenderer.h"  // InstanceHighlightRanges (tint test)
#include "ui/AppActionsInternal.h"
#include "ui/AssemblyActions.h" // UndoAssembly, PartDocById (S1)
#include "ui/Commands.h"        // CommandEnabled
#include "ui/DocumentSlots.h"   // AddAssembly, SetActivePart
#include "ui/ListSelection.h"   // SelectedIds, DeleteSelection
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (arrow-placement test)
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h" // the kept mesh, the check, the field on show
#include "ui/StudyHost.h"      // ModifyStudy, SimulationContacts, StudyStale
#include "ui/StudyPanel.h"
#include "ui/VariableActions.h"    // SetProjectVariable
#include "ui/ViewActions.h"        // FitCameraToDocument
#include "ui/ViewportHandleDraw.h" // DrawViewportHandles

#include <filesystem>
#include <fstream>
#include <limits>

// Simulation GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.

namespace {
// The face of `body` whose outward normal is `n` (a box's), as a study
// names it: {the body's feature, the face's stable name}.
ElementRef FaceFacing(const Body& body, const glm::dvec3& n) {
  for (const auto& [index, name] : body.elementMap.names)
    if (const auto info = Measure::DescribeFaceAt(body, index))
      if (glm::dot(info->normal, n) > 0.999)
        return ElementRef{body.createdBy, name};
  return {};
}

// A steel block on the part in front: 100 x 100 x 16 at the origin.
void SteelBlock(ImGuiTestContext* ctx) {
  AddFixtureBox(ctx);
  const size_t index = Doc()->FeatureCount() - 1;
  auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(index));
  IM_CHECK_SILENT(box != nullptr);
  nlohmann::json before = DocumentIO::FeatureToJson(*box);
  box->position = glm::vec3(0.0f);
  box->width = 100.0;
  box->height = 100.0;
  box->depth = 16.0;
  box->SetMaterial("steel");
  nlohmann::json after = DocumentIO::FeatureToJson(*box);
  s_App->commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
  ctx->Yield(2);
}

// The slot id of the simulation tab in front.
uint64_t FrontSimSlot() { return ui::ActiveSimSlotId(*s_App); }

// Leaves the simulation tab in front as any tab is left: its subject's tab
// clicked. There is no Exit button and Escape does not leave.
void ClickSubjectTab(ImGuiTestContext* ctx) {
  IM_CHECK_SILENT(Sim() != nullptr);
  const auto at = ui::SlotIndexById(*s_App, Sim()->Subject().docId);
  IM_CHECK_SILENT(at.has_value());
  ctx->ItemClick(("//##PartTabs/**/###part" + std::to_string(*at)).c_str());
  ctx->Yield(2);
}

// The window as it stands, checked: the toolbar's row fits, and the tool
// window sits inside the viewport at under half its width (it sizes itself
// to its content, so one unwrapped sentence once made it the app's width).
// Saved as `<name>.png` when the run was given --capture-dir, for a person
// to look at (docs/ui/testing.md#recording-a-scene).
void SimWindowShot(ImGuiTestContext* ctx, const char* name) {
  ctx->Yield(4);
  for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if (std::string(w->Name).find("##ToolbarRow") != std::string::npos)
      IM_CHECK(w->ContentSize.x <= w->InnerRect.GetWidth() + 1.0f);
  if (ImGuiWindow* tw = ImGui::FindWindowByID(ImHashStr("###ToolWindow"));
      tw != nullptr && tw->Active) {
    const glm::vec4 r = s_App->toolWindowRect;
    ctx->LogInfo("%s: tool window %.0f x %.0f", name, r.z - r.x, r.w - r.y);
    IM_CHECK(r.z - r.x < 0.5f * s_App->viewportSize.x);
  }
  if (GuiTestCaptureDir().empty())
    return;
  ctx->CaptureReset();
  ImGuiCaptureArgs* args = ctx->CaptureArgs;
  std::snprintf(args->InOutputFile, sizeof(args->InOutputFile), "%s/%s.png",
                GuiTestCaptureDir().c_str(), name);
  args->InPadding = 0.0f;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  args->InCaptureRect =
      ImRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
  IM_CHECK(ctx->CaptureScreenshot(ImGuiCaptureFlags_IncludeOtherWindows |
                                  ImGuiCaptureFlags_IncludePopups));
}
} // namespace

void RegisterSimulationGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "sim", "gravity_pulls_down");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ToolbarClick(ctx, "###tb_simulation"); // the button had no id before this
    ctx->Yield();
    IM_CHECK(s_App->simulation.active);
    ctx->ItemClick("//###ToolWindow/###study_gravity");
    ctx->Yield();
    const fem::Study* study = Sim()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    // The button's first study is named as the toolbar names one.
    IM_CHECK_STR_EQ(study->name.c_str(), "Static 1");
    // Straight down in the world frame — a hand-written axis here once
    // sagged the part sideways and bent it about the wrong axis.
    IM_CHECK(glm::length(study->gravity - conv::kGravity) < 1e-9);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "sim", "force_pushes_into_the_picked_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    // A study needs stiffness data, and a new body has none: the panel says
    // so and offers the fix in place, rather than sending the user out of
    // simulation mode to find the feature's Material combo.
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield();
    const uint64_t createdBy = Doc()->GetBodies()[0].createdBy;
    const std::string materialRef =
        "//###ToolWindow/###material_" + std::to_string(createdBy);
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
                 CADMaterials::MaterialLibrary::MaterialTypeFromString(
                     Doc()->GetBodies()[0].material))
                 .youngGPa > 0.0f);

    // Arm the force tool: the magnitude seeds from the part's own weight, so
    // the click cannot place a silent 0 N load.
    ToolbarClick(ctx, "###tb_sim_force");
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingMagnitude > 0.0);

    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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
    const fem::Study* study = Sim()->FindStudy(s_App->simulation.activeStudyId);
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
  // change its magnitude (docs/ui/viewport-handles.md#simulation-load-drag).
  // Both asserted on the drawn primitives and the study, not on pixels.
  t = IM_REGISTER_TEST(engine, "sim", "force_arrow_points_along_the_force");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);

    // A force on a known face, pushing -Y.
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.names.empty());
    fem::Load load;
    load.id = Sim()->AllocateId();
    load.type = fem::LoadType::Force;
    load.face =
        ElementRef{body.createdBy, body.elementMap.names.begin()->second};
    load.magnitude = 100.0;
    load.direction = glm::dvec3(0.0, -1.0, 0.0);
    study->loads.push_back(load);
    ctx->Yield(2);

    // The glyph: the arrow's HEAD is the end the force points toward, so
    // head-minus-tail must run along the force direction. The glyph pass
    // PUBLISHES the arrow and the shared drawer draws it (the head is two
    // barbs at the far end; the shaft is the first primitive it emits).
    std::vector<PrimVertex> prims;
    s_App->viewportHandles.clear();
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    const size_t first = prims.size();
    ui::DrawViewportHandles(*s_App, prims);
    IM_CHECK(prims.size() > first);
    const glm::vec3 shaftA = prims[first].posThis,
                    shaftB = prims[first].posOther;
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
    glm::dvec3 lo(0.0), hi(0.0);
    body.shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
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
    study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr && !study->loads.empty());
    IM_CHECK(study->loads[0].magnitude > 100.0); // pulling grew it
    // ONE undo restores the grab-time magnitude — not a step per frame.
    IM_CHECK(ui::UndoSim(*s_App));
    ctx->Yield();
    study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr && !study->loads.empty());
    IM_CHECK(std::abs(study->loads[0].magnitude - 100.0) < 1e-6);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // No part of a force glyph may sit BEHIND its face plane, whichever way the
  // force points. The tail-behind placement put a pulling force's whole shaft
  // inside the solid, where the depth test hides it — an applied load with no
  // visible arrow.
  t = IM_REGISTER_TEST(engine, "sim", "force_arrow_never_buried_in_the_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);

    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.names.empty());
    const ElementRef face{body.createdBy,
                          body.elementMap.names.begin()->second};
    // The face's outward frame, through the same resolution the glyphs use.
    uint32_t mesh = 0, index = 0;
    IM_CHECK(ui::ResolveFaceItem(
        *s_App, {ui::SelectionItem::Kind::Face, 0, face}, mesh, index));
    const glm::vec3 c = s_App->faceSelector->GetFaceCenter(mesh, index);
    const glm::vec3 n = s_App->faceSelector->GetFaceNormal(mesh, index);

    fem::Load load;
    load.id = Sim()->AllocateId();
    load.type = fem::LoadType::Force;
    load.face = face;
    load.magnitude = 100.0;
    load.direction = glm::dvec3(n.x, n.y, n.z); // a PULL, out of the part
    study->loads.push_back(load);
    ctx->Yield(2);

    // Signed height of the glyph's lowest vertex over the face plane, and
    // the direction the shaft (the first primitive the shared drawer emits
    // for the published arrow) points.
    std::vector<PrimVertex> prims;
    size_t first = 0;
    const auto rebuild = [&]() {
      prims.clear();
      s_App->viewportHandles.clear();
      ui::AppendSimulationBCGlyphs(*s_App, prims);
      first = prims.size();
      ui::DrawViewportHandles(*s_App, prims);
    };
    const auto lowest = [&]() {
      float worst = FLT_MAX;
      for (const PrimVertex& p : prims) {
        worst = std::min(worst, glm::dot(p.posThis - c, n));
        worst = std::min(worst, glm::dot(p.posOther - c, n));
      }
      return worst;
    };
    rebuild();
    IM_CHECK(prims.size() > first);
    IM_CHECK(lowest() > -1e-3f);
    // Still pointing WITH the force: outside placement must not flip it.
    glm::vec3 along =
        glm::normalize(prims[first].posOther - prims[first].posThis);
    IM_CHECK(glm::dot(along, n) > 0.99f);

    // And a push keeps the old approach-from-outside placement.
    study->loads[0].direction = -load.direction;
    rebuild();
    IM_CHECK(prims.size() > first);
    IM_CHECK(lowest() > -1e-3f);
    along = glm::normalize(prims[first].posOther - prims[first].posThis);
    IM_CHECK(glm::dot(along, n) < -0.99f);

    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // Entering simulation mode redraws the Model dock window as the Studies
  // panel — so it must also WIN the shared node's tab, or the studies open
  // hidden behind whatever tab the user browsed last.
  t = IM_REGISTER_TEST(engine, "sim",
                       "entering_simulation_brings_the_studies_tab_forward");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    // Put another tab in front, the way a user browsing History does. A dock
    // tab is selected by clicking it: ImGui::FocusWindow deliberately does
    // not select dock tabs, so WindowFocus cannot stand in for the click.
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    ImGuiWindow* history = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(model != nullptr && model->DockNode != nullptr);
    IM_CHECK(history != nullptr && history->DockNode == model->DockNode);
    ctx->ItemClick(history->TabId);
    ctx->Yield(2);
    IM_CHECK_EQ(model->DockNode->SelectedTabId, history->TabId);

    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield(8); // the focus request repeats; give it a few frames to win
    IM_CHECK(s_App->simulation.active);
    IM_CHECK_EQ(model->DockNode->SelectedTabId, model->TabId);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // CAM (docs/cam/README.md): a setup on a part, an operation with derived
  // depths, and a backplot drawn from the SAME toolpath the post consumes.
  // docs/cam/gui.md#verb-windows: Z zero is PICKED, not typed. Getting it wrong
  // — zeroing on the table instead of the stock top — is the mistake that
  // breaks tools, so the panel refuses to guess it.
  // A new document takes the CAM job with it. This state used to survive:
  // the panel claimed "Z zero at 17.000" and offered to Save G-code for a
  // part that no longer existed, and the armed face pick came along too —
  // into a document where the button that re-arms it is DISABLED while a pick
  // is live, so the panel was simply stuck (docs/cam/gui.md#verb-windows).
  // The only two cases that need a real tet mesh: both assert on solve
  // numbers (beam-theory deflection and stress; a held volume fraction), and
  // Netgen is the one mesher — without it fem::Mesh returns "built without
  // FEM support" and there is nothing to check. Registered only when the
  // build HAS it, the same gate test_fem.cpp and test_topopt.cpp use, so a
  // Netgen-less build (CI's apt job: Netgen is not in Ubuntu's repos, see
  // the README) reports a suite that PASSES rather than two red cases that
  // say nothing about the code. The #else says so out loud — a quietly
  // smaller total reads as "everything covered" when it is not.
#ifdef HAVE_NETGEN
  t = IM_REGISTER_TEST(engine, "sim", "a_static_run_matches_beam_theory");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);

    // Euler-Bernoulli for a uniformly distributed load, steel E = 210 GPa:
    // I = b h^3/12 = 20*10^3/12 = 1666.7 mm^4, w = 100/200 = 0.5 N/mm, so
    // the tip deflection w L^4 / (8 E I) = 0.2857 mm, held to 15 %.
    IM_CHECK(rt->maxDisplacement > 0.243);
    IM_CHECK(rt->maxDisplacement < 0.329);
    // The peak stress sits at the clamp; the coarse mesh samples it at the
    // first node inboard (x = 10.6), where w(L-x)^2/2 * c/I = 26.9 MPa.
    // Measured: 27.1. A load along the wrong axis misses this by far more
    // than the band.
    IM_CHECK(rt->maxVonMises > 23.0);
    IM_CHECK(rt->maxVonMises < 31.5);
  };

  // The mesh step, clicked (docs/simulation/README.md#the-mesh-you-look-at):
  // Mesh makes the mesh and draws it -- element edges over a plain body in
  // place of the part -- the Show combo colours it by quality, the report
  // lists what the mesh says about itself, and a click on its worst element
  // frames it. A load edit leaves the mesh current; the mesh size moving
  // marks it stale.
  t = IM_REGISTER_TEST(engine, "sim",
                       "the_mesh_is_drawn_and_its_worst_element_framed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    using MeshView = ui::SimulationState::MeshView;
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    IM_CHECK(ui::StudyMesh(*s_App, studyId) == nullptr);
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/###mesh_view"));
    ToolbarClick(ctx, "###tb_sim_mesh");
    IM_CHECK(WaitFor(
        ctx, "the mesh",
        [studyId] { return ui::StudyMesh(*s_App, studyId) != nullptr; },
        120.0));
    ctx->Yield(3);
    const ui::MeshReport* mr = ui::StudyMesh(*s_App, studyId);
    IM_CHECK(mr != nullptr && !mr->bodies.empty());
    IM_CHECK(mr->bodies[0].elements > 0);
    IM_CHECK(mr->bodies[0].report.quality.size() == mr->bodies[0].elements);

    // Drawn: the overlay holds the element faces with their edges on, the
    // body under it is hidden, and nothing is coloured yet.
    IM_CHECK(s_App->simulation.meshView == MeshView::Edges);
    IM_CHECK(s_App->fieldRenderer != nullptr &&
             s_App->fieldRenderer->HasMesh());
    IM_CHECK(s_App->fieldRenderer->Style().edges);
    IM_CHECK(s_App->fieldRenderer->Style().plain);
    IM_CHECK(!s_App->gpu.hiddenMeshes.empty());
    IM_CHECK(s_App->simulation.shown != nullptr &&
             s_App->simulation.shown->mesh);

    // Coloured by quality, from the combo.
    ctx->ItemClick("//###ToolWindow/###mesh_view");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Quality").c_str());
    ctx->Yield(3);
    IM_CHECK(s_App->simulation.meshView == MeshView::Quality);
    IM_CHECK(!s_App->fieldRenderer->Style().plain);
    IM_CHECK(s_App->fieldRenderer->Style().reversed);
    IM_CHECK(s_App->simulation.shown->title == "element quality");
    IM_CHECK(s_App->simulation.shown->lo > 0.0);
    IM_CHECK(s_App->simulation.shown->lo <= 1.0);

    // The report: its worst element's row frames it.
    const std::vector<ui::MeshFinding> rows = ui::MeshFindings(*s_App, *mr);
    int worstRow = -1;
    for (size_t i = 0; i < rows.size() && worstRow < 0; ++i)
      if (rows[i].code == "worst_element")
        worstRow = static_cast<int>(i);
    IM_CHECK(worstRow >= 0);
    IM_CHECK(rows[worstRow].hasPlace);
    ctx->ItemOpen("//###ToolWindow/###mesh_report");
    ctx->Yield(2);
    const std::string rowRef =
        "//###ToolWindow/###mesh_row_" + std::to_string(worstRow);
    IM_CHECK(ctx->ItemExists(rowRef.c_str()));
    ctx->ItemClick(rowRef.c_str());
    WaitFor(
        ctx, "the camera to settle",
        [] { return !s_App->camera->IsAnimating(); }, 10.0);
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.placeMarked);
    const glm::dvec3 at = rows[worstRow].at;
    IM_CHECK(glm::length(glm::dvec3(s_App->camera->GetTarget()) - at) <
             rows[worstRow].radiusMm);
    // Framed, not merely aimed at: the element fills the view, so the
    // camera is within a few of its radii.
    IM_CHECK(s_App->camera->GetDistance() < 12.0f * rows[worstRow].radiusMm);

    // A load is not a mesh setting; the mesh size is.
    IM_CHECK(!ui::StudyMeshStale(*s_App, studyId));
    ui::ModifyStudy(*s_App, studyId, "Set load", [](fem::Study& cs) {
      for (fem::Load& l : cs.loads)
        if (l.type == fem::LoadType::Force)
          l.magnitude = 2.0 * l.magnitude;
    });
    ctx->Yield(2);
    IM_CHECK(!ui::StudyMeshStale(*s_App, studyId));
    ui::ModifyStudy(*s_App, studyId, "Set mesh size",
                    [](fem::Study& cs) { cs.meshSize = 3.0; });
    ctx->Yield(2);
    IM_CHECK(ui::StudyMeshStale(*s_App, studyId));
  };

  // The toolbar's Check (docs/simulation/README.md#checking-a-study): a
  // study with its support removed lists that before anything is run, and
  // the row goes when the support is back.
  t = IM_REGISTER_TEST(engine, "sim", "the_check_lists_problems_beside_run");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_check"); // its findings in the tool window
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.focus.kind ==
             ui::SimulationState::Focus::Kind::Check);
    IM_CHECK(ui::CachedStudyCheck(*s_App, studyId).problems.empty());
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/###check_row_0"));
    const fem::Study before = *Sim()->FindStudy(studyId);
    ui::ModifyStudy(*s_App, studyId, "Remove support", [](fem::Study& cs) {
      std::erase_if(cs.loads, [](const fem::Load& l) {
        return l.type == fem::LoadType::Fixed;
      });
    });
    ctx->Yield(3);
    const ui::StudyCheck& check = ui::CachedStudyCheck(*s_App, studyId);
    IM_CHECK_EQ(check.problems.size(), (size_t)1);
    IM_CHECK(check.problems[0].code == "no_support");
    IM_CHECK(check.problems[0].fixMethod == "add_load");
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###check_row_0"));
    // The row frames the body it is about.
    ctx->ItemClick("//###ToolWindow/###check_row_0");
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.placeMarked);
    ui::ModifyStudy(*s_App, studyId, "Restore support",
                    [&](fem::Study& cs) { cs.loads = before.loads; });
    ctx->Yield(3);
    IM_CHECK(ui::CachedStudyCheck(*s_App, studyId).problems.empty());
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/###check_row_0"));
  };

  // The result fields and the legend's range, clicked
  // (docs/simulation/README.md#results-display): the Field combo shows the
  // largest principal stress with its unit, a typed maximum puts the part
  // of the beam over it in the over colour, "Above yield" tops the range at
  // the steel's yield, the markers stand at the field's ends, and the VTU
  // export writes the whole result to the file it is given.
  t = IM_REGISTER_TEST(engine, "sim", "a_result_field_is_clamped_and_marked");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    using Field = ui::SimulationState::Field;
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    ctx->Yield(2);
    const auto choose = [&](const char* item) {
      ctx->ItemClick("//###ToolWindow/###result_field");
      ctx->Yield();
      IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
      ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
      IM_CHECK_SILENT(popup != nullptr);
      ctx->ItemClick((std::string("//") + popup->Name + "/**/" + item).c_str());
      ctx->Yield(3);
    };
    // A field this study's kind does not have is not offered at all.
    ctx->ItemClick("//###ToolWindow/###result_field");
    ctx->Yield();
    {
      IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
      const std::string popup =
          std::string("//") +
          ctx->UiContext->OpenPopupStack.back().Window->Name;
      IM_CHECK(ctx->ItemExists((popup + "/**/###field_principal_max").c_str()));
      IM_CHECK(
          !ctx->ItemExists((popup + "/**/###field_plastic_strain").c_str()));
      IM_CHECK(
          !ctx->ItemExists((popup + "/**/###field_contact_pressure").c_str()));
      ctx->ItemClick((popup + "/**/###field_principal_max").c_str());
      ctx->Yield(3);
    }
    IM_CHECK(s_App->simulation.field == Field::PrincipalMax);
    const ui::ShownField* shown = s_App->simulation.shown.get();
    IM_CHECK(shown != nullptr);
    IM_CHECK(shown->title == "max principal");
    IM_CHECK(shown->unit == "MPa");
    // The beam's top fibre is in tension at the clamp (about 27 MPa) and its
    // bottom in compression, so the largest principal stress spans from
    // about nothing to that.
    IM_CHECK(shown->hi > 20.0);
    IM_CHECK(shown->hi < 40.0);
    IM_CHECK(shown->hasExtremes);
    IM_CHECK(!shown->over);
    const double peak = shown->hi;
    // The maximum is at the clamp end (x = 0), the minimum is not.
    IM_CHECK(shown->hiAt.x < 30.0);

    // A typed maximum of half the peak: the bar stops there and what is
    // past it is over.
    ctx->ItemInputValue("//###ToolWindow/###result_range_hi",
                        static_cast<float>(peak * 0.5));
    ctx->Yield(3);
    shown = s_App->simulation.shown.get();
    IM_CHECK(s_App->simulation.clampHi);
    IM_CHECK(std::abs(shown->barHi - peak * 0.5) < 1e-3 * peak);
    IM_CHECK(shown->over);
    IM_CHECK(std::abs(shown->hi - peak) < 1e-9); // the field's own maximum
    // "Above yield": steel's 250 MPa is far over this beam, so nothing is.
    ctx->ItemClick("//###ToolWindow/###result_range_yield");
    ctx->Yield(3);
    shown = s_App->simulation.shown.get();
    IM_CHECK(shown->barHi > 200.0);
    IM_CHECK(!shown->over);
    // Another field starts from its own range.
    choose("###field_von_mises");
    IM_CHECK(s_App->simulation.field == Field::VonMises);
    IM_CHECK(!s_App->simulation.clampHi);
    // The factor of safety reads the other way round: its low end the hot
    // one, at the peak stress.
    choose("###field_factor_of_safety");
    shown = s_App->simulation.shown.get();
    IM_CHECK(shown != nullptr && shown->reversed);
    IM_CHECK(shown->unit.empty());
    IM_CHECK(shown->lo > 5.0); // 250 / ~27
    IM_CHECK(shown->lo < 12.0);
    // Most of the beam is far safer than four times it: that is the top
    // colour, "and above", not the off-scale one a typed range gives.
    IM_CHECK(!shown->over);
    IM_CHECK(shown->topOpen);
    // The markers: a factor is marked at its least value alone, three
    // lines, and nothing with the box unticked.
    IM_CHECK(s_App->simulation.showExtremes);
    {
      std::vector<LineVertex3D> lines;
      ui::AppendStudyMarker(*s_App, lines);
      IM_CHECK_EQ(lines.size(), (size_t)6);
    }
    // A stress is marked at both ends.
    choose("###field_von_mises");
    {
      std::vector<LineVertex3D> lines;
      ui::AppendStudyMarker(*s_App, lines);
      IM_CHECK_EQ(lines.size(), (size_t)12);
    }
    ctx->ItemClick("//###ToolWindow/###result_extremes");
    ctx->Yield();
    IM_CHECK(!s_App->simulation.showExtremes);
    {
      std::vector<LineVertex3D> lines;
      ui::AppendStudyMarker(*s_App, lines);
      IM_CHECK(lines.empty());
    }
    ctx->ItemClick("//###ToolWindow/###result_extremes");

    // Export: the whole result, to the file the dialog would have named.
    const std::string path =
        (std::filesystem::temp_directory_path() / "lobole_gui_export.vtu")
            .string();
    std::filesystem::remove(path);
    s_App->saveDialogAnswer = path;
    ToolbarClick(ctx, "###tb_sim_export_vtu");
    ctx->Yield(2);
    IM_CHECK(std::filesystem::exists(path));
    IM_CHECK(std::filesystem::file_size(path) > 10000);
    IM_CHECK(s_App->simulation.exportNote.rfind("Exported ", 0) == 0);
    std::filesystem::remove(path);
  };

  // A buckling study, clicked (docs/simulation/README.md#buckling): the
  // analysis combo makes the cantilever's study a buckling one with its
  // three factors, Run solves it, the window lists the factors in place of
  // frequencies with the governing factor beside them, and picking one
  // shows its mode.
  t = IM_REGISTER_TEST(engine, "sim", "a_buckling_run_lists_its_factors");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ctx->ItemClick("//###ToolWindow/###study_analysis");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Buckling").c_str());
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(studyId);
    IM_CHECK(st->type == fem::AnalysisType::Buckling);
    IM_CHECK_EQ(st->numModes, fem::kBucklingFactorsDefault);
    // CalculiX solves buckling too: the solver combo stays.
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_solver"));
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);
    IM_CHECK(rt != nullptr && !rt->bodies.empty());
    IM_CHECK(rt->bodies[0].buckling.solved);
    IM_CHECK(rt->bodies[0].results.modes.size() >= 2);
    IM_CHECK(rt->bodies[0].buckling.governingFactor > 0.0);
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###buckle_mode_0"));
    ctx->ItemClick("//###ToolWindow/###buckle_mode_1");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->simulation.activeMode, 1);
    IM_CHECK(s_App->simulation.field ==
             ui::SimulationState::Field::Displacement);
  };

  // The solver combo (docs/simulation/README.md#calculix): there on a
  // static study, CalculiX choosable even where it is not found, and Run
  // then refuses in the window, saying how to install it, starting nothing.
  // A switch to a type CalculiX does not solve takes the study back
  // in-house.
  t = IM_REGISTER_TEST(engine, "sim", "calculix_is_chosen_and_refused_visibly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    const std::string before = s_App->settings.calculixPath;
    s_App->settings.calculixPath = "/nonexistent/lobole-test/ccx";
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_solver"));
    ctx->ItemClick("//###ToolWindow/###study_solver");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick(
        (std::string("//") + popup->Name + "/**/CalculiX (not found)").c_str());
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->solver == fem::Solver::Calculix);
    ToolbarClick(ctx, "###tb_sim_run");
    ctx->Yield(2);
    st = Sim()->FindStudy(studyId);
    IM_CHECK(st->error.find("calculix_path setting") != std::string::npos);
    IM_CHECK(st->error.find("sudo apt install calculix-ccx") !=
             std::string::npos);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "study"));
    IM_CHECK(Sim()->StudyResults(studyId) == nullptr);

    ctx->ItemClick("//###ToolWindow/###study_analysis");
    ctx->Yield();
    popup = ctx->UiContext->OpenPopupStack.back().Window;
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Modal").c_str());
    ctx->Yield(2);
    st = Sim()->FindStudy(studyId);
    IM_CHECK(st->type == fem::AnalysisType::Modal);
    IM_CHECK(st->solver == fem::Solver::Auto);
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/###study_solver"));
    s_App->settings.calculixPath = before;
  };

  // Load cases (docs/topopt/README.md#load-cases), clicked: the first case
  // takes the force there is, a force placed while a second is chosen goes
  // in it, and the run answers each case, the viewport showing the one
  // picked.
  t = IM_REGISTER_TEST(engine, "sim",
                       "load_cases_take_the_force_placed_in_them");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_add_case");
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_sim_add_case");
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(studyId);
    IM_CHECK_EQ(st->loadCases.size(), (size_t)2);
    IM_CHECK(st->loads[1].loadCase == st->loadCases[0].name);

    // Arm the force tool; the case it goes in is chosen in the panel.
    ToolbarClick(ctx, "###tb_sim_force");
    ctx->Yield();
    ctx->ItemClick("//###ToolWindow/###sim_pending_case");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Case 2").c_str());
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingCase == "Case 2");
    // Sideways on the far end face (the pick itself is the placement tests'
    // business): the same path a click takes.
    const uint64_t cb = Doc()->GetBodies()[0].createdBy;
    ui::SimulationApplyToFace(*s_App,
                              ElementRef{cb, std::to_string(cb) + "/face1"});
    ctx->Yield(2);
    st = Sim()->FindStudy(studyId);
    IM_CHECK_EQ(st->loads.size(), (size_t)3);
    IM_CHECK(st->loads[2].loadCase == "Case 2");

    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);
    IM_CHECK_EQ(rt->caseNames.size(), (size_t)2);
    IM_CHECK_EQ(rt->bodies[0].cases.size(), (size_t)2);
    // Case 1 bends the beam down, case 2 pushes its end: different answers.
    IM_CHECK(std::abs(rt->bodies[0].Case(0).maxDisplacement -
                      rt->bodies[0].Case(1).maxDisplacement) > 1e-6);
    ctx->ItemClick("//###ToolWindow/###case_sel_1");
    ctx->Yield();
    IM_CHECK_EQ(s_App->simulation.activeCase, 1);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "sim", "topology_holds_its_volume_target");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 60.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    // Topology is reachable ONLY through this combo, which had no ### id and
    // so could not be clicked by a test at all — which is why the whole
    // workstream had no clicked coverage.
    ctx->ItemClick("//###ToolWindow/###study_analysis");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Topology").c_str());
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr);
    IM_CHECK(st->type == fem::AnalysisType::Topology);

    // The auto mesh is sized for a stress field; a shape needs one that can
    // hold a shape. Four iterations is enough for this gate: the volume
    // constraint is enforced by the OC bisection on every one of them.
    // The size is the simulation's mesh's (its row's settings), the
    // iterations the study's.
    SimTreeClick(ctx, "###sim_mesh");
    ctx->ItemInputValue("//###ToolWindow/###sim_mesh_size", "10");
    ctx->Yield(2);
    SimFocusStudy(ctx);
    ctx->ItemInputValue("//###ToolWindow/###topo_max_iterations", "4");
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);
    // The OC bisection enforces the volume every iteration, so this holds
    // whatever the compliance does — UNLESS every sensitivity is zero, which
    // is exactly what a zero displacement field produces. That was the shape
    // the GPU failure took, and it landed on 0.107 against this 0.30.
    IM_CHECK(std::abs(rt->volumeFractionAchieved - st->volumeFraction) < 0.02);
    // A structure carrying a load has compliance > 0.
    IM_CHECK(!rt->complianceHistory.empty());
    IM_CHECK(rt->complianceHistory.back() > 0.0);

    // The Shape view is a mesh instance of its own; leaving the tab must
    // take it away. A tab change reset the simulation state and left the
    // instance drawn -- the pan/tilt yoke's optimised shape stood over every
    // part opened after it, in other projects too.
    s_App->simulation.field = ui::SimulationState::Field::Shape;
    ctx->Yield(3);
    const uint32_t iso = s_App->gpu.topoIsoInstance;
    IM_CHECK(iso != 0);
    IM_CHECK(s_App->meshRenderer->GetInstanceTransform(iso).has_value());
    ui::AddPart(*s_App, "Elsewhere");
    ctx->Yield(3);
    IM_CHECK(!s_App->meshRenderer->GetInstanceTransform(iso).has_value());
  };

#else  // !HAVE_NETGEN
  LOG_WARN("GUI suite: 4 sim cases NOT registered "
           "(a_static_run_matches_beam_theory, a_buckling_run_lists_its_"
           "factors, topology_holds_its_volume_target, a_probe_pins_a_face_"
           "and_holds_it) — this build has no Netgen, so a study cannot be "
           "meshed");
#endif // HAVE_NETGEN

  // A pressure is a FIELD: one arrow per cell of the face, each standing ON
  // the face and pressing into it. One arrow at the centre read as a point
  // load on a face that carries none, and on a curved or L-shaped face the
  // centre is not even on the face.
  t = IM_REGISTER_TEST(engine, "sim", "pressure_is_a_field_over_the_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    // The default box is 2 mm on a side, and the glyph length has a floor
    // of 1.2 mm, so a 4 mm^2 face can never hold a second arrow: make it a
    // part-sized box through the command layer, as the cantilever helper
    // does.
    {
      const size_t index = Doc()->FeatureCount() - 1;
      auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(index));
      IM_CHECK(box != nullptr);
      nlohmann::json before = DocumentIO::FeatureToJson(*box);
      box->width = 40.0;
      box->height = 40.0;
      box->depth = 40.0;
      nlohmann::json after = DocumentIO::FeatureToJson(*box);
      s_App->commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
      ctx->Yield(2);
    }
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.names.empty());
    const ElementRef face{body.createdBy,
                          body.elementMap.names.begin()->second};
    uint32_t mesh = 0, index = 0;
    IM_CHECK(ui::ResolveFaceItem(
        *s_App, {ui::SelectionItem::Kind::Face, 0, face}, mesh, index));
    const glm::vec3 c = s_App->faceSelector->GetFaceCenter(mesh, index);
    const glm::vec3 n = s_App->faceSelector->GetFaceNormal(mesh, index);
    const float area = s_App->faceSelector->GetFaceArea(mesh, index);
    IM_CHECK(area > 0.0f);

    // The sampler alone: a spacing wider than the face gives the centre and
    // nothing else; a spacing that divides it into ~16 cells gives about
    // that many, every one on the face's plane and inside its extent.
    const auto coarse =
        ui::SampleFace(*s_App, 0, face, 10.0f * std::sqrt(area), 160);
    IM_CHECK_EQ(coarse.size(), (size_t)1);
    IM_CHECK(glm::length(coarse[0].point - c) < 1e-3f);
    const auto fine =
        ui::SampleFace(*s_App, 0, face, std::sqrt(area / 16.0f), 160);
    IM_CHECK(fine.size() >= 12 && fine.size() <= 20);
    glm::dvec3 lo(0.0), hi(0.0);
    body.shape.GetBoundingBox(lo, hi);
    for (const ui::FaceSample& fs : fine) {
      IM_CHECK(std::abs(glm::dot(fs.point - c, n)) < 1e-3f); // on the plane
      IM_CHECK(glm::dot(fs.normal, n) > 0.99f);
      for (int k = 0; k < 3; ++k)
        IM_CHECK(fs.point[k] > lo[k] - 1e-3f && fs.point[k] < hi[k] + 1e-3f);
    }

    // The glyph pass, with the camera close enough that the view-scaled
    // spacing divides the face: more than one arrow, and every arrow's HEAD
    // (the shaft's far end, the second vertex of its first primitive) on
    // the face.
    fem::Load load;
    load.id = Sim()->AllocateId();
    load.type = fem::LoadType::Pressure;
    load.face = face;
    load.magnitude = 0.5;
    study->loads.push_back(load);
    ctx->Yield(2);
    s_App->camera->StopAnimation();
    const glm::vec3 mid = (lo + hi) * 0.5;
    s_App->camera->SetTarget(mid);
    s_App->camera->SetPosition(mid + n * 14.0f);
    ctx->Yield(2);
    std::vector<PrimVertex> prims;
    s_App->viewportHandles.clear();
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    // Three primitives per arrow (shaft + two barbs); each primitive is a
    // six-vertex quad in the prim channel, and the shaft comes first.
    constexpr size_t kArrowVerts = 3 * 6;
    IM_CHECK(prims.size() >= 2 * kArrowVerts);
    int arrows = 0, onFace = 0;
    for (size_t i = 0; i + kArrowVerts <= prims.size(); i += kArrowVerts) {
      const glm::vec3 head = prims[i].posOther;
      const glm::vec3 tail = prims[i].posThis;
      ++arrows;
      if (std::abs(glm::dot(head - c, n)) < 1e-3f &&
          glm::dot(glm::normalize(head - tail), n) < -0.99f) // INTO the face
        ++onFace;
    }
    IM_CHECK(arrows > 1);
    IM_CHECK_EQ(onFace, arrows);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A boundary condition tints the face it acts on, in its family's colour,
  // through the same highlight channel a selection uses -- so "what is
  // actually fixed" is an area on the part and not an inference from a
  // glyph at its centre.
  t = IM_REGISTER_TEST(engine, "sim", "fixed_and_loaded_faces_are_tinted");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(body.elementMap.names.size() >= 2);
    auto it = body.elementMap.names.begin();
    const ElementRef fixedFace{body.createdBy, it->second};
    ++it;
    const ElementRef loadedFace{body.createdBy, it->second};
    fem::Load fixed;
    fixed.id = Sim()->AllocateId();
    fixed.type = fem::LoadType::Fixed;
    fixed.face = fixedFace;
    study->loads.push_back(fixed);
    fem::Load press;
    press.id = Sim()->AllocateId();
    press.type = fem::LoadType::Pressure;
    press.face = loadedFace;
    press.magnitude = 1.0;
    study->loads.push_back(press);
    ctx->Yield(2);

    ui::SyncSelectionHighlights(*s_App);
    const auto rangeOf = [&](const ElementRef& f, uint32_t& mesh,
                             uint32_t& first, uint32_t& end) {
      uint32_t index = 0, count = 0;
      IM_CHECK_RETV(ui::ResolveFaceItem(*s_App,
                                        {ui::SelectionItem::Kind::Face, 0, f},
                                        mesh, index),
                    false);
      IM_CHECK_RETV(
          s_App->faceSelector->GetFaceVertexRange(mesh, index, first, count),
          false);
      end = first + count;
      return true;
    };
    const auto tinted = [&](const ElementRef& f, uint32_t tint) {
      uint32_t mesh = 0, first = 0, end = 0;
      if (!rangeOf(f, mesh, first, end))
        return false;
      for (const glm::uvec2& r :
           s_App->meshRenderer->InstanceHighlightRanges(mesh))
        if (r.x == first && (r.y & MeshRenderer::kRangeMask) == end &&
            (r.y >> MeshRenderer::kTintShift) == tint)
          return true;
      return false;
    };
    IM_CHECK(tinted(fixedFace, MeshRenderer::kTintFixture));
    IM_CHECK(tinted(loadedFace, MeshRenderer::kTintLoad));
    IM_CHECK(!tinted(fixedFace, MeshRenderer::kTintLoad));
    // Nothing is SELECTED, so no range carries the selection's tint.
    IM_CHECK(!tinted(fixedFace, MeshRenderer::kTintSelection));
    IM_CHECK(!tinted(loadedFace, MeshRenderer::kTintSelection));
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // Gravity is drawn: one arrow leaving the centre of mass along g. A
  // self-weight study whose picture showed no weight could not be checked
  // against the panel.
  t = IM_REGISTER_TEST(engine, "sim", "gravity_is_drawn_at_the_centre_of_mass");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Sim()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    study->gravity = conv::kGravity;
    ctx->Yield(2);
    // The arrow leaves the centre of mass, which is INSIDE the solid, so it
    // is drawn in the see-through channel the measurements use -- the
    // depth-tested one would hide it.
    std::vector<PrimVertex> prims;
    ui::AppendSimulationOverlayGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    // A box's centre of mass is its bounding-box centre.
    glm::dvec3 lo(0.0), hi(0.0);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5;
    const glm::vec3 g = glm::normalize(glm::vec3(conv::kGravity));
    // The longest primitive that starts at the centre of mass is the arrow's
    // shaft, and it runs along g.
    float best = 0.0f;
    glm::vec3 along(0.0f);
    for (size_t i = 0; i < prims.size(); i += 6) {
      if (glm::length(prims[i].posThis - mid) > 1e-2f)
        continue;
      const float l = glm::length(prims[i].posOther - prims[i].posThis);
      if (l > best) {
        best = l;
        along = glm::normalize(prims[i].posOther - prims[i].posThis);
      }
    }
    IM_CHECK(best > 0.0f);
    IM_CHECK(glm::dot(along, g) > 0.999f);
    // And with gravity off, nothing stands at the centre of mass.
    study->gravity = glm::dvec3(0.0);
    prims.clear();
    ui::AppendSimulationOverlayGlyphs(*s_App, prims);
    for (size_t i = 0; i < prims.size(); i += 6)
      IM_CHECK(glm::length(prims[i].posThis - mid) > 1e-2f);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A study on an ASSEMBLY tab (docs/simulation/README.md#assembly-studies).
  // Two placed blocks; the upper meshed, the lower a rigid support; one click
  // clamps the upper where the lower touches it. The picture says so: the
  // support is a ghost, its contact face tinted as a fixture.
  t = IM_REGISTER_TEST(engine, "sim",
                       "an_assembly_study_clamps_where_the_support_touches");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ctx->Yield();
    {
      const size_t index = Doc()->FeatureCount() - 1;
      auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(index));
      IM_CHECK(box != nullptr);
      nlohmann::json before = DocumentIO::FeatureToJson(*box);
      box->position = glm::vec3(0.0f);
      box->width = 100.0;
      box->height = 100.0;
      box->depth = 16.0;
      box->SetMaterial("steel");
      nlohmann::json after = DocumentIO::FeatureToJson(*box);
      s_App->commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after), *Doc());
      ctx->Yield(2);
    }
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Stack");
    ui::SetActivePart(*s_App, asmIdx);
    uint64_t lower = 0, upper = 0;
    {
      Occurrence lo;
      lo.name = "Lower";
      lo.partId = partId;
      lo.fixed = true;
      lower = s_App->assembly->AddOccurrence(lo);
      Occurrence up;
      up.name = "Upper";
      up.partId = partId;
      up.position = glm::vec3(0.0f, 0.0f, 16.0f);
      upper = s_App->assembly->AddOccurrence(up);
    }
    ctx->Yield(3);

    // The toolbar opens the assembly's simulation tab, made for it.
    ToolbarClick(ctx, "###tb_asm_simulation");
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.active);
    IM_CHECK(s_App->sim != nullptr);
    IM_CHECK(s_App->sim->Subject().kind == fem::SubjectKind::Assembly);
    // Its Add study group (whose face is the kind last used).
    IM_CHECK(ctx->ItemExists("//##DockHost/**/sim_study/###more"));
    IM_CHECK_EQ(Sim()->GetStudies().size(), (size_t)1);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    IM_CHECK(studyId != 0);

    // Roles through the study edit path (the combos draw per occurrence).
    ui::ModifyStudy(*s_App, studyId, "roles", [&](fem::Study& cs) {
      cs.participants.push_back(
          {upper, fem::ParticipantRole::Meshed, ExprValue{0.0}});
      cs.participants.push_back(
          {lower, fem::ParticipantRole::Rigid, ExprValue{0.0}});
      cs.meshSize = 12.0;
    });
    ctx->Yield(2);
    // The rigid participant's row offers the clamp in the tool window; one
    // click adds the contact anchor.
    SimTreeClick(ctx, "###sim_part_" + std::to_string(lower));
    IM_CHECK(s_App->simulation.focus.kind ==
             ui::SimulationState::Focus::Kind::Participant);
    const std::string clamp =
        "//###ToolWindow/###clamp_" + std::to_string(lower);
    IM_CHECK(ctx->ItemExists(clamp.c_str()));
    ctx->ItemClick(clamp.c_str());
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Fixed);
    IM_CHECK_EQ(st->loads[0].contactWith, lower);
    // And one undo takes the clamp away again: a snapshot of the simulation.
    ui::UndoSim(*s_App);
    ctx->Yield();
    st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->loads.empty());
    ctx->ItemClick(clamp.c_str());
    ctx->Yield(2);
    st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->loads.size() == 1);

    // A face clicked on the SUPPORT is refused in words, not dropped.
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pressure);
    const Body& body = Doc() ? Doc()->GetBodies()[0]
                             : ui::PartDocById(*s_App, partId)->GetBodies()[0];
    const ElementRef someFace{body.createdBy,
                              body.elementMap.names.begin()->second};
    ui::SimulationApplyToFace(*s_App, lower, someFace);
    IM_CHECK(!s_App->simulation.placeRefusal.empty());
    st = Sim()->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->loads.size() == 1);
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::None);

    // The picture: the support's contact face is tinted as a fixture and
    // the support itself is a ghost; the meshed part is neither.
    ctx->Yield(2);
    ui::SyncSelectionHighlights(*s_App);
    ui::AppendSimulationFaceTints(*s_App);
    const auto& contacts = ui::SimulationContacts(*s_App);
    IM_CHECK_EQ(contacts.size(), (size_t)1);
    IM_CHECK(!contacts[0].rigidFaces.empty());
    bool tinted = false, ghost = false, meshedGhost = false;
    for (const ui::DocumentRenderMesh& m : s_App->gpu.documentMeshes) {
      const uint64_t occ = ui::SimBodyOccurrence(*s_App, m.bodyIndex);
      if (occ == lower) {
        ghost = ghost || s_App->meshRenderer->IsInstanceGhost(m.meshId);
        for (const glm::uvec2& r :
             s_App->meshRenderer->InstanceHighlightRanges(m.meshId))
          tinted = tinted || (r.y >> MeshRenderer::kTintShift) ==
                                 MeshRenderer::kTintFixture;
      }
      if (occ == upper)
        meshedGhost =
            meshedGhost || s_App->meshRenderer->IsInstanceGhost(m.meshId);
    }
    IM_CHECK(tinted);
    IM_CHECK(ghost);
    IM_CHECK(!meshedGhost);

#ifdef HAVE_NETGEN
    // Run from the window; the result lands on the simulation, fresh.
    ToolbarClick(ctx, "###tb_sim_run");
    WaitFor(
        ctx, "the assembly study's result",
        [&] {
          const fem::Study* now = Sim()->FindStudy(studyId);
          return Sim()->StudyResults(studyId) != nullptr ||
                 (now != nullptr && !now->error.empty());
        },
        120.0);
    const fem::Study* cur = Sim()->FindStudy(studyId);
    IM_CHECK(cur != nullptr);
    if (!cur->error.empty())
      ctx->LogError("assembly study: %s", cur->error.c_str());
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);
    IM_CHECK(rt != nullptr);
    IM_CHECK(rt->maxDisplacement >= 0.0);
    IM_CHECK(!ui::StudyStale(*s_App, studyId));
#endif

    // Another tab leaves simulation mode and the window goes with it.
    ClickSubjectTab(ctx);
    IM_CHECK(!s_App->simulation.active);
    IM_CHECK(!s_App->simulationToolOpen);
  };

  t = IM_REGISTER_TEST(engine, "sim", "new_document_leaves_the_mode");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
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

  // A load typed as a variable shows the variable's value. A study's
  // stored magnitude is evaluated at its run, so before one (or when the run
  // reuses a twin's answer) it holds whatever was there, and the panel's row
  // and the viewport's label read 0.0 N on the SECOND project built the same
  // way in one instance (docs/simulation/README.md#gui).
  t = IM_REGISTER_TEST(engine, "sim",
                       "a_variable_driven_force_shows_its_value");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    for (int round = 0; round < 2; ++round) {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New");
      ctx->Yield(2);
      IM_CHECK_SILENT(ui::SetProjectVariable(*s_App, "load_n", "10000", ""));
      AddFixtureBox(ctx);
      ui::EnterSimulation(*s_App);
      ctx->Yield(2);
      const uint64_t id = s_App->simulation.activeStudyId;
      IM_CHECK_SILENT(Sim()->FindStudy(id) != nullptr);
      const Body& body = Doc()->GetBodies()[0];
      std::vector<ElementRef> faces;
      for (const auto& [index, name] : body.elementMap.names)
        faces.push_back(ElementRef{body.createdBy, name});
      IM_CHECK_SILENT(faces.size() >= 2);
      uint64_t forceId = 0;
      ui::ModifyStudy(*s_App, id, "loads", [&](fem::Study& st) {
        fem::Load fixed;
        fixed.id = Sim()->AllocateId();
        fixed.type = fem::LoadType::Fixed;
        fixed.face = faces[0];
        st.loads.push_back(fixed);
        fem::Load force;
        force.id = forceId = Sim()->AllocateId();
        force.type = fem::LoadType::Force;
        force.face = faces[1];
        force.magnitude.expr = "load_n";
        st.loads.push_back(force);
      });
      ctx->Yield(3);
      const fem::Study* st = Sim()->FindStudy(id);
      IM_CHECK_SILENT(st != nullptr);
      const fem::Load* force = nullptr;
      for (const fem::Load& l : st->loads)
        if (l.id == forceId)
          force = &l;
      IM_CHECK_SILENT(force != nullptr);
      ctx->LogInfo("round %d: stored magnitude %g", round,
                   force->magnitude.value);
      // What the row, the label and a drag start from.
      const fem::Load shown = ui::StudyHostOf(*s_App).Shown(*force);
      IM_CHECK_EQ(shown.magnitude, 10000.0);
      IM_CHECK_STR_EQ(ui::SimulationLoadLabel(shown).c_str(), "10 kN");
      IM_CHECK(ctx->ItemExists(
          ("//###Model/**/###load_row_" + std::to_string(forceId)).c_str()));
      ui::ExitSimulation(*s_App);
      ctx->Yield();
    }
  };

  // A study is a row in the simulation's tree (ui::ListRow): click selects
  // it, its settings are the tool window's, and Delete is in its menu
  // (docs/ui/panels.md#simulation-tab-the-model-panel).
  t = IM_REGISTER_TEST(engine, "sim", "a_study_is_a_row_edited_in_the_window");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 60.0);
    const uint64_t first = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_add_static");
    ctx->Yield(2);
    const uint64_t second = s_App->simulation.activeStudyId;
    IM_CHECK(second != first);
    IM_CHECK_EQ(Sim()->GetStudies().size(), (size_t)2);

    ctx->ItemClick(
        ("//###Model/**/###study_sel_" + std::to_string(first)).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->simulation.activeStudyId, first);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_name"));
    IM_CHECK(!ctx->ItemExists("//###Model/**/###study_name"));

    ctx->ItemClick(
        ("//###Model/**/###study_sel_" + std::to_string(second)).c_str(),
        ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###study_delete");
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->GetStudies().size(), (size_t)1);
    IM_CHECK_EQ(Sim()->GetStudies()[0].id, first);

    ClickSubjectTab(ctx);
    IM_CHECK(!s_App->simulation.active);
    ctx->SetRef("//##MainMenuBar");
  };

  // Torque (docs/simulation/README.md#torque-and-remote-loads): armed in the
  // panel, its magnitude typed, and a click on a cylinder's side takes the
  // cylinder's own axis -- nothing stored, so the load follows the face --
  // and the viewport draws its circular arrow.
  t = IM_REGISTER_TEST(engine, "sim", "a_torque_on_a_cylinder_takes_its_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureCylinder(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const Body& body = Doc()->GetBodies()[0];
    ElementRef side;
    for (const auto& [index, name] : body.elementMap.names)
      if (const auto info = Measure::DescribeFaceAt(body, index);
          info && info->kind == "cylinder")
        side = ElementRef{body.createdBy, name};
    IM_CHECK_SILENT(!side.name.empty());
    ToolbarClick(ctx, "###tb_sim_torque");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Torque);
    ctx->ItemInputValue("//###ToolWindow/###sim_torque", "500");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, side);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK_SILENT(st != nullptr);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Torque);
    IM_CHECK(std::abs(st->loads[0].magnitude - 500.0) < 1e-9);
    IM_CHECK(!st->loads[0].HasAxis()); // the face's, not a copy of it
    IM_CHECK(s_App->simulation.placeRefusal.empty());
    // The glyph: an arc of many short strokes, drawn about the axis.
    std::vector<PrimVertex> prims;
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(prims.size() > 30);
    IM_CHECK(ctx->ItemExists(
        ("//###Model/**/###load_row_" + std::to_string(st->loads[0].id))
            .c_str()));
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A flat face has no axis of its own: the click is refused in the panel
  // and nothing is placed. With an axis chosen, the same click places it.
  t = IM_REGISTER_TEST(engine, "sim", "a_torque_on_a_flat_face_needs_an_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const uint64_t cb = Doc()->GetBodies()[0].createdBy;
    const ElementRef flat{cb, std::to_string(cb) + "/face1"};
    ToolbarClick(ctx, "###tb_sim_torque");
    ctx->Yield();
    ctx->ItemInputValue("//###ToolWindow/###sim_torque", "250");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    const uint64_t sid = s_App->simulation.activeStudyId;
    IM_CHECK(Sim()->FindStudy(sid)->loads.empty());
    IM_CHECK(s_App->simulation.placeRefusal.find("not cylindrical") !=
             std::string::npos);
    // Still armed: choose Z in the axis combo and click again.
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Torque);
    ctx->ItemClick("//###ToolWindow/###sim_torque_axis");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick(
        (std::string("//") + popup->Name + "/**/###axis_choice_2").c_str());
    ctx->Yield();
    IM_CHECK(s_App->simulation.torqueAxisChosen);
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Torque);
    IM_CHECK(st->loads[0].HasAxis());
    IM_CHECK(std::abs(st->loads[0].axisDirection.z - 1.0) < 1e-12);
    IM_CHECK(s_App->simulation.placeRefusal.empty());
    s_App->simulation.torqueAxisChosen = false;
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A pin support (docs/simulation/README.md#pin-supports): armed in the
  // panel, a click on a flat face is refused in the red line and places
  // nothing; on the cylinder it places a pin, held from turning when the
  // box is ticked, tinted and hatched as a support.
  t = IM_REGISTER_TEST(engine, "sim", "a_pin_goes_on_a_cylinder_only");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureCylinder(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const Body& body = Doc()->GetBodies()[0];
    ElementRef side, flat;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(body, index);
      if (info && info->kind == "cylinder")
        side = ElementRef{body.createdBy, name};
      else if (info)
        flat = ElementRef{body.createdBy, name};
    }
    IM_CHECK_SILENT(!side.name.empty() && !flat.name.empty());
    ToolbarClick(ctx, "###tb_sim_pin");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Pin);
    const uint64_t sid = s_App->simulation.activeStudyId;
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    IM_CHECK(Sim()->FindStudy(sid)->loads.empty());
    IM_CHECK(s_App->simulation.placeRefusal.find("not cylindrical") !=
             std::string::npos);
    ctx->ItemClick("//###ToolWindow/###sim_pin_fix_rotation");
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingPinFixRotation);
    ui::SimulationApplyToFace(*s_App, side);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Pin);
    IM_CHECK(st->loads[0].fixRotation);
    IM_CHECK(st->loads[0].loadCase.empty());
    IM_CHECK(s_App->simulation.placeRefusal.empty());
    std::vector<PrimVertex> prims;
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    IM_CHECK(ui::SimulationLoadLabel(st->loads[0]) == "pin, held");
    s_App->simulation.pendingPinFixRotation = false;
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A roller and a held direction (docs/simulation/README.md
  // #rollers-held-directions-and-cylindrical-supports), placed by clicking:
  // the roller on a flat face holds it along its normal; the held tool with
  // nothing ticked is refused in the red line and places nothing, and with
  // x ticked places a held support that holds x. Both draw their glyphs.
  t = IM_REGISTER_TEST(engine, "sim", "a_roller_and_a_held_direction_place");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const Body& body = Doc()->GetBodies()[0];
    std::vector<ElementRef> flats;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(body, index);
      if (info && info->kind == "plane")
        flats.push_back(ElementRef{body.createdBy, name});
    }
    IM_CHECK_SILENT(flats.size() >= 2);
    const uint64_t sid = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_roller");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Roller);
    ui::SimulationApplyToFace(*s_App, flats[0]);
    ctx->Yield(2);
    {
      const fem::Study* st = Sim()->FindStudy(sid);
      IM_CHECK_EQ(st->loads.size(), (size_t)1);
      IM_CHECK(st->loads[0].type == fem::LoadType::Roller);
      IM_CHECK(ui::SimulationLoadLabel(st->loads[0]) == "roller");
    }
    ToolbarClick(ctx, "###tb_sim_held");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Held);
    s_App->simulation.pendingHold = {false, false, false};
    ui::SimulationApplyToFace(*s_App, flats[1]);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)1);
    IM_CHECK(s_App->simulation.placeRefusal.find("Tick the directions") !=
             std::string::npos);
    ctx->ItemClick("//###ToolWindow/###sim_held_x");
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingHold[0]);
    ui::SimulationApplyToFace(*s_App, flats[1]);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_EQ(st->loads.size(), (size_t)2);
    IM_CHECK(st->loads[1].type == fem::LoadType::Held);
    IM_CHECK(st->loads[1].hold[0] && !st->loads[1].hold[1] &&
             !st->loads[1].hold[2]);
    IM_CHECK(st->loads[1].frame == 0);
    IM_CHECK(ui::SimulationLoadLabel(st->loads[1]) == "held x");
    IM_CHECK(s_App->simulation.placeRefusal.empty());
    std::vector<PrimVertex> prims;
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    s_App->simulation.pendingHold = {false, false, false};
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A contact support (docs/simulation/README.md#contact): armed in the
  // panel, a click on the curved face is refused in the red line and places
  // nothing (the ground is the plane a flat face lies on); on a flat end it
  // places a contact with the friction typed, hatched as a support, and the
  // study now solves on CalculiX.
  t = IM_REGISTER_TEST(engine, "sim", "a_contact_rests_a_flat_face_only");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureCylinder(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const Body& body = Doc()->GetBodies()[0];
    ElementRef side, flat;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(body, index);
      if (info && info->kind == "cylinder")
        side = ElementRef{body.createdBy, name};
      else if (info && info->kind == "plane")
        flat = ElementRef{body.createdBy, name};
    }
    IM_CHECK_SILENT(!side.name.empty() && !flat.name.empty());
    ToolbarClick(ctx, "###tb_sim_contact");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Contact);
    const uint64_t sid = s_App->simulation.activeStudyId;
    ui::SimulationApplyToFace(*s_App, side);
    ctx->Yield(2);
    IM_CHECK(Sim()->FindStudy(sid)->loads.empty());
    IM_CHECK(s_App->simulation.placeRefusal.find("not flat") !=
             std::string::npos);
    ctx->ItemInputValue("//###ToolWindow/###sim_contact_friction", 0.25f);
    ctx->Yield();
    IM_CHECK(s_App->simulation.pendingContactFriction == 0.25);
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Contact);
    IM_CHECK(st->loads[0].friction == 0.25);
    IM_CHECK(s_App->simulation.placeRefusal.empty());
    IM_CHECK(fem::EffectiveSolver(*st) == fem::Solver::Calculix);
    std::vector<PrimVertex> prims;
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    IM_CHECK(ui::SimulationLoadLabel(st->loads[0]) == "contact");
    s_App->simulation.pendingContactFriction = 0.0;
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A thermal study, clicked (docs/simulation/README.md#thermal): the
  // analysis combo makes it thermal and its tools replace the structural
  // ones; each refuses the click, visibly and placing nothing, until its
  // values are typed (a held temperature, a film coefficient and an ambient
  // have no default), and places once they are; Run solves it, and the
  // temperature field is shown with its legend in degrees C.
  t = IM_REGISTER_TEST(engine, "sim", "a_thermal_tool_refuses_until_typed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureCylinder(ctx);
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
    const uint64_t sid = s_App->simulation.activeStudyId;
    ctx->ItemClick("//###ToolWindow/###study_analysis");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    popup = ctx->UiContext->OpenPopupStack.back().Window;
    ctx->ItemClick((std::string("//") + popup->Name + "/**/Thermal").c_str());
    ctx->Yield(2);
    IM_CHECK(Sim()->FindStudy(sid)->type == fem::AnalysisType::Thermal);
    // A fixed support is not a thermal study's: its button is off, saying
    // why, and the supports' face shows a thermal one instead.
    IM_CHECK(!ui::CommandEnabled(*s_App, "sim.fixed"));
    IM_CHECK(!ui::SimCommandRefusal(*s_App, "sim.fixed").empty());
    IM_CHECK(ui::CommandEnabled(*s_App, "sim.convection"));
    const Body& body = Doc()->GetBodies()[0];
    ElementRef side, flat;
    for (const auto& [index, name] : body.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(body, index);
      if (info && info->kind == "cylinder")
        side = ElementRef{body.createdBy, name};
      else if (info && info->kind == "plane" && flat.name.empty())
        flat = ElementRef{body.createdBy, name};
    }
    IM_CHECK_SILENT(!side.name.empty() && !flat.name.empty());
    // Convection, untyped: refused, naming the ranges to choose from.
    ToolbarClick(ctx, "###tb_sim_convection");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, side);
    ctx->Yield(2);
    IM_CHECK(Sim()->FindStudy(sid)->loads.empty());
    IM_CHECK(s_App->simulation.placeRefusal.find("Incropera") !=
             std::string::npos);
    ctx->ItemInputValue("//###ToolWindow/###sim_film_w_m2k", "10");
    ctx->ItemInputValue("//###ToolWindow/###sim_ambient_c", "25");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, side);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)1);
    IM_CHECK(Sim()->FindStudy(sid)->loads[0].filmWm2K == 10.0);
    // A heat source on the body whose face is clicked.
    ToolbarClick(ctx, "###tb_sim_heat_source");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)1);
    IM_CHECK(s_App->simulation.placeRefusal.find("power_w") !=
             std::string::npos);
    ctx->ItemInputValue("//###ToolWindow/###sim_source_power_w", "0.2");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, flat);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_EQ(st->loads.size(), (size_t)2);
    IM_CHECK(st->loads[1].type == fem::LoadType::HeatGeneration);
    IM_CHECK(st->loads[1].body == body.name);
#ifdef HAVE_NETGEN // the run needs a mesh; the refusals above do not
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, sid));
    const fem::StudyRuntime* rt = Sim()->StudyResults(sid);
    IM_CHECK(rt != nullptr && !rt->bodies.empty());
    IM_CHECK(rt->bodies[0].thermal.solved);
    IM_CHECK(rt->bodies[0].thermal.maxC > 25.0);
    ctx->Yield(2);
    ctx->ItemClick("//###ToolWindow/###result_field");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ctx->ItemClick((std::string("//") +
                    ctx->UiContext->OpenPopupStack.back().Window->Name +
                    "/**/###field_temperature")
                       .c_str());
    ctx->Yield(3);
    IM_CHECK(s_App->simulation.field ==
             ui::SimulationState::Field::Temperature);
#endif
    s_App->simulation.pendingFilmWm2K =
        std::numeric_limits<double>::quiet_NaN();
    s_App->simulation.pendingAmbientC =
        std::numeric_limits<double>::quiet_NaN();
    s_App->simulation.pendingHeatW = std::numeric_limits<double>::quiet_NaN();
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // A remote load: its force typed, and its point -- untyped -- off the
  // clicked face along its normal by a tenth of the body's diagonal, so the
  // default is a lever arm the picture shows.
  t = IM_REGISTER_TEST(engine, "sim", "a_remote_load_acts_off_the_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    const Body& body = Doc()->GetBodies()[0];
    const ElementRef face{body.createdBy,
                          std::to_string(body.createdBy) + "/face1"};
    fem::ResolvedFace rf;
    IM_CHECK_SILENT(fem::ResolveFace(*Doc(), face, "t", rf, nullptr));
    const auto info = Measure::DescribeFaceAt(body, rf.faceIndex);
    IM_CHECK_SILENT(info.has_value());
    glm::dvec3 lo(0.0), hi(0.0);
    body.shape.GetBoundingBox(lo, hi);
    ToolbarClick(ctx, "###tb_sim_remote");
    ctx->Yield();
    s_App->simulation.pointOverridden = false;
    ctx->ItemInputValue("//###ToolWindow/###sim_remote_force", "60");
    ctx->Yield();
    ui::SimulationApplyToFace(*s_App, face);
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    const fem::Load& l = st->loads[0];
    IM_CHECK(l.type == fem::LoadType::Remote);
    IM_CHECK(std::abs(l.magnitude - 60.0) < 1e-9);
    IM_CHECK(l.hasPoint);
    const glm::dvec3 want =
        info->centroid + info->normal * (0.1 * glm::length(hi - lo));
    IM_CHECK(glm::length(l.point - want) < 1e-6);
    // Pushing into the face, like a force placed there.
    IM_CHECK(glm::dot(l.direction, info->normal) < -0.999);
    std::vector<PrimVertex> prims;
    ui::AppendSimulationBCGlyphs(*s_App, prims);
    IM_CHECK(!prims.empty());
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

#ifdef HAVE_NETGEN // a probe reads a SOLVED study; no Netgen, no mesh
  // Probe (docs/simulation/README.md#probing-a-face): a solved study's
  // stress on the clamped face, pinned to the Probes list, and the pin made
  // a requirement on that face -- held at the material's yield.
  t = IM_REGISTER_TEST(engine, "sim", "a_probe_pins_a_face_and_holds_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, studyId));
    ctx->Yield(2);
    const ElementRef root = Sim()->FindStudy(studyId)->loads[0].face;
    ToolbarClick(ctx, "###tb_sim_probe");
    ctx->Yield();
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Probe);
    ui::SimulationApplyToFace(*s_App, root);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->simulation.probes.size(), (size_t)1);
    // Still armed: a probe pins several faces in a row. Off again.
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Probe);
    ToolbarClick(ctx, "###tb_sim_probe");
    ctx->Yield();
    // The clamped root carries the bending stress; its peak is real.
    const fem::StudyRuntime* rt = Sim()->StudyResults(studyId);
    fem::ResolvedFace rf;
    IM_CHECK_SILENT(fem::ResolveFace(*Doc(), root, "t", rf, nullptr));
    const auto stress = fem::FaceStressOf(
        rt->bodies[0].mesh, rt->bodies[0].results.vonMises, rf.faceIndex);
    IM_CHECK_SILENT(stress.has_value());
    IM_CHECK(stress->maxMPa > 0.0);
    IM_CHECK(stress->maxMPa <= rt->maxVonMises + 1e-9);

    const size_t before = s_App->requirements.size();
    ctx->ItemClick("//###ToolWindow/**/###probe_row_0", ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###probe_req");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->requirements.size(), before + 1);
    const req::Requirement& r = s_App->requirements.back();
    IM_CHECK(r.measure.at("kind") == "study_probe");
    IM_CHECK(r.measure.at("quantity") == "von_mises_max");
    IM_CHECK(r.measure.at("face").at("element") == root.name);
    IM_CHECK(r.bound.kind == "at_most");
    IM_CHECK(r.bound.value > stress->maxMPa); // steel's yield, not the reading
    s_App->requirements.pop_back();
    s_App->simulation.probes.clear();
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };
#endif // HAVE_NETGEN

  // The Simulation button on a part opens the part's simulation tab -- made
  // for it, with a study -- and the study is set up, meshed, run and read
  // there, through the window (docs/simulation/README.md#the-simulation-tab).
  t = IM_REGISTER_TEST(engine, "sim", "the_button_opens_the_parts_simulation");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    const uint64_t partId = s_App->parts[s_App->activePart].id;
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield(3);
    IM_CHECK(s_App->sim != nullptr);
    IM_CHECK(s_App->simulation.active);
    IM_CHECK(s_App->sim->Subject().docId == partId);
    IM_CHECK_EQ(s_App->sim->GetStudies().size(), (size_t)1);
    IM_CHECK(ctx->ItemExists("//###Model/**/###sim_subject"));
    // The toolbar is the simulation's: its Add study group. Not one button
    // of it -- the group's face shows the kind last used, which an earlier
    // test in a shuffled run may have made Modal.
    IM_CHECK(ctx->ItemExists("//##DockHost/**/sim_study/###more"));
    const uint64_t sid = s_App->simulation.activeStudyId;
    const Body body = Doc()->GetBodies()[0];
    const ElementRef fixed = FaceFacing(body, {0, 0, -1});
    const ElementRef top = FaceFacing(body, {0, 0, 1});
    IM_CHECK(!fixed.name.empty() && !top.name.empty());
    // A support and a load, placed as a click places them.
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Fixed);
    ui::SimulationApplyToFace(*s_App, 0, fixed);
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pressure);
    ui::SimulationApplyToFace(*s_App, 0, top);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)2);
#ifdef HAVE_NETGEN
    ToolbarClick(ctx, "###tb_sim_mesh");
    WaitFor(
        ctx, "the mesh",
        [sid] { return ui::StudyMesh(*s_App, sid) != nullptr; }, 120.0);
    IM_CHECK(ui::SimMeshReportFor(*s_App, FrontSimSlot(), sid) != nullptr);
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, sid));
    ctx->Yield(3);
    IM_CHECK(!ui::StudyStale(*s_App, sid));
    IM_CHECK(Sim()->StudyResults(sid)->maxVonMises > 0.0);
    // The result is shown on the tab: a field, and a legend for it.
    ctx->ItemClick("//###ToolWindow/###result_field");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ctx->ItemClick((std::string("//") +
                    ctx->UiContext->OpenPopupStack.back().Window->Name +
                    "/**/###field_von_mises")
                       .c_str());
    ctx->Yield(3);
    IM_CHECK(s_App->simulation.field == ui::SimulationState::Field::VonMises);
#endif
    // Its subject's tab goes back to the part.
    ClickSubjectTab(ctx);
    IM_CHECK(s_App->sim == nullptr);
    IM_CHECK(s_App->document != nullptr);
    IM_CHECK_EQ(s_App->parts[s_App->activePart].id, partId);
    IM_CHECK(!s_App->simulation.active);
  };

  // An assembly's simulation: a lone participant is the part's own problem
  // (a pressure on part of a face's study works there), and beside a rigid
  // neighbour it is clamped where the neighbour touches. Each is set up and
  // run on the assembly's simulation tab.
  t = IM_REGISTER_TEST(engine, "sim", "an_assembly_simulation_runs_on_its_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    const uint64_t partId = s_App->parts[0].id;
    const Body body = Doc()->GetBodies()[0];
    const ElementRef bottom = FaceFacing(body, {0, 0, -1});
    const ElementRef top = FaceFacing(body, {0, 0, 1});
    const size_t asmIdx = ui::AddAssembly(*s_App, "Stack");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence lo;
    lo.name = "Lower";
    lo.partId = partId;
    lo.fixed = true;
    const uint64_t lower = s_App->assembly->AddOccurrence(lo);
    ctx->Yield(3);
    const uint64_t asmId = s_App->parts[asmIdx].id;

    ToolbarClick(ctx, "###tb_asm_simulation");
    ctx->Yield(3);
    IM_CHECK(s_App->sim != nullptr);
    IM_CHECK(s_App->sim->Subject().docId == asmId);
    const uint64_t sid = s_App->simulation.activeStudyId;
    // The lone participant: meshed, a support and a load on its faces.
    ui::ModifyStudy(*s_App, sid, "roles", [&](fem::Study& cs) {
      cs.participants.push_back(
          {lower, fem::ParticipantRole::Meshed, ExprValue{0.0}});
      cs.meshSize = 12.0;
    });
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sim->Roles().size(), (size_t)1); // the document's
    IM_CHECK(Sim()->FindStudy(sid)->participants.empty());
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Fixed);
    ui::SimulationApplyToFace(*s_App, lower, bottom);
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pressure);
    ui::SimulationApplyToFace(*s_App, lower, top);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads[0].occurrenceId, lower);
    // A pin on a lone participant is the planner's to judge, not refused
    // for being on an assembly: a flat face is refused for its shape.
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pin);
    ui::SimulationApplyToFace(*s_App, lower, top);
    IM_CHECK(s_App->simulation.placeRefusal.find("cylindrical") !=
             std::string::npos);
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::None);
#ifdef HAVE_NETGEN
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, sid));
    IM_CHECK(!ui::StudyStale(*s_App, sid));
    const double lone = Sim()->StudyResults(sid)->maxDisplacement;
    IM_CHECK(lone > 0.0);
#endif

    // Beside a rigid neighbour: a second simulation of the same assembly,
    // its own roles, clamped where the support touches.
    ClickSubjectTab(ctx);
    IM_CHECK(s_App->assembly != nullptr);
    Occurrence up;
    up.name = "Upper";
    up.partId = partId;
    up.position = glm::vec3(0.0f, 0.0f, 16.0f);
    const uint64_t upper = s_App->assembly->AddOccurrence(up);
    ctx->Yield(3);
    std::string why;
    fem::SimDocument second;
    second.SetSubject({fem::SubjectKind::Assembly, asmId, 0, "Stack"});
    IM_CHECK(ui::AddSimulation(*s_App, "Stack stacked", std::move(second),
                               &why) != SIZE_MAX);
    ctx->Yield(2);
    IM_CHECK(s_App->sim != nullptr);
    ui::EnterSimulation(*s_App); // a study, as the button makes
    ctx->Yield(2);
    const uint64_t sid2 = s_App->simulation.activeStudyId;
    ui::ModifyStudy(*s_App, sid2, "roles", [&](fem::Study& cs) {
      cs.participants.push_back(
          {upper, fem::ParticipantRole::Meshed, ExprValue{0.0}});
      cs.participants.push_back(
          {lower, fem::ParticipantRole::Rigid, ExprValue{0.0}});
      cs.meshSize = 12.0;
    });
    ctx->Yield(2);
    SimTreeClick(ctx, "###sim_part_" + std::to_string(lower));
    const std::string clamp =
        "//###ToolWindow/###clamp_" + std::to_string(lower);
    ctx->ItemClick(clamp.c_str());
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pressure);
    ui::SimulationApplyToFace(*s_App, upper, top);
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::None);
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid2)->loads.size(), (size_t)2);
    IM_CHECK_EQ(ui::SimulationContacts(*s_App).size(), (size_t)1);
#ifdef HAVE_NETGEN
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, sid2));
    IM_CHECK(!ui::StudyStale(*s_App, sid2));
    IM_CHECK(Sim()->StudyResults(sid2)->maxDisplacement > 0.0);
#endif
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // Studies saved on a part and on an assembly (every file before the
  // simulation document) move into simulation tabs when the project opens:
  // not in front, the project unedited; and they stay so through a save,
  // which writes them in the simulations alone, and a reopen
  // (docs/simulation/README.md#moving-studies-into-a-simulation). That a
  // moved result reads current is tests/test_sim_storage.cpp's.
  t = IM_REGISTER_TEST(engine, "sim", "studies_move_into_simulations_on_open");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    namespace fs = std::filesystem;
    ctx->SetRef("//##MainMenuBar");
    // A golden part that saved its studies and their results.
    std::string err;
    const std::string golden =
        SourceFixture("../golden/v153-saved-study-results.json");
    IM_CHECK(ui::OpenProject(*s_App, golden, &err));
    ui::MarkProjectOpened(*s_App, golden);
    ctx->Yield(2);
    IM_CHECK(s_App->document != nullptr); // the part stays in front
    IM_CHECK(s_App->document->OldStudiesRead() == nullptr); // moved out
    std::vector<size_t> sims =
        ui::SimulationSlotsFor(*s_App, s_App->parts[0].id);
    IM_CHECK_EQ(sims.size(), (size_t)1);
    IM_CHECK(!ui::ProjectDirty(*s_App));
    const fem::SimDocument* moved = ui::SlotSim(*s_App, sims[0]);
    IM_CHECK(moved != nullptr);
    IM_CHECK(moved->FindStudy(3) != nullptr && moved->FindStudy(7) != nullptr);

    // A project whose part and assembly kept studies, as every file before
    // simulations did: one saved today, given those studies on disk at the
    // version before.
    ctx->MenuClick("File/New");
    ctx->Yield();
    SteelBlock(ctx);
    const uint64_t partId = s_App->parts[0].id;
    const Body body = Doc()->GetBodies()[0];
    const ElementRef top = FaceFacing(body, {0, 0, 1});
    fem::Study plate;
    plate.id = Doc()->AllocateId();
    plate.name = "Plate";
    plate.meshSize = 12.0;
    {
      fem::Load fix;
      fix.id = Doc()->AllocateId();
      fix.type = fem::LoadType::Fixed;
      fix.face = FaceFacing(body, {0, 0, -1});
      plate.loads.push_back(fix);
      fem::Load p1;
      p1.id = Doc()->AllocateId();
      p1.type = fem::LoadType::Pressure;
      p1.face = top;
      p1.magnitude = 1.0;
      plate.loads.push_back(p1);
    }
    const uint64_t partStudy = plate.id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Stack");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence lo;
    lo.name = "Lower";
    lo.partId = partId;
    lo.fixed = true;
    const uint64_t lower = s_App->assembly->AddOccurrence(lo);
    Occurrence up;
    up.name = "Upper";
    up.partId = partId;
    up.position = glm::vec3(0.0f, 0.0f, 16.0f);
    const uint64_t upper = s_App->assembly->AddOccurrence(up);
    ctx->Yield(3);
    AssemblyDocument& asmDoc = *s_App->assembly;
    fem::Study stacked;
    stacked.id = asmDoc.AllocateId();
    stacked.name = "Stacked";
    stacked.participants = {
        {upper, fem::ParticipantRole::Meshed, ExprValue{0.0}},
        {lower, fem::ParticipantRole::Rigid, ExprValue{0.0}}};
    stacked.meshSize = 12.0;
    {
      fem::Load clampLoad;
      clampLoad.id = asmDoc.AllocateId();
      clampLoad.type = fem::LoadType::Fixed;
      clampLoad.contactWith = lower;
      stacked.loads.push_back(clampLoad);
      fem::Load press;
      press.id = asmDoc.AllocateId();
      press.type = fem::LoadType::Pressure;
      press.face = top;
      press.occurrenceId = upper;
      press.magnitude = 1.0;
      stacked.loads.push_back(press);
    }
    const uint64_t oldId = stacked.id;
    asmDoc.BumpVersion();
    const fs::path dir = fs::temp_directory_path() / "lobole_test_sim_move.lob";
    fs::remove_all(dir);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    // The document directories (an index.json of each kind), and whether
    // one lists studies.
    const auto indexOf = [&](const char* kind) {
      for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().filename() == "index.json") {
          std::ifstream in(e.path());
          const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
          // A part's envelope names no kind.
          if (!j.is_discarded() &&
              j.contains("features") == (std::string(kind) == "part") &&
              j.value("kind", "part") == kind)
            return e.path();
        }
      return fs::path();
    };
    const auto listsStudies = [](const fs::path& index) {
      std::ifstream in(index);
      return nlohmann::json::parse(in).contains("studies");
    };
    for (const auto& [kind, study] :
         {std::pair<const char*, const fem::Study*>{"part", &plate},
          {"assembly", &stacked}}) {
      const fs::path index = indexOf(kind);
      IM_CHECK_SILENT(!index.empty());
      IM_CHECK(!listsStudies(index)); // written today: none
      nlohmann::json j;
      {
        std::ifstream in(index);
        j = nlohmann::json::parse(in);
      }
      j["version"] = j.at("version").get<int>() - 1; // the format before
      j["studies"] = {study->id};
      fs::create_directories(index.parent_path() / "studies");
      std::ofstream(index.parent_path() / "studies" /
                    (std::to_string(study->id) + "-study.json"))
          << DocumentIO::StudyToJson(*study).dump(2);
      std::ofstream(index) << j.dump(2);
    }
    // Opened: each study is in a simulation of its document, with its id.
    for (int pass = 0; pass < 2; pass++) {
      ctx->MenuClick("File/New");
      ctx->Yield();
      IM_CHECK(ui::OpenProject(*s_App, dir.string(), &err));
      ui::MarkProjectOpened(*s_App, dir.string());
      ctx->Yield(2);
      IM_CHECK(s_App->sim == nullptr); // not brought forward
      IM_CHECK(!ui::ProjectDirty(*s_App));
      std::optional<size_t> at;
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].kind == ui::DocKind::Assembly)
          at = i;
      IM_CHECK_SILENT(at.has_value());
      IM_CHECK(ui::SlotAssembly(*s_App, *at)->OldStudiesRead() == nullptr);
      sims = ui::SimulationSlotsFor(*s_App, s_App->parts[*at].id);
      IM_CHECK_EQ(sims.size(), (size_t)1);
      const fem::SimDocument* sim = ui::SlotSim(*s_App, sims[0]);
      IM_CHECK(sim->FindStudy(oldId) != nullptr);
      IM_CHECK_EQ(sim->Roles().size(), (size_t)2);
      IM_CHECK_EQ(sim->FindStudy(oldId)->loads.size(), (size_t)2);
      // The part's study too.
      std::optional<size_t> partAt;
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].kind == ui::DocKind::Part)
          partAt = i;
      IM_CHECK_SILENT(partAt.has_value());
      IM_CHECK(ui::SlotDoc(*s_App, *partAt)->OldStudiesRead() == nullptr);
      const std::vector<size_t> partSims =
          ui::SimulationSlotsFor(*s_App, s_App->parts[*partAt].id);
      IM_CHECK_EQ(partSims.size(), (size_t)1);
      const fem::SimDocument* psim = ui::SlotSim(*s_App, partSims[0]);
      IM_CHECK(psim->FindStudy(partStudy) != nullptr);
      // Saved again: the part and the assembly write no studies, the
      // simulations do.
      IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
      ui::MarkProjectSaved(*s_App, dir.string(), "test");
      IM_CHECK(!listsStudies(indexOf("part")));
      IM_CHECK(!listsStudies(indexOf("assembly")));
    }
    fs::remove_all(dir);
  };

  // A study made over the RPCs on a part with no simulation makes the
  // part's simulation behind the tab in front -- the part stays in front --
  // and its edit is one undo step on that simulation's tab.
  t = IM_REGISTER_TEST(engine, "sim", "an_rpc_study_makes_a_simulation_behind");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    const uint64_t partId = s_App->parts[s_App->activePart].id;
    const Body body = Doc()->GetBodies()[0];
    const ElementRef bottom = FaceFacing(body, {0, 0, -1});
    const ElementRef top = FaceFacing(body, {0, 0, 1});
    const auto rpc = [](const char* m, const nlohmann::json& params) {
      return remote::HandleSimulationRpc(*s_App, s_App->document.get(), m,
                                         remote::RpcParams{params});
    };
    const auto made = rpc("add_study", {{"name", "Over RPC"}});
    IM_CHECK_SILENT(made.has_value());
    const uint64_t sid = made->at("id").get<uint64_t>();
    ctx->Yield(2);
    IM_CHECK(s_App->document != nullptr); // no switch
    IM_CHECK_EQ(s_App->parts[s_App->activePart].id, partId);
    const std::vector<size_t> sims = ui::SimulationSlotsFor(*s_App, partId);
    IM_CHECK_EQ(sims.size(), (size_t)1);
    fem::SimDocument* sim = ui::SlotSim(*s_App, sims[0]);
    IM_CHECK(sim->FindStudy(sid) != nullptr);
    const auto face = [](const ElementRef& r) {
      return nlohmann::json{{"feature_id", r.featureId}, {"element", r.name}};
    };
    rpc("set_study", {{"id", sid}, {"mesh_size", 12.0}});
    rpc("add_load",
        {{"study", sid}, {"type", "fixed"}, {"face", face(bottom)}});
    rpc("add_load", {{"study", sid},
                     {"type", "pressure"},
                     {"face", face(top)},
                     {"magnitude", 1.0}});
    IM_CHECK_EQ(sim->FindStudy(sid)->loads.size(), (size_t)2);
#ifdef HAVE_NETGEN
    const auto ran = rpc("run_study", {{"id", sid}});
    IM_CHECK_SILENT(ran.has_value());
    IM_CHECK(sim->StudyResults(sid) != nullptr);
    IM_CHECK(!ui::SimStudyStale(*s_App, *sim, sid));
#endif
    IM_CHECK(s_App->document != nullptr); // still no switch
    // The last load undoes on the simulation's own tab.
    ui::SetActivePart(*s_App, sims[0]);
    ctx->Yield(2);
    IM_CHECK(s_App->sim.get() == sim);
    IM_CHECK(ui::UndoSim(*s_App));
    IM_CHECK_EQ(sim->FindStudy(sid)->loads.size(), (size_t)1);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // The tree (docs/ui/panels.md#simulation-tab-the-model-panel): one row
  // per thing the simulation holds, no tool in it; a row's click puts its
  // settings in the tool window and lights the face it stands on; a face
  // picked in the viewport lights its load's row; Ctrl adds a row, Delete
  // removes what is lit as one undo step.
  t = IM_REGISTER_TEST(engine, "sim",
                       "the_tree_shows_what_the_simulation_holds");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    using Kind = ui::SimulationState::Focus::Kind;
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t sid = s_App->simulation.activeStudyId;
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_SILENT(st != nullptr && st->loads.size() == 2);
    const uint64_t fixedId = st->loads[0].id, forceId = st->loads[1].id;
    ToolbarClick(ctx, "###tb_sim_add_case");
    ctx->Yield(2);
    const std::string sidText = std::to_string(sid);
    for (const std::string& row :
         {std::string("###sim_subject"), "###study_sel_" + sidText,
          std::string("###sim_mesh"), "###load_row_" + std::to_string(fixedId),
          "###load_row_" + std::to_string(forceId),
          "###case_row_" + sidText + "_0"})
      IM_CHECK(ctx->ItemExists(("//###Model/**/" + row).c_str()));
    // Not one tool in it: the toolbar holds them.
    for (const char* gone : {"###study_add", "###sim_bc_fixed", "###case_add",
                             "###study_run", "###study_mesh", "###sim_probe"})
      IM_CHECK(
          !ctx->ItemExists((std::string("//###Model/**/") + gone).c_str()));
    // The case row shows its settings; the force went into the first case.
    IM_CHECK(s_App->simulation.focus.kind == Kind::Case);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###case_del"));

    // A load's row: its settings, and its face lit as a selection.
    SimTreeClick(ctx, "###load_row_" + std::to_string(forceId));
    IM_CHECK(s_App->simulation.focus.kind == Kind::Load);
    IM_CHECK_EQ(s_App->simulation.focus.id, forceId);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###simulation_close"));
    ctx->Yield(2);
    ui::SyncSelectionHighlights(*s_App);
    bool lit = false;
    for (const ui::DocumentRenderMesh& m : s_App->gpu.documentMeshes)
      for (const glm::uvec2& r :
           s_App->meshRenderer->InstanceHighlightRanges(m.meshId))
        lit = lit ||
              (r.y >> MeshRenderer::kTintShift) == MeshRenderer::kTintSelection;
    IM_CHECK(lit);

    // The viewport's pick: the fixed face picked lights the fixed row.
    s_App->selection.Replace({ui::SelectionItem::Kind::Face, 0,
                              Sim()->FindStudy(sid)->loads[0].face});
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.focus.kind == Kind::Load);
    IM_CHECK_EQ(s_App->simulation.focus.id, fixedId);

    // Two rows lit, Ctrl adding the second; Delete takes both, one step.
    SimTreeClick(ctx, "###load_row_" + std::to_string(fixedId));
    ctx->KeyDown(ImGuiMod_Ctrl);
    SimTreeClick(ctx, "###load_row_" + std::to_string(forceId));
    ctx->KeyUp(ImGuiMod_Ctrl);
    IM_CHECK_EQ(
        ui::SelectedIds(*s_App, ui::SelectionItem::Kind::SimLoad).size(),
        (size_t)2);
    IM_CHECK_EQ(ui::DeleteSelection(*s_App), 2);
    ctx->Yield(2);
    IM_CHECK(Sim()->FindStudy(sid)->loads.empty());
    IM_CHECK(ui::UndoSim(*s_App));
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->loads.size(), (size_t)2);

    // The study's row: its settings, the mesh row's: the mesh's.
    SimFocusStudy(ctx, sid);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_analysis"));
    SimTreeClick(ctx, "###sim_mesh");
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###sim_mesh_size"));
    // The mesh is the simulation's: its size lands on the document, and a
    // study that solves on it has no Mesh row of its own.
    ctx->ItemInputValue("//###ToolWindow/###sim_mesh_size", "12");
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->Mesh().size.value, 12.0);
    IM_CHECK(
        !ctx->ItemExists(("//###Model/**/###sim_mesh_" + sidText).c_str()));
    // A size of its own gives the study its own mesh and row; the shared
    // row goes with the last study on it.
    SimFocusStudy(ctx, sid);
    ctx->ItemInputValue("//###ToolWindow/###study_mesh_size", "7");
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(sid)->meshSize.value, 7.0);
    IM_CHECK(ctx->ItemExists(("//###Model/**/###sim_mesh_" + sidText).c_str()));
    IM_CHECK(!ctx->ItemExists("//###Model/**/###sim_mesh"));
    SimTreeClick(ctx, "###sim_mesh_" + sidText);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_mesh_size"));
    // Close puts the window away; nothing in focus, nothing drawn.
    ctx->ItemClick("//###ToolWindow/###simulation_close");
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.focus.kind == Kind::None);
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/###study_mesh_size"));
    IM_CHECK(s_App->simulation.active);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // The toolbar (docs/ui/panels.md#the-simulation-toolbar): every tool is on
  // it -- the supports and the loads in two drop-downs showing the last
  // used -- the row fits the window, and a tool that cannot act on the
  // study says why rather than vanishing.
  t = IM_REGISTER_TEST(engine, "sim", "the_toolbar_holds_every_tool");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 10.0);
    ctx->Yield(2);
    ImGuiWindow* bar = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
      if (std::string(w->Name).find("##ToolbarRow") != std::string::npos)
        bar = w;
    IM_CHECK_SILENT(bar != nullptr);
    ctx->LogInfo("simulation toolbar %.0f px in %.0f px", bar->ContentSize.x,
                 bar->InnerRect.GetWidth());
    IM_CHECK(bar->ContentSize.x <= bar->InnerRect.GetWidth() + 1.0f);
    ctx->SetRef("//##DockHost");
    for (const char* id :
         {"###tb_sim_mesh", "###tb_sim_add_case", "###tb_sim_check",
          "###tb_sim_run", "###tb_sim_result", "###tb_sim_probe",
          "###tb_sim_export_vtu", "###tb_sim_labels", "###tb_sim_clear_storage",
          "###tb_sim_fit", "###tb_sim_measure"})
      IM_CHECK(ctx->ItemExists((std::string("**/") + id).c_str()));
    // A simulation is a tab, left as any tab is: no Exit on its toolbar,
    // and Escape stops at the armed tool and the tool window.
    IM_CHECK(!ctx->ItemExists("**/###tb_sim_exit"));
    ToolbarClick(ctx, "###tb_sim_fixed");
    IM_CHECK(s_App->simulation.tool != ui::SimulationState::Tool::None);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::None);
    SimFocusStudy(ctx);
    IM_CHECK(s_App->simulation.focus.kind !=
             ui::SimulationState::Focus::Kind::None);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.focus.kind ==
             ui::SimulationState::Focus::Kind::None);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(s_App->sim != nullptr && s_App->simulation.active);
    // The way back is the subject row's Open, or the subject's tab.
    const uint64_t subject = Sim()->Subject().docId;
    ctx->ItemClick("//###Model/**/###sim_subject", ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###sim_subject_open");
    ctx->Yield(2);
    IM_CHECK(s_App->sim == nullptr && !s_App->simulation.active);
    IM_CHECK_EQ(s_App->parts[s_App->activePart].id, subject);
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    ctx->SetRef("//##DockHost");
    const auto inGroup = [&](const char* group,
                             std::initializer_list<const char*> ids) {
      ctx->SetRef("//##DockHost");
      ctx->ItemClick((std::string("**/") + group + "/###more").c_str());
      ctx->Yield(2);
      for (const char* id : ids)
        IM_CHECK(ctx->ItemExists((std::string("//$FOCUSED/") + id).c_str()));
      ctx->PopupCloseAll();
      ctx->Yield();
    };
    inGroup("sim_study", {"###tb_sim_add_static", "###tb_sim_add_modal",
                          "###tb_sim_add_buckling", "###tb_sim_add_thermal",
                          "###tb_sim_add_topology"});
    inGroup("sim_supports",
            {"###tb_sim_fixed", "###tb_sim_pin", "###tb_sim_contact",
             "###tb_sim_roller", "###tb_sim_held", "###tb_sim_cylindrical",
             "###tb_sim_temperature", "###tb_sim_convection"});
    inGroup("sim_loads", {"###tb_sim_force", "###tb_sim_pressure",
                          "###tb_sim_torque", "###tb_sim_remote",
                          "###tb_sim_heat_flux", "###tb_sim_heat_source"});
    // A static study: a heat source is off and says why; Keep (a topology
    // study's) is not on the bar at all; nothing to export before a run.
    IM_CHECK(!ui::CommandEnabled(*s_App, "sim.heat_source"));
    IM_CHECK(!ui::SimCommandRefusal(*s_App, "sim.heat_source").empty());
    IM_CHECK(!ctx->ItemExists("**/###tb_sim_keep"));
    IM_CHECK(!ui::CommandEnabled(*s_App, "sim.export_vtu"));
    IM_CHECK(!ui::CommandEnabled(*s_App, "sim.clear_storage"));
    // Arming a support lights the group's face, which shows it.
    ToolbarClick(ctx, "###tb_sim_roller");
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::Roller);
    ctx->SetRef("//##DockHost");
    IM_CHECK(ctx->ItemExists("**/sim_supports/###tb_sim_roller"));
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###sim_held_x") == false);
    ToolbarClick(ctx, "###tb_sim_roller"); // again: put down
    IM_CHECK(s_App->simulation.tool == ui::SimulationState::Tool::None);
    // A study of each kind from the first drop-down, each focused.
    const size_t before = Sim()->GetStudies().size();
    ToolbarClick(ctx, "###tb_sim_add_modal");
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->GetStudies().size(), before + 1);
    const fem::Study* modal = Sim()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(modal != nullptr && modal->type == fem::AnalysisType::Modal);
    IM_CHECK(modal->name == "Modal 1");
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###study_analysis"));
    // One undo takes it back.
    IM_CHECK(ui::UndoSim(*s_App));
    IM_CHECK_EQ(Sim()->GetStudies().size(), before);
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // Clear stored meshes and results (docs/simulation/README.md
  // #clearing-stored-meshes-and-results): asked first, then the simulation
  // keeps its studies and none of what was solved; the next save writes no
  // result, and the button is off once nothing is left.
  t = IM_REGISTER_TEST(engine, "sim", "clear_storage_drops_meshes_and_results");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield(2);
    const uint64_t sid = s_App->simulation.activeStudyId;
    // A result as a run stores one (no mesher needed for what is tested).
    fem::StudyRuntime rt;
    rt.maxVonMises = 12.0;
    Sim()->StoreStudyResults(sid, rt);
    ctx->Yield(2);
    IM_CHECK_EQ(ui::SimStorageOf(*s_App).results, (size_t)1);
    IM_CHECK(ui::CommandEnabled(*s_App, "sim.clear_storage"));
    IM_CHECK(ctx->ItemExists(
        ("//###Model/**/###sim_result_" + std::to_string(sid)).c_str()));
    ToolbarClick(ctx, "###tb_sim_clear_storage");
    ctx->Yield(2);
    // Asked: Cancel keeps it.
    ctx->ItemClick(
        "//Clear stored meshes and results/###sim_clear_storage_cancel");
    ctx->Yield(2);
    IM_CHECK(Sim()->StudyResults(sid) != nullptr);
    ToolbarClick(ctx, "###tb_sim_clear_storage");
    ctx->Yield(2);
    ctx->ItemClick("//Clear stored meshes and results/###sim_clear_storage_ok");
    ctx->Yield(2);
    IM_CHECK(Sim()->StudyResults(sid) == nullptr);
    IM_CHECK(Sim()->FindStudy(sid) != nullptr); // the study stays
    IM_CHECK_EQ(ui::SimStorageOf(*s_App).results, (size_t)0);
    IM_CHECK(!ui::CommandEnabled(*s_App, "sim.clear_storage"));
    IM_CHECK(!ctx->ItemExists(
        ("//###Model/**/###sim_result_" + std::to_string(sid)).c_str()));
    IM_CHECK(ui::ProjectDirty(*s_App)); // its files go at the next save
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // Every simulation tool, given what it acts on, changes the study or says
  // why it will not (docs/ui/tool-sessions.md#defaults-and-refusals): a new
  // study of each kind is one more study, and each support and load placed
  // on a face either is a load or leaves a refusal in words.
  t = IM_REGISTER_TEST(engine, "defaults", "simulation_tools_place_or_refuse");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield(2);
    for (const char* id : {"###tb_sim_add_static", "###tb_sim_add_modal",
                           "###tb_sim_add_buckling", "###tb_sim_add_thermal",
                           "###tb_sim_add_topology"}) {
      const size_t before = Sim()->GetStudies().size();
      ToolbarClick(ctx, id);
      ctx->Yield();
      IM_CHECK_EQ(Sim()->GetStudies().size(), before + 1);
    }
    const Body body = Doc()->GetBodies()[0];
    const ElementRef top = FaceFacing(body, {0, 0, 1});
    IM_CHECK_SILENT(!top.name.empty());
    using A = fem::AnalysisType;
    struct Try {
      const char* tbId;
      A on;
    };
    for (const Try& tr :
         {Try{"###tb_sim_fixed", A::Static}, Try{"###tb_sim_pin", A::Static},
          Try{"###tb_sim_contact", A::Static},
          Try{"###tb_sim_roller", A::Static}, Try{"###tb_sim_held", A::Static},
          Try{"###tb_sim_cylindrical", A::Static},
          Try{"###tb_sim_force", A::Static},
          Try{"###tb_sim_pressure", A::Static},
          Try{"###tb_sim_torque", A::Static},
          Try{"###tb_sim_remote", A::Static},
          Try{"###tb_sim_temperature", A::Thermal},
          Try{"###tb_sim_convection", A::Thermal},
          Try{"###tb_sim_heat_flux", A::Thermal},
          Try{"###tb_sim_heat_source", A::Thermal},
          Try{"###tb_sim_keep", A::Topology}}) {
      uint64_t sid = 0;
      for (const fem::Study& s : Sim()->GetStudies())
        if (s.type == tr.on)
          sid = s.id;
      IM_CHECK_SILENT(sid != 0);
      SimFocusStudy(ctx, sid);
      const fem::Study* s = Sim()->FindStudy(sid);
      const size_t loads = s->loads.size() + s->keepFaces.size();
      ToolbarClick(ctx, tr.tbId);
      ctx->Yield();
      IM_CHECK(s_App->simulation.tool != ui::SimulationState::Tool::None);
      ui::SimulationApplyToFace(*s_App, 0, top);
      ctx->Yield(2);
      s = Sim()->FindStudy(sid);
      const bool placed = s->loads.size() + s->keepFaces.size() > loads;
      ctx->LogInfo("%s: %s", tr.tbId,
                   placed ? "placed" : s_App->simulation.placeRefusal.c_str());
      IM_CHECK(placed || !s_App->simulation.placeRefusal.empty());
      ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::None);
    }
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };

  // The window in the states a person meets it in, each checked as it
  // stands and, with --capture-dir, saved to look at: a new simulation with
  // its one empty study; a study with several loads and two load cases; a
  // solved part study with its field; an assembly's participants.
  t = IM_REGISTER_TEST(engine, "sim", "the_window_in_four_states");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // Empty: one study, nothing on it.
    ctx->SetRef("//##MainMenuBar");
    SteelBlock(ctx);
    ToolbarClick(ctx, "###tb_simulation");
    ctx->Yield(4);
    SimWindowShot(ctx, "1-empty");

    // Loads and cases: the cantilever's support and force, a pressure and
    // a second force in a second case, the force's row in focus.
    ui::ExitSimulation(*s_App);
    ctx->MenuClick("File/New");
    ctx->Yield();
    BuildCantileverStudy(ctx, 10.0);
    const uint64_t sid = s_App->simulation.activeStudyId;
    const uint64_t cb = Doc()->GetBodies()[0].createdBy;
    ToolbarClick(ctx, "###tb_sim_add_case");
    ToolbarClick(ctx, "###tb_sim_add_case");
    ToolbarClick(ctx, "###tb_sim_pressure");
    s_App->simulation.pendingMagnitude = 0.05;
    ui::SimulationApplyToFace(*s_App,
                              ElementRef{cb, std::to_string(cb) + "/face3"});
    ToolbarClick(ctx, "###tb_sim_force");
    s_App->simulation.pendingCase = "Case 2";
    ui::SimulationApplyToFace(*s_App,
                              ElementRef{cb, std::to_string(cb) + "/face1"});
    ctx->Yield(2);
    const fem::Study* st = Sim()->FindStudy(sid);
    IM_CHECK_SILENT(st != nullptr && st->loads.size() >= 3);
    {
      // From above and aside, so the faces each load is on show.
      glm::dvec3 lo(0.0), hi(0.0);
      Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
      const glm::vec3 mid = (lo + hi) * 0.5;
      s_App->camera->StopAnimation();
      s_App->camera->SetTarget(mid);
      s_App->camera->SetUp(glm::vec3(0, 1, 0));
      s_App->camera->SetPosition(mid + glm::vec3(-160.0f, 170.0f, 260.0f));
    }
    SimTreeClick(ctx, "###load_row_" + std::to_string(st->loads[1].id));
    SimWindowShot(ctx, "2-loads-and-cases");
    // The supports' drop-down open, with what is off saying why.
    ctx->SetRef("//##DockHost");
    ctx->ItemClick("**/sim_supports/###more");
    SimWindowShot(ctx, "3-supports-dropdown");
    ctx->PopupCloseAll();
    ctx->Yield();

#ifdef HAVE_NETGEN
    // Solved: the field on the model and the result in the window.
    ToolbarClick(ctx, "###tb_sim_run");
    IM_CHECK(WaitForStudyResult(ctx, sid));
    s_App->simulation.field = ui::SimulationState::Field::VonMises;
    s_App->simulation.deformScale = 50.0f;
    SimWindowShot(ctx, "4-part-solved");
#endif

    // An assembly's participants: a meshed block on a rigid one, clamped.
    ui::ExitSimulation(*s_App);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    SteelBlock(ctx);
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Stack");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence lo;
    lo.name = "Lower";
    lo.partId = partId;
    lo.fixed = true;
    const uint64_t lower = s_App->assembly->AddOccurrence(lo);
    Occurrence up;
    up.name = "Upper";
    up.partId = partId;
    up.position = glm::vec3(0.0f, 0.0f, 16.0f);
    const uint64_t upper = s_App->assembly->AddOccurrence(up);
    ctx->Yield(3);
    ToolbarClick(ctx, "###tb_asm_simulation");
    ctx->Yield(3);
    ui::FitCameraToDocument(*s_App);
    ui::SetSimParticipantRole(*s_App, upper, fem::ParticipantRole::Meshed);
    ui::SetSimParticipantRole(*s_App, lower, fem::ParticipantRole::Rigid);
    ctx->Yield(2);
    SimTreeClick(ctx, "###sim_part_" + std::to_string(lower));
    ctx->ItemClick(
        ("//###ToolWindow/###clamp_" + std::to_string(lower)).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Sim()->FindStudy(s_App->simulation.activeStudyId)->loads.size(),
                (size_t)1);
    SimWindowShot(ctx, "5-assembly-participants");
    ui::ExitSimulation(*s_App);
    ctx->Yield();
  };
}
