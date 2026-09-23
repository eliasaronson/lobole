#include "app/guitests/GuiTestsCommon.h"

// Drawing GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterDrawingGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "drawing", "build_a_drawing_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    // A plate lying flat: 80 wide (X), 60 deep (Y), 8 thick along world up.
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 80.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Height/##Height", 60.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Depth/##Depth", 8.0f);
    ctx->Yield(2); // editing a committed feature commits as you type

    // A drawing tab, from the tab bar's + menu.
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield(3);
    IM_CHECK(s_App->drawing != nullptr);

    // Two views, by clicking: the orientation combo then Add view.
    // ComboClick wants a ref RELATIVE to the window
    // (docs/ui/testing.md#making-a-widget-addressable). Add view is a tool
    // window, opened from the toolbar
    // (docs/ui/panels.md#drawing-tab-the-drawing-panel).
    ToolbarClick(ctx, "###tb_dwg_add_view");
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose a scale for me
    for (const char* orient : {"front", "top"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    ctx->ItemClick(
        "###dwg_add_view_close"); // a floating window a test opens, it closes
    ctx->Yield();
    const auto& views = s_App->drawing->GetViews();
    IM_CHECK_EQ(views.size(), (size_t)2);

    // What each named view actually SHOWS: the front of a plate 80 wide and
    // 8 thick is 80 x 8, and looking down on it gives the 80 x 60 face —
    // the axis-swap that a mangled orientation table produces is exactly
    // what this catches (docs/conventions.md#world-axes).
    // No IM_CHECK in here: its failure path returns void, which makes the
    // lambda's return type ambiguous. An unprojected view comes back as
    // zero and fails the size checks below just as loudly.
    const auto extent = [&](size_t i) -> glm::dvec2 {
      const auto it = s_App->drawingRenders.find(views[i].id);
      if (it == s_App->drawingRenders.end() || !it->second.hasGeometry)
        return glm::dvec2(0.0);
      return it->second.bboxMax - it->second.bboxMin;
    };
    const glm::dvec2 front = extent(0), top = extent(1);
    ctx->LogInfo("front %.1f x %.1f, top %.1f x %.1f", front.x, front.y, top.x,
                 top.y);
    IM_CHECK(std::abs(front.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(front.y - 8.0) < 1e-6);
    IM_CHECK(std::abs(top.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(top.y - 60.0) < 1e-6);

    // Two views of one part share a scale — a top view at 2:1 beside a front
    // view at 1:1 is a drafting error, and it is what fitting each view to
    // the sheet on its own produced.
    IM_CHECK_EQ(views[0].scale, views[1].scale);

    // ...and they do not land on top of each other. The sheet validator is
    // the same one the panel shows, so this is the user-visible verdict.
    const std::vector<drawing::DrawingIssue> issues = drawing::ValidateDrawing(
        *s_App->drawing, s_App->drawingRenders, {}, {});
    for (const drawing::DrawingIssue& issue : issues)
      ctx->LogWarning("%s", issue.detail.c_str());
    IM_CHECK(issues.empty());

    // A view is a row (docs/ui/panels.md#drawing-tab-the-drawing-panel): its
    // menu sets the scale, which only the add form could before, and deletes it
    // -- where a bare "x" button was.
    const uint64_t firstId = views[0].id;
    const std::string row =
        "//###Model/**/###dwg_view_" + std::to_string(firstId);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    ctx->MenuClick(
        ("###dwg_view_scale/###dwg_scale_" + std::to_string(0.5)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->drawing->FindView(firstId)->scale, 0.5);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###dwg_view_delete");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->drawing->GetViews().size(), (size_t)1);
    IM_CHECK(s_App->drawing->FindView(firstId) == nullptr);
  };

  // F11 layer 3: what the GEOMETRY did between a version and now. The
  // numbers have to be real — a box grown from 20 to 30 mm on one axis adds
  // exactly the slab that appeared, and nothing is removed.
}
