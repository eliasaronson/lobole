#include "app/guitests/GuiTestsCommon.h"
#include "render/MeshRenderer.h" // InstanceHighlightRanges (tint test)
#include "ui/AssemblyActions.h"  // UndoAssembly, PartDocById (S1)
#include "ui/DocumentSlots.h"    // AddAssembly, SetActivePart
#include "ui/SelectionPipeline.h" // ResolveFaceItem (arrow-placement test)
#include "ui/SimulationActions.h"
#include "ui/StudyHost.h" // ModifyStudy, SimulationContacts, StudyStale
#include "ui/StudyPanel.h"
#include "ui/ViewportHandleDraw.h" // DrawViewportHandles

// Simulation GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterSimulationGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "sim", "gravity_pulls_down");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarClick(ctx, "###tb_simulation"); // the button had no id before this
    ctx->Yield();
    IM_CHECK(s_App->simulation.active);
    ctx->ItemClick("//###ToolWindow/###study_gravity");
    ctx->Yield();
    const fem::Study* study = Doc()->FindStudy(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    // Straight down in the world frame — a hand-written axis here once
    // sagged the part sideways and bent it about the wrong axis.
    IM_CHECK(glm::length(study->gravity - conv::kGravity) < 1e-9);
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
  // change its magnitude (docs/ui/viewport-handles.md#simulation-load-drag).
  // Both asserted on the drawn primitives and the study, not on pixels.
  t = IM_REGISTER_TEST(engine, "sim", "force_arrow_points_along_the_force");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
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
    study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr && !study->loads.empty());
    IM_CHECK(study->loads[0].magnitude > 100.0); // pulling grew it
    // ONE undo restores the grab-time magnitude — not a step per frame.
    IM_CHECK(s_App->commands->CanUndo());
    s_App->commands->Undo(*Doc());
    ctx->Yield();
    study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
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
    load.id = Doc()->AllocateId();
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ctx->ItemClick("//###ToolWindow/###study_run");
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
    ctx->ItemClick("//###ToolWindow/###study_analysis");
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
    ctx->ItemInputValue("//###ToolWindow/###study_mesh_size", "10");
    ctx->Yield(2);
    ctx->ItemInputValue("//###ToolWindow/###topo_max_iterations", "4");
    ctx->Yield(2);

    ctx->ItemClick("//###ToolWindow/###topo_run");
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

#else  // !HAVE_NETGEN
  LOG_WARN("GUI suite: 2 sim cases NOT registered "
           "(a_static_run_matches_beam_theory, topology_holds_its_volume_"
           "target) — this build has no Netgen, so a study cannot be meshed");
#endif // HAVE_NETGEN

  // A pressure is a FIELD: one arrow per cell of the face, each standing ON
  // the face and pressing into it. One arrow at the centre read as a point
  // load on a face that carries none, and on a curved or L-shaped face the
  // centre is not even on the face.
  t = IM_REGISTER_TEST(engine, "sim", "pressure_is_a_field_over_the_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
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
    fem::Study* study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
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
    load.id = Doc()->AllocateId();
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
    IM_CHECK(study != nullptr);
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(body.elementMap.names.size() >= 2);
    auto it = body.elementMap.names.begin();
    const ElementRef fixedFace{body.createdBy, it->second};
    ++it;
    const ElementRef loadedFace{body.createdBy, it->second};
    fem::Load fixed;
    fixed.id = Doc()->AllocateId();
    fixed.type = fem::LoadType::Fixed;
    fixed.face = fixedFace;
    study->loads.push_back(fixed);
    fem::Load press;
    press.id = Doc()->AllocateId();
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::EnterSimulation(*s_App);
    ctx->Yield(2);
    fem::Study* study = Doc()->StudyForRun(s_App->simulation.activeStudyId);
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
    ToolbarCreateAccept(ctx, "###tb_box");
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

    // The toolbar opens the Simulation tool window on an assembly tab.
    ToolbarClick(ctx, "###tb_asm_simulation");
    ctx->Yield(2);
    IM_CHECK(s_App->simulation.active);
    IM_CHECK(s_App->simulationToolOpen);
    IM_CHECK(ctx->ItemExists("//Simulation###ToolWindow/###study_add"));
    IM_CHECK_EQ(s_App->assembly->GetStudies().size(), (size_t)1);
    const uint64_t studyId = s_App->simulation.activeStudyId;
    IM_CHECK(studyId != 0);

    // Roles through the study edit path (the combos draw per occurrence).
    ui::ModifyStudy(*s_App, studyId, "roles", [&](fem::Study& cs) {
      cs.participants.push_back({upper, fem::ParticipantRole::Meshed, 0, ""});
      cs.participants.push_back({lower, fem::ParticipantRole::Rigid, 0, ""});
      cs.meshSize = 12.0;
    });
    ctx->Yield(2);
    // The rigid row offers the clamp; one click adds the contact anchor.
    const std::string clamp =
        "//Simulation###ToolWindow/###clamp_" + std::to_string(lower);
    IM_CHECK(ctx->ItemExists(clamp.c_str()));
    ctx->ItemClick(clamp.c_str());
    ctx->Yield(2);
    const fem::Study* st = s_App->assembly->FindStudy(studyId);
    IM_CHECK(st != nullptr);
    IM_CHECK_EQ(st->loads.size(), (size_t)1);
    IM_CHECK(st->loads[0].type == fem::LoadType::Fixed);
    IM_CHECK_EQ(st->loads[0].contactWith, lower);
    // And one undo takes the clamp away again: a full-state snapshot.
    ui::UndoAssembly(*s_App);
    ctx->Yield();
    st = s_App->assembly->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->loads.empty());
    ctx->ItemClick(clamp.c_str());
    ctx->Yield(2);
    st = s_App->assembly->FindStudy(studyId);
    IM_CHECK(st != nullptr && st->loads.size() == 1);

    // A face clicked on the SUPPORT is refused in words, not dropped.
    ui::SimulationArmTool(*s_App, ui::SimulationState::Tool::Pressure);
    const Body& body = Doc() ? Doc()->GetBodies()[0]
                             : ui::PartDocById(*s_App, partId)->GetBodies()[0];
    const ElementRef someFace{body.createdBy,
                              body.elementMap.names.begin()->second};
    ui::SimulationApplyToFace(*s_App, lower, someFace);
    IM_CHECK(!s_App->simulation.placeRefusal.empty());
    st = s_App->assembly->FindStudy(studyId);
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
    for (const ui::DocumentRenderMesh& m : s_App->documentMeshes) {
      if (m.occurrenceId == lower) {
        ghost = ghost || s_App->meshRenderer->IsInstanceGhost(m.meshId);
        for (const glm::uvec2& r :
             s_App->meshRenderer->InstanceHighlightRanges(m.meshId))
          tinted = tinted || (r.y >> MeshRenderer::kTintShift) ==
                                 MeshRenderer::kTintFixture;
      }
      if (m.occurrenceId == upper)
        meshedGhost =
            meshedGhost || s_App->meshRenderer->IsInstanceGhost(m.meshId);
    }
    IM_CHECK(tinted);
    IM_CHECK(ghost);
    IM_CHECK(!meshedGhost);

#ifdef HAVE_NETGEN
    // Run from the window; the result lands on the assembly and is fresh.
    ctx->ItemClick("//Simulation###ToolWindow/###study_run");
    for (int i = 0; i < 6000 && !s_App->assembly->StudyResults(studyId); ++i) {
      const fem::Study* cur = s_App->assembly->FindStudy(studyId);
      if (cur && !cur->error.empty())
        break;
      ctx->Yield();
    }
    const fem::Study* cur = s_App->assembly->FindStudy(studyId);
    IM_CHECK(cur != nullptr);
    if (!cur->error.empty())
      ctx->LogError("assembly study: %s", cur->error.c_str());
    const fem::StudyRuntime* rt = s_App->assembly->StudyResults(studyId);
    IM_CHECK(rt != nullptr);
    IM_CHECK(rt->maxDisplacement >= 0.0);
    IM_CHECK(!ui::StudyStale(*s_App, studyId));
#endif

    // Close leaves simulation mode and the window goes with it.
    ctx->ItemClick("//Simulation###ToolWindow/###simulation_close");
    ctx->Yield(2);
    IM_CHECK(!s_App->simulation.active);
    IM_CHECK(!s_App->simulationToolOpen);
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

  // A study is a row in the part's dock (ui::ListRow): click selects it, its
  // editor is the Simulation window, and Delete is in its menu -- where the
  // whole editor, and a Delete button at its foot, used to be inline under
  // the list (docs/ui/panels.md#simulation-the-study-panel-and-window).
  t = IM_REGISTER_TEST(engine, "sim", "a_study_is_a_row_edited_in_the_window");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    BuildCantileverStudy(ctx, 60.0);
    const uint64_t first = s_App->simulation.activeStudyId;
    ctx->ItemClick("//###Model/**/###study_add");
    ctx->Yield(2);
    const uint64_t second = s_App->simulation.activeStudyId;
    IM_CHECK(second != first);
    IM_CHECK_EQ(Doc()->GetStudies().size(), (size_t)2);

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
    IM_CHECK_EQ(Doc()->GetStudies().size(), (size_t)1);
    IM_CHECK_EQ(Doc()->GetStudies()[0].id, first);

    ctx->ItemClick("//###ToolWindow/###simulation_close");
    ctx->Yield(2);
    IM_CHECK(!s_App->simulation.active);
    ctx->SetRef("//##MainMenuBar");
  };
}
