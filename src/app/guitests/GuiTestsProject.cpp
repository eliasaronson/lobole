// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/guitests/GuiTestsCommon.h"
#include "core/Command.h"
#include "core/MassProps.h"
#include "core/Requirements.h"
#include "core/features/DerivedFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/CrashReports.h"
#include "core/io/ProjectMerge.h"
#include "core/io/ProjectStamp.h"
#include "remote/RemoteRequirements.h"
#include "ui/BackgroundJobs.h"
#include "ui/DerivedActions.h"
#include "ui/DesignSweepPanel.h"
#include "ui/DiskPoller.h"
#include "ui/DocumentSlots.h"
#include "ui/OpenJob.h"
#include "ui/PanelTitles.h"
#include "ui/ProjectActions.h"
#include "ui/RequirementsPanel.h"
#include "ui/SelectionPipeline.h"
#include "ui/VariableActions.h"

#include <nlohmann/json.hpp>

#include "common/Platform.h" // CurrentPid

#include <chrono>
#include <fstream>

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
    AddFixtureBox(ctx);
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);
    AddFixtureBox(ctx);
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);
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
    AddFixtureBox(ctx);
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
    // An open is a job: quick ones land at the end of the frame, a loaded
    // machine's after the modal.
    IM_CHECK(WaitForOpen(ctx));
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

  // Right-click a tab > Duplicate. Worth clicking rather than testing
  // ui::DuplicateDocument alone (test_actions.cpp does that): the item appears
  // in TWO menus — a top-level tab's and a folder flyout entry's — and a
  // duplicate that is only reachable on one of them is the bug.
  t = IM_REGISTER_TEST(engine, "tabs", "duplicate_from_context_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // something to copy
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->document->FeatureCount(), (size_t)1);

    ctx->ItemClick("//##PartTabs/**/###part0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Duplicate part");
    ctx->Yield(2);

    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK_EQ(s_App->activePart, 1); // the copy is what you are now on
    IM_CHECK_EQ(s_App->parts[1].name, std::string("Part 1 copy"));
    IM_CHECK(s_App->document != nullptr);
    IM_CHECK_EQ(s_App->document->FeatureCount(), (size_t)1); // the box came too
    IM_CHECK(ui::ProjectDirty(*s_App)); // a new tab is a project change

    // The same item on an assembly names its kind, and reaches the copy
    // through a folder flyout — the second menu it has to be in.
    ui::AddAssembly(*s_App, "Rig");
    s_App->parts.back().folder = "Rigs";
    ctx->Yield(2);
    ctx->ItemClick("//##PartTabs/**/###folderRigs");
    ctx->ItemClick("//$FOCUSED/###fpart2", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Duplicate assembly");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts.size(), (size_t)4);
    IM_CHECK(s_App->parts[3].kind == ui::DocKind::Assembly);
    IM_CHECK_EQ(s_App->parts[3].name, std::string("Rig copy"));
    // A copy made inside a folder belongs in that folder.
    IM_CHECK_EQ(s_App->parts[3].folder, std::string("Rigs"));
  };

  // Right-click a tab > Rename... — every document kind, from both menus a
  // tab has (top-level and folder flyout). A taken name is refused in the
  // popup, which stays open with the reason, and nothing is renamed.
  t = IM_REGISTER_TEST(engine, "tabs", "rename_from_context_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddAssembly(*s_App, "Assembly 2");
    ui::AddDrawing(*s_App, "Drawing 3");
    ui::AddJob(*s_App, "Job 4");
    s_App->parts.back().folder = "Shop";
    ctx->Yield(2);
    const auto rename = [&](const char* tab, const char* to) {
      ctx->ItemClick(tab, ImGuiMouseButton_Right);
      ctx->ItemClick("//$FOCUSED/Rename...##docrename");
      ctx->Yield();
      ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
      ctx->KeyChars(to);
      ctx->KeyPress(ImGuiKey_Enter);
      ctx->Yield(2);
    };
    rename("//##PartTabs/**/###part0", "Bracket");
    rename("//##PartTabs/**/###part1", "Rig");
    rename("//##PartTabs/**/###part2", "Sheet A");
    IM_CHECK_EQ(s_App->parts[0].name, std::string("Bracket"));
    IM_CHECK_EQ(s_App->parts[1].name, std::string("Rig"));
    IM_CHECK_EQ(s_App->parts[2].name, std::string("Sheet A"));
    IM_CHECK(ui::ProjectDirty(*s_App)); // names live in project.json

    ctx->ItemClick("//##PartTabs/**/###folderShop");
    ctx->ItemClick("//$FOCUSED/###fpart3", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Rename...##docrename");
    ctx->Yield();
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("Rig"); // taken by the assembly
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts[3].name, std::string("Job 4"));
    IM_CHECK(ctx->ItemExists("//$FOCUSED/##dname")); // still open, to fix
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("Mill job");
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts[3].name, std::string("Mill job"));
    IM_CHECK_EQ(s_App->parts[3].folder, std::string("Shop"));
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

  // Folder management (tab folders): rename via right-click,
  // move docs in/out via the tab context menus, and drag-drop between the
  // bar and folder buttons. Folder edits mark the project dirty even though
  // no document version changes (they live only in project.json).
  // Foldering an ASSEMBLY tab. The coverage matrix
  // (docs/ui/testing.md#the-tab-kind-coverage-matrix) flags the tab bar as
  // thin on assembly tabs, and the tests there were about the kind glyphs,
  // not the folder machinery.
  //
  // The code says this cannot break: folders live on PartSlot, every tab kind
  // IS a PartSlot, and the folder loop never looks at `kind`. That is an
  // argument from structure, though, and the two bugs this matrix exists
  // because of were both "the code obviously handles both tabs" until someone
  // clicked. So: click it.
  t = IM_REGISTER_TEST(engine, "tabs", "an_assembly_tab_folders_like_a_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ui::AddAssembly(*s_App, "Gearbox");
    ctx->Yield(2);
    const size_t asmIdx = s_App->parts.size() - 1;
    IM_CHECK(s_App->parts[asmIdx].kind == ui::DocKind::Assembly);
    IM_CHECK(s_App->parts[asmIdx].folder.empty());

    // Right-click the assembly's tab > Move to folder > New folder...
    ctx->ItemClick(("//##PartTabs/**/###part" + std::to_string(asmIdx)).c_str(),
                   ImGuiMouseButton_Right);
    ctx->MenuClick("//$FOCUSED/Move to folder/New folder...##mvnew");
    ctx->Yield(2);
    ctx->KeyCharsReplaceEnter("Assemblies");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->parts[asmIdx].folder, std::string("Assemblies"));
    IM_CHECK(std::find(s_App->tabFolders.begin(), s_App->tabFolders.end(),
                       std::string("Assemblies")) != s_App->tabFolders.end());

    // And it SURVIVES a save/open round trip — the half a purely in-memory
    // assertion would miss, and where a kind-specific serialization path
    // would show up.
    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_asm_folder")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield(2);
    const auto it = std::find_if(s_App->parts.begin(), s_App->parts.end(),
                                 [](const ui::PartSlot& sl) {
                                   return sl.kind == ui::DocKind::Assembly;
                                 });
    IM_CHECK(it != s_App->parts.end());
    IM_CHECK_EQ(it->folder, std::string("Assemblies"));
    std::filesystem::remove_all(dir);
  };

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
    AddFixtureBox(ctx);
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    AddFixtureBox(ctx);

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

  // The notice a crash leaves (core/io/CrashReports.h): a session that died
  // with unsaved work is collected into a report with a copy of its
  // autosave, the modal says so at start-up, and Recover unsaved work brings
  // the edit back — dirty, since it was never saved. The dead session is
  // PLANTED (a marker nobody holds), in a state dir of its own, and collected
  // the way start-up collects; what is under test is the notice and the way
  // back.
  t = IM_REGISTER_TEST(engine, "crash", "the_notice_recovers_unsaved_work");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    namespace fs = std::filesystem;
    const fs::path state = fs::temp_directory_path() / "lobole_crash_notice";
    fs::remove_all(state);
    fs::create_directories(state / "sessions");

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 44.0f);
    ctx->Yield();
    IM_CHECK(s_App->projectPath.empty()); // never saved: the unsaved slot
    Autosaver autosaver(0.001);
    autosaver.Prime(0.0);
    IM_CHECK(ui::AutosaveProject(*s_App, autosaver, 1.0) >= 1);

    // The session that "crashed" with that autosave.
    {
      const nlohmann::json marker = {
          {"stem", "20200101-000000-1"},
          {"mode", "windowed"},
          {"started", "2020-01-01 00:00:00"},
          {"started_epoch", 0},
          {"project", ""},
          {"autosaves", {Autosaver::SidecarPath("")}}};
      std::ofstream(state / "sessions" / "20200101-000000-1.json")
          << marker.dump();
    }
    std::vector<crash::Report> reports = crash::CollectDeadSessions(state);
    IM_CHECK_EQ(reports.size(), static_cast<size_t>(1));
    IM_CHECK(!reports[0].autosaves.empty());

    // The next session starts empty, and is told.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), static_cast<size_t>(0));
    crash::SetNotice(reports[0]);
    ctx->Yield(3);
    ctx->SetRef("//###CrashNotice");
    ctx->ItemClick("Recover unsaved work###crash_recover");
    ctx->Yield(3);
    IM_CHECK(!crash::Notice().has_value());
    IM_CHECK(Doc()->FeatureCount() > 0);
    BoxFeature* box = nullptr;
    for (size_t i = 0; i < Doc()->FeatureCount() && !box; i++)
      box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(i));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 44.0);
    IM_CHECK(ui::ProjectDirty(*s_App)); // recovered, not saved

    Autosaver::Clear("");
    fs::remove_all(state);
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
    AddFixtureBox(ctx);
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
    IM_CHECK(PollDiskNow(ctx));
    IM_CHECK(WaitFor(ctx, "the rescan the change asks for", [before, during] {
      return s_App->conflictScans > before + during;
    }));
  };

  // The disk poll walks the whole project and asks git for HEAD every few
  // seconds; on a 4 100-file project on Windows that was a dropped frame each
  // time. It runs on a worker now (ui/DiskPoller.h). With the walk held for
  // half a second, frames must keep coming WHILE it is held — a frame that
  // sees a stamp in progress is a frame that did not wait on it — and an
  // outside write must still raise the banner.
  t = IM_REGISTER_TEST(engine, "disk", "the_poll_does_not_hold_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_disk_offframe.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    IM_CHECK(PollDiskNow(ctx)); // settled: nothing in flight, nothing seen
    IM_CHECK(!s_App->diskChanged);

    struct HoldStamps {
      HoldStamps() { DocumentIO::SetTestStampHold(500); }
      ~HoldStamps() { DocumentIO::SetTestStampHold(0); }
    } hold;
    const uint64_t before = s_App->diskPollsAnswered;
    s_App->diskCheckStamp = -1.0; // due now
    int framesDuringWalk = 0;
    IM_CHECK(WaitFor(ctx, "the held poll's answer", [&] {
      if (DocumentIO::StampsInProgress() > 0)
        framesDuringWalk++;
      return s_App->diskPollsAnswered > before;
    }));
    ctx->LogInfo("frames drawn while the walk was held: %d", framesDuringWalk);
    IM_CHECK_GT(framesDuringWalk, 0);
    IM_CHECK(!s_App->diskChanged);

    // Somebody else writes: the banner comes up within a poll or two, on the
    // poll's own clock (not driven here), the walk still held.
    {
      std::ofstream(dir / "NOTES.txt") << "not this app\n";
    }
    IM_CHECK(
        WaitFor(ctx, "the banner", [] { return s_App->diskChanged; }, 30.0));
    std::filesystem::remove_all(dir);
  };

  // A save owes a conflicts scan (the tree it wrote is new), and the scan
  // walks the whole project and starts `git` — on Windows a hitch the size
  // of the old disk poll after every save. It runs on the disk poller's
  // worker now: with the scan held half a second, frames must keep coming
  // while it is held, and its answer must still land.
  t = IM_REGISTER_TEST(engine, "disk",
                       "a_save_does_not_hold_the_frame_for_the_scan");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_scan_offframe.dsgn";
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    IM_CHECK(WaitForConflictScan(ctx));

    struct HoldScans {
      HoldScans() { ProjectMerge::SetTestScanHold(500); }
      ~HoldScans() { ProjectMerge::SetTestScanHold(0); }
    } hold;
    const uint64_t before = s_App->conflictScans;
    AddFixtureBox(ctx); // something to save
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    ui::MarkProjectSaved(*s_App, dir.string(), "save");
    int framesDuringScan = 0;
    IM_CHECK(WaitFor(ctx, "the held scan's answer", [&] {
      if (ProjectMerge::ScansInProgress() > 0)
        framesDuringScan++;
      return s_App->conflictScans > before && !ui::ConflictScanPending(*s_App);
    }));
    ctx->LogInfo("frames drawn while the scan was held: %d", framesDuringScan);
    IM_CHECK_GT(framesDuringScan, 0);
    IM_CHECK(s_App->conflicts.Empty());
    std::filesystem::remove_all(dir);
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
    AddFixtureBox(ctx);

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

  // A project whose open could not keep all of it (a feature file its index
  // does not list) is not saved back over itself without asking
  // (docs/versioning/README.md#a-lossy-load): Save raises the choice, with
  // "Save a copy..." the default. Cancel leaves the file; Overwrite anyway
  // saves and the drops go to the log. "Save a copy..." is not clicked: it
  // opens the system's save dialog, which no test may show.
  t = IM_REGISTER_TEST(engine, "disk", "a_lossy_open_asks_before_saving_over");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("designer_lossy_gui_" + std::to_string(platform::CurrentPid()) +
         ".dsgn");
    std::filesystem::remove_all(dir);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir.string(), &err));
    // A feature file the index does not list: what a hand edit or a sync
    // leaves when it drops the index entry and keeps the file.
    std::filesystem::path listed;
    for (const auto& e : std::filesystem::directory_iterator(dir / "features"))
      listed = e.path();
    IM_CHECK(!listed.empty());
    const std::filesystem::path orphan = dir / "features" / "999-box.json";
    {
      nlohmann::json j;
      std::ifstream(listed) >> j;
      j["id"] = 999;
      std::ofstream(orphan) << j.dump(2) << "\n";
    }
    IM_CHECK(ui::OpenProject(*s_App, dir.string(), &err));
    ui::MarkProjectOpened(*s_App, dir.string());
    ctx->Yield(2);
    IM_CHECK(s_App->openDrops.Lossy());

    // Save asks; the file is untouched.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->lossySaveBlocked);
    IM_CHECK(std::filesystem::exists(orphan));
    ctx->SetRef("//###lossy_save");
    IM_CHECK(ctx->ItemExists("Save a copy...###lossy_copy"));
    ctx->ItemClick("Cancel###lossy_cancel");
    ctx->Yield(3);
    IM_CHECK(!s_App->lossySaveBlocked);
    IM_CHECK(std::filesystem::exists(orphan));
    IM_CHECK(s_App->openDrops.Lossy()); // still owed: nothing was saved

    // Overwrite anyway saves over it: the file the open could not keep is
    // gone from the project, and the drops with it.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->lossySaveBlocked);
    ctx->SetRef("//###lossy_save");
    ctx->ItemClick("Overwrite anyway###lossy_overwrite");
    ctx->Yield(3);
    IM_CHECK(!s_App->lossySaveBlocked);
    IM_CHECK(!std::filesystem::exists(orphan));
    IM_CHECK(s_App->openDrops.Empty());
    // And a second save goes straight through.
    ui::SaveProjectAction(*s_App);
    ctx->Yield(2);
    IM_CHECK(!s_App->lossySaveBlocked);
    std::filesystem::remove_all(dir);
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
    AddFixtureBox(ctx);
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
    IM_CHECK(PollDiskNow(ctx));
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
    AddFixtureBox(ctx);
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
  // (docs/ui/testing.md#making-a-widget-addressable), so no test and no agent
  // could name a cell.
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
      AddFixtureBox(ctx);
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
      // No drawing is a failure the size checks report; a null walked into
      // here was a segfault that took the shard's other tests with it.
      if (!s_App->drawing)
        return rows;
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
    // Add view is a tool window, opened from the toolbar
    // (docs/ui/panels.md#drawing-tab-the-drawing-panel).
    ToolbarClick(ctx, "###tb_dwg_add_view");
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose one for me
    for (const char* orient : {"front", "top", "right"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    ctx->ItemClick(
        "###dwg_add_view_close"); // a floating window a test opens, it closes
    ctx->Yield();
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
    AddFixtureBox(ctx); // 2 x 2 x 2 by default
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
    IM_CHECK(s_App->gpu.deltaExtraInstance != 0);
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
    IM_CHECK_EQ(s_App->gpu.deltaExtraInstance, 0u);
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
    AddFixtureBox(ctx);
    const size_t second = ui::AddPart(*s_App, "Second");
    ctx->Yield(2);
    ui::SetActivePart(*s_App, second);
    ctx->Yield();
    AddFixtureBox(ctx);

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
    AddFixtureBox(ctx);

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
    IM_CHECK(WaitForConflictScan(ctx));
    ctx->Yield(); // the panel appears on the frame after the answer

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
    ctx->Yield();
    IM_CHECK(WaitForConflictScan(ctx));
    ctx->Yield();
    IM_CHECK_EQ(s_App->conflicts.NeedsDecision(), 0);

    ctx->ItemClick("Finish###cf_finish");
    ctx->Yield();
    IM_CHECK(WaitForConflictScan(ctx));

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
    AddFixtureBox(ctx);

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
    AddFixtureBox(ctx);

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
    // Naming is our own write: the disk check must not call it someone
    // else's. The empty commit that carries the name came after the save's
    // stamp, and the poll read the new HEAD as a change from outside.
    IM_CHECK(PollDiskNow(ctx)); // poll now, not in a second
    IM_CHECK(!s_App->diskChanged);

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
    AddFixtureBox(ctx);
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
    IM_CHECK(WaitFor(ctx, "the History panel up with its log", [&] {
      return panel->DockNode->SelectedTabId == panel->TabId &&
             !s_App->historyLog.empty();
    }));
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
  // Derived (docs/modeling/derived.md#verbs): the tool copies the one
  // other part as a ghost — translucent, tinted, weightless, still there to
  // be drawn on — and the tree verb retakes the copy once the source moves.
  t = IM_REGISTER_TEST(engine, "derived",
                       "tool_copies_the_other_part_as_a_ghost");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ui::AddPart(*s_App, "Servo");
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    AddFixtureBox(ctx);
    const double servoVolume = Doc()->GetBodies()[0].shape.Volume();
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);

    // One candidate: the preview already carries its snapshot, so Accept
    // is live and the copy is on screen before anything is committed.
    ToolbarClick(ctx, "###tb_derive");
    ctx->Yield(2);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);

    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(Doc()->GetFeature(0)->TypeName() == "Derived");
    IM_CHECK_EQ(Doc()->GetFeature(0)->GetName(),
                std::string("Servo (derived)"));
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(Doc()->GetBodies()[0].reference);
    IM_CHECK(std::abs(Doc()->GetBodies()[0].shape.Volume() - servoVolume) <
             1e-6);
    // Drawn as a ghost (the in-context neighbour's look), no outline.
    IM_CHECK_EQ(s_App->gpu.documentMeshes.size(), (size_t)1);
    IM_CHECK(s_App->meshRenderer->IsInstanceGhost(
        s_App->gpu.documentMeshes[0].meshId));
    IM_CHECK_EQ(s_App->gpu.documentMeshes[0].outlineId, 0u);
    // Weightless: the mass report has no body in it.
    const auto mass = Measure::MassOfBodies(Doc()->GetBodies());
    IM_CHECK(mass.has_value());
    IM_CHECK(mass->bodies.empty());
    IM_CHECK(!ui::DerivedStale(*s_App, *dynamic_cast<const DerivedFeature*>(
                                           Doc()->GetFeature(0)))
                  .value_or(true));

    // The source grows a second body. Showing this tab again is a gate
    // (docs/modeling/derived.md#copies-that-keep-up): the copy follows, and
    // the menu bar says so.
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    AddFixtureSphere(ctx);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    IM_CHECK(!ui::DerivedStale(*s_App, *dynamic_cast<const DerivedFeature*>(
                                           Doc()->GetFeature(0)))
                  .value_or(true));
    IM_CHECK(s_App->linksNotice.find("updated from Servo") !=
             std::string::npos);

    // Follow source off (the row's menu): the next change waits. The row
    // says it is behind, and the verb retakes it.
    ctx->ItemClick("//###Model/Servo (derived)##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_derived_follow");
    ctx->Yield(2);
    IM_CHECK(
        !dynamic_cast<const DerivedFeature*>(Doc()->GetFeature(0))->follow);
    const auto volume = [] {
      double v = 0.0;
      for (const Body& b : Doc()->GetBodies())
        v += b.shape.Volume();
      return v;
    };
    const double followed = volume();
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    AddFixtureCylinder(ctx);
    const double source = volume();
    IM_CHECK(std::abs(source - followed) > 1e-3);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    IM_CHECK(std::abs(volume() - followed) < 1e-6);
    IM_CHECK(ui::DerivedStale(*s_App, *dynamic_cast<const DerivedFeature*>(
                                          Doc()->GetFeature(0)))
                 .value_or(false));
    ctx->ItemClick("//###Model/Servo (derived)##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/###feat_derived_update");
    ctx->Yield(2);
    IM_CHECK(std::abs(volume() - source) < 1e-6);
    IM_CHECK(!ui::DerivedStale(*s_App, *dynamic_cast<const DerivedFeature*>(
                                           Doc()->GetFeature(0)))
                  .value_or(true));
    // One undo step gives the previous snapshot back.
    ctx->MenuClick("Edit/Undo");
    ctx->Yield(2);
    IM_CHECK(std::abs(volume() - followed) < 1e-6);
  };

  // A ghost belongs to the part that drew on it, and to NOTHING else: in an
  // assembly the thing it copies is already there, at its own occurrence.
  // Both were drawn, so the SO-101 gripper showed a second servo and a
  // second horn inside the real ones — a different colour, offset by the
  // copy's placement, and impossible to select apart.
  t = IM_REGISTER_TEST(engine, "derived", "a_ghost_is_not_drawn_in_assemblies");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ui::AddPart(*s_App, "Servo");
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    AddFixtureBox(ctx);
    const uint64_t servoId = s_App->parts[1].id;
    // Part 0 draws on a ghost of the servo AND has a body of its own.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    ToolbarCreateAccept(ctx, "###tb_derive");
    ctx->Yield(2);
    AddFixtureSphere(ctx);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    IM_CHECK(Doc()->GetBodies()[0].reference);
    IM_CHECK(!Doc()->GetBodies()[1].reference);
    IM_CHECK_EQ(s_App->gpu.documentMeshes.size(),
                (size_t)2); // its own tab: both

    // Placed in an assembly, only its own body is drawn — the servo is one
    // occurrence, not two.
    const uint64_t bracketId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(2);
    for (uint64_t partId : {bracketId, servoId}) {
      Occurrence o;
      o.name = "occ" + std::to_string(partId);
      o.partId = partId;
      o.fixed = true;
      s_App->assembly->AddOccurrence(o);
    }
    s_App->assembly->BumpVersion();
    ctx->Yield(3);
    // Two occurrences, one body each: the ghost adds nothing.
    IM_CHECK_EQ(s_App->gpu.documentMeshes.size(), (size_t)2);
    for (const ui::DocumentRenderMesh& m : s_App->gpu.documentMeshes)
      IM_CHECK(!s_App->meshRenderer->IsInstanceGhost(m.meshId));
  };

  // With nothing to derive from, the window says so and Accept is disabled
  // (the defaults gate pins the same case); once a second part exists the
  // source list lists it, and choosing it takes the snapshot.
  t = IM_REGISTER_TEST(engine, "derived", "source_list_takes_the_snapshot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ui::AddPart(*s_App, "Camera");
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    AddFixtureCylinder(ctx);
    ui::AddPart(*s_App, "Servo");
    ui::SetActivePart(*s_App, 2);
    ctx->Yield(2);
    AddFixtureBox(ctx);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    // Two candidates: the tool asks. Accept is disabled until one is picked.
    ToolbarClick(ctx, "###tb_derive");
    ctx->Yield(2);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ImGuiTestItemInfo accept = ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) != 0);
    const uint64_t servoId = s_App->parts[2].id;
    ctx->ItemClick(
        ("//###ToolWindow/###derive_src_" + std::to_string(servoId) + "_0")
            .c_str());
    ctx->Yield(2);
    accept = ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    const auto* d = dynamic_cast<const DerivedFeature*>(Doc()->GetFeature(0));
    IM_CHECK(d != nullptr);
    IM_CHECK_EQ(d->sourcePart, servoId);
    IM_CHECK_EQ(d->sourceName, std::string("Servo"));
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
  };

  // A part that is only a sketch (docs/modeling/derived.md#sketches): the
  // Derive tool offers its sketches, ticking one copies it, and "Sketch on
  // it" opens a sketch here on the copy's plane with all of it projected --
  // a profile, solver-fixed, following the other part.
  t = IM_REGISTER_TEST(engine, "derived", "a_copied_sketch_is_sketched_on");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ui::AddPart(*s_App, "Lid");
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    auto outline =
        std::make_unique<SketchFeature>("Outline", SketchPlane(PlaneType::XY));
    outline->AddEntity(sketch::Circle{{0.0, 0.0}, 4.0});
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(outline)), *Doc());
    const uint64_t outlineId = Doc()->GetFeature(0)->GetId();
    const uint64_t lidId = s_App->parts[1].id;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_derive");
    ctx->Yield(2);
    IM_CHECK(ui::HasPendingFeature(*s_App));
    ctx->ItemClick(
        ("//###ToolWindow/###derive_sketch_" + std::to_string(outlineId))
            .c_str());
    ctx->Yield(2);
    const ImGuiTestItemInfo accept =
        ctx->ItemInfo("//###ToolWindow/###tw_accept");
    IM_CHECK((accept.ItemFlags & ImGuiItemFlags_Disabled) == 0);
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield(2);
    const auto* d = dynamic_cast<const DerivedFeature*>(Doc()->GetFeature(0));
    IM_CHECK(d != nullptr);
    IM_CHECK_EQ(d->sourcePart, lidId);
    IM_CHECK_EQ(d->sketchSnapshot.size(), (size_t)1);
    IM_CHECK(Doc()->GetBodies().empty()); // a sketch is no body

    s_App->editFeatureId = d->GetId();
    ctx->Yield(2);
    ctx->ItemClick(
        ("//###ToolWindow/###derived_sketch_on_" + std::to_string(outlineId))
            .c_str());
    ctx->Yield(2);
    const uint64_t on = s_App->sketchEdit.activeSketchId;
    IM_CHECK(on != 0);
    const auto* sk =
        dynamic_cast<const SketchFeature*>(Doc()->FindFeatureById(on));
    IM_CHECK(sk != nullptr);
    IM_CHECK_EQ(sk->external.size(), (size_t)1);
    int profileCircles = 0;
    for (const sketch::Entity& e : sk->entities)
      if (const auto* c = std::get_if<sketch::Circle>(&e))
        profileCircles += !c->construction && std::abs(c->radius - 4.0) < 1e-9;
    IM_CHECK_EQ(profileCircles, 1);
    s_App->editFeatureId = 0;
  };

  // Opening a project off the frame
  // (docs/ui/long-operations.md#opening-a-project): a slow open puts the shared
  // progress bar up, Cancel leaves the project that was open exactly as it was,
  // and an open that had to rebuild parts says so until the project is saved.
  // The per-document delay makes "slow" true on any machine; the real cause is
  // baked geometry that cannot be used.
  t = IM_REGISTER_TEST(engine, "file",
                       "a_slow_open_shows_progress_and_cancels");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    namespace fs = std::filesystem;
    const fs::path two = fs::temp_directory_path() / "designer_open_two.dsgn";
    const fs::path one = fs::temp_directory_path() / "designer_open_one.dsgn";
    fs::remove_all(two);
    fs::remove_all(one);
    std::string err;

    // The project to open: two parts, saved WITHOUT its baked geometry.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    ui::AddPart(*s_App, "Second");
    ctx->Yield();
    IM_CHECK(ui::SaveProject(*s_App, two.string(), &err));
    for (const auto& e : fs::directory_iterator(two / "parts"))
      fs::remove_all(e.path() / "geometry");

    // The project that is open while it loads: one part.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    IM_CHECK(ui::SaveProject(*s_App, one.string(), &err));
    ui::MarkProjectSaved(*s_App, one.string(), "test");
    ctx->Yield();
    const size_t tabsBefore = s_App->parts.size();

    // Slow, then cancelled: nothing changes.
    ui::OpenJobOptions slow;
    slow.quietWaitMs = 0;
    slow.delayEachMs = 1500;
    IM_CHECK(ui::StartOpenProject(*s_App, two.string(), &err, slow));
    IM_CHECK(ui::OpenRunning(*s_App)); // a job, not a call
    const char* cancel = "//Opening project###open_project_modal/"
                         "Cancel###open_cancel";
    IM_CHECK(WaitFor(ctx, "the open modal's Cancel",
                     [&] { return ctx->ItemExists(cancel); }));
    ctx->ItemClick(cancel);
    IM_CHECK(WaitForOpen(ctx));
    IM_CHECK_EQ(s_App->projectPath, one.string());
    IM_CHECK_EQ(s_App->parts.size(), tabsBefore);
    IM_CHECK(s_App->openError.empty()); // a cancel is not a failure

    // Slow, and let it finish: the modal was up, then the project is open,
    // and it says it rebuilt both parts and why.
    slow.delayEachMs = 200;
    IM_CHECK(ui::StartOpenProject(*s_App, two.string(), &err, slow));
    bool sawModal = false;
    IM_CHECK(WaitFor(ctx, "the slow open", [&] {
      sawModal = sawModal || ctx->ItemExists(cancel);
      return !ui::OpenRunning(*s_App);
    }));
    IM_CHECK(sawModal);
    IM_CHECK_EQ(s_App->projectPath, two.string());
    IM_CHECK_EQ(s_App->parts.size(), (size_t)2);
    IM_CHECK_EQ(s_App->openRebuiltParts,
                1); // "Second" is empty: nothing to build
    IM_CHECK_EQ(s_App->openRebuildReason, std::string("no saved geometry"));

    // A save re-bakes, so the notice goes.
    IM_CHECK(ui::SaveProject(*s_App, two.string(), &err));
    ui::MarkProjectSaved(*s_App, two.string(), "test");
    IM_CHECK_EQ(s_App->openRebuiltParts, 0);

    // And a project that loads quickly opens at the end of the frame that
    // asked, with no modal at all -- the way every baked open has always
    // looked. "Quickly" is the quiet wait's to judge, and a loaded machine
    // can take longer than the default 300 ms over a read that is instant on
    // a quiet one -- when the modal is right. So the quiet wait here is one
    // no machine misses: what is asserted is what happens INSIDE it.
    ui::OpenJobOptions quick;
    quick.quietWaitMs = 30000;
    IM_CHECK(ui::StartOpenProject(*s_App, one.string(), &err, quick));
    // (Not ItemExists: the engine remembers the Cancel button from the
    // modals above after they close. Whether any popup is open is the state.)
    IM_CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    ctx->Yield();
    IM_CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    IM_CHECK(!ui::OpenRunning(*s_App));
    IM_CHECK_EQ(s_App->projectPath, one.string());
    IM_CHECK_EQ(s_App->openRebuiltParts, 0);

    fs::remove_all(two);
    fs::remove_all(one);
  };

  // The Requirements panel (docs/requirements/README.md#the-panel): it
  // appears when the project states a requirement, Check measures them as
  // a job, the states it shows are the evidence's, and an edit to the model
  // makes them stale until checked again.
  t = IM_REGISTER_TEST(engine, "requirements", "panel_checks_and_goes_stale");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // 2 x 2 x 2: 8 mm3
    ctx->Yield(2);
    // Not shown. (ImGui keeps the window of an earlier test that showed
    // the panel -- sim/a_probe_pins_a_face_and_holds_it does -- so "never
    // created" only held when this test ran first.)
    {
      const ImGuiWindow* w = ImGui::FindWindowByName(ui::kPanelRequirements);
      IM_CHECK(w == nullptr || !w->WasActive);
    }
    const uint64_t part = s_App->parts[s_App->activePart].id;
    const auto make = [part](uint64_t id, const char* name, double most) {
      req::Requirement r;
      r.id = id;
      r.name = name;
      r.measure = {{"kind", "volume"}, {"document", part}};
      r.bound = req::BoundFromJson({{"kind", "at_most"}, {"value", most}});
      return r;
    };
    s_App->requirements = {make(1, "fits the pocket", 10.0),
                           make(2, "fits the small pocket", 5.0)};
    s_App->nextRequirementId = 3;
    s_App->requirementsEdits++;
    ctx->Yield(3);
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelRequirements);
    IM_CHECK(panel != nullptr);
    IM_CHECK(panel->DockId != 0); // in the left dock, not over the viewport
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    const auto states = [] {
      const nlohmann::json rows =
          remote::RequirementRows(*s_App).at("requirements");
      return rows[0].at("state").get<std::string>() + "," +
             rows[1].at("state").get<std::string>();
    };
    IM_CHECK_STR_EQ(states().c_str(), "unchecked,unchecked");

    ctx->SetRef("//###Requirements");
    ctx->ItemClick("Check###req_check");
    IM_CHECK(WaitForJobs(ctx, "requirements"));
    ctx->Yield(2);
    IM_CHECK_STR_EQ(states().c_str(), "pass,fail");
    IM_CHECK(s_App->requirements[0].evidence.value.has_value());
    IM_CHECK(std::abs(*s_App->requirements[0].evidence.value - 8.0) < 1e-6);

    // The box grows: what was measured is not the model any more.
    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    const nlohmann::json before = DocumentIO::FeatureToJson(*box);
    box->width = 3.0;
    const nlohmann::json after = DocumentIO::FeatureToJson(*box);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield(2);
    IM_CHECK_STR_EQ(states().c_str(), "stale,stale");

    ctx->ItemClick("Check###req_check");
    IM_CHECK(WaitForJobs(ctx, "requirements"));
    ctx->Yield(2);
    IM_CHECK_STR_EQ(states().c_str(), "fail,fail"); // 12 mm3 now
    s_App->requirements.clear();
    s_App->requirementsEdits++;
    ctx->Yield(2);
  };

  // A requirement that does not pass says why in the panel
  // (docs/requirements/README.md#the-panel): a number against its bound, a
  // failed verdict's findings, an unresolved one's reason, each whole on a
  // wrapped line under its row, and the Value column fitting what it says
  // rather than cutting "findings" short.
  t = IM_REGISTER_TEST(engine, "requirements", "a_failure_says_why");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // 2 x 2 x 2: 8 mm3
    ctx->Yield(2);
    const uint64_t part = s_App->parts[s_App->activePart].id;
    // An assembly with no screw in it: check_fasteners examines none, which
    // is not a pass.
    const size_t asmIdx = ui::AddAssembly(*s_App, "Bare");
    const uint64_t bare = s_App->parts[asmIdx].id;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    const auto make = [](uint64_t id, const char* name, nlohmann::json m,
                         nlohmann::json bound) {
      req::Requirement r;
      r.id = id;
      r.name = name;
      r.measure = std::move(m);
      r.bound = req::BoundFromJson(bound);
      return r;
    };
    s_App->requirements = {
        make(1, "fits the small pocket",
             {{"kind", "volume"}, {"document", part}},
             {{"kind", "at_most"}, {"value", 5.0}}),
        make(2, "every screw holds",
             {{"kind", "check_fasteners"}, {"document", bare}},
             {{"kind", "pass"}}),
        make(3, "the other part's mass",
             {{"kind", "mass"}, {"document", part + 1000}},
             {{"kind", "at_most"}, {"value", 1.0}})};
    s_App->nextRequirementId = 4;
    s_App->requirementsEdits++;
    ctx->Yield(3);
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelRequirements);
    IM_CHECK(panel != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->SetRef("//###Requirements");
    ctx->ItemClick("Check###req_check");
    IM_CHECK(WaitForJobs(ctx, "requirements"));
    ctx->Yield(4); // the fitted columns settle on what they hold
    const nlohmann::json rows =
        remote::RequirementRows(*s_App).at("requirements");
    IM_CHECK_STR_EQ(rows[0].at("state").get<std::string>().c_str(), "fail");
    IM_CHECK_STR_EQ(rows[1].at("state").get<std::string>().c_str(), "fail");
    IM_CHECK_STR_EQ(rows[2].at("state").get<std::string>().c_str(),
                    "unresolved");

    const auto* volume = ui::RequirementRowDrawnForTest(1);
    IM_CHECK(volume != nullptr);
    IM_CHECK_STR_EQ(volume->finding.c_str(),
                    "measured 8 mm3, must be <= 5 mm3");
    const auto* screws = ui::RequirementRowDrawnForTest(2);
    IM_CHECK(screws != nullptr);
    IM_CHECK_STR_EQ(screws->value.c_str(), "findings");
    // The finding is the evidence's, whole: every line of it.
    std::string whole;
    for (const std::string& l : req::FindingLines(rows[1]))
      whole += (whole.empty() ? "" : "\n") + l;
    IM_CHECK_STR_EQ(screws->finding.c_str(), whole.c_str());
    IM_CHECK_STR_EQ(screws->finding.c_str(), "no fastener was examined");
    const auto* gone = ui::RequirementRowDrawnForTest(3);
    IM_CHECK(gone != nullptr);
    IM_CHECK(gone->finding.find("is gone") != std::string::npos);
    for (const auto* d : {volume, screws, gone}) {
      ctx->LogInfo("value '%s' %.0f of %.0f px; finding %.0f of %.0f px",
                   d->value.c_str(), d->valueTextWidth, d->valueCellWidth,
                   d->findingWidth, d->findingCellWidth);
      IM_CHECK_LE(d->valueTextWidth, d->valueCellWidth + 0.5f);
      IM_CHECK_LE(d->findingWidth, d->findingCellWidth + 0.5f);
    }

    // The name's tooltip carries the finding too.
    ctx->MouseMove("**/###req_row2");
    ctx->Yield(3);
    ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
    IM_CHECK(tip != nullptr && tip->Active);

    s_App->requirements.clear();
    s_App->requirementsEdits++;
    ctx->Yield(2);
  };

  // A check measures on a worker
  // (docs/requirements/README.md#checking-is-a-job): a slow clearance keeps
  // the frames coming, Cancel stops it and keeps what was measured, an edit
  // while it measures leaves the evidence stale (not a pass for a model
  // nobody measured), and a requirement removed meanwhile gets nothing. The
  // hold is in the measure itself, so a measure put back on the frame fails the
  // first part.
  t = IM_REGISTER_TEST(engine, "requirements", "a_slow_check_keeps_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx); // 2 x 2 x 2
    ctx->Yield(2);
    const uint64_t partId = s_App->parts[s_App->activePart].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Pair");
    ui::SetActivePart(*s_App, asmIdx);
    const uint64_t asmId = s_App->parts[asmIdx].id;
    Occurrence lo;
    lo.name = "Left";
    lo.partId = partId;
    lo.fixed = true;
    const uint64_t left = s_App->assembly->AddOccurrence(lo);
    Occurrence ro;
    ro.name = "Right";
    ro.partId = partId;
    ro.position = glm::dvec3(10.0, 0.0, 0.0); // 8 mm clear
    const uint64_t right = s_App->assembly->AddOccurrence(ro);
    ctx->Yield(3);
    req::Requirement r;
    r.id = 1;
    r.revision = 1;
    r.name = "the pair clears";
    r.measure = {{"kind", "clearance"},
                 {"document", asmId},
                 {"a", left},
                 {"b", right},
                 {"mode", "exact"}};
    r.bound = req::BoundFromJson({{"kind", "at_least"}, {"value", 1.0}});
    s_App->requirements = {r};
    s_App->nextRequirementId = 2;
    s_App->requirementsEdits++;
    ctx->Yield(3);
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelRequirements);
    IM_CHECK(panel != nullptr);
    ctx->ItemClick(panel->TabId);
    ctx->Yield(2);
    const auto state = [] {
      return remote::RequirementRows(*s_App)
          .at("requirements")[0]
          .at("state")
          .get<std::string>();
    };
    struct Hold {
      explicit Hold(int ms) { remote::SetTestRequirementHold(ms); }
      ~Hold() { remote::SetTestRequirementHold(0); }
    };
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point t0) {
      return std::chrono::duration<double>(Clock::now() - t0).count();
    };

    // Measured beside the frame: it holds 1.5 s, and no frame pays for it.
    {
      const Hold hold(1500);
      ctx->SetRef("//###Requirements");
      ctx->ItemClick("Check###req_check");
      const auto t0 = Clock::now();
      int frames = 0;
      double worstMs = 0.0;
      while (ui::BackgroundJobRunning(*s_App, "requirements")) {
        if (since(t0) > kWaitSeconds) {
          IM_ERRORF("the check ran %.0f s", since(t0));
          return;
        }
        ctx->Yield();
        worstMs = std::max(worstMs, LastFrameWorkCpuMs());
        frames++;
      }
      IM_CHECK(since(t0) > 1.4); // the hold was in the measure
      IM_CHECK_GE(frames, 5);
      IM_CHECK_LT(worstMs, 250.0);
    }
    ctx->Yield(2);
    IM_CHECK_STR_EQ(state().c_str(), "pass");
    IM_CHECK(s_App->requirements[0].evidence.value.has_value());
    IM_CHECK(std::abs(*s_App->requirements[0].evidence.value - 8.0) < 1e-6);

    // Cancel: the worker stops at its next look at the flag, not after
    // the 30 s hold, and what was measured stays.
    {
      const Hold hold(30000);
      remote::StartRequirementsCheck(*s_App, true);
      ctx->SetRef("//###Requirements");
      IM_CHECK(WaitFor(ctx, "the check's Cancel",
                       [&] { return ctx->ItemExists("###req_cancel"); }));
      const auto t0 = Clock::now();
      ctx->ItemClick("###req_cancel");
      IM_CHECK(WaitForJobs(ctx, "requirements"));
      IM_CHECK_LT(since(t0), 15.0);
    }
    IM_CHECK_STR_EQ(state().c_str(), "pass");
    IM_CHECK(std::abs(*s_App->requirements[0].evidence.value - 8.0) < 1e-6);

    // An edit while it measures: the reading lands, about the model it
    // copied, so the requirement reads stale.
    {
      const Hold hold(1500);
      remote::StartRequirementsCheck(*s_App, true);
      ctx->Yield(2);
      IM_CHECK(ui::BackgroundJobRunning(*s_App, "requirements"));
      Occurrence* moved = s_App->assembly->FindById(right);
      IM_CHECK(moved != nullptr);
      moved->position = glm::dvec3(20.0, 0.0, 0.0);
      IM_CHECK(WaitForJobs(ctx, "requirements"));
    }
    ctx->Yield(2);
    IM_CHECK_STR_EQ(state().c_str(), "stale");
    IM_CHECK(std::abs(*s_App->requirements[0].evidence.value - 8.0) < 1e-6);

    // Removed while it measures: the check ends, and nothing lands.
    {
      const Hold hold(1500);
      remote::StartRequirementsCheck(*s_App, true);
      ctx->Yield(2);
      IM_CHECK(ui::BackgroundJobRunning(*s_App, "requirements"));
      s_App->requirements.clear();
      s_App->requirementsEdits++;
      IM_CHECK(WaitForJobs(ctx, "requirements"));
    }
    IM_CHECK(s_App->requirements.empty());
    ctx->Yield(2);
  };

  // An expression requirement is measured where the values move
  // (docs/requirements/README.md#expression): a value typed into the
  // Variables panel turns its row red with its finding, no Check pressed.
  t = IM_REGISTER_TEST(engine, "requirements",
                       "an_expression_fails_as_you_type");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);
    IM_CHECK(ui::SetProjectVariable(*s_App, "seat_od", "22", ""));
    IM_CHECK(ui::SetProjectVariable(*s_App, "seat_bore", "15", ""));
    const nlohmann::json request{
        {"name", "wall round the seat"},
        {"measure",
         {{"kind", "expression"}, {"expression", "seat_od - seat_bore"}}},
        {"bound", {{"kind", "at_least"}, {"value", 4}}}};
    const remote::RpcParams add{request}; // holds a reference to it
    const uint64_t id =
        remote::HandleRequirementsRpc(*s_App, "add_requirement", add)
            ->at("id")
            .get<uint64_t>();
    ctx->Yield(3);
    const auto rowState = [id] {
      // Named: iterating RequirementRows(...).at(...) directly outlives the
      // temporary on a compiler before P2718 (GCC 13).
      const nlohmann::json rows = remote::RequirementRows(*s_App);
      for (const nlohmann::json& r : rows.at("requirements"))
        if (r.at("id") == id)
          return r.at("state").get<std::string>();
      return std::string("missing");
    };
    IM_CHECK_STR_EQ(rowState().c_str(), "pass");
    const auto showPanel = [&](const char* title) {
      ImGuiWindow* w = ImGui::FindWindowByName(title);
      IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
      ctx->ItemClick(w->TabId);
      ctx->Yield(2);
    };
    showPanel(ui::kPanelVariables);
    ctx->ItemInputValue(
        "//###Variables/###vars_table/###var_expr_project_seat_bore", "19");
    ctx->Yield(3);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "requirements")); // no Check
    showPanel(ui::kPanelRequirements);
    ctx->Yield(2);
    const auto* drawn = ui::RequirementRowDrawnForTest(id);
    IM_CHECK(drawn != nullptr);
    IM_CHECK_STR_EQ(drawn->value.c_str(), "3 mm");
    IM_CHECK_STR_EQ(
        drawn->finding.c_str(),
        "seat_od - seat_bore = 3 mm (seat_bore 19, seat_od 22), must be >= 4 "
        "mm");
    // Put back from the panel: green again, still without a Check.
    showPanel(ui::kPanelVariables);
    ctx->ItemInputValue(
        "//###Variables/###vars_table/###var_expr_project_seat_bore", "15");
    ctx->Yield(3);
    IM_CHECK_STR_EQ(rowState().c_str(), "pass");
    s_App->requirements.clear();
    s_App->requirementsEdits++;
    showPanel(ui::kPanelModel);
  };

  // The design sweep by clicking (docs/requirements/README.md#design-sweep):
  // Sweep... on a variable's row opens it on half to twice the value, Run
  // finds where the fillet breaks and says so above the table, the failing
  // row in the fail colour; and Cancel on a held sweep ends it at once with
  // the frames still coming and the points measured kept.
  t = IM_REGISTER_TEST(engine, "requirements", "a_sweep_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield(2);
    IM_CHECK(ui::SetProjectVariable(*s_App, "t", "10", "mm, the wall"));
    IM_CHECK(ui::SetProjectVariable(*s_App, "r", "3", "mm, its edges"));
    auto box = std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 20.0,
                                            10.0, 20.0);
    box->heightExpr = "t";
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(box)), *Doc());
    IM_CHECK(Doc()->GetBodies().size() == 1);
    const Body& body = Doc()->GetBodies()[0];
    std::vector<ElementRef> edges;
    const size_t n = body.shape.GenerateEdgePolylines().size();
    for (size_t e = 0; e < n; e++)
      if (const std::string* name =
              body.elementMap.EdgeNameOf(static_cast<int>(e)))
        edges.push_back({body.createdBy, *name});
    auto fillet = std::make_unique<FilletFeature>("Edges round", edges, 3.0);
    fillet->radiusExpr = "r";
    s_App->commands->Execute(
        std::make_unique<AddFeatureCommand>(std::move(fillet)), *Doc());
    IM_CHECK(Doc()->GetFeature(1)->GetError().empty());
    ctx->Yield(2);
    const uint64_t version = Doc()->Version();

    ImGuiWindow* vars = ImGui::FindWindowByName(ui::kPanelVariables);
    IM_CHECK_SILENT(vars != nullptr && vars->DockNode != nullptr);
    ctx->ItemClick(vars->TabId);
    ctx->Yield(2);
    ctx->ItemClick("//###Variables/###vars_table/###var_sweep_project_t");
    ctx->Yield(3);
    const ui::DesignSweepDrawn& drawn = ui::DesignSweepDrawnForTest();
    IM_CHECK(drawn.open);
    // 5 .. 20: half to twice t, derived from it.
    IM_CHECK_STR_EQ(drawn.planned.c_str(), "5 points planned, cap 64");
    ctx->SetRef("//###DesignSweep");
    ctx->ItemClick("Run###sweep_run");
    ctx->Yield();
    IM_CHECK(WaitForJobs(ctx, "design_sweep"));
    ctx->Yield(3);
    IM_CHECK_EQ(drawn.points, 5);
    IM_CHECK_EQ(drawn.failing, 1); // t = 5: thinner than two 3 mm fillets
    IM_CHECK(drawn.firstFailures.size() == 1);
    IM_CHECK(drawn.firstFailures[0].starts_with(
        "t below: fails at 5 (good at 8.75): "));
    IM_CHECK(drawn.firstFailures[0].find("Edges round") != std::string::npos);
    IM_CHECK(drawn.firstFailingWhy.find("Edges round") != std::string::npos);
    IM_CHECK_EQ(Doc()->Version(), version); // copies, not the document
    IM_CHECK(s_App->projectSymbols.at("t") == 10.0);

    // A held sweep: Cancel ends it at once, the frames keep coming, and
    // what was measured stays.
    {
      struct Hold {
        explicit Hold(int ms) { remote::SetTestSweepHold(ms); }
        ~Hold() { remote::SetTestSweepHold(0); }
      } hold(30000);
      ctx->ItemClick("Run###sweep_run");
      IM_CHECK(WaitFor(ctx, "the sweep's Cancel",
                       [&] { return ctx->ItemExists("###sweep_cancel"); }));
      const auto t0 = std::chrono::steady_clock::now();
      ctx->ItemClick("###sweep_cancel");
      IM_CHECK(WaitForJobs(ctx, "design_sweep"));
      IM_CHECK_LT(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count(),
          15.0);
    }
    ctx->Yield(3);
    IM_CHECK(drawn.cancelled);
    IM_CHECK_EQ(Doc()->Version(), version);
    ctx->WindowClose("//###DesignSweep");
    ctx->Yield(2);
    IM_CHECK(!ui::DesignSweepDrawnForTest().open);
  };
}
