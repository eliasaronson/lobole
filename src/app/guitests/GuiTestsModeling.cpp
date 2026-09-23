#include "app/guitests/GuiTestsCommon.h"
#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/SheetMetal.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/FeatureTreePanel.h"
#include "ui/MeasureActions.h"
#include "ui/PanelTitles.h"
#include "ui/VariableActions.h"

// Modeling GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
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

    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 1);

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
  };

  t = IM_REGISTER_TEST(engine, "document", "create_sphere_and_cylinder");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const size_t before = Doc()->FeatureCount();
    const size_t bodiesBefore = Doc()->GetBodies().size(); // see above

    ToolbarCreateAccept(ctx, "###tb_sphere");
    ToolbarCreateAccept(ctx, "###tb_cylinder");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), bodiesBefore + 2);

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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");    // Box 1    -> index 0
    ToolbarCreateAccept(ctx, "###tb_sphere"); // Sphere 1 -> index 1
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
    ToolbarCreateAccept(ctx, "###tb_box");

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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    // Pending: in the doc + rendered, but NOT on the undo stack yet
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(!s_App->commands->CanUndo());

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(s_App->commands->CanUndo()); // now one undoable action

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_cancel_discards");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
    IM_CHECK(!s_App->commands->CanUndo()); // stack untouched
  };

  t = IM_REGISTER_TEST(engine, "preview", "ctrl_z_cancels_pending");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
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

    ToolbarClick(ctx, "###tb_box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the preview

    ui::AddPart(*s_App, "Part 2"); // stashes the first tab
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));

    ui::SetActivePart(*s_App, 0); // back to the tab that had the preview
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // not stranded
    IM_CHECK(!s_App->commands->CanUndo());         // and never committed
  };

  // U9: the Hole tool. A hole is NOT sugar for an extruded cut (its tool is
  // built from primitives, so counterbore/countersink are impossible as a
  // cut), and before this it could only be created over the script API.
  t = IM_REGISTER_TEST(engine, "hole", "sk_hole_button_on_existing_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // something to drill into
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
    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_cylinder");
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    // Straight down at the middle of the box's top face. Primitives are
    // placed by their min corner, so this must come from the bounding box —
    // a guessed centre lands on a corner edge, which outranks the face.
    glm::vec3 lo(0.0f), hi(0.0f);
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    glm::vec3 lo(0.0f), hi(0.0f);
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

      // Whatever else it is, the click must have DONE something. A toolbar
      // button that is enabled and silently does nothing is the same bug in
      // its earliest form.
      IM_CHECK(ui::HasPendingFeature(*s_App));

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
      if (tc->expect == Expect::Changes)
        IM_CHECK(!(ui::FingerprintOf(*Doc()) == before));
    };
  }

  // U10: a tool that runs cleanly and changes nothing says so — and is still
  // acceptable, because Refine on an already-clean body is a fair answer.
  t = IM_REGISTER_TEST(engine, "defaults", "no_op_warns_without_blocking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // a clean box: nothing to merge
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double solid = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
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

  // Draft has no body slot, and nothing in the UI ever wrote bodyId — Execute
  // rejects faces whose ref does not match it, so the pick must set it.
  t = IM_REGISTER_TEST(engine, "draft", "face_pick_names_the_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    glm::vec3 lo(0.0f), hi(0.0f);
    Doc()->GetBodies()[0].shape.GetBoundingBox(lo, hi);
    const glm::vec3 mid = (lo + hi) * 0.5f;
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

  // Split's default knife is the XY plane at the body's mid-height, so the
  // preview shows two pieces before the user touches anything.
  t = IM_REGISTER_TEST(engine, "split", "default_plane_cuts_the_body_in_two");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    // A datum plane on the box's top face — the worst case, and the one hit
    // in practice: tangent, so any offset in the wrong direction misses.
    ToolbarClick(ctx, "###tb_datum_plane");
    ctx->Yield();
    auto* dp = dynamic_cast<DatumPlaneFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(dp != nullptr);
    glm::vec3 bmin, bmax;
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
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

  // Repro for the reported circular-pattern segfault (dogfooding backlog):
  // preview a pattern on a body, switch it to Circular, and work the axis/
  // count/angle widgets across frames — the crash lived in this flow.
  t = IM_REGISTER_TEST(engine, "pattern",
                       "circular_preview_param_edits_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const double before = Doc()->GetBodies()[0].shape.Volume();

    // Frame the box deterministically (same recipe as the face-pick test).
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5f;
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

    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5f;
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_sphere");
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield(2);
    IM_CHECK(ui::FeaturesThatMadeThePick(*s_App).empty()); // nothing picked
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();

    // Straight down at the middle of the top face, off the bounding box: a
    // guessed centre lands on a corner edge, which outranks the face.
    glm::vec3 lo(0.0f), hi(0.0f);
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
    ToolbarCreateAccept(ctx, "###tb_cylinder");
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

    // ComboClick resolves relative to the current ref, not from the root.
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###tol_letter/H  bore");
    ctx->Yield();
    ctx->ComboClick("###tol_grade/IT7");
    ctx->Yield();
    ctx->ItemClick("//###ToolWindow/###tol_set");
    ctx->Yield(2);

    IM_CHECK_EQ(Doc()->GetTolerances().size(), (size_t)1);
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
    ToolbarCreateAccept(ctx, "###tb_box");
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
    ToolbarCreateAccept(ctx, "###tb_cylinder");
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
}
