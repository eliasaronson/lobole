#include "app/guitests/GuiTestsCommon.h"

// Sketch GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterSketchGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "sketch", "front_plane_enters_edit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    // A SketchFeature was added and we entered sketch edit (Select is default).
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(Doc()->GetFeature(0)->TypeName() == "Sketch");
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    // Escape leaves sketch edit (nothing pending, Select tool, no selection).
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // The primary way a user opens a sketch now that the Create menu is gone:
  // the toolbar's sketch button arms plane-picking, then a viewport click on a
  // base-plane quad enters edit mode. The other sketch tests take the direct
  // EnterSketchOnPlane() shortcut, so this is the one that guards the whole
  // interactive entry path end to end.
  t = IM_REGISTER_TEST(engine, "sketch", "toolbar_plane_pick_enters_edit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_new_sketch");
    IM_CHECK(s_App->sketchEdit.pickingPlane); // armed, waiting for a plane
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    // Frame the front (XY) plane head-on. The doc is empty, so all three base
    // quads pass through the origin; looking straight down +Z makes a click
    // near the visible outer edge, at world (7,7,0), land unambiguously on XY
    // (the other two are edge-on there).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0, 0, 0));
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(glm::vec3(0, 0, 10));
    ctx->Yield();

    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(7.0f, 7.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    IM_CHECK(!s_App->sketchEdit.pickingPlane); // pick consumed
    IM_CHECK(s_App->sketchEdit.IsEditing());   // now editing the new sketch
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    auto* sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(Doc()->FeatureCount() - 1));
    IM_CHECK(sk != nullptr);
    IM_CHECK(sk->TypeName() == "Sketch");
  };

  t = IM_REGISTER_TEST(engine, "sketch", "draw_line_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 1);
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);

    // Entering a sketch aligns the camera to the plane over a 1s animation.
    // Wait for it to settle before projecting click coordinates — otherwise
    // SketchUVToScreen reads a mid-flight camera and the clicks land off
    // target.
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();

    // Select is the default tool now; pick Line, then two viewport clicks place
    // a line. Anchor at the projected origin and offset in screen pixels (an
    // empty sketch is fit tightly, so a UV offset would project off-screen).
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    glm::vec2 c = ui::SketchUVToScreen(*s_App, sketch->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(c.x - 60.0f, c.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(c.x + 60.0f, c.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    // Committing the line rebuilds the sketch feature through the command
    // system, so the pointer captured above now dangles — re-fetch it before
    // reading the entity list (this stale-pointer read, not the clicks, was why
    // this test used to fail).
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    IM_CHECK_EQ(sketch->entities.size(), (size_t)1);

    // Undo removes the line but keeps the sketch
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    IM_CHECK_EQ(sketch->entities.size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "sketch", "rect_extrude_produces_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();

    // Inject a rectangle deterministically via a command (viewport click
    // coordinates are resolution-dependent; drawing is covered separately)
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2}); // 4 lines + corner/H/V constraints
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Extrude from the sketch toolbar (U8a) → preview, then accept
    ToolbarClick(ctx, "###tb_sk_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App)); // extrude is a pending preview
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();

    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->TypeName() == "Extrude");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() > 1.0); // rect x default dist
  };

  t = IM_REGISTER_TEST(engine, "sketch", "model_toolbar_extrudes_finished");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();

    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Finish the sketch — the state the user was previously stuck in.
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->TypeName() == "Extrude");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
  };

  // A pending preview is added straight to the Document (not via a command),
  // so switching tabs used to stash it into the old part permanently — off
  // the undo stack, unremovable. The tab switch must discard it first.
  t = IM_REGISTER_TEST(engine, "sketch", "tree_plane_row_consumes_pick");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();

    ToolbarClick(ctx, "###tb_new_sketch");
    IM_CHECK(s_App->sketchEdit.pickingPlane);
    ctx->ItemClick("//##DockHost/**/Top Plane");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.pickingPlane); // consumed
    IM_CHECK(s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);

    // Unarmed, a single click still only selects (double-click opens) — the
    // gesture must not change for ordinary browsing.
    ctx->ItemClick("//##DockHost/**/Top Plane");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // U10 — the defaults-and-refusals gate.
  //
  // For EVERY creation tool, with its own defaults and no picks made, exactly
  // one of two things must be true: it changes the document, or it refuses
  // visibly. "Accepts and changes nothing" is the bug this suite exists to
  // catch — five of them shipped and were found by hand months later (a hole
  // whose countersink defaulted four times the bore, a split plane at the
  // origin missing the body, drawing views stacked at 1:1, occurrences at one
  // point, an edge-less fillet committing a no-op feature).
  //
  // One registered test per tool (named for its toolbar id) so a failure says
  // WHICH tool regressed; the case is looked up by test name because
  // ImGuiTest::TestFunc is a raw function pointer and cannot capture.
  t = IM_REGISTER_TEST(engine, "constraints", "coincident_merges_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    // Inject two lines with near-but-not-equal shared endpoints
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 0}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{4.2f, 0.3f}, {4, 3}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Select line1.a and line0.b, apply Coincident
    s_App->sketchEdit.selected = {{l1, 0}, {l0, 1}};
    bool ok = ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Coincident, 0.0);
    IM_CHECK(ok);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    glm::vec2 l0b = std::get<sketch::Line>(sk->entities[0]).b;
    glm::vec2 l1a = std::get<sketch::Line>(sk->entities[1]).a;
    IM_CHECK(glm::length(l0b - l1a) < 1e-3f);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);

    // Undo restores (constraint removed, points back apart)
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "constraints", "horizontal_levels_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 1.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& ln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f);
  };

  // Clicking a line's body (not an endpoint) selects the whole entity, so a
  // whole-line constraint like Horizontal can then be applied.
  t = IM_REGISTER_TEST(engine, "trim", "removes_middle_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t h = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}}); // horizontal
    sk->AddEntity(sketch::Line{{-1, -1}, {-1, 1}}); // crosses at x=-1
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});   // crosses at x=1
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    // Switch to the Trim tool and click the middle of the horizontal line.
    s_App->sketchEdit.tool = ui::SketchTool::Trim;
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    // The middle span [-1,1] is gone; the two outer pieces + 2 crossers remain.
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)4);
    IM_CHECK(sk->IndexOfEntity(h) != SIZE_MAX); // piece 1 kept the id
  };

  // Hovering (no click) with the Trim tool computes the preview span for the
  // segment under the cursor.
  t = IM_REGISTER_TEST(engine, "trim", "preview_highlights_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    sk->AddEntity(sketch::Line{{-1, -1}, {-1, 1}});
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    s_App->sketchEdit.tool = ui::SketchTool::Trim;
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y)); // hover, no click
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.hasTrimPreview);
    // The previewed span is the middle piece [-1,0]..[1,0].
    float lo = std::min(s_App->sketchEdit.trimPreviewA.x,
                        s_App->sketchEdit.trimPreviewB.x);
    float hi = std::max(s_App->sketchEdit.trimPreviewA.x,
                        s_App->sketchEdit.trimPreviewB.x);
    IM_CHECK(std::abs(lo + 1.0f) < 1e-3f);
    IM_CHECK(std::abs(hi - 1.0f) < 1e-3f);
    IM_CHECK_EQ(sk->entities.size(), (size_t)3); // preview didn't mutate
  };

  // Fillet a right-angle corner via the UI path (pick corner → tangent arc).
  t = IM_REGISTER_TEST(engine, "mirror", "box_select_and_tap_advances");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
    sk->AddEntity(sketch::Line{{0, 1}, {1, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;

    // Box around both lines (empty-space corners) adds them to the set.
    glm::vec2 a = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    glm::vec2 b = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 2));
    ctx->MouseMoveToPos(ImVec2(a.x, a.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(b.x, b.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2);
    IM_CHECK(!s_App->sketchEdit.mirrorAxisPhase);

    // A tap in empty space continues to the axis step (bottom-left, clear
    // of the top-right session window).
    glm::vec2 t2 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, -2));
    ctx->MouseMoveToPos(ImVec2(t2.x, t2.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.mirrorAxisPhase);
  };

  // U8 polish: a pre-selection whose sole construction line names the mirror
  // axis commits the mirror on tool activation — one gesture.
  t = IM_REGISTER_TEST(engine, "mirror", "preselected_construction_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l1 = sk->AddEntity(sketch::Line{{1, 0}, {2, 1}});
    sketch::Line axisLine{{0, -2}, {0, 2}};
    axisLine.construction = true;
    const uint32_t ax = sk->AddEntity(axisLine);
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const size_t entitiesBefore = sk->entities.size();

    s_App->sketchEdit.selected = {{l1, 0}, {ax, 0}};
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    ui::MirrorToolActivated(*s_App); // the toolbar activation hook
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->entities.size() > entitiesBefore); // mirrored copy exists
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select); // done
    IM_CHECK(s_App->sketchEdit.selected.empty());
  };

  // U8 polish: Pattern selects in-tool (box select), a tap in empty space
  // applies, and Apply hands the tool back to Select.
  t = IM_REGISTER_TEST(engine, "sketch", "ops_row_opens_pattern_session");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 0.5});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // A LINKED pattern is a derived op (one-shot copies wouldn't list).
    s_App->sketchEdit.selected = {{c1, 0}};
    s_App->sketchEdit.patternCircular = false;
    s_App->sketchEdit.patternCount = 3;
    s_App->sketchEdit.patternStep = glm::vec2(2.0f, 0.0f);
    s_App->sketchEdit.linkGenerative = true;
    IM_CHECK(ui::ApplyPattern(*s_App));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->derived.size(), (size_t)1);

    // Double-click the Operations row → the edit session opens.
    ctx->ItemDoubleClick("//###Model/###op0");
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.editOpIndex, 0);
    IM_CHECK(ctx->GetWindowByRef("//Linear Pattern###ToolWindow") != nullptr);

    // Edit the count in the session; the op regenerates.
    ctx->ItemInputValue("//Linear Pattern###ToolWindow/Count", 5);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->derived[0].count, 5);

    ctx->KeyPress(ImGuiKey_Escape); // closes the session
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.editOpIndex, -1);
  };

  // Escape backs out one level per press: tool → clear selection → leave edit.
  t = IM_REGISTER_TEST(engine, "infer", "horizontal_snaps");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, 0));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.06f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    bool hasH = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Horizontal)
        hasH = true;
    IM_CHECK(hasH);
    const auto& ln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f); // snapped flat
  };

  // Ending a line near an existing point snaps coincident to it.
  t = IM_REGISTER_TEST(engine, "infer", "endpoint_coincident");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    // Keep the point well within the sketch's visible UV range (~±2 at the
    // default framing); further out it projects onto a side panel and the click
    // that should land on it misses the viewport.
    uint32_t target = sk->AddEntity(sketch::Point{{1.5f, 0.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    // Let the sketch-entry camera animation settle so the projected click
    // coordinates land on target (see draw_line_undo).
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 = ui::SketchUVToScreen(
        *s_App, sk->plane, glm::vec2(1.5f, 0.5f)); // onto the point
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    bool coincidentToTarget = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Coincident &&
          (c.a.entity == target || c.b.entity == target))
        coincidentToTarget = true;
    IM_CHECK(coincidentToTarget);
  };

  // Ending a line under an existing point (shared X, far in Y) infers a
  // two-point Vertical alignment constraint (Phase N5).
  t = IM_REGISTER_TEST(engine, "infer", "endpoint_aligns_vertical");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t target =
        sk->AddEntity(sketch::Point{{1.5f, 1.8f}}); // shares X below
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    // Line from (-1.5,-1) to (1.5,-1): the end shares X (1.5) with the point
    // but is well away in Y → vertical alignment, not coincident.
    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -1.0f));
    glm::vec2 p1 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, -1.0f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    bool verticalToTarget = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Vertical &&
          (c.a.entity == target || c.b.entity == target))
        verticalToTarget = true;
    IM_CHECK(verticalToTarget);
  };

  // The staged tool prompt reports the right step for each tool/state (N1).
  t = IM_REGISTER_TEST(engine, "mirror", "guided_flow");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(
        sketch::Line{{0.5f, 0.6f}, {1.5f, 0.6f}}); // geometry to mirror
    sk->AddEntity(sketch::Line{{0.0f, -1.0f}, {0.0f, 1.0f}}); // vertical axis
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    // Pin the camera: at an inherited far-out zoom, the geometry line's
    // endpoint falls within the (screen-px) point tolerance of the axis
    // click below and would be picked as the mirror axis.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const size_t n0 = sk->entities.size(); // 2
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    s_App->sketchEdit.mirrorAxisPhase = false;
    s_App->sketchEdit.selected.clear();

    // Step 1: click the geometry line's midpoint to select it.
    glm::vec2 g =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.selected.empty());

    // Enter advances to the axis-pick step.
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.mirrorAxisPhase);

    // Step 2: click the axis line → the mirror runs and adds a copy.
    glm::vec2 ax =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 0.0f));
    ctx->MouseMoveToPos(ImVec2(ax.x, ax.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->entities.size() > n0); // a mirrored copy was added
  };

  // Guided mirror across the Y datum axis (NF1): the axis is pickable even
  // though it isn't an entity.
  t = IM_REGISTER_TEST(engine, "mirror", "across_datum_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0.5f, 0.6f}, {1.5f, 0.6f}}); // off the Y axis
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Mirror;
    s_App->sketchEdit.mirrorAxisPhase = false;
    s_App->sketchEdit.selected.clear();

    // Select the line, Enter, then click on the Y axis (x=0, well away from the
    // line) — no entity there, so the datum axis is picked.
    glm::vec2 g =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    glm::vec2 ax =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, -1.2f));
    ctx->MouseMoveToPos(ImVec2(ax.x, ax.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    // v51: the linked mirror is a derived OP recording the datum axis.
    IM_CHECK(!sk->derived.empty());
    IM_CHECK(sk->derived[0].kind == sketch::DerivedKind::Mirror);
    IM_CHECK(sk->derived[0].axisEntity == sketch::kAxisY);
  };

  // With auto-constrain off, no constraints are inferred.
  t = IM_REGISTER_TEST(engine, "infer", "toggle_suppresses");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = false; // suppressed

    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-2, 0));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.06f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0); // nothing inferred
    s_App->sketchEdit.autoConstrain = true; // restore default for other tests
  };

  // Dragging a line's body translates the whole line and its coincident
  // neighbour, end to end through the viewport drag.
  t = IM_REGISTER_TEST(engine, "drag", "move_whole_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{2, 0}, {2, 3}}); // l1.a ~ l0.b
    sk->constraints.push_back(
        {sketch::ConstraintKind::Coincident, {l0, 1}, {l1, 0}, 0.0});
    sk->constraints.push_back(
        {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Select;

    // Press on the line's midpoint (its body), drag up 1 unit, release.
    glm::vec2 mid = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 0));
    glm::vec2 to = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0, 1));
    ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& moved =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l0)]);
    // A driving dimension makes this a whole-sketch translation: the line
    // keeps its length and both endpoints of its neighbour move by the same
    // delta, not merely the coincident join.
    IM_CHECK(moved.a.y > 0.5f && moved.b.y > 0.5f);
    IM_CHECK(std::abs(glm::length(moved.b - moved.a) - 4.0f) < 1e-2f);
    const auto& nbr =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l1)]);
    IM_CHECK(glm::length(nbr.a - moved.b) < 1e-2f);
    IM_CHECK(nbr.b.y > 3.5f);
    IM_CHECK(s_App->commands->CanUndo()); // one undoable edit
  };

  // 3D fillet via edge picking (M7/T5): create a box, start a fillet, click a
  // visible B-Rep edge, accept — the body loses volume along that edge.
  t = IM_REGISTER_TEST(engine, "sketch", "selection_seeds_extrude_regions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
    Doc()->Regenerate();
    ctx->Yield();

    // Select circle 1, then Extrude from the sketch toolbar.
    s_App->sketchEdit.selected.push_back({c1, 0});
    ToolbarClick(ctx, "###tb_sk_extrude");
    ctx->Yield();
    auto* ext =
        dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)1);
    IM_CHECK(ext->regions[0] == std::vector<uint32_t>{c1});
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    // One cylinder's volume, not two (r=1, default distance 1 → pi).
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - M_PI) < 1e-2);
  };

  t = IM_REGISTER_TEST(engine, "region", "pick_one_of_two_regions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    auto sk =
        std::make_unique<SketchFeature>("Profiles", SketchPlane(PlaneType::XY));
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                       ExtrudeMode::NewBody));
    Doc()->Regenerate();
    const uint64_t extId = Doc()->GetFeature(1)->GetId();
    ui::OpenFeatureEditor(*s_App, extId); // committed feature → Tool window

    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(2.5f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(2.5f, 0.5f, 12.0f));
    ctx->Yield();

    // Arm the Profiles slot (committed feature → Properties panel).
    ctx->ItemClick("//Extrude###ToolWindow/###slot7");
    IM_CHECK(s_App->slotPick.Active());

    // Click inside circle 1's region (the sketch-plane point, not the body).
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(0.0f, 0.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)1);
    // NOLINT: ext is re-fetched after every committed edit below.
    IM_CHECK(ext->regions[0] == std::vector<uint32_t>{c1});
    IM_CHECK(s_App->slotPick.Active()); // multi slot stays armed
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 2.0 * M_PI) <
             1e-2);

    // Clicking the same region again removes it: back to all regions.
    // (Re-fetch: the committed edit replaced the feature object.)
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)0);
    // A multi-region extrude is one body (compound of both cylinders).
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 4.0 * M_PI) <
             1e-2);

    ctx->KeyPress(ImGuiKey_Escape); // disarm the slot
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active());
  };

  // U6: the Ctrl+K command palette — fuzzy find "box", Enter runs the
  // Create Box command (same action the menu calls).
  t = IM_REGISTER_TEST(engine, "dims", "label_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = false;
    glm::vec2 p0 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, -0.5f));
    ctx->MouseMoveToPos(ImVec2(p0.x, p0.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(p1.x, p1.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    const uint32_t lineId = sketch::EntityId(sk->entities[0]);

    // Dimension the two endpoints; the seed pushes the label off the line.
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.selected = {{lineId, 0}, {lineId, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0)); // replaced
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    const glm::vec2 seeded = sk->constraints[0].labelOffset;
    IM_CHECK_GT(glm::length(seeded), 0.1f);

    // The label pill sits at the dimension line's midpoint: mid + offset.
    const glm::vec2 labelUV = glm::vec2(0.0f, -0.5f) + seeded;
    const glm::vec2 labelScr = ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
    const auto lineBefore = std::get<sketch::Line>(sk->entities[0]);
    ctx->MouseMoveToPos(ImVec2(labelScr.x, labelScr.y));
    ctx->Yield(2); // dimHot latches for the canvas gate
    ctx->MouseDown(ImGuiMouseButton_Left);
    // Diagonal drag: only the perpendicular (vertical, for this horizontal
    // line) component may take effect — the axis constraint (Phase 3.5).
    ctx->MouseMoveToPos(ImVec2(labelScr.x + 45.0f, labelScr.y - 45.0f));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // still just the dim
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);    // nothing drawn through
    const glm::vec2 dragged = sk->constraints[0].labelOffset;
    IM_CHECK_GT(glm::length(dragged - seeded), 0.1f); // the label moved
    // The along-span (x) component was killed: the label slid on its axis.
    IM_CHECK_LE(std::abs(dragged.x - seeded.x), 1e-3f);
    const auto lineAfter = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK_LE(glm::length(lineAfter.a - lineBefore.a), 1e-5f);
    IM_CHECK_LE(glm::length(lineAfter.b - lineBefore.b), 1e-5f);

    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit cleanly
    ctx->Yield();
  };

  // The Dimension tool (Decision 1A): D arms it; clicks accumulate targets
  // and the tool infers the dimension kind — two parallel lines = the
  // perpendicular distance, two non-parallel = angle, a circle = radius, a
  // line + empty click = its length. Each creation opens the inline editor.
  t = IM_REGISTER_TEST(engine, "dims", "dimension_tool_infers");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t lA = sk->AddEntity(sketch::Line{{-1.5f, -1}, {1.5f, -1}});
    uint32_t lB = sk->AddEntity(sketch::Line{{-1.5f, 1}, {1.5f, 1}});
    uint32_t lC = sk->AddEntity(sketch::Line{{-1.5f, 0}, {1.5f, 0.8f}});
    uint32_t ci = sk->AddEntity(sketch::Circle{{0.8f, 1.8f}, 0.5});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    // Pin the camera (zoom from earlier tests changes the pick tolerances).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    ctx->KeyPress(ImGuiKey_D); // arm the tool
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    auto closeEditor = [&](size_t expectDims) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(sk->constraints.size(), expectDims);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, (int)expectDims - 1);
      ctx->KeyPress(ImGuiKey_Escape); // closes the inline editor only
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
      IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    };

    // A single line enters LENGTH placement immediately (B); a placement
    // click landing on a PARALLEL line refines it to the perpendicular
    // distance (DistancePL) placement, value = gap.
    clickUV({0, -1});
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK(s_App->sketchEdit.dimPlacing); // length ghost is up right away
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    clickUV({0, 1}); // lands on lB → refine, not place
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(!s_App->sketchEdit.dimPlacePair); // now fixed kind: label only
    clickUV({-0.5f, 0});                       // place the label
    closeEditor(1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::DistancePL);
    IM_CHECK_EQ(sk->constraints[0].a.entity, lA);
    IM_CHECK_EQ(sk->constraints[0].b.entity, lB);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 1e-3);

    // A placement click on a NON-parallel line refines to angle, pinned at
    // the current wedge (immediate — angle placement is deferred).
    clickUV({0, -1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0, 0.4f}); // lands on lC → angle
    IM_CHECK(!s_App->sketchEdit.dimPlacing);
    closeEditor(2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::Angle);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - std::atan2(0.8, 3.0)),
                2e-2);
    {
      const uint32_t ea = sk->constraints[1].a.entity;
      const uint32_t eb = sk->constraints[1].b.entity;
      IM_CHECK((ea == lA && eb == lC) || (ea == lC && eb == lA));
    }

    // A circle rim → radius, with label-only placement.
    clickUV({1.3f, 1.8f});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({2.2f, 2.4f}); // place the label
    closeEditor(3);
    IM_CHECK(sk->constraints[2].kind == sketch::ConstraintKind::Radius);
    IM_CHECK_EQ(sk->constraints[2].a.entity, ci);
    IM_CHECK_LE(std::abs(sk->constraints[2].value - 0.5), 1e-3);

    // A line click → immediate LENGTH placement; an empty-space click
    // PLACES it (the span is axis-parallel, so every zone is aligned).
    // (x off 0 — a click ON the Y axis would refine against the axis.)
    clickUV({0.5f, -1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    clickUV({0.7f, -1.8f}); // empty space: place below the line
    closeEditor(4);
    IM_CHECK(sk->constraints[3].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints[3].a.entity, lA);
    IM_CHECK_EQ(sk->constraints[3].b.entity, lA);
    IM_CHECK_LE(std::abs(sk->constraints[3].value - 3.0), 1e-3);

    // A placement click on a POINT (the circle's centre) refines to the
    // perpendicular point-line distance.
    clickUV({0.5f, -1}); // lA → length placement again
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.8f, 1.8f}); // the circle centre → refine to DistancePL
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(!s_App->sketchEdit.dimPlacePair);
    clickUV({2.0f, -0.2f}); // place the label
    closeEditor(5);
    IM_CHECK(sk->constraints[4].kind == sketch::ConstraintKind::DistancePL);
    IM_CHECK_EQ(sk->constraints[4].a.entity, ci); // the centre point
    IM_CHECK_EQ(sk->constraints[4].b.entity, lA);
    IM_CHECK_LE(std::abs(sk->constraints[4].value - 2.8), 1e-3);

    // No geometry moved (every dimension pinned its current value).
    const auto& la =
        std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lA)]);
    IM_CHECK_LE(std::abs(la.a.y + 1.0f), 1e-3f);

    // Esc with no accumulated targets disarms the tool (staged).
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Placement-driven kind inference (Phase 3.7, the Onshape behavior): a
  // point pair enters a placement stage; the cursor zone picks the kind —
  // above/below the pair = horizontal (DistanceX), sideways = vertical
  // (DistanceY), pulled perpendicular to the span = aligned (Distance) —
  // and the click also places the label. Esc walks the staged ladder.
  t = IM_REGISTER_TEST(engine, "dims", "placement_zones");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const glm::vec2 pa(-1.0f, -0.6f), pb(1.0f, 0.6f); // a diagonal pair
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t p0 = sk->AddEntity(sketch::Point{pa});
    uint32_t p1 = sk->AddEntity(sketch::Point{pb});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    auto closeEditor = [&](size_t expectDims) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(sk->constraints.size(), expectDims);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, (int)expectDims - 1);
      ctx->KeyPress(ImGuiKey_Escape);
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    };

    // The staged Esc ladder: placing → targets kept → targets cleared →
    // tool disarmed.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    IM_CHECK(s_App->sketchEdit.dimPlacePair);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.dimPlacing);
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2); // kept
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.selected.empty());
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);

    // Placed ABOVE the pair → horizontal dimension (DistanceX), |dx|.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.0f, 1.5f});
    closeEditor(1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::DistanceX);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 0.03);
    IM_CHECK_EQ(sk->constraints[0].a.entity, p0); // ordered: positive dx
    IM_CHECK_EQ(sk->constraints[0].b.entity, p1);
    // The label landed where clicked: x locked to the span (item 5), y at
    // the cursor's height above the lower point.
    IM_CHECK_LE(std::abs(sk->constraints[0].labelOffset.x), 1e-4f);
    IM_CHECK_LE(std::abs(sk->constraints[0].labelOffset.y - (1.5f + 0.6f)),
                0.05f);

    // Placed to the SIDE → vertical dimension (DistanceY), |dy|.
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({2.2f, 0.0f});
    closeEditor(2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::DistanceY);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 1.2), 0.03);
    IM_CHECK_LE(std::abs(sk->constraints[1].labelOffset.y), 1e-4f);
    IM_CHECK_LE(std::abs(sk->constraints[1].labelOffset.x - (2.2f + 1.0f)),
                0.05f);

    // Placed on the span's PERPENDICULAR → aligned (Distance), full length.
    const glm::vec2 d = glm::normalize(pb - pa);
    const glm::vec2 n(-d.y, d.x);
    const glm::vec2 mid = 0.5f * (pa + pb);
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV(mid + n * 1.2f);
    closeEditor(3);
    IM_CHECK(sk->constraints[2].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_LE(std::abs(sk->constraints[2].value - glm::length(pb - pa)),
                0.03);
    const glm::vec2 off = sk->constraints[2].labelOffset;
    IM_CHECK_LE(std::abs(glm::dot(off, d)), 0.02f);  // ⟂ to the span
    IM_CHECK_LE(glm::length(off - n * 1.2f), 0.05f); // lands where clicked

    ctx->KeyPress(ImGuiKey_Escape); // disarm
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Click-select + Delete for dimensions and constraint glyphs: a single
  // click on a pill (or glyph) selects that constraint — mutually exclusive
  // with entity selection so Delete is never ambiguous — and Delete/
  // Backspace removes it (one undoable edit). Esc deselects first.
  t = IM_REGISTER_TEST(engine, "dims", "select_delete");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5f, -0.5f}, {1.5f, -0.5f}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{1.5f, -0.5f}, {1.5f, 1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // [0] a distance dim (pinned at the current length), [1] a coincident.
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    s_App->sketchEdit.selected = {{l0, 1}, {l1, 0}};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Coincident, 0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);
    const glm::vec2 seeded = sk->constraints[0].labelOffset; // (0, ±0.8)

    // Single-click the dimension pill: SELECTS (entity selection cleared).
    const glm::vec2 pillUV = glm::vec2(0.0f, -0.5f) + seeded;
    auto clickPill = [&]() {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, pillUV);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->Yield(2); // dimHot latches for the canvas gate
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickPill();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 0);
    IM_CHECK(s_App->sketchEdit.selected.empty());

    // Esc deselects first (the ladder), still editing the sketch.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    IM_CHECK(s_App->sketchEdit.IsEditing());

    // Re-select, Delete removes the dimension; undo restores it.
    clickPill();
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 0);
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // the coincident remains
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Coincident);
    IM_CHECK_EQ(sk->entities.size(), (size_t)2); // entities untouched
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);

    // The coincident's GLYPH ("co", anchored at l0's first point + a small
    // offset): clicking inside its rect selects the constraint — the glyph
    // wins over the endpoint underneath. Backspace removes it.
    {
      glm::vec2 s =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
      // Badge rect starts at (s.x + 10, s.y - 16); aim mid-glyph.
      ctx->MouseMoveToPos(ImVec2(s.x + 17.0f, s.y - 10.0f));
      ctx->Yield(2); // glyphHot latches for the canvas gate
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, 1);
    IM_CHECK(s_App->sketchEdit.selected.empty()); // no entity grabbed
    ctx->KeyPress(ImGuiKey_Backspace);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->entities.size(), (size_t)2);

    // Entity click clears the constraint selection; Delete then removes the
    // ENTITY (and its dependent dimension), not a constraint by index.
    {
      glm::vec2 mid =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, -0.5f));
      ctx->MouseMoveToPos(ImVec2(mid.x, mid.y));
      ctx->Yield(2);
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    IM_CHECK_EQ(s_App->sketchEdit.selectedConstraint, -1);
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);    // l0 gone
    IM_CHECK_EQ(sk->constraints.size(), (size_t)0); // its dim dropped with it

    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Negative dimension values flip the dimension's direction (the SW/
  // Onshape convention): signed kinds (DistanceX/Y, Angle) move the free
  // geometry to the other side/wedge and store |v| (literals swap refs;
  // negative-evaluating exprs keep the raw signed value so regeneration
  // stays stable); DistancePL flips its side pin; unsigned kinds (aligned
  // Distance, Radius) silently use |v| with no flip.
  t = IM_REGISTER_TEST(engine, "dims", "negative_flip");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t p0 = sk->AddEntity(sketch::Point{{-1, -0.6f}});
    uint32_t p1 = sk->AddEntity(sketch::Point{{1, 0.6f}});
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 1.6f}, {0, 1.6f}});
    uint32_t pp = sk->AddEntity(sketch::Point{{-1, 0.9f}});
    uint32_t lA = sk->AddEntity(sketch::Line{{-2, -1.5f}, {-0.5f, -1.5f}});
    uint32_t lB = sk->AddEntity(sketch::Line{{-2, -1.5f}, {-0.65f, -0.825f}});
    uint32_t lD = sk->AddEntity(sketch::Line{{2.4f, -1.5f}, {2.4f, 1.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    using CK = sketch::ConstraintKind;
    auto addCon = [&](std::vector<sketch::PointRef> refs, CK kind, double v) {
      s_App->sketchEdit.selected = std::move(refs);
      IM_CHECK(ui::ApplySketchConstraint(*s_App, kind, v));
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    };
    addCon({{p0, 0}}, CK::Fixed, 0.0);                 // [0]
    addCon({{p0, 0}, {p1, 0}}, CK::DistanceX, 2.0);    // [1]
    addCon({{l0, 0}}, CK::Fixed, 0.0);                 // [2]
    addCon({{l0, 1}}, CK::Fixed, 0.0);                 // [3]
    addCon({{pp, 0}, {l0, 0}}, CK::DistancePL, 0.7);   // [4]
    addCon({{lA, 0}}, CK::Fixed, 0.0);                 // [5]
    addCon({{lA, 1}}, CK::Fixed, 0.0);                 // [6]
    addCon({{lB, 0}}, CK::Fixed, 0.0);                 // [7]
    addCon({{lA, 0}, {lB, 0}}, CK::Angle, 0.46364761); // [8]
    addCon({{lD, 0}, {lD, 1}}, CK::Distance, 3.0);     // [9]
    IM_CHECK_EQ(sk->constraints.size(), (size_t)10);

    auto editDim = [&](int idx, const char* text) {
      s_App->sketchEdit.editingDim = idx;
      ctx->Yield(2);
      for (int i = 0; i < 12; i++)
        ctx->KeyPress(ImGuiKey_Backspace);
      ctx->KeyChars(text);
      ctx->KeyPress(ImGuiKey_Enter);
      ctx->Yield(2);
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1); // committed cleanly
    };
    auto pointAt = [&](uint32_t id) {
      return std::get<sketch::Point>(sk->entities[sk->IndexOfEntity(id)]).p;
    };

    // DistanceX, literal "-2": refs swap (stored value stays +2), the free
    // point solves to the OTHER side of the fixed one. Undo restores both.
    editDim(1, "-2");
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 2.0), 1e-6);
    IM_CHECK_EQ(sk->constraints[1].a.entity, p1); // swapped
    IM_CHECK_EQ(sk->constraints[1].b.entity, p0);
    IM_CHECK_LE(std::abs(pointAt(p1).x - (-3.0f)), 1e-3f);
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints[1].a.entity, p0); // refs restored
    IM_CHECK_LE(std::abs(pointAt(p1).x - 1.0f), 1e-3f);

    // DistanceX, expression "-4/2": refs stay, the RAW signed value drives
    // (regen re-evaluates the expr, so the sign must carry the direction).
    editDim(1, "-4/2");
    IM_CHECK_LE(std::abs(sk->constraints[1].value - (-2.0)), 1e-6);
    IM_CHECK(sk->constraints[1].valueExpr == "-4/2");
    IM_CHECK_EQ(sk->constraints[1].a.entity, p0);          // NOT swapped
    IM_CHECK_LE(std::abs(pointAt(p1).x - (-3.0f)), 1e-3f); // other side

    // DistancePL, literal "-0.7": the side pin flips — the point crosses to
    // the other side of the line; stored value +0.7.
    editDim(4, "-0.7");
    IM_CHECK_LE(std::abs(sk->constraints[4].value - 0.7), 1e-6);
    IM_CHECK_LE(std::abs(pointAt(pp).y - 2.3f), 1e-3f); // was 0.9 (below)
    // ... and a negative-evaluating expression flips again, keeping the raw
    // value + the expression as typed.
    editDim(4, "0.7-1.4");
    IM_CHECK(sk->constraints[4].valueExpr == "0.7-1.4");
    IM_CHECK_LE(std::abs(sk->constraints[4].value - (-0.7)), 1e-6);
    IM_CHECK_LE(std::abs(pointAt(pp).y - 0.9f), 1e-3f); // back below

    // Angle, literal negative degrees: the mirrored wedge — lB rotates to
    // the other side of lA; stored value stays positive.
    editDim(8, "-26.565051");
    IM_CHECK_LE(std::abs(sk->constraints[8].value - 0.46364761), 1e-4);
    IM_CHECK_EQ(sk->constraints[8].a.entity, lB); // swapped
    {
      const auto& lb =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lB)]);
      const float dir = std::atan2(lb.b.y - lb.a.y, lb.b.x - lb.a.x);
      IM_CHECK_LE(std::abs(dir - (-0.46364761f)), 2e-2f); // below lA now
    }

    // Aligned Distance: no direction — a negative input is just |v|.
    editDim(9, "-2");
    IM_CHECK_LE(std::abs(sk->constraints[9].value - 2.0), 1e-6);
    IM_CHECK_EQ(sk->constraints[9].a.entity, lD); // untouched refs
    IM_CHECK_EQ((int)sk->constraints[9].a.which, 0);
    IM_CHECK_EQ((int)sk->constraints[9].b.which, 1);
    {
      const auto& ld =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lD)]);
      IM_CHECK_LE(std::abs(glm::length(ld.b - ld.a) - 2.0f), 1e-3f);
    }

    // Dimension-on-create with a negative typed value: same rule — the
    // created radius is |v|.
    ctx->KeyPress(ImGuiKey_C);
    ctx->Yield();
    {
      glm::vec2 c =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.3f, -0.4f));
      glm::vec2 r =
          ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.6f, -0.4f));
      ctx->MouseMoveToPos(ImVec2(c.x, c.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(ImVec2(r.x, r.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    }
    ctx->KeyChars("5");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    for (int i = 0; i < 4; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("-2");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints.back().kind == CK::Radius);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 2.0), 1e-6);
    {
      int circles = 0;
      double rad = 0.0;
      for (const auto& e : sk->entities)
        if (const auto* c = std::get_if<sketch::Circle>(&e)) {
          circles++;
          rad = c->radius;
        }
      IM_CHECK_EQ(circles, 1);
      IM_CHECK_LE(std::abs(rad - 2.0), 1e-3);
    }

    s_App->sketchEdit.autoConstrain = true;
    ctx->KeyPress(ImGuiKey_Escape); // Circle → Select
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Inline dimension editor accepts full expressions: "3*4" stores
  // valueExpr and drives value 12; a later plain number clears the expr.
  t = IM_REGISTER_TEST(engine, "dims", "inline_editor_expression");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5f, -0.5f}, {1.5f, -0.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    s_App->sketchEdit.editingDim = 0; // open the inline editor
    ctx->Yield(2);

    // The editor's InputText is auto-focused; replace the value. (Backspace
    // clearing — Ctrl+A select-all is unreliable on a just-refocused field.)
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("3*4");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 12.0), 1e-6);
    IM_CHECK(sk->constraints[0].valueExpr == "3*4");
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    // The dimension drove the geometry: the line is now 12 long.
    {
      const auto& ln = std::get<sketch::Line>(sk->entities[0]);
      IM_CHECK_LE(std::abs(glm::length(ln.b - ln.a) - 12.0f), 1e-2f);
    }

    // A plain number reverts to a literal (expr cleared).
    s_App->sketchEdit.editingDim = 0;
    ctx->Yield(2);
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("5");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 5.0), 1e-6);
    IM_CHECK(sk->constraints[0].valueExpr.empty());

    ctx->KeyPress(ImGuiKey_Escape); // clear selection state / leave edit
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // Dimension-on-create (Phase 3.4): typing a number while a draw tool is
  // live opens a value field that applies a driving dimension to the entity
  // just drawn — circle radius, line length, rectangle width then height
  // (expressions accepted). Esc dismisses without touching the tool's state;
  // digits with no fresh entity (Select tool) do nothing.
  t = IM_REGISTER_TEST(engine, "dims", "dimension_on_create");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };

    // Circle + typed "2" = a radius dimension driving r to 2.
    ctx->KeyPress(ImGuiKey_C);
    clickUV({0, 1.5f});
    clickUV({0.6f, 1.5f});
    ctx->KeyChars("2");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::Radius);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 2.0), 1e-6);
    IM_CHECK_LE(
        std::abs(std::get<sketch::Circle>(sk->entities[0]).radius - 2.0), 1e-3);

    // Line: Esc dismisses the field without a dimension and keeps the
    // tool's chained pending point; a second try applies the length.
    ctx->KeyPress(ImGuiKey_L);
    clickUV({-1, -1});
    clickUV({1, -1});
    IM_CHECK(s_App->sketchEdit.hasPendingPoint); // the line tool chains
    ctx->KeyChars("9");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);
    IM_CHECK(s_App->sketchEdit.hasPendingPoint);    // staged state untouched
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1); // no dimension created
    ctx->KeyChars("4");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);
    IM_CHECK(sk->constraints[1].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_LE(std::abs(sk->constraints[1].value - 4.0), 1e-6);
    {
      const auto& ln = std::get<sketch::Line>(sk->entities[1]);
      IM_CHECK_LE(std::abs(glm::length(ln.b - ln.a) - 4.0f), 1e-3f);
    }

    // Rectangle: first number = width, typing again = height (as an
    // expression, which stores valueExpr).
    ctx->KeyPress(ImGuiKey_G);
    clickUV({-1, -0.5f});
    clickUV({0.5f, 0.5f});
    ctx->KeyChars("4");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const uint32_t l0 = sketch::EntityId(sk->entities[2]); // bottom side
    const uint32_t l1 = sketch::EntityId(sk->entities[3]); // right side
    IM_CHECK(sk->constraints.back().kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints.back().a.entity, l0);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 4.0), 1e-6);
    ctx->KeyChars("1");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_End); // collapse any select-all, cursor to end
    ctx->KeyChars("+1");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints.back().kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(sk->constraints.back().a.entity, l1);
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 2.0), 1e-6);
    IM_CHECK(sk->constraints.back().valueExpr == "1+1");
    {
      const auto& bottom = std::get<sketch::Line>(sk->entities[2]);
      const auto& right = std::get<sketch::Line>(sk->entities[3]);
      IM_CHECK_LE(std::abs(glm::length(bottom.b - bottom.a) - 4.0f), 1e-3f);
      IM_CHECK_LE(std::abs(glm::length(right.b - right.a) - 2.0f), 1e-3f);
    }

    // Digits while NOT in a draw tool do nothing.
    const size_t nCons = sk->constraints.size();
    ctx->KeyPress(ImGuiKey_Escape); // Rectangle → Select
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    ctx->KeyChars("7");
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.dimOnCreateOpen);
    IM_CHECK_EQ(sk->constraints.size(), nCons);

    s_App->sketchEdit.autoConstrain = true; // restore the default
    ctx->KeyPress(ImGuiKey_Escape);         // leave sketch edit
    ctx->Yield();
  };

  // Center rectangle (Phase 3.6): Shift+G arms it; centre + corner clicks
  // build 4 lines plus a centre point anchored by a symmetric-about-point
  // constraint — dimensioning the width grows the rectangle symmetrically.
  t = IM_REGISTER_TEST(engine, "sketch", "center_rectangle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;

    ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_G);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::RectangleCenter);

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickUV({0.5f, 0.25f}); // centre
    clickUV({1.5f, 0.75f}); // corner
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->entities.size(), (size_t)5); // 4 sides + centre point
    // Clicks quantize to pixels (~0.005 UV here): loose bound on placement,
    // then exact bounds against the ACTUAL centre below.
    const glm::vec2 c0 = std::get<sketch::Point>(sk->entities[4]).p;
    IM_CHECK_LE(glm::length(c0 - glm::vec2(0.5f, 0.25f)), 0.05f);
    // The diagonal corners sit symmetric about the centre.
    const auto& bottom0 = std::get<sketch::Line>(sk->entities[0]);
    const auto& top0 = std::get<sketch::Line>(sk->entities[2]);
    IM_CHECK_LE(glm::length(0.5f * (bottom0.a + top0.a) - c0), 1e-3f);
    const uint32_t pointId = sketch::EntityId(sk->entities[4]);
    const uint32_t l0 = sketch::EntityId(sk->entities[0]);
    bool anchored = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Symmetric && c.c.entity == pointId)
        anchored = true;
    IM_CHECK(anchored);

    // Pin the centre, dimension the width: symmetric growth about it.
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.selected = {{pointId, 0}};
    IM_CHECK(
        ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Fixed, 0.0));
    ctx->Yield();
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& bottom = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK_LE(std::abs(glm::length(bottom.b - bottom.a) - 3.0f), 1e-3f);
    IM_CHECK_LE(std::abs(0.5f * (bottom.a.x + bottom.b.x) - c0.x), 1e-3f);
    const auto& centerAfter = std::get<sketch::Point>(sk->entities[4]);
    IM_CHECK_LE(glm::length(centerAfter.p - c0), 1e-4f); // centre stayed put

    s_App->sketchEdit.autoConstrain = true;
    ctx->KeyPress(ImGuiKey_Escape); // clear selection
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Tab-bar folders: foldered parts collapse to one flyout button (assembly
  // import drops 100+ parts in one), the flyout switches tabs, and the
  // folder persists through project save/load.
  t = IM_REGISTER_TEST(engine, "sketch",
                       "picking_a_point_does_not_light_its_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    size_t index = 0;
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App, &index);
    IM_CHECK(sk != nullptr);
    // A rectangle: its corners are shared by two lines, which is where the
    // report came from.
    const auto ids = sk->AddRectangle({0.0f, 0.0f}, {3.0f, 2.0f});
    IM_CHECK(ids.size() >= 4);
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    ctx->Yield(2);

    // params.y is the primitive's shape: 0 = line, 1 = dot. Counting them
    // separately is the report in one assertion — "it highlights both the
    // point and one of the lines next to it".
    const glm::vec4 sel(1.00f, 0.62f, 0.20f, 1.0f);
    const auto litCounts = [&]() {
      const std::vector<PrimVertex> prims = ui::BuildActiveSketchPrimitives(
          *s_App, s_App->sketchEdit.activeSketchId);
      int lines = 0, dots = 0;
      for (const PrimVertex& v : prims) {
        if (glm::length(v.color - sel) > 0.01f)
          continue;
        (v.params.y > 0.5f ? dots : lines)++;
      }
      return std::pair<int, int>(lines, dots);
    };
    IM_CHECK_EQ(litCounts().first, 0); // nothing selected yet
    IM_CHECK_EQ(litCounts().second, 0);

    // The LINE BODY selected: its shaft is lit.
    s_App->sketchEdit.selected = {{ids[0], sketch::kWholeEntity}};
    ctx->Yield();
    IM_CHECK(litCounts().first > 0);

    // One of that line's ENDPOINTS instead: the dot lights, the line does
    // NOT. That second half is the bug.
    s_App->sketchEdit.selected = {{ids[0], 1}};
    ctx->Yield();
    IM_CHECK(litCounts().second > 0);  // the point itself is highlighted
    IM_CHECK_EQ(litCounts().first, 0); // and no line is
    s_App->sketchEdit.selected.clear();
  };

  // Sketch reference images (v49) — the first test to touch them at all.
  //
  // The feature shipped its model, its RPCs and its rendering, and its own
  // plan says phase 3's GUI "couldn't be driven end-to-end headlessly, so
  // the panel/tool need a quick interactive check". That check was never
  // done, so every widget below has been unverified since 2026-07-21.
  //
  // Insert is NOT clicked here: the first thing it does is open a native file
  // dialog, which the test engine cannot drive — the same reason Import has
  // no clicked coverage (docs/ui/gui-coverage.md). The action behind it is
  // called directly and everything AFTER it is clicked, which is where the
  // panel actually lives.
  t = IM_REGISTER_TEST(engine, "sketch", "reference_image_panel_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);

    const std::string fixture = std::string(DESIGNER_SOURCE_DIR) +
                                "/tests/fixtures/images/checker16x12.png";
    const uint32_t id = ui::AddReferenceImageToSketch(*s_App, fixture);
    IM_CHECK(id != 0);
    ctx->Yield(2);

    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->images.size(), (size_t)1);
    // The decoded pixel size, which is what every placement is scaled from.
    IM_CHECK_EQ(sk->images[0].widthPx, 16);
    IM_CHECK_EQ(sk->images[0].heightPx, 12);

    // Open the image session the way the toolbar's Insert does on success.
    s_App->sketchEdit.selectedImageId = id;
    ui::SetSketchTool(*s_App, ui::SketchTool::Image);
    ctx->Yield(4);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/Opacity"));

    // Opacity is a PERSISTED field, so an edit belongs on the undo stack —
    // and it was a live cosmetic write with no undo entry at all until
    // 2026-08-25. Drag it across several frames: if the fix pushed one
    // command per frame instead of one per drag, a single undo would leave
    // the value part-way and this fails.
    sk = ui::ResolveActiveSketch(*s_App);
    const double before = sk->images[0].opacity;
    ctx->ItemInputValue("//###ToolWindow/Opacity", 0.9f);
    ctx->Yield(3);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(std::abs(sk->images[0].opacity - before) > 0.01);
    IM_CHECK(s_App->commands->CanUndo());

    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK_EQ(sk->images.size(), (size_t)1); // undid the EDIT, not the image
    IM_CHECK(std::abs(sk->images[0].opacity - before) < 1e-6);

    // Calibrate arms its own tool and, from 2026-08-25, draws where the
    // picks landed — before that the tool asked for two points and showed
    // nothing, so a mis-picked point silently rescaled the whole image.
    ctx->ItemClick("//###ToolWindow/Calibrate...");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::CalibrateImage);
    IM_CHECK_EQ(s_App->sketchEdit.calibImageId, id);

    std::vector<PrimVertex> prims;
    ui::AppendCalibrationMarkers(*s_App, prims);
    IM_CHECK(prims.empty()); // nothing picked yet, so nothing drawn

    s_App->sketchEdit.calibP1 = glm::vec2(0.0f, 0.0f);
    s_App->sketchEdit.calibP2 = glm::vec2(8.0f, 0.0f);
    s_App->sketchEdit.calibStage = 2;
    prims.clear();
    ui::AppendCalibrationMarkers(*s_App, prims);
    IM_CHECK(!prims.empty()); // two dots and the span between them

    // Apply: the typed distance becomes the image scale. 8 plane-units of
    // picked span declared to be 40 mm is a 5x rescale, and unitsPerPixel is
    // what every traced dimension then inherits.
    const double upp = sk->images[0].unitsPerPixel;
    s_App->sketchEdit.calibDistance = 40.0f;
    ctx->ItemClick("//###ToolWindow/Apply##calib");
    ctx->Yield(4);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    IM_CHECK(std::abs(sk->images[0].unitsPerPixel - upp) > 1e-9);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
  };
}
