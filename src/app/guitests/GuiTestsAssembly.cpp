#include "app/guitests/GuiTestsCommon.h"
#include "ui/AssemblyActions.h"

// Assembly GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterAssemblyGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "assembly", "bom_counts_through_a_sub_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    Doc()->GetFeature(0)->SetMaterial("steel");
    Doc()->Regenerate();
    ctx->Yield(2);
    const uint64_t partId = s_App->parts[0].id;

    const size_t unitIdx = ui::AddAssembly(*s_App, "Unit");
    ui::SetActivePart(*s_App, unitIdx);
    for (int i = 0; i < 2; ++i) {
      Occurrence o;
      o.name = "Inner " + std::to_string(i + 1);
      o.partId = partId;
      o.fixed = true;
      s_App->assembly->AddOccurrence(o);
    }
    const size_t topIdx = ui::AddAssembly(*s_App, "Top");
    ui::SetActivePart(*s_App, topIdx);
    {
      Occurrence sub;
      sub.name = "Unit 1";
      sub.partId = s_App->parts[unitIdx].id;
      sub.refKind = OccurrenceRef::Assembly;
      sub.fixed = true;
      s_App->assembly->AddOccurrence(sub);
      Occurrence direct;
      direct.name = "Loose box";
      direct.partId = partId;
      direct.fixed = true;
      s_App->assembly->AddOccurrence(direct);
    }
    ctx->Yield(3);

    // The section is collapsed by default and had no id until this round.
    ctx->ItemClick("//###Model/###bom_header");
    ctx->Yield(2);
    // The export button only draws inside the open section, so its presence
    // is the proof the header actually expanded. (The table itself is an ID
    // SCOPE, not an item — ItemExists never finds one.)
    IM_CHECK(ctx->ItemExists("//###Model/###bom_export"));

    // One row for the one part, counting through the sub-assembly.
    const ui::BomTable bom = ui::ComputeBom(*s_App);
    IM_CHECK_EQ(bom.rows.size(), (size_t)1);
    IM_CHECK_EQ(bom.rows[0].quantity, 3);
    IM_CHECK_EQ(bom.rows[0].material, std::string("steel"));
    IM_CHECK(bom.rows[0].massKnown);
    IM_CHECK(bom.massKnown);
    IM_CHECK(std::abs(bom.totalMassG - 3.0 * bom.rows[0].unitMassG) < 1e-6);
    IM_CHECK(bom.warnings.empty());
  };

  // An exploded view is pure display state: it must move what is drawn and
  // leave the occurrence transforms alone (that invariant is what makes a
  // saved exploded view unable to corrupt a placement).
  t = IM_REGISTER_TEST(engine, "assembly",
                       "an_exploded_view_moves_what_is_drawn");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Exploding");
    ui::SetActivePart(*s_App, asmIdx);
    for (int i = 0; i < 3; ++i) {
      Occurrence o;
      o.name = "Part " + std::to_string(i + 1);
      o.partId = partId;
      o.fixed = true;
      o.position = glm::vec3(60.0f * static_cast<float>(i), 0.0f, 0.0f);
      s_App->assembly->AddOccurrence(o);
    }
    ctx->Yield(3);
    std::vector<glm::vec3> before;
    for (const Occurrence& o : s_App->assembly->GetOccurrences())
      before.push_back(o.position);

    ctx->ItemClick("//###Model/###explode_add");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->assembly->GetExplodedViews().size(), (size_t)1);
    const uint64_t viewId = s_App->assembly->GetExplodedViews()[0].id;
    IM_CHECK_EQ(s_App->activeExplodedView, viewId);

    // "Auto-explode" and "Delete view" shared a SameLine that ran past the
    // panel edge, so the second of them could not be clicked at all.
    IM_CHECK(ctx->ItemExists("//###Model/###explode_auto"));
    IM_CHECK(ctx->ItemExists("//###Model/###explode_delete"));
    ctx->ItemClick("//###Model/###explode_auto");
    ctx->Yield(3);

    const ExplodedView* view = s_App->assembly->FindExplodedViewById(viewId);
    IM_CHECK(view != nullptr);
    IM_CHECK_EQ(view->steps.size(), (size_t)3);
    double moved = 0.0;
    for (const ExplodeStep& s : view->steps)
      moved += glm::length(s.offset);
    IM_CHECK(moved > 0.0); // it actually pushed them apart

    // ...and the placements themselves did not move.
    for (size_t i = 0; i < before.size(); ++i)
      IM_CHECK(s_App->assembly->GetOccurrences()[i].position == before[i]);

    // A view selectable used to sit in a PushID(int) scope, so no test could
    // pick one; naming it is what makes this reachable.
    ctx->ItemClick(
        ("//###Model/###explode_view_" + std::to_string(viewId)).c_str());
    ctx->Yield();
    IM_CHECK_EQ(s_App->activeExplodedView, viewId);

    ctx->ItemClick("//###Model/###explode_delete");
    ctx->Yield(2);
    IM_CHECK(s_App->assembly->GetExplodedViews().empty());
    IM_CHECK_EQ(s_App->activeExplodedView, (uint64_t)0);
  };

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
}
