#include "app/guitests/GuiTestsCommon.h"

// Project GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterProjectGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "slots", "body_row_fills_an_armed_slot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t firstBody = Doc()->GetFeature(0)->GetId();

    ToolbarClick(ctx, "###tb_shell"); // Body defaults to the NEWEST producer
    ctx->Yield();
    auto* sh = dynamic_cast<ShellFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(sh != nullptr);
    IM_CHECK(sh->bodyId != firstBody);

    ctx->ItemClick("//###ToolWindow/###slot4"); // arm the Body slot
    ctx->Yield();
    IM_CHECK(s_App->slotPick.Active());
    ctx->ItemClick("//##DockHost/**/Box 1##body"); // the row, not the viewport
    ctx->Yield();
    IM_CHECK_EQ(sh->bodyId, firstBody);

    // A pick that hits nothing explains itself instead of vanishing.
    ctx->ItemClick("//###ToolWindow/###slot5"); // Open faces (Face kind)
    ctx->Yield();
    // Aim at empty space so the click certainly hits nothing, and use the
    // viewport's own centre so it certainly IS over the viewport.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f, 400.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 400.0f, 40.0f));
    ctx->Yield(2);
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + s_App->viewportSize.x * 0.5f,
               s_App->viewportPos.y + s_App->viewportSize.y * 0.5f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.miss.empty());
    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  // Coverage gaps the U10 gate exposed: these tools were only ever proved to
  // OPEN. A gate case says "it changes the document or refuses"; it does not
  // say a human can finish the tool and get the geometry they meant.

  // Loft collects its sections from a combo rather than a pick slot, so the
  // gate can only watch it refuse — before this, nothing had ever built one
  // through the GUI at all.
  t = IM_REGISTER_TEST(engine, "slots", "boolean_via_viewport_picks");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_box");
    auto* box2 = dynamic_cast<BoxFeature*>(Doc()->GetFeature(1));
    auto* box3 = dynamic_cast<BoxFeature*>(Doc()->GetFeature(2));
    IM_CHECK(box2 != nullptr && box3 != nullptr);
    box2->position = glm::vec3(-3.0f, 0.0f, 0.0f); // overlaps box 1 by 1x2x2
    box3->position = glm::vec3(2.0f, 0.0f, 0.0f);  // far to the right
    Doc()->Regenerate();
    ctx->Yield();

    // Frame all three with the +Z faces toward the camera.
    glm::vec3 min(-4.0f, 0.0f, 0.0f), max(4.0f, 2.0f, 2.0f);
    const glm::vec3 center = (min + max) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(0.3f, 0.5f, 1.0f)) * 20.0f);
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_boolean");
    ctx->Yield();
    IM_CHECK(s_App->pendingFeatureId != 0);
    const uint64_t box1Id = Doc()->GetFeature(0)->GetId();
    const uint64_t box2Id = box2->GetId();

    // Arm Target, click box 1's TOP face; arm Tool, click box 2's —
    // top faces project high on screen, clear of overlapping windows.
    ctx->ItemClick("//Boolean###ToolWindow/###slot1"); // Target
    IM_CHECK(s_App->slotPick.Active());
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(-3.7f, 2.0f, 1.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active()); // single slot disarms on pick
    ctx->ItemClick("//Boolean###ToolWindow/###slot2"); // Tool
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(-1.3f, 2.0f, 1.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    auto* boo = dynamic_cast<BooleanFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(boo != nullptr);
    IM_CHECK_EQ(boo->targetId, box1Id);
    IM_CHECK_EQ(boo->toolId, box2Id);

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    // Union of boxes 1+2 (8 + 8 - 1x2x2 overlap = 12) plus untouched box 3.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    bool sawUnion = false;
    for (const Body& b : Doc()->GetBodies())
      if (std::abs(b.shape.Volume() - 12.0) < 1e-3)
        sawUnion = true;
    IM_CHECK(sawUnion);
  };

  // U7b select-first: two pre-selected bodies seed Boolean's target/tool in
  // pick order — the session opens fully satisfied (nothing armed), Accept
  // commits without a single extra pick.
  t = IM_REGISTER_TEST(engine, "slots", "boolean_seeded_from_preselection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "A", glm::vec3(-1.0f, 0.0f, 0.0f), 2.0, 2.0, 2.0));
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "B", glm::vec3(0.0f, 0.0f, 0.0f), 2.0, 2.0, 2.0));
    Doc()->Regenerate();
    ctx->Yield();
    const uint64_t aId = Doc()->GetFeature(0)->GetId();
    const uint64_t bId = Doc()->GetFeature(1)->GetId();
    // Pick order matters: A first (target), B second (tool).
    s_App->selection.Replace({ui::SelectionItem::Kind::Body, aId, {}});
    s_App->selection.Toggle({ui::SelectionItem::Kind::Body, bId, {}});
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_boolean");
    ctx->Yield();
    auto* boo = dynamic_cast<BooleanFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(boo != nullptr);
    IM_CHECK_EQ(boo->targetId, aId);
    IM_CHECK_EQ(boo->toolId, bId);
    IM_CHECK(!s_App->slotPick.Active()); // fully seeded: nothing armed
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Union of two 2x2x2 boxes overlapping 1x2x2: 8 + 8 - 4.
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - 12.0) < 1e-3);
  };

  // U7b select-first: pre-selected edges seed the fillet's edge list — ALL
  // of them, not just the first (a regen between batched toggles blended
  // the body mid-seed and stranded the remaining pre-picked names).
  t = IM_REGISTER_TEST(engine, "slots", "fillet_seeded_from_selected_edges");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const Body& body = Doc()->GetBodies()[0];
    const double before = body.shape.Volume();
    // Three distinct edges, Ctrl+click style (ordered toggles).
    for (int e = 0; e < 3; e++) {
      const std::string* edgeName = body.elementMap.EdgeNameOf(e);
      IM_CHECK(edgeName != nullptr);
      s_App->selection.Toggle({ui::SelectionItem::Kind::Edge, 0,
                               ElementRef{body.createdBy, *edgeName}});
    }
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ToolbarClick(ctx, "###tb_fillet");
    ctx->Yield();
    auto* fil = dynamic_cast<FilletFeature*>(
        Doc()->FindFeatureById(s_App->pendingFeatureId));
    IM_CHECK(fil != nullptr);
    IM_CHECK_EQ(fil->edges.size(), (size_t)3);
    fil->radius = 0.3;
    Doc()->Regenerate();
    IM_CHECK(fil->GetError().empty());
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < before - 1e-6);
  };

  // U2: region picking — arm the extrude's Profiles slot and toggle one of
  // a two-region sketch's profiles by clicking inside it in the viewport.
  // U8b select-first in sketch mode: a selected profile seeds the extrude's
  // regions — only that loop fills.
  t = IM_REGISTER_TEST(engine, "tabs", "folder_flyout");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Imported";
    ui::AddPart(*s_App, "Nut");
    s_App->parts.back().folder = "Imported";
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    // One flyout button for the folder; clicking lists the parts.
    ctx->ItemClick("//##PartTabs/**/###folderImported");
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###fpart2");
    ctx->Yield();
    IM_CHECK_EQ(s_App->activePart, 2);
    IM_CHECK_EQ(s_App->parts[2].name, "Nut");

    // The folder survives a save/load round-trip.
    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_folders")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield();
    int foldered = 0;
    for (const auto& s : s_App->parts)
      if (s.folder == "Imported")
        foldered++;
    IM_CHECK_EQ(foldered, 2);
    std::filesystem::remove_all(dir);
  };

  // The bottom-bar "+" collapses the three add buttons into one menu.
  // Opening a project must frame what it opened. The fit was guarded on
  // `state.document`, which is null when the active tab is an ASSEMBLY, so a
  // 5 m dock opened with the camera at its default — inside the geometry.
  // It read as an empty scene, and the first thing you did was press F.
  t = IM_REGISTER_TEST(engine, "tabs", "kind_glyphs_and_stable_ids");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddAssembly(*s_App, "Rig");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Hardware";
    ctx->Yield(2);

    // Addressable by id whatever the label says.
    IM_CHECK(ctx->ItemExists("//##PartTabs/**/###part0"));
    IM_CHECK(ctx->ItemExists("//##PartTabs/**/###part1"));
    IM_CHECK(ctx->ItemExists("//##PartTabs/**/###folderHardware"));

    // And the labels carry the kind glyph — a part is a cube, an assembly is
    // cubes, a folder is a folder.
    ImGuiTestItemInfo part = ctx->ItemInfo("//##PartTabs/**/###part0");
    ImGuiTestItemInfo asm_ = ctx->ItemInfo("//##PartTabs/**/###part1");
    ImGuiTestItemInfo folder =
        ctx->ItemInfo("//##PartTabs/**/###folderHardware");
    IM_CHECK(strstr(part.DebugLabel, ICON_FA_CUBE) != nullptr);
    IM_CHECK(strstr(asm_.DebugLabel, ICON_FA_CUBES) != nullptr);
    IM_CHECK(strstr(folder.DebugLabel, ICON_FA_FOLDER) != nullptr);
    // The glyphs must be DIFFERENT, or the indicator indicates nothing.
    IM_CHECK(strcmp(part.DebugLabel, asm_.DebugLabel) != 0);
  };

  // File > Open Recent: the app's own history, kept in the state dir. The
  // menu is disabled when empty, lists what was opened, and keeps entries
  // whose file has since gone (disabled, with the reason).
  t = IM_REGISTER_TEST(engine, "file", "open_recent_lists_what_was_opened");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_recent_gui";
    std::filesystem::remove_all(dir);
    RecentProjects::Clear();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "test");
    ctx->Yield();

    // Saving is what puts it there — the same hook as opening.
    const auto recent = RecentProjects::List();
    IM_CHECK_EQ(recent.size(), (size_t)1);
    IM_CHECK(recent[0].find("designer_recent_gui") != std::string::npos);

    // The menu shows it, and clicking it opens that project.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // a fresh document
    ctx->MenuClick("File/Open Recent/###recent0");
    ctx->Yield(2);
    IM_CHECK(Doc()->FeatureCount() > 0); // the box came back
    IM_CHECK_EQ(s_App->projectPath, dir.string());

    std::filesystem::remove_all(dir);
    RecentProjects::Clear();
  };

  t = IM_REGISTER_TEST(engine, "tabs", "add_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Part##addpart");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK(s_App->parts[1].kind == ui::DocKind::Part);
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Assembly##addasm");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)3);
    IM_CHECK(s_App->parts[2].kind == ui::DocKind::Assembly);
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)4);
    IM_CHECK(s_App->parts[3].kind == ui::DocKind::Drawing);
  };

  // U13b: enough documents to overrun the bar, which the Marsliden dock (17)
  // does at 1280x720. Three properties, none of which held before:
  //
  //   1. "+" stays on screen. It used to be the last item on one SameLine
  //      run, so it went over the edge with the tabs — and it is the only
  //      way to add a document.
  //   2. The strip scrolls INSTEAD of culling. Note this cannot be tested by
  //      probing tab ids: the test engine scrolls an item into view before
  //      reporting it, so every id resolves either way (the same trap the
  //      toolbar's every_tool_is_reachable documents).
  //   3. The scrollbar is actually SHOWN. Reserving room for it is not the
  //      same as drawing it — the first cut of this grew the bar by 12 px
  //      and rendered nothing there, because the bar's height predated the
  //      child and was shorter than one button row.
  t = IM_REGISTER_TEST(engine, "tabs", "overflow_scrolls_and_plus_stays");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    for (int i = 0; i < 20; i++)
      ui::AddPart(*s_App, "Document " + std::to_string(i));
    ctx->Yield(4); // the height is measured a frame late, by construction

    // By substring: a child's real name carries the parent's and a hashed
    // suffix ("##PartTabs/##tabstrip_1A2B3C4D"), so an exact lookup finds
    // nothing — the same wrinkle every_tool_is_reachable hit on the toolbar.
    ImGuiWindow* strip = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
      if (std::string(w->Name).find("##tabstrip") != std::string::npos) {
        strip = w;
        break;
      }
    IM_CHECK(strip != nullptr);
    ImGuiWindow* bar = ImGui::FindWindowByName("##PartTabs");
    IM_CHECK(bar != nullptr);
    ctx->LogInfo("bar h=%.0f (asked %.0f)  strip %.0f px in %.0f px, "
                 "row %.0f in %.0f, scrollbar %d",
                 bar->Size.y, ui::PartTabsBarHeight(), strip->ContentSize.x,
                 strip->InnerRect.GetWidth(), ImGui::GetFrameHeight(),
                 strip->InnerRect.GetHeight(), (int)strip->ScrollbarX);
    // The bar gets the height it asks for. It did not: the theme's 180x60
    // WindowMinSize floor (for dock nodes) silently applied here too, so the
    // bar was 60 px while BeginDockspace reserved 36 and the bottom of every
    // docked panel sat underneath it.
    IM_CHECK_LE(bar->Size.y, ui::PartTabsBarHeight() + 1.0f);
    IM_CHECK(strip->ContentSize.x > strip->InnerRect.GetWidth());
    IM_CHECK(strip->ScrollbarX);
    // ScrollbarX alone is not enough: ImGui sets it whenever the content is
    // too wide, whether or not there is anywhere to draw it. The bar has to
    // have GROWN to hold both — and InnerRect is the strip MINUS its
    // scrollbar, so "a full button row still fits in InnerRect" is exactly
    // the property that fails when the bar did not grow (the row is squeezed
    // to make room, or the scrollbar is clipped away).
    IM_CHECK_GE(strip->InnerRect.GetHeight(), ImGui::GetFrameHeight() - 1.0f);

    // "+" is outside the strip and inside the viewport, at any tab count.
    const ImGuiTestItemInfo plus = ctx->ItemInfo("//##PartTabs/+##addmenu");
    IM_CHECK(plus.ID != 0);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    IM_CHECK(plus.RectFull.Max.x <= vp->WorkPos.x + vp->WorkSize.x);
    IM_CHECK(plus.RectFull.Min.x >= strip->Rect().Max.x);
    // ...and still opens its menu from there.
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Part##addpart");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)22);
  };

  // U13b: the dock's panel tabs are icon-only, and every one of them FITS.
  // They used to render as `Mo…`, `His…`, `CA…` at the default 218 px split —
  // and this ImGui draws no tooltip for a tab item, so hovering one said
  // nothing either. Two properties, the same pair the toolbar row asserts:
  // the strip fits its contents, and the name is still reachable.
  t = IM_REGISTER_TEST(engine, "tabs", "panel_tabs_fit_and_name_on_hover");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->Yield(4);
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(model != nullptr);
    IM_CHECK(model->DockNode != nullptr);
    ImGuiTabBar* bar = model->DockNode->TabBar;
    IM_CHECK(bar != nullptr);
    ctx->LogInfo("panel tabs want %.0f px, bar is %.0f px (%d tabs)",
                 bar->WidthAllTabsIdeal, bar->BarRect.GetWidth(),
                 bar->Tabs.Size);
    // Ideal = what the tabs would need if none were clipped. Under the bar's
    // width means no tab is truncated — which no arrangement of the WORDS
    // could satisfy ("Configurations" alone is wider than half the panel).
    IM_CHECK(bar->WidthAllTabsIdeal <= bar->BarRect.GetWidth());

    ImGuiTabItem* tab = ImGui::TabBarFindTabByID(bar, model->TabId);
    IM_CHECK(tab != nullptr);
    const float x = bar->BarRect.Min.x + tab->Offset - bar->ScrollingAnim;
    ctx->MouseMoveToPos(
        ImVec2(x + tab->Width * 0.5f, bar->BarRect.GetCenter().y));
    ctx->Yield(3);
    // The hand-rolled tooltip (DockLayout DrawPanelTabTooltips) is what
    // carries the word now.
    ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
    IM_CHECK(tip != nullptr);
    IM_CHECK(tip->Active);
  };

  // "+ > New Folder" creates an EMPTY folder (registry-backed): it renders
  // as a (0) flyout button, survives save/load while empty, and accepts a
  // dropped tab.
  t = IM_REGISTER_TEST(engine, "tabs", "folder_empty_create");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    ctx->Yield();
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Folder##addfolder");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Fasteners");
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)1);
    IM_CHECK_EQ(s_App->tabFolders[0], std::string("Fasteners"));
    IM_CHECK(ui::ProjectDirty(*s_App));

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_empty_folder")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)0);
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield();
    IM_CHECK_EQ(s_App->tabFolders.size(), (size_t)1);
    IM_CHECK_EQ(s_App->tabFolders[0], std::string("Fasteners"));

    // The empty folder is a live drop target.
    ctx->ItemDragAndDrop("//##PartTabs/**/###part1",
                         "//##PartTabs/**/###folderFasteners");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[1].folder, std::string("Fasteners"));
    std::filesystem::remove_all(dir);
  };

  // Folder management (Onshape-style tab folders): rename via right-click,
  // move docs in/out via the tab context menus, and drag-drop between the
  // bar and folder buttons. Folder edits mark the project dirty even though
  // no document version changes (they live only in project.json).
  t = IM_REGISTER_TEST(engine, "tabs", "folder_manage");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Imported";
    ui::AddPart(*s_App, "Nut");
    s_App->parts.back().folder = "Imported";
    ui::SetActivePart(*s_App, 0);
    s_App->projectMetaDirty = false; // direct setup writes above don't count
    ctx->Yield(2);

    // Rename the folder via right-click > Rename.
    ctx->ItemClick("//##PartTabs/**/###folderImported", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Rename...");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Hardware");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[1].folder, std::string("Hardware"));
    IM_CHECK_EQ(s_App->parts[2].folder, std::string("Hardware"));
    IM_CHECK(ui::ProjectDirty(*s_App));

    // Move the top-level part into a NEW folder via its context menu.
    ctx->ItemClick("//##PartTabs/**/###part0", ImGuiMouseButton_Right);
    ctx->MenuClick("//$FOCUSED/Move to folder/New folder...##mvnew");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Misc");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string("Misc"));

    // ...and back out of it via the flyout entry's context menu.
    ctx->ItemClick("//##PartTabs/**/###folderMisc");
    ctx->ItemClick("//$FOCUSED/###fpart0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Move out of folder");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string(""));

    // Drag the top-level tab onto the folder button to file it.
    ctx->ItemDragAndDrop("//##PartTabs/**/###part0",
                         "//##PartTabs/**/###folderHardware");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[0].folder, std::string("Hardware"));

    // Drag "Nut" out of the open flyout onto the bar's empty tail.
    // Hand-rolled with raw positions: ItemDragAndDrop refocuses the target's
    // window to resolve it, which closes the flyout mid-drag.
    const ImVec2 tailPos =
        ctx->ItemInfo("//##PartTabs/**/##tabsdroptail").RectFull.GetCenter();
    ctx->ItemClick("//##PartTabs/**/###folderHardware");
    const ImVec2 nutPos =
        ctx->ItemInfo("//$FOCUSED/###fpart2").RectFull.GetCenter();
    ctx->MouseMoveToPos(nutPos);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(tailPos);
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts[2].folder, std::string(""));

    // Dissolve the folder: contents move to the top level, nothing deleted.
    ctx->ItemClick("//##PartTabs/**/###folderHardware", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Dissolve folder");
    ctx->Yield();
    IM_CHECK_EQ(s_App->parts.size(), (size_t)3);
    for (const auto& s : s_App->parts)
      IM_CHECK_EQ(s.folder, std::string(""));
  };

  // Mate-tool hover feedback uses the same fresh face inference as a click:
  // before committing either connector it identifies the candidate occurrence,
  // then clears the transient marker as soon as the live mate preview starts.
  t = IM_REGISTER_TEST(engine, "autosave", "recovers_every_document");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_recover.dsgn";
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(dir.string() + ".autosave.d");

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Dirty the part that is NOT the open tab.
    const auto slotOf = [](const char* name) -> size_t {
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].name == name)
          return i;
      return 0;
    };
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 44.0f);
    ctx->Yield();
    ui::SetActivePart(*s_App, slotOf("Second")); // and look away from it
    ctx->Yield(2);

    Autosaver autosaver(0.001);
    autosaver.Prime(0.0);
    IM_CHECK(ui::AutosaveProject(*s_App, autosaver, 1.0) >= 1);
    IM_CHECK(!Autosaver::RecoverableDocuments(dir.string()).empty());

    // The crash: reopen from disk, which has the SAVED width, not 44.
    IM_CHECK(ui::OpenProject(*s_App, dir.string(), &err));
    ui::MarkProjectOpened(*s_App, dir.string());
    ctx->Yield(2);
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0); // the saved value — the edit is gone

    // Recovery puts it back, into the right document.
    IM_CHECK(ui::RecoverProject(*s_App, dir.string(), &err));
    ctx->Yield(3);
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 44.0);
    // And it is not pretending to be saved.
    IM_CHECK(ui::ProjectDirty(*s_App));
  };

  // A clean project must stop costing anything. The conflicts scan walks the
  // whole project (for the duplicate ids git cannot report) AND spawns
  // `git` — it used to do both once a second, forever, to answer "no".
  t = IM_REGISTER_TEST(engine, "conflicts", "a_clean_project_stops_scanning");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_scan_cost.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(5);

    const uint64_t before = s_App->conflictScans;
    ctx->Yield(60); // a second of frames, and then some
    const uint64_t during = s_App->conflictScans - before;
    IM_CHECK_LE(during, (uint64_t)1); // at most the one the settle allowed
    ctx->LogInfo("scans over 60 clean frames: %d", (int)during);

    // But an actual change still gets noticed — the point is to stop asking,
    // not to stop answering.
    {
      std::ofstream(dir / "NOTES.txt") << "someone else\n";
    }
    s_App->diskCheckStamp = -1.0;
    ctx->Yield(5);
    IM_CHECK(s_App->conflictScans > before + during);
  };

  // F8: the project moved on disk while the app held it in memory. This is
  // the terminal-panel scenario — the panel ships inside the app and starts
  // in the project's directory, so a `git checkout` there is a normal thing
  // to do. Before this, the next Ctrl+S replaced it with no message.
  t = IM_REGISTER_TEST(engine, "disk", "a_moved_project_refuses_to_be_saved");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_disk_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);
    // A project we just wrote has not moved, and saving again is fine — the
    // app's own writes must never read as somebody else's.
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Somebody else writes.
    {
      std::ofstream(dir / "NOTES.txt") << "not this app\n";
    }
    IM_CHECK(ui::ProjectChangedOnDisk(*s_App));

    // The save refuses and raises the modal rather than overwriting.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->diskChangeBlockedSave);
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));

    // Cancel leaves the project moved: dismissing a save does not un-move it.
    ctx->SetRef("//Project changed on disk");
    ctx->ItemClick("Cancel###disk_cancel");
    ctx->Yield(3);
    IM_CHECK(!s_App->diskChangeBlockedSave);
    IM_CHECK(s_App->diskChanged);
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));

    // Save anyway goes through, and re-stamps — the same change must not be
    // reported twice.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->diskChangeBlockedSave);
    ctx->SetRef("//Project changed on disk");
    ctx->ItemClick("Save anyway###disk_overwrite");
    ctx->Yield(3);
    IM_CHECK(!s_App->diskChangeBlockedSave);
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
    // A save is not a delete: an unrelated external file is still there.
    IM_CHECK(std::filesystem::exists(dir / "NOTES.txt"));
  };

  // The banner is the half that makes the refusal rarely the first news. It
  // shows on any saved project, git or not — "something else wrote to your
  // project" is true either way.
  t = IM_REGISTER_TEST(engine, "disk", "the_history_panel_says_it_moved");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_disk_banner.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    // Nothing has happened: no banner.
    IM_CHECK(!s_App->diskChanged);

    // Go back to the model, which is where a person actually is when
    // somebody else writes to their project.
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(model != nullptr);
    ctx->ItemClick(model->TabId);
    ctx->Yield(2);

    {
      std::ofstream(dir / "NOTES.txt") << "not this app\n";
    }
    // The poll is deliberately slow (a directory walk), so drive it rather
    // than yielding for three seconds of test time.
    s_App->diskCheckStamp = -1.0;
    ctx->Yield(3);
    // Noticed with the History panel BEHIND another tab: the poll runs from
    // the top of the frame, not from inside the panel, which would only ever
    // have caught changes made while you were looking at the History tab.
    IM_CHECK(s_App->diskChanged);
    // The menu-bar notice this flag drives (MainMenuBar's ###disk_notice) is
    // NOT asserted here: a disabled MenuItem in the main menu bar does not
    // resolve through the test engine's item paths, and three ways of asking
    // failed. What matters and IS covered is the flag being true while
    // another tab owns the dock node — the notice is fifteen lines of ImGui
    // reading it.

    // Now go read it.
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("Reload###hist_disk_reload");
    ctx->Yield(3);

    // Reloading agrees with the disk again, so the banner goes.
    IM_CHECK(!s_App->diskChanged);
    IM_CHECK(!ui::ProjectChangedOnDisk(*s_App));
  };

  // F11 G2: clicking a version shows what has changed since it. The point is
  // that it reads as the model, not as files — "Box \"Box 1\" — width 2 to 9",
  // not "features/1-box.json | M".
  t = IM_REGISTER_TEST(engine, "history", "a_version_shows_what_changed_since");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_diff_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(3);

    // Change the model AFTER that version, and write it — the diff is against
    // what is ON DISK, which is what "since that version" means. Saved but
    // not committed, so this is the everyday question: what have I done since
    // the last version?
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 9.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ctx->Yield(2);

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("**/###hist_row0"); // the version, not a button
    ctx->Yield(3);

    IM_CHECK(!s_App->diffFrom.empty());
    IM_CHECK_EQ(s_App->diffChanges.size(), (size_t)1);
    const ProjectMerge::ItemChange& c = s_App->diffChanges[0];
    IM_CHECK_STR_EQ(c.itemKind.c_str(), "Box");
    IM_CHECK(c.summary.find("width 2 to 9") != std::string::npos);

    // Back returns to the list, and the versions are still there.
    ctx->ItemClick("Back###hist_diff_back");
    ctx->Yield(2);
    IM_CHECK(s_App->diffFrom.empty());
    IM_CHECK(ctx->ItemExists("**/###hist_row0"));
  };

  // Variables and Configurations: two v1-complete workstreams with, until
  // now, ZERO gui-test contact of any kind. That was not an oversight — the
  // panels could not be clicked. Every row sat in an ImGui::PushID(int)
  // scope, which leaves a widget reachable only by numeric id
  // (docs/ui/gui-coverage.md), so no test and no agent could name a cell.
  //
  // Behind that, the flow was broken end to end: the Variables panel creates
  // every variable at PROJECT scope, and the Configurations panel listed
  // only PART-scoped ones — so the first thing anyone would try (add a
  // variable, make a configuration of it) answered "No variables to
  // override" immediately after they had added one. Document::Regenerate had
  // always handled the project case; only the panel did not show it.
  //
  // This walks the whole parametric-family story by clicking: a variable, a
  // dimension driven by it, a configuration that overrides it, and the
  // geometry following in both directions.
  t = IM_REGISTER_TEST(engine, "parity", "clicking_and_scripting_agree");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // The part both sheets will show: three different dimensions, so a
    // difference in orientation or scale cannot hide.
    const auto buildPart = [&] {
      ctx->SetRef("//##MainMenuBar");
      ctx->MenuClick("File/New");
      ctx->Yield();
      ToolbarCreateAccept(ctx, "###tb_box");
      ctx->ItemDoubleClick("//###Model/Box 1##0");
      ctx->Yield();
      ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 80.0f);
      ctx->ItemInputValue("//Box###ToolWindow/Height/##Height", 8.0f);
      ctx->ItemInputValue("//Box###ToolWindow/Depth/##Depth", 60.0f);
      ctx->Yield(2);
      ctx->ItemClick("//##PartTabs/+##addmenu");
      ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
      ctx->Yield(3);
    };
    // What a sheet IS, for comparison: every view's orientation, scale and
    // where it sits. Ids differ between the two runs and are left out.
    const auto sheetState = [&] {
      std::vector<std::string> rows;
      for (const DrawingView& v : s_App->drawing->GetViews()) {
        char buf[160];
        snprintf(buf, sizeof buf, "%s scale=%.4f at (%.3f, %.3f)",
                 ViewOrientationToString(v.orientation), v.scale, v.position.x,
                 v.position.y);
        rows.push_back(buf);
      }
      return rows;
    };

    buildPart();
    ctx->SetRef("//###Model");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose one for me
    for (const char* orient : {"front", "top", "right"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    const std::vector<std::string> clicked = sheetState();
    IM_CHECK_EQ(clicked.size(), (size_t)3);

    // The same three views, asked for the way a script asks: no position and
    // no scale, because that is what "just put a front view on it" means.
    buildPart();
    for (const char* orient : {"front", "top", "right"}) {
      const nlohmann::json params = {{"source", "Part 1"},
                                     {"orientation", orient}};
      remote::RpcParams rp(params);
      const auto reply =
          remote::HandleDrawingRpc(*s_App, "add_drawing_view", rp);
      IM_CHECK(reply.has_value());
      ctx->Yield(2);
    }
    const std::vector<std::string> scripted = sheetState();

    IM_CHECK_EQ(scripted.size(), clicked.size());
    for (size_t i = 0; i < clicked.size() && i < scripted.size(); i++) {
      ctx->LogInfo("view %d: clicked [%s] scripted [%s]", (int)i,
                   clicked[i].c_str(), scripted[i].c_str());
      IM_CHECK_STR_EQ(scripted[i].c_str(), clicked[i].c_str());
    }
  };

  // Drawings had ONE gui test and all it asserted was that the tab appeared.
  // The workspace behind it — pick a source, pick an orientation, add a view
  // — had never been clicked, which is how a Z-up orientation table survived
  // in a Y-up app: every "front" view was really the top. It could not have
  // been clicked either. A bare ImGui::Combo gathers with an EMPTY label, so
  // no ref could reach the two combos that drive the panel; they carry
  // ###ids now.
  //
  // The plate has three different dimensions on purpose. Against a cube, or
  // even a square plate, a swapped axis looks exactly like a correct one.
  t = IM_REGISTER_TEST(engine, "history", "compare_shape_against_a_version");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_geomdiff.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box"); // 2 x 2 x 2 by default
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);
    const std::string version = GitVault::Log(dir.string())[0].hash;

    // Grow one axis: 2x2x2 -> 6x2x2. That is +16 mm3 and nothing lost.
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 6.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ctx->Yield(2);

    const ui::GeometryChange g =
        ui::CompareVersionGeometry(*s_App, version, "parts/Part 1");
    ctx->LogInfo("geometry diff: %s (old %.1f, new %.1f)", g.summary.c_str(),
                 g.oldVolume, g.newVolume);
    IM_CHECK(g.valid);
    IM_CHECK(std::abs(g.oldVolume - 8.0) < 1e-3);
    IM_CHECK(std::abs(g.newVolume - 24.0) < 1e-3);
    IM_CHECK(std::abs(g.added - 16.0) < 1e-3); // the slab that appeared
    IM_CHECK(g.removed < 1e-6);                // nothing was cut away
    IM_CHECK(g.summary.find("added") != std::string::npos);
    IM_CHECK(g.summary.find("removed") == std::string::npos);

    // The diff is also ON the model now: the slab that appeared is a green
    // overlay solid, drawn over the part. A volume answers "how much"; only
    // the shape answers "where", which is the question people actually ask.
    ctx->Yield(2); // the resync that turns the shapes into instances
    IM_CHECK(s_App->HasGeometryDelta());
    IM_CHECK(s_App->deltaExtraInstance != 0);
    IM_CHECK(s_App->deltaSummary.find("added") != std::string::npos);

    // And a version compared against itself is "same shape", not noise.
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);
    const std::string now = GitVault::Log(dir.string())[0].hash;
    const ui::GeometryChange same =
        ui::CompareVersionGeometry(*s_App, now, "parts/Part 1");
    IM_CHECK(same.valid);
    IM_CHECK_STR_EQ(same.summary.c_str(), "same shape");
    ctx->Yield(2);
    IM_CHECK(!s_App->HasGeometryDelta()); // nothing to show, nothing shown

    // The overlay is true only of the shape it was measured on. Edit the
    // part and it has to go: a stale red/green picture of a part that has
    // moved on is worse than no picture at all.
    ui::CompareVersionGeometry(*s_App, version, "parts/Part 1");
    ctx->Yield(2);
    IM_CHECK(s_App->HasGeometryDelta());
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 7.0f);
    ctx->Yield(3);
    IM_CHECK(!s_App->HasGeometryDelta());
    IM_CHECK_EQ(s_App->deltaExtraInstance, 0u);
  };

  // Clicking a diff row goes and finds the thing. The case that earns it is a
  // change in a document that is NOT the open tab — on a 48-part project,
  // "Extrude \"Boss\" in part Stand" is otherwise a sentence you have to act
  // on yourself.
  t = IM_REGISTER_TEST(engine, "history", "clicking_a_change_selects_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_reveal.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(3);

    // Change the FIRST part, then go and stand on the second one.
    const auto slotOf = [](const char* name) -> size_t {
      for (size_t i = 0; i < s_App->parts.size(); i++)
        if (s_App->parts[i].name == name)
          return i;
      return 0;
    };
    ui::SetActivePart(*s_App, slotOf("Part 1"));
    ctx->Yield(2);
    const uint64_t changedId = Doc()->GetFeature(0)->GetId();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 7.0f);
    ctx->Yield(2);
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::SetActivePart(*s_App, slotOf("Second"));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->activePart, (int)slotOf("Second"));

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###History");
    ctx->ItemClick("**/###hist_row0");
    ctx->Yield(3);
    // project.json changes too, because the active tab is stored in it —
    // so assert the row that matters rather than the count.
    int boxRow = -1;
    for (int r = 0; r < (int)s_App->diffChanges.size(); r++)
      if (s_App->diffChanges[r].itemKind == "Box")
        boxRow = r;
    IM_CHECK(boxRow >= 0);
    IM_CHECK(s_App->diffChanges[boxRow].summary.find("width 2 to 7") !=
             std::string::npos);

    // The click: it switches tab AND selects, so the tree row highlights.
    ctx->ItemClick(("**/###diff_row" + std::to_string(boxRow)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->activePart, (int)slotOf("Part 1"));
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), changedId);

    if (ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel)) {
      ctx->ItemClick(model->TabId);
      ctx->Yield(2);
    }
  };

  // Conflicts (F7): two lines of work that changed the same thing, brought
  // together through the panel. The whole point is that the row reads as a
  // FEATURE — "Box 'Box 1' ... width 5 vs 9" — and not as a path.
  t = IM_REGISTER_TEST(engine, "conflicts", "resolve_a_merge_in_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_conflicts_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK(ui::ProjectHasHistory(*s_App));

    // A second line of work, started the way a user does: the History
    // panel's own controls. Without a verb like this the app could not
    // produce a conflict at all.
    ImGuiWindow* history = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(history != nullptr && history->DockNode != nullptr);
    ctx->ItemClick(history->TabId);
    ctx->Yield(2);
    // Whatever git called the first branch — "master" on older installs,
    // "main" on newer ones. Never assume it.
    const std::string firstLine = GitVault::CurrentBranch(s_App->projectPath);
    IM_CHECK(!firstLine.empty());

    ctx->SetRef("//###History");
    ctx->ItemOpen("Branches");
    ctx->Yield();
    // A TreeNode pushes an id scope, so its contents are addressed THROUGH
    // it — "//###History/###hist_branch" finds nothing.
    ctx->ItemClick("Branches/###hist_branch");
    ctx->KeyCharsReplace("alt");
    ctx->ItemClick("Branches/Create###hist_branch_go");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(GitVault::CurrentBranch(s_App->projectPath).c_str(), "alt");

    // The History panel shares its dock node with Model, so Model is BEHIND
    // it now — the feature tree has to be brought forward before its rows can
    // be clicked, exactly as a user would.
    const auto showModel = [&ctx]() {
      ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
      IM_CHECK(model != nullptr && model->DockNode != nullptr);
      ctx->ItemClick(model->TabId);
      ctx->Yield(2);
    };

    // Their width, saved on the branch.
    showModel();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 9.0f);
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Back to the first line, and a different width there.
    IM_CHECK(ui::SwitchToBranch(*s_App, firstLine, &err));
    ctx->Yield(2);
    showModel();
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 5.0f);
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Bring the branch in — a conflict, which is an outcome and not a
    // failure.
    IM_CHECK(ui::MergeBranch(*s_App, "alt", &err));
    ui::InvalidateConflicts(*s_App);
    ctx->Yield(3);

    // The panel exists ONLY now: a permanent tab saying "no conflicts" every
    // day would be noise, so its appearance is the signal.
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelConflicts);
    IM_CHECK(panel != nullptr);
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 1);
    const ProjectMerge::Conflict& c = s_App->conflicts.conflicts[0];
    // The row is a feature, not a file.
    IM_CHECK_STR_EQ(c.itemKind.c_str(), "Box");
    IM_CHECK(c.summary.find("width") != std::string::npos);
    IM_CHECK(c.summary.find("5 vs 9") != std::string::npos);

    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    ctx->SetRef("//###Conflicts");

    // Finishing while something is undecided must do nothing: a half-resolved
    // tree committed as if it were finished is the silent loss F7 exists to
    // stop.
    ctx->ItemClick("Finish###cf_finish");
    ctx->Yield(2);
    IM_CHECK(GitVault::InMerge(s_App->projectPath));
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 1);

    ctx->ItemClick("Keep theirs###cf_theirs0");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 0);

    ctx->ItemClick("Finish###cf_finish");
    ctx->Yield(3);

    // Concluded: the merge is committed, the project reopened, and the panel
    // is gone because there is nothing left to say.
    IM_CHECK(!GitVault::InMerge(s_App->projectPath));
    IM_CHECK(s_App->conflicts.Empty());
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 9.0); // theirs, as chosen
  };

  // The bar is "the resolved project loads and regenerates", never "git
  // exited 0". A resolution can be perfectly valid JSON and still leave the
  // document broken.
  t = IM_REGISTER_TEST(engine, "conflicts", "finish_refuses_a_broken_project");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_conflicts_bad.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    ctx->Yield(2);

    // Break the saved project the way a bad resolution does: a file that is
    // valid on disk but not loadable.
    for (const auto& entry :
         std::filesystem::directory_iterator(dir / "features")) {
      std::ofstream(entry.path(), std::ios::trunc)
          << "<<<<<<< ours\n{}\n=======\n{}\n>>>>>>> theirs\n";
      break;
    }

    s_App->conflictsBreakage.clear();
    IM_CHECK(!ui::FinishMerge(*s_App, &err));
    // It names the document and what broke, rather than leaving a project
    // that opens to an error tomorrow.
    IM_CHECK_EQ(s_App->conflictsBreakage.size(), (size_t)1);
    IM_CHECK(s_App->conflictsBreakage[0].reason.find("conflict markers") !=
             std::string::npos);
  };

  // Version history (F6): the panel is the one home for versions, it reads
  // the whole PROJECT (so it works from any tab kind), naming a state adds a
  // version, and restoring is refused while there are unsaved changes.
  t = IM_REGISTER_TEST(engine, "history", "name_and_restore_a_version");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_history_gui.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");

    // Before: Model owns the shared node. Without this the check below is a
    // false positive — the default layout docks History last, and the last
    // window docked is the one the node selects.
    //
    // SELECTED, not merely asserted: this used to read the incoming state and
    // check it, which held only because no earlier test had left another tab
    // of this node in front. The disk/* cases do exactly that, and the
    // precondition then failed in whichever shard ran them first.
    ImGuiWindow* model = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(model != nullptr && model->DockNode != nullptr);
    ctx->ItemClick(model->TabId);
    ctx->Yield(2);
    IM_CHECK_EQ(model->DockNode->SelectedTabId, model->TabId);

    // Reach the panel the way a user does: click its dock tab. (A tab item's
    // id IS the window's TabId, so it addresses directly — ImGui::FocusWindow
    // deliberately does not select dock tabs, so neither a menu item nor the
    // test engine's WindowFocus can stand in for the click.)
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    IM_CHECK_EQ(panel->DockNode->SelectedTabId, panel->TabId);

    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    // The panel caches the log (reading it spawns `git`) and refreshes on a
    // ~1 s timer; a test's frames are far quicker than that, so a save made
    // around the panel rather than through it has to invalidate the cache.
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK(ui::ProjectHasHistory(*s_App));

    // Name the state we just saved. It is unchanged, so this only works
    // because a name gets a version of its own.
    ctx->SetRef("//###History");
    ctx->ItemClick("###hist_name");
    ctx->KeyCharsReplace("before the lid");
    ctx->ItemClick("###hist_name_go");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->historyLog.size(), (size_t)2);
    IM_CHECK_EQ(s_App->historyLog[0].subject, std::string("before the lid"));
    IM_CHECK(s_App->historyLog[0].named);
    IM_CHECK(!s_App->historyLog[1].named); // the plain save under it

    // A second part, saved: the version above predates it.
    ui::AddPart(*s_App, "Lid");
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK(s_App->historyLog.size() >= 3);

    // Unsaved changes: restoring would discard them, so it is refused.
    ui::AddPart(*s_App, "Scratch");
    ctx->Yield(2);
    IM_CHECK(ui::ProjectDirty(*s_App));
    IM_CHECK((ctx->ItemInfo("//###History/**/###hist_restore1").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);
    ctx->ItemClick("//###History/###hist_save");
    ctx->Yield(2);
    IM_CHECK(!ui::ProjectDirty(*s_App));

    // Restore the named version: the parts added after it are gone, and the
    // restore is itself a version (nothing is lost).
    const size_t before = s_App->historyLog.size();
    int named = -1;
    for (int i = 0; i < static_cast<int>(s_App->historyLog.size()); i++)
      if (s_App->historyLog[i].named)
        named = i;
    IM_CHECK(named >= 0);
    ctx->ItemClick(
        ("//###History/**/###hist_restore" + std::to_string(named)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->parts.size(), (size_t)1);
    IM_CHECK(s_App->historyLog.size() > before);

    std::filesystem::remove_all(dir);
  };

  // The panel's rows must stay INSIDE the panel. The first version reserved
  // room for its buttons with pixel literals (70 for "Name", 58 for the
  // Restore column); the style scales with the display's content scale and
  // the font-size setting, so on a HiDPI screen those literals were about
  // half a button short — the buttons hung off the right edge and the panel
  // scrolled sideways. The trigger is FONT SIZE, not window width, so that
  // is what this drives.
  t = IM_REGISTER_TEST(engine, "history", "rows_fit_at_a_large_font");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (!GitVault::Available()) {
      ctx->LogWarning("git not on PATH — skipping");
      return;
    }
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_history_fit.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    s_App->historyStamp = -1.0;

    // Font size is a PREFERENCE now (Edit > Preferences); the View > Font Size
    // submenu this used to click is gone. Set it directly — driving the
    // Preferences window would be testing that window, not these widths.
    s_App->settings.uiFontScale = 1.4f;
    ui::MarkSettingsChanged(*s_App);
    ctx->Yield(4); // the atlas rebuild happens between frames

    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelHistory);
    IM_CHECK(panel != nullptr && panel->DockNode != nullptr);
    ctx->ItemClick(panel->TabId); // the tab, as a user would

    // Wait for the state the assertions need rather than guessing a frame
    // count — the row list is only read once the panel is actually up, and a
    // fixed Yield(4) passed alone while failing in the full suite.
    for (int i = 0; i < 90; i++) {
      if (panel->DockNode->SelectedTabId == panel->TabId &&
          !s_App->historyLog.empty())
        break;
      ctx->Yield();
    }
    IM_CHECK(!s_App->historyLog.empty());
    const float rightEdge = panel->InnerRect.Max.x;
    // 1 px of slack: rounding, not a missing button.
    IM_CHECK_LE(ctx->ItemInfo("//###History/###hist_name_go").RectFull.Max.x,
                rightEdge + 1.0f);
    IM_CHECK_LE(
        ctx->ItemInfo("//###History/**/###hist_restore0").RectFull.Max.x,
        rightEdge + 1.0f);
    // And nothing else pushed the content wider than the panel, which is
    // what the sideways scroll was.
    IM_CHECK_LE(panel->ContentSize.x, panel->InnerRect.GetWidth() + 1.0f);

    // Put the font back. ResetAppForTest restores the preference for the next
    // test, but the ATLAS is rebuilt by the main loop, so leaving it large
    // would follow this test into the next one.
    s_App->settings.uiFontScale = 1.0f;
    ui::MarkSettingsChanged(*s_App);
    ctx->Yield(4);
    std::filesystem::remove_all(dir);
  };

  // Preferences (docs/ui/settings.md): one window over one Settings struct,
  // every control addressable, and a change reaching the renderer's own
  // fields — the "it reverted when I opened a second project" class of bug
  // starts with two copies of a preference.
}
