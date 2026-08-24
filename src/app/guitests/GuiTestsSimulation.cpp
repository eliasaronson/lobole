#include "app/guitests/GuiTestsCommon.h"
#include "ui/SimulationActions.h"
#include "ui/StudyPanel.h"

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
}
