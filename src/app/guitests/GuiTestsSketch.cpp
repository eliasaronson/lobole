// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/guitests/GuiTestsCommon.h"
#include "core/sketch/SketchOrientation.h"
#include "render/UnderlayRenderer.h"
#include "ui/BackgroundJobs.h"
#include "ui/ImportActions.h"
#include "ui/ImportJob.h"
#include "ui/ProjectActions.h"
#include "ui/SceneOverlays.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchOverlay.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include <fstream>

// Sketch GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
std::string SourceFixture(const std::string& rel) {
  return std::string(LOBOLE_SOURCE_DIR) + "/tests/fixtures/" + rel;
}

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

  // Reference geometry is sized from the part, not from a literal. The base
  // planes were a fixed 8 mm half-extent and the sketch background a fixed
  // 50 mm one, so on a metre-scale part — a speaker panel, a dock — the plane
  // you were being asked to sketch on was a speck at the origin and the grid
  // under the sketch was a postage stamp. The cell count stays put while the
  // grid grows: 1 mm cells over a metre would be a solid grey sheet.
  t = IM_REGISTER_TEST(engine, "sketch", "references_are_sized_by_the_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Empty document: the old literals ARE the floor, unchanged.
    IM_CHECK_EQ(ui::ScenePlaneHalfExtent(*s_App), ui::kDefaultPlaneHalfExtent);

    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Panel", glm::vec3(0), 210, 16, 1122));
    ctx->Yield(2);
    // The plane reaches just past the geometry it is a reference for — the
    // part runs 0..1122 from the origin the planes are centred on, so it has
    // to span at least that, and not much more.
    const float half = ui::ScenePlaneHalfExtent(*s_App);
    ctx->LogInfo("plane half-extent %.0f mm for a 1122 mm part", half);
    IM_CHECK_GT(half, 1122.0f);
    IM_CHECK_LT(half, 2.0f * 1122.0f); // a reference, not a horizon

    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr);
    const ui::SketchGridMetrics grid = ui::SketchGridFor(*s_App, *sk);
    ctx->LogInfo("grid: half %.0f mm, %.0f mm cells", grid.half, grid.step);
    IM_CHECK_GE(grid.half, half); // covers the part it is drawn on
    IM_CHECK_GT(grid.step, 1.0f); // not a thousand 1 mm cells
    // ~50 cells out from the origin, less wherever the 1-2-5 rounding takes
    // the step up a rung (worst case 2.5x, so never fewer than 20).
    const float cells = grid.half / grid.step;
    ctx->LogInfo("%.0f cells from the origin out", cells);
    IM_CHECK(cells >= 20.0f && cells <= 51.0f);
    // The step is a number you can count in (1-2-5 decade).
    float mantissa = grid.step;
    while (mantissa >= 10.0f)
      mantissa /= 10.0f;
    IM_CHECK(mantissa == 1.0f || mantissa == 2.0f || mantissa == 5.0f);

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
  };

  // A flyout group is a tool button plus a DISCLOSURE CARET, and the caret
  // must stay a hint. It was ICON_FA_CARET_DOWN in a button of its own, so it
  // was drawn at the ICON size inside a full tool frame — on the sketch bar
  // the three carets read as three more tools, twice the arrow commercial CAD
  // draws. Sized off the frame, it cannot come back as a glyph:
  // a full icon button is at least as wide as it is tall.
  t = IM_REGISTER_TEST(engine, "sketch", "a_flyout_caret_stays_a_hint");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);

    ImGuiTestItemInfo caret =
        ctx->ItemInfo("//##DockHost/**/circlegrp/###more");
    IM_CHECK(caret.ID != 0);
    const float frame = ImGui::GetFrameHeight();
    ctx->LogInfo("caret is %.0f x %.0f px, frame height %.0f",
                 caret.RectFull.GetWidth(), caret.RectFull.GetHeight(), frame);
    IM_CHECK_LT(caret.RectFull.GetWidth(), frame * 0.6f);
    IM_CHECK_EQ(caret.RectFull.GetHeight(), frame); // still easy to hit

    // And it still does its job: the popup switches which variant the group
    // holds, and picking one arms that tool.
    IM_CHECK(s_App->sketchEdit.tool != ui::SketchTool::Ellipse);
    ctx->ItemClick("//##DockHost/**/circlegrp/###more");
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK(popup != nullptr);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, (std::string("//") + popup->Name).c_str());
    IM_CHECK_EQ(items.size(), 2); // Circle, Ellipse
    ctx->ItemClick(items[1]->ID);
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Ellipse);

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
  };

  // The Text tool (docs/sketching/README.md#text): a click places a label
  // whose cap height came from the sketch's own size, not a literal, and the
  // Tool window then edits THAT label — one undo record per committed edit.
  t = IM_REGISTER_TEST(engine, "sketch", "text_tool_places_and_edits_a_label");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 1);
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    ToolbarClick(ctx, "###tb_sk_text");
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Text);
    IM_CHECK(ctx->GetWindowByRef("//Text###ToolWindow") != nullptr);
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr);
    const float expected = 5.0f * ui::SketchGridFor(*s_App, *sk).step;
    IM_CHECK_EQ(s_App->sketchEdit.textHeight, expected);

    const glm::vec2 c = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0));
    ctx->MouseMoveToPos(ImVec2(c.x + 40.0f, c.y + 20.0f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->entities.size(), (size_t)1);
    const auto* placed = std::get_if<sketch::Text>(&sk->entities[0]);
    IM_CHECK(placed != nullptr);
    IM_CHECK_EQ(static_cast<float>(placed->height), expected);
    IM_CHECK_EQ(s_App->sketchEdit.textEditId, placed->id);

    // Edit the placed label's string and font from the window.
    ctx->ItemInputValue("//Text###ToolWindow/###tx_text", "b8");
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    const auto* edited = std::get_if<sketch::Text>(&sk->entities[0]);
    IM_CHECK(edited != nullptr);
    IM_CHECK(edited->text == "b8");
    IM_CHECK(sk->GetError().empty());

    // A character the font lacks is refused in the window, not committed.
    ctx->ItemInputValue("//Text###ToolWindow/###tx_text", "\xe4\xb8\xad");
    ctx->Yield(2);
    IM_CHECK(!s_App->sketchEdit.textError.empty());
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(std::get<sketch::Text>(sk->entities[0]).text == "b8");

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
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

    // A tap in EMPTY SPACE continues to the axis step. -U,-V is the corner
    // away from the session window, which sits top-right of the viewport —
    // and the clearance is asserted, not assumed: this used to tap the other
    // corner with a comment claiming it was clear, and narrowing the camera's
    // FOV (which magnifies the sketch at the same distance) slid that point
    // under the window, where the tap went to the window and the phase never
    // advanced.
    const glm::vec2 t2 =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    const glm::vec4 win = s_App->toolWindowRect;
    ctx->LogInfo("tap at %.0f,%.0f; session window %.0f,%.0f..%.0f,%.0f", t2.x,
                 t2.y, win.x, win.y, win.z, win.w);
    IM_CHECK(t2.x < win.x || t2.x > win.z || t2.y < win.y || t2.y > win.w);
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

  // An ellipse by direct manipulation
  // (docs/sketching/README.md#ellipses-and-splines): its major-axis end is a
  // handle that resizes it in place,
  // its body drags it whole, and the Dimension tool on its body places the
  // major radius as a distance from the centre to that end.
  t = IM_REGISTER_TEST(engine, "drag", "ellipse_handles");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t el = sk->AddEntity(sketch::Ellipse{{0, 0}, 2.0, 1.0, 0.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f * LensScale()));
    ctx->Yield(2);
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    const auto ellipse = [&]() {
      auto* s = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      return std::get<sketch::Ellipse>(s->entities[s->IndexOfEntity(el)]);
    };
    const auto dragUV = [&](glm::vec2 from, glm::vec2 to) {
      auto* s = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const glm::vec2 a = ui::SketchUVToScreen(*s_App, s->plane, from);
      const glm::vec2 b = ui::SketchUVToScreen(*s_App, s->plane, to);
      ctx->MouseMoveToPos(ImVec2(a.x, a.y));
      ctx->MouseDown(ImGuiMouseButton_Left);
      ctx->MouseMoveToPos(ImVec2(0.5f * (a.x + b.x), 0.5f * (a.y + b.y)));
      ctx->MouseMoveToPos(ImVec2(b.x, b.y));
      ctx->MouseUp(ImGuiMouseButton_Left);
      ctx->Yield();
    };

    // The major end (2, 0) pulled to (3, 0): longer, same centre and minor.
    dragUV({2, 0}, {3, 0});
    IM_CHECK_LE(std::abs(ellipse().rMajor - 3.0), 2e-2);
    IM_CHECK_LE(std::abs(ellipse().rMinor - 1.0), 2e-2);
    IM_CHECK_LE(glm::length(ellipse().center), 2e-2);
    IM_CHECK(s_App->commands->CanUndo());

    // The body (a point on the curve, away from the handles) moves it whole.
    const glm::vec2 onCurve(3.0f * std::cos(0.8f), std::sin(0.8f));
    dragUV(onCurve, onCurve + glm::vec2(0.0f, 1.0f));
    IM_CHECK_LE(std::abs(ellipse().center.y - 1.0), 2e-2);
    IM_CHECK_LE(std::abs(ellipse().center.x), 2e-2);
    IM_CHECK_LE(std::abs(ellipse().rMajor - 3.0), 2e-2);
    IM_CHECK_LE(std::abs(ellipse().rMinor - 1.0), 2e-2);

    // Dimension tool on the body: the major radius, centre to major end.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    auto clickUV = [&](glm::vec2 uv) {
      auto* s = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const glm::vec2 p = ui::SketchUVToScreen(*s_App, s->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickUV(onCurve + glm::vec2(0.0f, 1.0f));
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({1.5f, 2.8f}); // place the label above the span
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    const sketch::Constraint& dim = sk->constraints[0];
    IM_CHECK(dim.kind == sketch::ConstraintKind::Distance);
    IM_CHECK_EQ(dim.a.entity, el);
    IM_CHECK_EQ((int)dim.a.which, 0);
    IM_CHECK_EQ((int)dim.b.which, (int)sketch::kEllipseMajorPos);
    IM_CHECK_LE(std::abs(dim.value - 3.0), 2e-2);
    ctx->KeyPress(ImGuiKey_Escape); // the inline editor
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Escape); // the tool
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Escape); // the sketch
    ctx->Yield();
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
    // One cylinder's volume, not two: r=1, and the default distance is the
    // extruded REGION's width (2), not the whole sketch's -> 2 pi.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 2.0 * M_PI) <
             1e-2);
  };

  // A sketch opens the way it is read: +U to the right and, on Top, +V up --
  // on the others the world's up stays up. Looking straight down, world up
  // is the view axis, and the camera's fallback turned Top's sketch 180
  // degrees: +X pointing left and +Y down.
  t = IM_REGISTER_TEST(engine, "sketch", "a_base_plane_opens_the_right_way_up");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    for (int plane = 0; plane < 3; plane++) {
      ctx->MenuClick("File/New");
      ctx->Yield();
      EnterSketchOnPlane(ctx, plane);
      const SketchPlane p = ui::DefaultPlane(plane);
      const glm::vec2 o = ui::SketchUVToScreen(*s_App, p, {0, 0});
      const glm::vec2 u = ui::SketchUVToScreen(*s_App, p, {1, 0});
      const glm::vec2 v = ui::SketchUVToScreen(*s_App, p, {0, 1});
      ctx->LogInfo("plane %d: +U (%.1f, %.1f) +V (%.1f, %.1f)", plane,
                   u.x - o.x, u.y - o.y, v.x - o.x, v.y - o.y);
      const bool top =
          std::abs(glm::dot(glm::vec3(p.GetNormal()), conv::kWorldUpF)) > 0.9f;
      if (top) {
        IM_CHECK_GT(u.x, o.x + 1.0f); // +U right
        IM_CHECK_LT(v.y, o.y - 1.0f); // +V up (screen y grows downwards)
      } else {
        const ImVec2 up =
            WorldToWindow(glm::vec3(p.GetOrigin()) + conv::kWorldUpF);
        IM_CHECK_LT(up.y, WorldToWindow(glm::vec3(p.GetOrigin())).y - 1.0f);
      }
      ToolbarClick(ctx, "###tb_sk_finish");
      ctx->Yield();
    }
  };

  // A circle drawn inside an outline, picked by clicking inside it: its own
  // region, extruded as a disc (a boss). The pick used to count the loops
  // around the click and refuse an even count as "inside a hole", so the
  // circle could not be picked at all.
  t = IM_REGISTER_TEST(engine, "region", "a_circle_inside_an_outline_picks");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    auto sk =
        std::make_unique<SketchFeature>("Profiles", SketchPlane(PlaneType::XY));
    sk->AddRectangle({-4.0, -4.0}, {4.0, 4.0});
    const uint32_t circle = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                       ExtrudeMode::NewBody));
    Doc()->Regenerate();
    const uint64_t extId = Doc()->GetFeature(1)->GetId();
    ui::OpenFeatureEditor(*s_App, extId);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.5f, 30.0f));
    ctx->Yield();
    ctx->ItemClick("//Extrude###ToolWindow/###slot7");
    IM_CHECK(s_App->slotPick.Active());
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(0.4f, 0.3f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)1);
    IM_CHECK(ext->regions[0] == std::vector<uint32_t>{circle});
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 2.0 * M_PI) <
             1e-2); // the disc, r 1 h 2 -- not the square around it
    ctx->KeyPress(ImGuiKey_Escape); // disarm the slot
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active());
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

  // A linked pattern is ONE profile family: clicking one hole of a bolt
  // circle picks every hole; clicking again removes them all. The five-hole
  // flange this comes from took five hand picks — one per identical hole.
  t = IM_REGISTER_TEST(engine, "region", "pattern_family_picks_together");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    auto sk =
        std::make_unique<SketchFeature>("Profiles", SketchPlane(PlaneType::XY));
    const uint32_t src = sk->AddEntity(sketch::Circle{{2, 0}, 0.8});
    sk->AddCircularPattern({src}, {0, 0}, 2.0 * M_PI / 3.0, /*count=*/3,
                           /*link=*/true);
    const uint32_t lone = sk->AddEntity(sketch::Circle{{6, 0}, 0.8});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                       ExtrudeMode::NewBody));
    Doc()->Regenerate();
    const uint64_t extId = Doc()->GetFeature(1)->GetId();
    ui::OpenFeatureEditor(*s_App, extId);

    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(1.5f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(1.5f, 0.5f, 16.0f));
    ctx->Yield();

    ctx->ItemClick("//Extrude###ToolWindow/###slot7");
    IM_CHECK(s_App->slotPick.Active());

    // One click inside the SOURCE hole: the whole pattern family, and only
    // it — the free-standing circle is not part of the pattern.
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(2.0f, 0.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)3);
    for (const std::vector<uint32_t>& r : ext->regions)
      IM_CHECK(r != std::vector<uint32_t>{lone});
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() -
                      3.0 * M_PI * 0.64 * 2.0) < 1e-2);

    // Ctrl narrows to ONE region: with the family picked, Ctrl+clicking the
    // source hole drops just it — the "all of the pattern but this hole"
    // intent the family toggle alone would make unreachable.
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)2);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() -
                      2.0 * M_PI * 0.64 * 2.0) < 1e-2);
    // And Ctrl+click adds a single region back the same way.
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)3);

    // A plain second click on a picked hole removes the family: back to all
    // regions, the lone circle included again.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
    IM_CHECK(ext != nullptr);
    IM_CHECK_EQ(ext->regions.size(), (size_t)0);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() -
                      4.0 * M_PI * 0.64 * 2.0) < 1e-2);

    ctx->KeyPress(ImGuiKey_Escape);
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
    // Through LensScale, so it pins the MAGNIFICATION: 8 units of standoff
    // frames a different amount of sketch through a different lens, and
    // narrowing the default FOV to 30 pushed these clicks off their targets.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f * LensScale()));
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

  // Placement-driven kind inference (Phase 3.7, the mainstream CAD
  // behavior): a point pair enters a placement stage; the cursor zone picks
  // the kind — above/below the pair = horizontal (DistanceX), sideways =
  // vertical (DistanceY), pulled perpendicular to the span = aligned
  // (Distance) — and the click also places the label. Esc walks the staged
  // ladder.
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
    // The two above already fix it, so it is added DRIVEN: no editor opens
    // (docs/sketching/README.md#gui).
    const glm::vec2 d = glm::normalize(pb - pa);
    const glm::vec2 n(-d.y, d.x);
    const glm::vec2 mid = 0.5f * (pa + pb);
    clickUV(pa);
    clickUV(pb);
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV(mid + n * 1.2f);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)3);
    IM_CHECK(sk->constraints[2].driven);
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    IM_CHECK(sk->constraints[2].kind == sketch::ConstraintKind::Distance);
    IM_CHECK_LE(
        std::abs(*sketch::MeasureDimension(*sk, 2) - glm::length(pb - pa)),
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

  // Negative dimension values flip the dimension's direction (the usual CAD
  // convention): signed kinds (DistanceX/Y, Angle) move the free
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

  // A value the sketch cannot take is refused, not stored beside geometry
  // that ignores it (docs/sketching/README.md#solving): an arc with both
  // ends fixed 2 apart cannot have a radius under 1. The label still reads
  // the old radius, the prompt bar says why, and undo has nothing new.
  t = IM_REGISTER_TEST(engine, "dims", "edit_refuses_flip");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;
    // Ends (1,0) and (-1,0), centre (0,-0.6): a 118 degree arc.
    const double r0 = std::sqrt(1.36);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t arc = sk->AddEntity(sketch::Arc{
        {0, -0.6}, r0, std::atan2(0.6, 1.0), std::atan2(0.6, -1.0)});
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
    addCon({{arc, 1}}, CK::Fixed, 0.0);
    addCon({{arc, 2}}, CK::Fixed, 0.0);
    addCon({{arc, 0}}, CK::Radius, r0);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)3);
    const size_t depth = s_App->commands->UndoDepth();
    s_App->sketchEdit.actionMsg.clear();

    s_App->sketchEdit.editingDim = 2;
    ctx->Yield(2);
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("0.9");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    IM_CHECK_LE(std::abs(sk->constraints[2].value - r0), 1e-9); // the label
    IM_CHECK(sk->constraints[2].valueExpr.empty());
    {
      const auto& a =
          std::get<sketch::Arc>(sk->entities[sk->IndexOfEntity(arc)]);
      IM_CHECK_LE(std::abs(a.radius - r0), 1e-9); // the geometry
    }
    IM_CHECK(s_App->sketchEdit.actionMsg.rfind("Not changed: ", 0) == 0);
    IM_CHECK(s_App->sketchEdit.actionMsg.find("cannot be met") !=
             std::string::npos);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), depth); // nothing to undo

    s_App->sketchEdit.autoConstrain = true;
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // ...and completes the document's variables, like every other expression
  // field (u14). The inline editor is a separate widget from ui::ExprInput —
  // its own InputText in a floating window — so it is separately wired and
  // separately gated.
  t = IM_REGISTER_TEST(engine, "dims", "inline_editor_completes_a_variable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(Doc()->SetParameter("wall_d", "10"));
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
    s_App->sketchEdit.editingDim = 0;
    ctx->Yield(2);

    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("wal");
    ctx->Yield(2);
    IM_CHECK(ImGui::FindWindowByName("##expr_complete") != nullptr);
    ctx->KeyPress(ImGuiKey_Tab);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].valueExpr == "wall_d");
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 10.0), 1e-6);
    ctx->KeyPress(ImGuiKey_Escape);
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
  // Dimensioning geometry the sketch already fixes reads it: the Dimension
  // tool adds a DRIVEN (reference) dimension, says why, paints nothing red
  // and opens no editor -- a double-click says how to edit it instead
  // (docs/sketching/README.md#gui).
  t = IM_REGISTER_TEST(engine, "dims", "over_constraining_becomes_driven");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    using K = sketch::ConstraintKind;
    // A fully defined 3 x 2 rectangle: corner fixed, sides H/V, two lengths.
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t bot = sk->AddEntity(sketch::Line{{-1.5, -1}, {1.5, -1}});
    const uint32_t rgt = sk->AddEntity(sketch::Line{{1.5, -1}, {1.5, 1}});
    const uint32_t top = sk->AddEntity(sketch::Line{{1.5, 1}, {-1.5, 1}});
    const uint32_t lft = sk->AddEntity(sketch::Line{{-1.5, 1}, {-1.5, -1}});
    sk->constraints.push_back({K::Coincident, {bot, 1}, {rgt, 0}});
    sk->constraints.push_back({K::Coincident, {rgt, 1}, {top, 0}});
    sk->constraints.push_back({K::Coincident, {top, 1}, {lft, 0}});
    sk->constraints.push_back({K::Coincident, {lft, 1}, {bot, 0}});
    sk->constraints.push_back({K::Horizontal, {bot, 0}, {}});
    sk->constraints.push_back({K::Horizontal, {top, 0}, {}});
    sk->constraints.push_back({K::Vertical, {rgt, 0}, {}});
    sk->constraints.push_back({K::Vertical, {lft, 0}, {}});
    sk->constraints.push_back({K::Fixed, {bot, 0}, {}});
    sk->constraints.push_back({K::Distance, {bot, 0}, {bot, 1}, 3.0});
    sk->constraints.push_back({K::Distance, {rgt, 0}, {rgt, 1}, 2.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    s_App->sketchEdit.autoConstrain = false;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f * LensScale()));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 0);
    IM_CHECK(s_App->sketchEdit.conflictingConstraints.empty());

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    // The top side's length: the sketch already fixes it.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Dimension);
    clickUV({0.5f, 1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.7f, 1.8f}); // place it above
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)12);
    const sketch::Constraint& made = sk->constraints.back();
    IM_CHECK(made.kind == K::Distance);
    IM_CHECK(made.driven);
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1); // no editor
    IM_CHECK(s_App->sketchEdit.actionMsg.find(
                 "added as a driven (reference) dimension") !=
             std::string::npos);
    IM_CHECK(s_App->sketchEdit.conflictingConstraints.empty()); // no red
    IM_CHECK_EQ(s_App->sketchEdit.dof, 0);
    IM_CHECK_LE(std::abs(*sketch::MeasureDimension(*sk, 11) - 3.0), 1e-6);

    // Back to Select, then double-click its label: no editor, and why.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.actionMsg.clear();
    const glm::vec2 labelUV =
        glm::vec2(0.0f, 1.0f) + glm::vec2(made.labelOffset);
    const glm::vec2 labelScr = ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
    ctx->MouseMoveToPos(ImVec2(labelScr.x, labelScr.y));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dimHot, 11);
    ctx->MouseDoubleClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    IM_CHECK(s_App->sketchEdit.actionMsg.find(
                 "A driven dimension measures the sketch") !=
             std::string::npos);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // The usual intent behind dimensioning what is already fixed: keep the
  // NEW dimension. The status names the one in the way, "Use this one
  // instead" swaps them in one undo step, and the label menu deletes one
  // (docs/sketching/README.md#gui).
  t = IM_REGISTER_TEST(engine, "dims", "replace_over_defining_dimension");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    using K = sketch::ConstraintKind;
    // A fully defined 3 x 2 rectangle: corner fixed, sides H/V, two lengths.
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t bot = sk->AddEntity(sketch::Line{{-1.5, -1}, {1.5, -1}});
    const uint32_t rgt = sk->AddEntity(sketch::Line{{1.5, -1}, {1.5, 1}});
    const uint32_t top = sk->AddEntity(sketch::Line{{1.5, 1}, {-1.5, 1}});
    const uint32_t lft = sk->AddEntity(sketch::Line{{-1.5, 1}, {-1.5, -1}});
    sk->constraints.push_back({K::Coincident, {bot, 1}, {rgt, 0}});
    sk->constraints.push_back({K::Coincident, {rgt, 1}, {top, 0}});
    sk->constraints.push_back({K::Coincident, {top, 1}, {lft, 0}});
    sk->constraints.push_back({K::Coincident, {lft, 1}, {bot, 0}});
    sk->constraints.push_back({K::Horizontal, {bot, 0}, {}});
    sk->constraints.push_back({K::Horizontal, {top, 0}, {}});
    sk->constraints.push_back({K::Vertical, {rgt, 0}, {}});
    sk->constraints.push_back({K::Vertical, {lft, 0}, {}});
    sk->constraints.push_back({K::Fixed, {bot, 0}, {}});
    sk->constraints.push_back({K::Distance, {bot, 0}, {bot, 1}, 3.0});
    sk->constraints.push_back({K::Distance, {rgt, 0}, {rgt, 1}, 2.0});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    s_App->sketchEdit.autoConstrain = false;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f * LensScale()));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 0);

    auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    // The top side's length, which the bottom's dimension already fixes.
    ctx->KeyPress(ImGuiKey_D);
    ctx->Yield();
    clickUV({0.5f, 1});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.7f, 1.8f});
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)12);
    IM_CHECK(sk->constraints.back().driven);
    IM_CHECK_STR_EQ(s_App->sketchEdit.actionMsg.c_str(),
                    "That is already fixed by the 3 distance, so it was added "
                    "as a driven (reference) dimension");
    const auto* offer = ui::LiveReplaceOffer(*s_App);
    IM_CHECK(offer != nullptr);
    IM_CHECK_EQ(offer->dim, 11);
    IM_CHECK_EQ(offer->old.size(), (size_t)1);
    IM_CHECK_EQ(offer->old[0], (size_t)9);

    // The button under the status swaps them.
    const size_t undoDepth = s_App->commands->UndoDepth();
    ctx->ItemClick("//##dimreplace/Use this one instead");
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)11);
    const sketch::Constraint& kept = sk->constraints.back();
    IM_CHECK(kept.kind == K::Distance);
    IM_CHECK(!kept.driven);
    IM_CHECK_EQ(kept.a.entity, top);
    IM_CHECK_LE(std::abs(kept.value - 3.0), 1e-9);
    for (const sketch::Constraint& c : sk->constraints)
      IM_CHECK(!(c.kind == K::Distance && c.a.entity == bot)); // gone
    IM_CHECK_EQ(s_App->sketchEdit.dof, 0); // still fully defined
    IM_CHECK(s_App->sketchEdit.conflictingConstraints.empty());
    IM_CHECK(s_App->sketchEdit.actionMsg.empty());
    IM_CHECK(ui::LiveReplaceOffer(*s_App) == nullptr);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoDepth + 1); // one step

    // One undo brings both back: the old driving, the new driven.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)12);
    IM_CHECK(!sk->constraints[9].driven);
    IM_CHECK(sk->constraints[11].driven);

    // The label menu's Delete removes a dimension: the driven one here.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    const glm::vec2 labelUV =
        glm::vec2(0.0f, 1.0f) + glm::vec2(sk->constraints[11].labelOffset);
    const glm::vec2 labelScr = ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
    ctx->MouseMoveToPos(ImVec2(labelScr.x, labelScr.y));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dimHot, 11);
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->Yield();
    IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
    ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
    IM_CHECK_SILENT(popup != nullptr);
    ctx->ItemClick((std::string("//") + popup->Name + "/Delete").c_str());
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)11);
    IM_CHECK(!sk->constraints[9].driven);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 0);
    s_App->sketchEdit.autoConstrain = true; // restore the default
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

  // A relation the sketch already states is refused, nothing added, and the
  // status line says why.
  t = IM_REGISTER_TEST(engine, "constraints", "repeat_is_refused");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 1.5}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();
    const size_t undoDepth = s_App->commands->UndoDepth();
    s_App->sketchEdit.selected = {{l0, 0}};
    IM_CHECK(!ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK_EQ(s_App->commands->UndoDepth(), undoDepth); // nothing recorded
    IM_CHECK(
        s_App->sketchEdit.actionMsg.rfind(
            "Not added: the sketch already fixes that (constraint 0)", 0) == 0);
    IM_CHECK(s_App->sketchEdit.conflictingConstraints.empty());
  };

  // The Driven toggle on a dimension's label menu (right-click): made
  // driven, the dimension stops constraining; made driving again on an
  // under-defined sketch, it takes what the geometry measures and the DOF
  // drops. Each is one undo step.
  t = IM_REGISTER_TEST(engine, "dims", "toggle_driven");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5, -0.5}, {1.5, -0.5}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f * LensScale()));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 4);

    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 3);

    // Right-click the label, pick Driven from its menu.
    const auto toggleFromMenu = [&]() {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const auto& line = std::get<sketch::Line>(sk->entities[0]);
      const glm::vec2 mid = glm::vec2((line.a + line.b) * 0.5);
      const glm::vec2 labelUV = mid + glm::vec2(sk->constraints[0].labelOffset);
      const glm::vec2 scr = ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
      ctx->MouseMoveToPos(ImVec2(scr.x, scr.y));
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->sketchEdit.dimHot, 0);
      ctx->MouseClick(ImGuiMouseButton_Right);
      ctx->Yield();
      IM_CHECK_SILENT(ctx->UiContext->OpenPopupStack.Size > 0);
      ImGuiWindow* popup = ctx->UiContext->OpenPopupStack.back().Window;
      IM_CHECK_SILENT(popup != nullptr);
      ctx->ItemClick((std::string("//") + popup->Name + "/Driven").c_str());
      ctx->Yield(2);
    };
    toggleFromMenu();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints[0].driven);
    IM_CHECK_EQ(s_App->sketchEdit.dof, 4); // it constrains nothing now

    // The line is stretched to 4 while the dimension only measures it.
    before = DocumentIO::FeatureToJson(*sk);
    std::get<sketch::Line>(sk->entities[0]).b = {2.5, -0.5};
    after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield(2);

    toggleFromMenu();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!sk->constraints[0].driven);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 4.0), 1e-9); // its measure
    const auto& line = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK_LE(std::abs(line.b.x - 2.5), 1e-6); // nothing moved
    IM_CHECK_EQ(s_App->sketchEdit.dof, 3);

    // One undo step back to driven.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints[0].driven);

    // A dimension an expression drives (and so a configuration, through
    // its variable) is not made driven: that would drop its input.
    before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l1 = sk->AddEntity(sketch::Line{{-1.5, 1.0}, {0.5, 1.0}});
    after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{l1, 0}, {l1, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       2.0, "2"));
    ctx->Yield();
    IM_CHECK(!ui::SetConstraintDriven(*s_App, 1, true));
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!sk->constraints[1].driven);
    IM_CHECK(sk->constraints[1].valueExpr == "2");
    IM_CHECK(s_App->sketchEdit.actionMsg.find("clear the expression first") !=
             std::string::npos);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
  };

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
    const glm::dvec2 c0 = std::get<sketch::Point>(sk->entities[4]).p;
    IM_CHECK_LE(glm::length(c0 - glm::dvec2(0.5, 0.25)), 0.05f);
    // The diagonal corners sit symmetric about the centre.
    const auto& bottom0 = std::get<sketch::Line>(sk->entities[0]);
    const auto& top0 = std::get<sketch::Line>(sk->entities[2]);
    IM_CHECK_LE(glm::length(0.5 * (bottom0.a + top0.a) - c0), 1e-3f);
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

  // Import formats (docs/interchange/README.md#up-axis): a SolidWorks STEP
  // arrives standing up, the viewport says why, and the Import's editor lays
  // it back down as one undoable modify. File > Import opens a native dialog
  // the engine cannot drive, so the action behind it is called with the
  // fixture and everything after it is clicked.
  t = IM_REGISTER_TEST(engine, "import",
                       "solidworks_part_stands_up_and_lies_down");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();
    std::string error;
    IM_CHECK(ui::StartImport(
        *s_App, SourceFixture("import/box-solidworks.step"), &error));
    IM_CHECK(WaitFor(ctx, "the import to land",
                     [] { return !ui::ImportRunning(*s_App); }));
    IM_CHECK(s_App->importError.empty());
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    auto* imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp != nullptr);
    IM_CHECK(imp->UpApplied().applied == interchange::UpAxis::PosY);
    const auto height = [] {
      glm::dvec3 mn, mx;
      Doc()->GetBodies().back().shape.GetBoundingBox(mn, mx);
      return mx.z - mn.z;
    };
    IM_CHECK(std::abs(height() - 30.0) < 1e-6);
    // The pill: the note is set by the import and its clock started by the
    // frame that drew it.
    ctx->Yield(2);
    IM_CHECK(s_App->importNote.find("Stood up") != std::string::npos);
    IM_CHECK(s_App->importNote.find("SolidWorks") != std::string::npos);
    IM_CHECK(s_App->importNoteUntil > 0.0);

    // The editor: the axis, the turn and why.
    ui::OpenFeatureEditor(*s_App, imp->GetId());
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###Up axis/none");
    ctx->Yield(3);
    imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp->upAxis == interchange::UpAxis::None);
    IM_CHECK(std::abs(height() - 20.0) < 1e-6);
    IM_CHECK(s_App->commands->CanUndo());
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp->upAxis == interchange::UpAxis::Auto);
    IM_CHECK(std::abs(height() - 30.0) < 1e-6);
  };

  // A downloaded STL is recognised by default
  // (docs/interchange/README.md#recognising-a-faceted-mesh): its holes are
  // cylinders, the viewport's note says what was found, and the Import's
  // editor turns it back into facets as one undoable modify.
  t = IM_REGISTER_TEST(engine, "import", "a_downloaded_stl_is_recognised");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    const size_t before = Doc()->FeatureCount();
    std::string error;
    IM_CHECK(ui::StartImport(*s_App, SourceFixture("mesh/nema17_mount.stl"),
                             &error));
    IM_CHECK(WaitFor(ctx, "the import to land",
                     [] { return !ui::ImportRunning(*s_App); }));
    IM_CHECK(s_App->importError.empty());
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    auto* imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp != nullptr);
    IM_CHECK(imp->representation == mesh::Representation::Recognize);
    IM_CHECK_EQ(imp->RecognizedBodies().size(), size_t{1});
    IM_CHECK_EQ(
        imp->RecognizedBodies()[0].recognition.Count(mesh::Kind::Cylinder),
        size_t{9});
    const auto faces = [] {
      return Doc()->GetBodies().back().shape.FaceCount();
    };
    const int recognised = faces();
    ctx->Yield(2);
    IM_CHECK(s_App->importNote.find("9 cylinders") != std::string::npos);

    ui::OpenFeatureEditor(*s_App, imp->GetId());
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###Mesh/facets");
    ctx->Yield(3);
    imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp->representation == mesh::Representation::Facets);
    IM_CHECK(faces() > 10 * recognised);
    IM_CHECK(s_App->commands->CanUndo());
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    imp = dynamic_cast<ImportFeature*>(Doc()->GetFeature(before));
    IM_CHECK(imp->representation == mesh::Representation::Recognize);
    IM_CHECK_EQ(faces(), recognised);
  };

  // A mesh is the import job too (docs/ui/long-operations.md#import): it
  // used to be read and sewn on the frame, a 100k-triangle scan holding the
  // window for minutes with nothing to show or stop it. The modal says what
  // it costs; Cancel takes the modal down at once and imports nothing.
  t = IM_REGISTER_TEST(engine, "import", "a_mesh_import_runs_in_background");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CADKernel& k = CADKernel::Instance();
    const auto stl =
        std::filesystem::temp_directory_path() / "lobole_gui_mesh_import.stl";
    IM_CHECK(
        k.SaveSTL(stl.string(), k.CreateSphere({0, 0, 0}, 10.0), 0.2, 0.5));
    const size_t before = Doc()->FeatureCount();
    ui::HoldImportWorkerForTesting(true);
    struct Release {
      ~Release() { ui::HoldImportWorkerForTesting(false); }
    } release;
    std::string error;
    IM_CHECK(ui::RequestImport(*s_App, stl.string(), &error));
    IM_CHECK(ui::ImportRunning(*s_App)); // a job, not a call
    IM_CHECK_EQ(Doc()->FeatureCount(), before);
    IM_CHECK(WaitFor(ctx, "the import modal", [&] {
      return ctx->ItemExists("//###import_modal/Cancel###import_cancel");
    }));
    IM_CHECK(ui::ImportStatusLine(*s_App).find("triangles") !=
             std::string::npos);
    ui::HoldImportWorkerForTesting(false);
    IM_CHECK(WaitFor(ctx, "the import to land",
                     [] { return !ui::ImportRunning(*s_App); }));
    IM_CHECK(s_App->importError.empty());
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    IM_CHECK(Doc()->GetBodies().back().shape.SolidCount() == 1);

    // Cancelled from the modal: it goes at once, and nothing is added.
    ui::HoldImportWorkerForTesting(true);
    IM_CHECK(ui::RequestImport(*s_App, stl.string(), &error));
    IM_CHECK(WaitFor(ctx, "the second modal", [&] {
      return ctx->ItemExists("//###import_modal/Cancel###import_cancel");
    }));
    ctx->ItemClick("//###import_modal/Cancel###import_cancel");
    IM_CHECK(WaitFor(ctx, "the modal to go", [&] {
      return !ctx->ItemExists("//###import_modal/Cancel###import_cancel");
    }));
    IM_CHECK(WaitFor(ctx, "the cancelled job to end",
                     [] { return !ui::ImportRunning(*s_App); }));
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    std::filesystem::remove(stl);
  };

  // A mesh past the triangle limit (interchange::MeshTriangleLimit) to be
  // sewn as facets asks before it starts, saying what it will cost: Cancel
  // imports nothing, Import anyway starts the job. One to be recognised
  // sews nothing, so it starts without asking
  // (docs/interchange/README.md#what-a-mesh-costs).
  t = IM_REGISTER_TEST(engine, "import", "a_large_mesh_asks_first");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A binary STL just past the limit, written as bytes: the ask reads only
    // its header, and the job is cancelled before it reads the rest.
    const uint64_t n = interchange::MeshTriangleLimit() + 1000;
    const auto stl =
        std::filesystem::temp_directory_path() / "lobole_gui_large_mesh.stl";
    {
      std::ofstream out(stl, std::ios::binary);
      const char header[80] = {};
      out.write(header, 80);
      const auto count = static_cast<uint32_t>(n);
      out.write(reinterpret_cast<const char*>(&count), 4);
      const std::vector<char> records(50 * n, 0);
      out.write(records.data(), static_cast<std::streamsize>(records.size()));
    }
    const size_t before = Doc()->FeatureCount();
    std::string error;
    const std::string was = s_App->settings.meshRepresentation;
    struct Restore {
      std::string was;
      ~Restore() { s_App->settings.meshRepresentation = was; }
    } restore{was};
    {
      // Recognised: no ask, the job.
      ui::HoldImportWorkerForTesting(true);
      IM_CHECK(ui::RequestImport(*s_App, stl.string(), &error));
      IM_CHECK(ui::ImportRunning(*s_App));
      IM_CHECK(s_App->importAsk.path.empty());
      ui::CancelBackgroundJobs(*s_App, "import");
      ui::HoldImportWorkerForTesting(false);
      IM_CHECK(WaitFor(ctx, "the recognised job to end",
                       [] { return !ui::ImportRunning(*s_App); }));
      IM_CHECK_EQ(Doc()->FeatureCount(), before);
    }
    s_App->settings.meshRepresentation = "facets";
    IM_CHECK(ui::RequestImport(*s_App, stl.string(), &error));
    IM_CHECK(!ui::ImportRunning(*s_App)); // asked, not started
    IM_CHECK_EQ(s_App->importAsk.triangles, n);
    IM_CHECK(WaitFor(ctx, "the ask", [&] {
      return ctx->ItemExists("//###import_ask/Cancel###import_ask_cancel");
    }));
    ctx->ItemClick("//###import_ask/Cancel###import_ask_cancel");
    ctx->Yield(2);
    IM_CHECK(s_App->importAsk.path.empty());
    IM_CHECK(!ui::ImportRunning(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), before);

    ui::HoldImportWorkerForTesting(true);
    struct Release {
      ~Release() { ui::HoldImportWorkerForTesting(false); }
    } release;
    IM_CHECK(ui::RequestImport(*s_App, stl.string(), &error));
    IM_CHECK(WaitFor(ctx, "the ask again", [&] {
      return ctx->ItemExists("//###import_ask/Import anyway###import_ask_go");
    }));
    ctx->ItemClick("//###import_ask/Import anyway###import_ask_go");
    IM_CHECK(WaitFor(ctx, "the job to start",
                     [] { return ui::ImportRunning(*s_App); }));
    ui::CancelBackgroundJobs(*s_App, "import");
    IM_CHECK(WaitFor(ctx, "the cancelled job to end",
                     [] { return !ui::ImportRunning(*s_App); }));
    IM_CHECK_EQ(Doc()->FeatureCount(), before);
    std::filesystem::remove(stl);
  };

  // A hover pick on a 2M-triangle mesh body fits a frame
  // (docs/rendering/README.md#picking): past FaceSelector::kPickBvhTriangles
  // the body hands over its tree, and the pick walks it. A RESPONSIVENESS
  // check of main-thread CPU (docs/ui/testing.md#tests-that-time-things).
  t = IM_REGISTER_TEST(engine, "import",
                       "a_hover_pick_on_a_2m_mesh_fits_a_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A UV sphere of radius 40 and about 2M triangles, written as a binary
    // STL: the size a phone scan arrives at.
    const auto stl =
        std::filesystem::temp_directory_path() /
        ("lobole_gui_2m_mesh_" + std::to_string(getpid()) + ".stl");
    const int rings = 710, segs = 2 * rings;
    const double r = 40.0;
    {
      std::vector<glm::vec3> pts;
      for (int i = 0; i <= rings; i++)
        for (int j = 0; j < segs; j++) {
          const double th = M_PI * i / rings, ph = 2 * M_PI * j / segs;
          pts.emplace_back(r * std::sin(th) * std::cos(ph),
                           r * std::sin(th) * std::sin(ph), r * std::cos(th));
        }
      std::vector<std::array<int, 3>> tris;
      for (int i = 0; i < rings; i++)
        for (int j = 0; j < segs; j++) {
          const int a = i * segs + j, b = i * segs + (j + 1) % segs,
                    c = (i + 1) * segs + j, e = (i + 1) * segs + (j + 1) % segs;
          if (i > 0)
            tris.push_back({a, c, b});
          if (i < rings - 1)
            tris.push_back({b, c, e});
        }
      std::ofstream out(stl, std::ios::binary);
      const char header[80] = {};
      out.write(header, 80);
      const auto count = static_cast<uint32_t>(tris.size());
      out.write(reinterpret_cast<const char*>(&count), 4);
      for (const auto& tr : tris) {
        const float n[3] = {0, 0, 0};
        out.write(reinterpret_cast<const char*>(n), 12);
        for (int k : tr)
          out.write(reinterpret_cast<const char*>(&pts[k]), 12);
        const char attr[2] = {};
        out.write(attr, 2);
      }
    }
    const size_t depth = s_App->commands->UndoDepth();
    std::string error;
    IM_CHECK(ui::ImportAsFeature(
        *s_App, stl.string(), nullptr, &error, units::Length::Millimetre, false,
        interchange::UpAxis::None, 0.0, mesh::Representation::Mesh));
    IM_CHECK(!Doc()->GetBodies().empty() && Doc()->GetBodies().back().IsMesh());
    const size_t triangles = Doc()->GetBodies().back().mesh->triangles.size();
    ctx->LogInfo("a mesh body of %zu triangles", triangles);
    IM_CHECK(triangles >= FaceSelector::kPickBvhTriangles);
    ctx->Yield(3); // synced: the tree is built with the tessellation
    // The pick itself, straight down onto the sphere's top.
    const Ray down{glm::vec3(0.0f, 0.0f, 500.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    double worstPick = 0.0;
    for (int i = 0; i < 20; i++) {
      const double p0 = MainThreadCpuMs();
      const RayHit hit = s_App->faceSelector->PickFace(down);
      worstPick = std::max(worstPick, MainThreadCpuMs() - p0);
      IM_CHECK(hit.hit);
      IM_CHECK(std::abs(hit.point.z - float(r)) < 0.1f);
    }
    // ...and the frames the cursor hovers it in.
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(0.0f, 0.0f, float(r))));
    double worstFrame = 0.0;
    for (int i = 0; i < 10; i++) {
      const double f0 = MainThreadCpuMs();
      ctx->Yield();
      worstFrame = std::max(worstFrame, MainThreadCpuMs() - f0);
    }
    ctx->LogInfo("worst pick %.2f ms, worst hovered frame %.1f ms", worstPick,
                 worstFrame);
    IM_CHECK(worstPick < 1000.0 / 60.0);
    while (s_App->commands->UndoDepth() > depth)
      s_App->commands->Undo(*Doc());
    ctx->Yield(2);
    std::filesystem::remove(stl);
  };

  // A DXF into the sketch being edited: one undo step of baked entities,
  // which close and extrude like any others.
  t = IM_REGISTER_TEST(engine, "import", "dxf_into_open_sketch_extrudes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr);
    const size_t had = sk->entities.size();
    std::string error;
    IM_CHECK(
        ui::StartImport(*s_App, SourceFixture("import/unitless.dxf"), &error));
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK_EQ(sk->entities.size(), had + 4);
    IM_CHECK(s_App->sketchEdit.actionMsg.find("Imported 4 entities") !=
             std::string::npos);
    // One undo step takes the whole drawing back out, and redo puts it in.
    ui::GlobalUndo(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(ui::ResolveActiveSketch(*s_App)->entities.size(), had);
    ui::GlobalRedo(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(ui::ResolveActiveSketch(*s_App)->entities.size(), had + 4);

    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    ToolbarClick(ctx, "###tb_extrude");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // 40 x 25 mm (the file declares no unit: mm).
    glm::dvec3 mn, mx;
    Doc()->GetBodies()[0].shape.GetBoundingBox(mn, mx);
    IM_CHECK(std::abs((mx.x - mn.x) - 40.0) < 1e-6);
    IM_CHECK(std::abs((mx.y - mn.y) - 25.0) < 1e-6);

    // A refused file changes nothing and says why in the viewport.
    const size_t features = Doc()->FeatureCount();
    IM_CHECK(!ui::StartImport(
        *s_App, SourceFixture("import/refuse-mixed-z.dxf"), &error));
    IM_CHECK(error.find("mixed elevations") != std::string::npos);
    IM_CHECK_EQ(Doc()->FeatureCount(), features);
    IM_CHECK(s_App->importNoteWarn);
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
  // no clicked coverage (docs/ui/testing.md#what-the-gui-tests-cover). The
  // action behind it is called directly and everything AFTER it is clicked,
  // which is where the panel actually lives.
  t = IM_REGISTER_TEST(engine, "sketch", "reference_image_panel_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);

    const std::string fixture = std::string(LOBOLE_SOURCE_DIR) +
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

  // Show always (v166): an underlay that stays in the viewport once its
  // sketch is closed. Before it, closing the sketch took every underlay off
  // screen, so a model could never be looked at over the plan it was traced
  // from. The checkbox is one click and one undo step; the renderer is what
  // is read, because the flag alone says nothing about what is drawn.
  t = IM_REGISTER_TEST(engine, "sketch", "reference_image_show_always");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);
    const std::string fixture = std::string(LOBOLE_SOURCE_DIR) +
                                "/tests/fixtures/images/checker16x12.png";
    const uint32_t id = ui::AddReferenceImageToSketch(*s_App, fixture);
    IM_CHECK(id != 0);
    s_App->sketchEdit.selectedImageId = id;
    ui::SetSketchTool(*s_App, ui::SketchTool::Image);
    ctx->Yield(4);
    IM_CHECK(s_App->underlayRenderer != nullptr);
    IM_CHECK_EQ(s_App->underlayRenderer->DrawCount(), (size_t)1);

    ctx->ItemClick("//###ToolWindow/Show always");
    ctx->Yield(2);
    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    IM_CHECK(sk->images[0].showAlways);
    // A persisted field, so one undo step.
    ui::GlobalUndo(*s_App);
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    IM_CHECK(!sk->images[0].showAlways);
    ctx->ItemClick("//###ToolWindow/Show always");
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images[0].showAlways);
    const uint64_t sketchId = sk->GetId();

    ctx->ItemClick("//###ToolWindow/Close###image_close");
    ToolbarClick(ctx, "###tb_sk_finish");
    ctx->Yield(4);
    IM_CHECK(!s_App->sketchEdit.IsEditing());
    IM_CHECK_EQ(s_App->underlayRenderer->DrawCount(), (size_t)1);

    // Without the flag, a closed sketch's underlay is off screen again.
    auto* closed =
        dynamic_cast<SketchFeature*>(Doc()->FindFeatureById(sketchId));
    IM_CHECK(closed != nullptr && closed->images.size() == 1);
    closed->images[0].showAlways = false;
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->underlayRenderer->DrawCount(), (size_t)0);
  };

  // Moving a reference image: with its session open, grab anywhere ON the
  // image and drag. Before 2026-08-25 an image sat wherever Insert centred
  // it — the only thing that could shift one was Calibrate, as a side effect
  // of a rescale.
  t = IM_REGISTER_TEST(engine, "sketch", "reference_image_drag_moves_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield(2);
    const std::string fixture = std::string(LOBOLE_SOURCE_DIR) +
                                "/tests/fixtures/images/checker16x12.png";
    const uint32_t id = ui::AddReferenceImageToSketch(*s_App, fixture);
    IM_CHECK(id != 0);
    s_App->sketchEdit.selectedImageId = id;
    ui::SetSketchTool(*s_App, ui::SketchTool::Image);
    ctx->Yield(4);

    SketchFeature* sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    const glm::dvec2 origin0 = sk->images[0].origin;
    // Insert centres the image on the plane origin, so UV (0,0) is on it.
    const glm::vec2 from =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 0.0f));
    const glm::vec2 to =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(3.0f, 2.0f));
    ctx->MouseMoveToPos(ImVec2(from.x, from.y));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.imageDragActive);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    const glm::dvec2 moved = sk->images[0].origin - origin0;
    // The image followed the mouse by the dragged UV span.
    IM_CHECK(std::abs(moved.x - 3.0f) < 0.2f);
    IM_CHECK(std::abs(moved.y - 2.0f) < 0.2f);
    // ONE undo entry for the whole drag, restoring the grab-time position.
    IM_CHECK(s_App->commands->CanUndo());
    ui::GlobalUndo(*s_App);
    ctx->Yield(2);
    sk = ui::ResolveActiveSketch(*s_App);
    IM_CHECK(sk != nullptr && sk->images.size() == 1);
    IM_CHECK(glm::length(sk->images[0].origin - origin0) < 1e-4f);
  };

  // Editing a sketch after it has been extruded: the solid built FROM the
  // sketch is the thing standing in front of the sketch, so opening the
  // sketch rolls the model back past it (SolidWorks and Fusion both do this
  // on Edit Sketch). The marker is a VIEW of the history, not an edit —
  // hence the clean-document check, which is the part that regressed when
  // this was first tried through the persisted rollback bar.
  t = IM_REGISTER_TEST(engine, "sketch",
                       "editing_a_sketch_rolls_past_its_solid");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    auto sk =
        std::make_unique<SketchFeature>("Profile", SketchPlane(PlaneType::XY));
    sk->AddRectangle({0, 0}, {4, 2});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                       ExtrudeMode::NewBody));
    Doc()->Regenerate();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);

    // Pretend the project was just saved, so the dirty check below means
    // something: opening a sketch must not modify anything.
    s_App->lastSavedVersion = Doc()->SaveStamp();
    IM_CHECK(!ui::ProjectDirty(*s_App));

    s_App->sketchEdit = ui::SketchEditState{};
    s_App->sketchEdit.activeSketchId = skId;
    ctx->Yield(2); // SyncSketchViewRollback runs in the frame loop

    IM_CHECK_EQ(Doc()->ViewRollback(), 1);
    IM_CHECK_EQ(Doc()->Rollback(), -1); // the user's own bar is untouched
    IM_CHECK(Doc()->GetBodies().empty());
    IM_CHECK(!ui::ProjectDirty(*s_App)); // a view, not an edit

    // Leaving puts it back, and leaves the project clean either way.
    s_App->sketchEdit = ui::SketchEditState{};
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->ViewRollback(), -1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(!ui::ProjectDirty(*s_App));
  };

  // A sketch nothing is built from has nothing in the way, so nothing is
  // hidden. Rolling back to the sketch's own position instead would blank
  // every unrelated feature after it — worse than the problem being fixed.
  t = IM_REGISTER_TEST(engine, "sketch",
                       "editing_an_unused_sketch_hides_nothing");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0), 2.0, 2.0, 2.0));
    auto sk =
        std::make_unique<SketchFeature>("Loose", SketchPlane(PlaneType::XY));
    sk->AddRectangle({0, 0}, {4, 2});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(1)->GetId();
    Doc()->AddFeature(std::make_unique<BoxFeature>("After", glm::vec3(9, 0, 0),
                                                   1.0, 1.0, 1.0));
    Doc()->Regenerate();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);

    s_App->sketchEdit = ui::SketchEditState{};
    s_App->sketchEdit.activeSketchId = skId;
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->ViewRollback(), -1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2); // both boxes stay
  };

  // The slice: what helps when the sketch is INSIDE existing material, where
  // there is no consuming feature to roll back past. It follows the camera —
  // the half kept is the one away from the viewer — so orbiting to the far
  // side of the plane does not blank the model instead of revealing it.
  t = IM_REGISTER_TEST(engine, "sketch", "slice_cuts_toward_the_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0), 4.0, 4.0, 4.0));
    auto sk =
        std::make_unique<SketchFeature>("Inside", SketchPlane(PlaneType::XY));
    sk->AddRectangle({0, 0}, {1, 1});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(1)->GetId();
    Doc()->Regenerate();

    s_App->sketchEdit = ui::SketchEditState{};
    s_App->sketchEdit.activeSketchId = skId;
    s_App->sketchSlice = false;
    ctx->Yield(2);
    IM_CHECK(!ui::SketchSlicePlane(*s_App).has_value());
    IM_CHECK(!ui::SectionActive(*s_App));

    // XY plane, normal +Z. Camera above, looking down: the half ABOVE the
    // sketch is what goes. The normal names the half being REMOVED (the
    // fragment stage discards where dot(n, p) - w > 0), so with the camera
    // above it points up — asserting the opposite is exactly the mistake
    // this test was written with, and it took a screenshot to catch.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 20.0f));
    s_App->sketchSlice = true;
    ctx->Yield(2);
    auto plane = ui::SketchSlicePlane(*s_App);
    IM_CHECK(plane.has_value());
    IM_CHECK(ui::SectionActive(*s_App));
    IM_CHECK(plane->z > 0.0f); // points at the near half, toward the camera
    IM_CHECK(glm::dot(glm::vec3(*plane), s_App->camera->GetForward()) < 0.0f);
    // ...and it rides in a clip slot, which is what gives it cap fills.
    IM_CHECK(glm::length(glm::vec3(ui::SectionPlanes(*s_App)[0])) > 0.5f);

    // Orbit under the part: the kept half flips, rather than the slice
    // cutting away everything the camera can now see.
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, -20.0f));
    ctx->Yield(2);
    plane = ui::SketchSlicePlane(*s_App);
    IM_CHECK(plane.has_value());
    IM_CHECK(plane->z < 0.0f);
    IM_CHECK(glm::dot(glm::vec3(*plane), s_App->camera->GetForward()) < 0.0f);

    // Leaving the sketch takes the slice with it (the toggle itself is
    // sticky for the session, the cut is not).
    s_App->sketchEdit = ui::SketchEditState{};
    ctx->Yield(2);
    IM_CHECK(s_App->sketchSlice);
    IM_CHECK(!ui::SketchSlicePlane(*s_App).has_value());
  };
  // docs/sketching/README.md#the-first-dimension: a sketch drawn by eye,
  // given its first length in the inline editor, keeps its drawn shape at
  // the new size -- every corner is the old one scaled about the anchor --
  // and ONE undo puts the drawn size back.
  t = IM_REGISTER_TEST(engine, "dims", "first_dimension_rescales");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const std::vector<glm::dvec2> drawn = {
        {-1.1, -0.9}, {1.3, -0.8}, {0.2, 1.1}};
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < 3; i++)
      ids.push_back(sk->AddEntity(sketch::Line{drawn[i], drawn[(i + 1) % 3]}));
    for (size_t i = 0; i < 3; i++)
      sk->constraints.push_back({sketch::ConstraintKind::Coincident,
                                 {ids[i], 1},
                                 {ids[(i + 1) % 3], 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    // The Dimension tool's commit: the measured value, then the editor.
    const double measured = glm::length(drawn[1] - drawn[0]);
    s_App->sketchEdit.selected = {{ids[0], 0}, {ids[0], 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       measured));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.editingDim = static_cast<int>(sk->constraints.size()) - 1;
    ctx->Yield(2);
    for (int i = 0; i < 12; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("30");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const double f = 30.0 / measured;
    const glm::dvec2 anchor(0.5 * (-1.1 + 1.3), 0.5 * (-0.9 + 1.1));
    for (size_t i = 0; i < 3; i++) {
      const auto& l = std::get<sketch::Line>(sk->entities[i]);
      IM_CHECK_LE(glm::length(l.a - (anchor + (drawn[i] - anchor) * f)), 1e-6);
    }
    IM_CHECK_LE(std::abs(sk->constraints.back().value - 30.0), 1e-9);
    // One undo: the drawn size again.
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    for (size_t i = 0; i < 3; i++) {
      const auto& l = std::get<sketch::Line>(sk->entities[i]);
      IM_CHECK_LE(glm::length(l.a - drawn[i]), 1e-9);
    }
    IM_CHECK_LE(std::abs(sk->constraints.back().value - measured), 1e-9);
  };

  // docs/ui/numeric-input.md#a-new-variable: `w = 40` in the inline editor
  // makes the project variable and drives the dimension by it, one undo
  // step. A taken name and a bad one are refused with the editor open and
  // nothing changed; an edit the sketch refuses makes no variable.
  t = IM_REGISTER_TEST(engine, "dims", "name_equals_value_makes_a_variable");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const size_t varsBefore = s_App->projectParameters.size();
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t l0 = sk->AddEntity(sketch::Line{{-1.5, -0.5}, {1.5, -0.5}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Distance,
                                       3.0));
    ctx->Yield();
    const auto type = [&](const char* text) {
      s_App->sketchEdit.editingDim = 0;
      ctx->Yield(2);
      for (int i = 0; i < 16; i++)
        ctx->KeyPress(ImGuiKey_Backspace);
      ctx->KeyChars(text);
      ctx->KeyPress(ImGuiKey_Enter);
      ctx->Yield(2);
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    };
    const auto length = [&] {
      const auto& l = std::get<sketch::Line>(sk->entities[0]);
      return glm::length(l.b - l.a);
    };

    type("gap_w = 40");
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, -1);
    const Parameter* w = ui::FindProjectParameter(*s_App, "gap_w");
    IM_CHECK(w != nullptr);
    IM_CHECK_STR_EQ(w->expression.c_str(), "40");
    IM_CHECK_STR_EQ(sk->constraints[0].valueExpr.c_str(), "gap_w");
    IM_CHECK_LE(std::abs(length() - 40.0), 1e-6);
    // One undo takes the variable AND the dimension's binding back.
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(ui::FindProjectParameter(*s_App, "gap_w") == nullptr);
    IM_CHECK(sk->constraints[0].valueExpr.empty());
    IM_CHECK_LE(std::abs(length() - 3.0), 1e-6);

    // A taken name: refused, naming its value, the editor kept open.
    IM_CHECK(ui::SetProjectVariable(*s_App, "taken_w", "7", ""));
    ctx->Yield();
    const nlohmann::json sketchBefore = DocumentIO::FeatureToJson(*sk);
    type("taken_w = 9");
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, 0);
    IM_CHECK_STR_EQ(
        ui::FindProjectParameter(*s_App, "taken_w")->expression.c_str(), "7");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(DocumentIO::FeatureToJson(*sk) == sketchBefore);
    // Not a name: refused the same way.
    type("2w = 9");
    IM_CHECK_EQ(s_App->sketchEdit.editingDim, 0);
    IM_CHECK(ui::FindProjectParameter(*s_App, "2w") == nullptr);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->projectParameters.size(), varsBefore + 1);
    std::vector<std::string> refs;
    IM_CHECK(ui::RemoveProjectVariable(*s_App, "taken_w", &refs));
    ctx->Yield();
  };

  // The dimension's own checks run first: an edit the sketch refuses (a
  // radius smaller than half the chord its fixed ends span) makes no
  // variable.
  t = IM_REGISTER_TEST(engine, "dims", "refused_name_equals_value_makes_none");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    const double r0 = std::sqrt(1.36);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t arc = sk->AddEntity(sketch::Arc{
        {0, -0.6}, r0, std::atan2(0.6, 1.0), std::atan2(0.6, -1.0)});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    using CK = sketch::ConstraintKind;
    for (uint8_t end : {1, 2}) {
      s_App->sketchEdit.selected = {{arc, end}};
      IM_CHECK(ui::ApplySketchConstraint(*s_App, CK::Fixed, 0.0));
    }
    s_App->sketchEdit.selected = {{arc, 0}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, CK::Radius, r0));
    ctx->Yield();
    const size_t vars = s_App->projectParameters.size();
    s_App->sketchEdit.editingDim = 2;
    ctx->Yield(2);
    for (int i = 0; i < 16; i++)
      ctx->KeyPress(ImGuiKey_Backspace);
    ctx->KeyChars("too_small = 0.9");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.actionMsg.rfind("Not changed: ", 0) == 0);
    IM_CHECK_EQ(s_App->projectParameters.size(), vars);
    IM_CHECK(ui::FindProjectParameter(*s_App, "too_small") == nullptr);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints[2].valueExpr.empty());
  };

  // docs/sketching/README.md#open-ends: the ends of a profile that does not
  // close are marked while the sketch is edited, and Extrude refused says
  // how many and where.
  t = IM_REGISTER_TEST(engine, "region", "open_ends_are_marked");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{-1, -1}, {1, -1}});
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});
    sk->AddEntity(sketch::Line{{1, 1}, {-1, 1}});
    sk->AddEntity(sketch::Line{{-1, 1}, {-1, -0.99}}); // almost closed
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchEdit.openEnds.size(), (size_t)2);
    IM_CHECK_EQ(s_App->sketchEdit.openGaps.size(), (size_t)1);
    const size_t features = Doc()->FeatureCount();
    IM_CHECK(ui::RunCommand(*s_App, "sketch.extrude"));
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.IsEditing()); // refused: still sketching
    IM_CHECK_EQ(Doc()->FeatureCount(), features);
    IM_CHECK(s_App->sketchEdit.actionMsg.find("2 open ends") !=
             std::string::npos);
    // Closing it takes the markers away.
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    before = DocumentIO::FeatureToJson(*sk);
    std::get<sketch::Line>(sk->entities[3]).b = {-1, -1};
    after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield(2);
    IM_CHECK(s_App->sketchEdit.openEnds.empty());
  };

  // docs/sketching/README.md#trim: pressed on empty space and dragged, the
  // Trim tool previews every span the path crosses; the release trims them
  // all, and one undo puts all of them back.
  t = IM_REGISTER_TEST(engine, "trim", "drag_trims_every_crossed_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    for (float x : {-1.0f, 0.0f, 1.0f})
      sk->AddEntity(sketch::Line{{x, -1.5f}, {x, 1.5f}});
    sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const nlohmann::json untrimmed = DocumentIO::FeatureToJson(*sk);
    s_App->sketchEdit.tool = ui::SketchTool::Trim;
    const auto at = [&](glm::vec2 uv) {
      const glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      return ImVec2(p.x, p.y);
    };
    ctx->MouseMoveToPos(at({-1.6f, 0.8f}));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    for (float x = -1.4f; x <= 1.61f; x += 0.2f) {
      ctx->MouseMoveToPos(at({x, 0.8f}));
      ctx->Yield();
    }
    IM_CHECK(s_App->sketchEdit.trimPathActive);
    IM_CHECK_EQ(s_App->sketchEdit.trimPathPreview.size(), (size_t)3);
    IM_CHECK_EQ(sk->entities.size(), (size_t)4); // the preview is a copy
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(!s_App->sketchEdit.trimPathActive);
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Trim); // still trimming
    int verticals = 0;
    for (const auto& e : sk->entities)
      if (const auto* l = std::get_if<sketch::Line>(&e))
        if (std::abs(l->a.x - l->b.x) < 1e-6) {
          verticals++;
          IM_CHECK_LE(std::max(l->a.y, l->b.y), 1e-6); // upper half gone
        }
    IM_CHECK_EQ(verticals, 3);
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(DocumentIO::FeatureToJson(*sk) == untrimmed);
  };

  // docs/sketching/README.md#arc-length: the Dimension tool on an arc reads
  // where its label goes -- outside the arc along its sweep is the arc
  // length, inside it the radius.
  t = IM_REGISTER_TEST(engine, "dims", "arc_length_by_placement");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.autoConstrain = false;
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t arc =
        sk->AddEntity(sketch::Arc{{0, 0}, 1.0, 0.25 * M_PI, 0.75 * M_PI});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.tool = ui::SketchTool::Dimension;
    const auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    clickUV({0.0f, 1.0f}); // the arc's body
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.1f, 1.6f}); // outside, along the sweep
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    IM_CHECK(sk->constraints[0].kind == sketch::ConstraintKind::ArcLength);
    IM_CHECK_EQ(sk->constraints[0].a.entity, arc);
    IM_CHECK_LE(std::abs(sk->constraints[0].value - 0.5 * M_PI), 1e-6);
    ctx->KeyPress(ImGuiKey_Escape); // close the inline editor
    ctx->Yield(2);
    s_App->sketchEdit.editingDim = -1;
    clickUV({0.0f, 1.0f});
    IM_CHECK(s_App->sketchEdit.dimPlacing);
    clickUV({0.1f, 0.4f}); // inside the circle: the radius
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk->constraints.back().kind == sketch::ConstraintKind::Radius);
  };

  // docs/sketching/README.md#curvature-continuity: the G2 command on a
  // spline and the arc it runs out of replaces their tangent with G2.
  t = IM_REGISTER_TEST(engine, "constraints", "curvature_replaces_tangent");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    const uint32_t arc =
        sk->AddEntity(sketch::Arc{{0, 0}, 1.0, 0.0, 0.5 * M_PI});
    const uint32_t sp = sk->AddEntity(
        sketch::BSpline{{{0, 1}, {-0.6, 1.1}, {-1.1, 0.8}, {-1.4, 0.4}}});
    using CK = sketch::ConstraintKind;
    sk->constraints.push_back({CK::Coincident, {arc, 2}, {sp, 0}});
    sk->constraints.push_back({CK::Tangent, {sp, 0}, {arc, 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{sp, sketch::kWholeEntity},
                                  {arc, sketch::kWholeEntity}};
    IM_CHECK(ui::RunCommand(*s_App, "sketch.constraint_curvature"));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK_EQ(sk->constraints.size(), (size_t)2);
    IM_CHECK(sk->constraints[0].kind == CK::Coincident);
    IM_CHECK(sk->constraints[1].kind == CK::Curvature);
    // The joint holds: the spline's first leg along the arc's end tangent.
    const auto& s = std::get<sketch::BSpline>(sk->entities[1]);
    const auto& a = std::get<sketch::Arc>(sk->entities[0]);
    const glm::dvec2 leg = glm::normalize(s.poles[1] - s.poles[0]);
    const glm::dvec2 tangent(-std::sin(a.endAngle), std::cos(a.endAngle));
    IM_CHECK_LE(std::abs(leg.x * tangent.y - leg.y * tangent.x), 1e-6);
  };
  // The dimension typed while drawing takes both: the first one rescales
  // what was drawn by eye, and `name = value` makes the variable -- each one
  // undo step (docs/sketching/README.md#the-first-dimension).
  t = IM_REGISTER_TEST(engine, "dims", "typed_dimension_rescales_and_names");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
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
    const uint32_t rough = sk->AddEntity(sketch::Line{{-1.5, -1}, {0.5, -1}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    const auto clickUV = [&](glm::vec2 uv) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const glm::vec2 p = ui::SketchUVToScreen(*s_App, sk->plane, uv);
      ctx->MouseMoveToPos(ImVec2(p.x, p.y));
      ctx->MouseClick(ImGuiMouseButton_Left);
      ctx->Yield();
    };
    const auto lengthOf = [&](uint32_t id) {
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
      const auto& l =
          std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(id)]);
      return glm::length(l.b - l.a);
    };
    const auto typeValue = [&](const char* text) {
      ctx->KeyChars("1");
      ctx->Yield(2);
      IM_CHECK(s_App->sketchEdit.dimOnCreateOpen);
      for (int i = 0; i < 4; i++)
        ctx->KeyPress(ImGuiKey_Backspace);
      ctx->KeyChars(text);
      ctx->KeyPress(ImGuiKey_Enter);
      ctx->Yield(2);
      sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    };

    ctx->KeyPress(ImGuiKey_L);
    ctx->Yield();
    clickUV({1.0f, 0.0f});
    clickUV({1.0f, 1.0f});
    IM_CHECK_EQ(s_App->sketchEdit.lastDrawn.size(), (size_t)1);
    const uint32_t drawn = s_App->sketchEdit.lastDrawn[0];
    const double drawnLen = lengthOf(drawn);
    typeValue("3");
    // The new line is 3, and the rough one drawn before it grew with it.
    IM_CHECK_LE(std::abs(lengthOf(drawn) - 3.0), 1e-6);
    IM_CHECK_LE(std::abs(lengthOf(rough) - 2.0 * 3.0 / drawnLen), 1e-6);
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    IM_CHECK_LE(std::abs(lengthOf(rough) - 2.0), 1e-9);
    IM_CHECK_LE(std::abs(lengthOf(drawn) - drawnLen), 1e-9);

    // Another line, and `seg = 4`: a variable, the rescale, one undo.
    const size_t vars = s_App->projectParameters.size();
    clickUV({-1.0f, 0.5f});
    clickUV({-1.0f, 1.5f});
    IM_CHECK_EQ(s_App->sketchEdit.lastDrawn.size(), (size_t)1);
    const uint32_t second = s_App->sketchEdit.lastDrawn[0];
    const double secondLen = lengthOf(second);
    typeValue("seg = 4");
    IM_CHECK(ui::FindProjectParameter(*s_App, "seg") != nullptr);
    IM_CHECK_STR_EQ(sk->constraints.back().valueExpr.c_str(), "seg");
    IM_CHECK_LE(std::abs(lengthOf(second) - 4.0), 1e-6);
    IM_CHECK_LE(std::abs(lengthOf(rough) - 2.0 * 4.0 / secondLen), 1e-6);
    ui::GlobalUndo(*s_App);
    ctx->Yield();
    IM_CHECK(ui::FindProjectParameter(*s_App, "seg") == nullptr);
    IM_CHECK_EQ(s_App->projectParameters.size(), vars);
    IM_CHECK_LE(std::abs(lengthOf(rough) - 2.0), 1e-9);
  };
}
