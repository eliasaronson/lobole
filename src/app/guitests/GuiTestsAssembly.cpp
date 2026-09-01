#include "app/guitests/GuiTestsCommon.h"
#include "ui/AppActionsInternal.h" // PartRenderCacheStore (the resync cache)
#include "ui/AssemblyActions.h"
#include "ui/ImportJob.h"

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
  // Instancing reaches the OUTLINES too: three placements of one part are
  // three instances of ONE outline asset, not three uploads of the same
  // vertex and index buffers plus three identical adjacency builds. That was
  // 3.2 ms per placement and most of an assembly resync — 394 ms of 670 on
  // the 98-instance dock. Counting assets is the honest check: a picture can
  // look right while the work behind it is done N times.
  t = IM_REGISTER_TEST(engine, "assembly", "outlines_are_instanced_per_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Repeats");
    ui::SetActivePart(*s_App, asmIdx);
    IM_CHECK(s_App->outlineRenderer != nullptr);
    for (int i = 0; i < 3; ++i) {
      Occurrence o;
      o.name = "Copy " + std::to_string(i + 1);
      o.partId = partId;
      o.fixed = true;
      o.position = glm::vec3(60.0f * static_cast<float>(i), 0.0f, 0.0f);
      s_App->assembly->AddOccurrence(o);
    }
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->documentMeshes.size(), (size_t)3);
    IM_CHECK_EQ(s_App->outlineRenderer->MeshCount(), (size_t)3);
    IM_CHECK_EQ(s_App->outlineRenderer->AssetCount(), (size_t)1);

    // Hiding one drops its instance and keeps the shared asset alive for the
    // two that remain.
    s_App->assembly->GetOccurrences()[0].visible = false;
    s_App->assembly->BumpVersion();
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->outlineRenderer->MeshCount(), (size_t)2);
    IM_CHECK_EQ(s_App->outlineRenderer->AssetCount(), (size_t)1);

    // Hiding the LAST one drops every instance — and deliberately KEEPS the
    // asset, held by AppState::partRenderCache so unhiding is instant rather
    // than a re-tessellation. That retention is the whole point of the cache;
    // it used to be released here, which is why hiding one occurrence of a
    // 299-part import cost 5.9 s.
    for (Occurrence& o : s_App->assembly->GetOccurrences())
      o.visible = false;
    s_App->assembly->BumpVersion();
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->outlineRenderer->MeshCount(), (size_t)0);
    IM_CHECK_EQ(s_App->outlineRenderer->AssetCount(), (size_t)1);

    // The refcount still has to balance, or "retained" would just be a leak
    // with a nicer name: dropping the cache frees it.
    ui::ReleasePartRenderCache(*s_App);
    IM_CHECK_EQ(s_App->outlineRenderer->AssetCount(), (size_t)0);
  };

  // Switching away from an assembly tab and back must NOT rebuild it.
  //
  // A resync removes every instance before re-adding them, and an asset used
  // to die with its last instance — so every return to the tab re-tessellated
  // and re-uploaded every part. On a 299-part STEP import that was 5.9 s of
  // frozen window, every single time, and the same 5.9 s for hiding one
  // occurrence. AppState::partRenderCache holds the tessellation and RETAINS
  // both renderer assets across the teardown.
  //
  // Asset ids are the honest witness: they are never reused (a released slot
  // is left null and CreateAsset appends), so an id that survives the round
  // trip proves the upload was not redone. Counting assets would not — a
  // destroy-then-recreate lands back on the same count.
  t = IM_REGISTER_TEST(engine, "assembly",
                       "switching_back_to_an_assembly_reuses_its_meshes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Reuse");
    ui::SetActivePart(*s_App, asmIdx);
    for (int i = 0; i < 2; ++i) {
      Occurrence o;
      o.name = "Copy " + std::to_string(i + 1);
      o.partId = partId;
      o.fixed = true;
      o.position = glm::vec3(60.0f * static_cast<float>(i), 0.0f, 0.0f);
      s_App->assembly->AddOccurrence(o);
    }
    ctx->Yield(3);
    IM_CHECK(s_App->partRenderCache != nullptr);
    auto entry = s_App->partRenderCache->parts.find(partId);
    IM_CHECK(entry != s_App->partRenderCache->parts.end());
    IM_CHECK(!entry->second.bodies.empty());
    const uint32_t meshAsset = entry->second.bodies[0].assetId;
    const uint32_t outlineAsset = entry->second.bodies[0].outlineAssetId;
    IM_CHECK(meshAsset != 0);
    IM_CHECK(outlineAsset != 0);

    // Away to the part tab and back — the full teardown/rebuild path.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(3);
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(3);
    entry = s_App->partRenderCache->parts.find(partId);
    IM_CHECK(entry != s_App->partRenderCache->parts.end());
    IM_CHECK(!entry->second.bodies.empty());
    IM_CHECK_EQ(entry->second.bodies[0].assetId, meshAsset);
    IM_CHECK_EQ(entry->second.bodies[0].outlineAssetId, outlineAsset);
    IM_CHECK_EQ(s_App->documentMeshes.size(), (size_t)2);
    IM_CHECK_EQ(s_App->outlineRenderer->AssetCount(), (size_t)1);

    // Editing the part DOES invalidate it: the cache is keyed on the part
    // document's version, so stale geometry can never survive a regen.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(3);
    entry = s_App->partRenderCache->parts.find(partId);
    IM_CHECK(entry != s_App->partRenderCache->parts.end());
    IM_CHECK(!entry->second.bodies.empty());
    IM_CHECK(entry->second.bodies[0].assetId != meshAsset);
  };

  // A STEP import runs on a worker thread behind a progress modal.
  //
  // The whole read used to happen inline: a 48 MB / 299-product file froze
  // the window for ~38 s with nothing on screen to say why. The parse is also
  // what DECIDES part-vs-assembly, so that decision had to move onto the
  // worker with it — which is the part worth a test, because a wrong answer
  // here silently imports an assembly as one dumb solid.
  t = IM_REGISTER_TEST(engine, "assembly", "a_step_import_runs_in_background");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Author a 2-product, 3-instance STEP with our own writer — no binary
    // fixture, and it exercises both directions (the I2 round-trip rule).
    CADKernel& k = CADKernel::Instance();
    CADKernel::ImportedModel model;
    // Enough products that the read spans many frames. A 2-product file
    // finishes inside one, which would make the modal assertion below a race
    // — and a test that sometimes catches the bug is not a gate.
    constexpr int kProducts = 40;
    for (int i = 0; i < kProducts; i++) {
      const bool box = (i % 2) == 0;
      model.products.push_back(
          {box ? k.CreateBox(2.0, 2.0, 2.0)
               : k.CreateCylinder(glm::vec3(0), glm::vec3(0, 0, 1), 0.5, 3.0),
           (box ? "Widget " : "Pin ") + std::to_string(i), false,
           glm::vec3(0.0f)});
      model.instances.push_back(
          {static_cast<size_t>(i),
           glm::translate(glm::mat4(1.0f),
                          glm::vec3(10.0f * static_cast<float>(i), 0, 0)),
           "I" + std::to_string(i)});
    }
    // One product placed twice, so the import is a real assembly rather than
    // a flat list — and the occurrence count differs from the part count.
    model.instances.push_back(
        {0, glm::translate(glm::mat4(1.0f), glm::vec3(0, 10, 0)), "W0 again"});
    const auto stepPath =
        std::filesystem::temp_directory_path() / "designer_gui_import.step";
    IM_CHECK(k.SaveSTEPStructured(stepPath.string(), model, "Rig"));

    const size_t tabsBefore = s_App->parts.size();
    std::string error;
    IM_CHECK(ui::StartImport(*s_App, stepPath.string(), &error));
    // It is a JOB, not a call: nothing has been created yet and the modal is
    // up. (If this ever fails because the import already finished, the import
    // stopped being asynchronous — which is the regression.)
    IM_CHECK(ui::ImportRunning(*s_App));
    IM_CHECK_EQ(s_App->parts.size(), tabsBefore);

    // PollImport runs from the frame loop; yield until it lands. Bounded so a
    // hang fails the test rather than wedging the suite. The modal must be up
    // for the duration — it is drawn by DockLayout rather than a toolbar
    // precisely so it survives the part->assembly tab switch that
    // materialization performs halfway through.
    bool sawModal = false;
    for (int i = 0; i < 2000 && ui::ImportRunning(*s_App); i++) {
      ctx->Yield();
      if (ctx->ItemExists("//Importing STEP/Cancel###import_cancel"))
        sawModal = true;
    }
    IM_CHECK(sawModal);
    IM_CHECK(!ui::ImportRunning(*s_App));
    IM_CHECK(s_App->importError.empty());

    // A part tab per product plus the assembly, and one more occurrence than
    // parts — i.e. the worker decided "assembly", not "one dumb solid".
    IM_CHECK_EQ(s_App->parts.size(), tabsBefore + kProducts + 1);
    IM_CHECK(s_App->assembly != nullptr);
    IM_CHECK_EQ(s_App->assembly->OccurrenceCount(), (size_t)(kProducts + 1));
    std::filesystem::remove(stepPath);
  };

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

  // U13d: a row too long for the dock elides and can still be READ. ImGui
  // clips rather than elides, so the left panel used to cut names mid-word
  // with nothing to say it had happened — `Gangway  [assembly]  (fixed` is
  // the real one, off the Marsliden dock at the default 218 px split.
  //
  // The name has to survive the elision as an ID, too: SelectableEllipsis
  // splits the "###occ_N" off and re-appends it, so the row a script or a
  // test addresses is the same row at every panel width.
  t = IM_REGISTER_TEST(engine, "assembly",
                       "a_long_row_elides_and_names_itself");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t partId = s_App->parts[0].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Wide");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence o;
    o.name = "A gangway hinge bracket nobody could fit in a dock panel";
    o.partId = partId;
    o.fixed = true;
    const uint64_t occId = s_App->assembly->AddOccurrence(o);
    ctx->Yield(4);

    // Addressable by id, elision or not.
    const std::string ref = "//###Model/###occ_" + std::to_string(occId);
    ImGuiTestItemInfo row = ctx->ItemInfo(ref.c_str());
    IM_CHECK(row.ID != 0);

    // It fits the panel: the whole point. A clipped Selectable would report
    // the same rect, so measure the TEXT that would be drawn instead.
    ImGuiWindow* panel = ImGui::FindWindowByName(ui::kPanelModel);
    IM_CHECK(panel != nullptr);
    const float full = ImGui::CalcTextSize(o.name.c_str()).x;
    ctx->LogInfo("name wants %.0f px, panel inner is %.0f px", full,
                 panel->InnerRect.GetWidth());
    IM_CHECK(full > panel->InnerRect.GetWidth()); // the case is real

    // Hovering it gives the whole name back.
    ctx->MouseMove(ref.c_str());
    ctx->Yield(3);
    ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
    IM_CHECK(tip != nullptr);
    IM_CHECK(tip->Active);
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

  // Right-clicking a row goes to what it references. The "Open"/"Edit"
  // button only exists for the SELECTED occurrence and lives below the list,
  // so reaching a part meant select-then-scroll; the row itself is where the
  // hand already is. The menu names the KIND it will open, since a
  // sub-assembly row and a part row take you to different sorts of tab.
  t = IM_REGISTER_TEST(engine, "assembly", "right_click_a_row_opens_its_doc");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const uint64_t partId = s_App->parts[0].id;

    const size_t unitIdx = ui::AddAssembly(*s_App, "Unit");
    ui::SetActivePart(*s_App, unitIdx);
    Occurrence inner;
    inner.name = "Inner";
    inner.partId = partId;
    inner.fixed = true;
    const uint64_t innerId = s_App->assembly->AddOccurrence(inner);

    const size_t topIdx = ui::AddAssembly(*s_App, "Top");
    ui::SetActivePart(*s_App, topIdx);
    Occurrence sub;
    sub.name = "Unit 1";
    sub.partId = s_App->parts[unitIdx].id;
    sub.refKind = OccurrenceRef::Assembly;
    sub.fixed = true;
    const uint64_t subId = s_App->assembly->AddOccurrence(sub);
    ctx->Yield(4);

    // A sub-assembly row: the item says "assembly", and lands on its tab.
    const std::string subRef = "//###Model/###occ_" + std::to_string(subId);
    ctx->ItemClick(subRef.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    IM_CHECK(ctx->ItemExists("//$FOCUSED/Open assembly###occ_open"));
    ctx->ItemClick("//$FOCUSED/###occ_open");
    ctx->Yield(3);
    IM_CHECK_EQ((size_t)s_App->activePart, unitIdx);

    // ...and inside it, a PART row lands on the part tab.
    const std::string innerRef = "//###Model/###occ_" + std::to_string(innerId);
    ctx->ItemClick(innerRef.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    IM_CHECK(ctx->ItemExists("//$FOCUSED/Open part###occ_open"));
    ctx->ItemClick("//$FOCUSED/###occ_open");
    ctx->Yield(3);
    IM_CHECK_EQ((size_t)s_App->activePart, (size_t)0);
    IM_CHECK(s_App->parts[s_App->activePart].id == partId);
    IM_CHECK(s_App->assembly == nullptr); // a part tab, not the assembly
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
    IM_CHECK(!s_App->mateTool.hover.anchor.snapEdge2.IsSet()); // not a corner

    // A CORNER of the top face (phase 4c). It outranks the midpoints and the
    // centroid because it is nearer, and it is named by BOTH edges meeting
    // there — a vertex has no name of its own in the ElementMap, so a single
    // edge could not say which end of itself was meant.
    const glm::vec3 corner(max.x, max.y, max.z);
    ctx->MouseMoveToPos(WorldToWindow(corner));
    ctx->Yield();
    IM_CHECK(s_App->mateTool.hasHover);
    IM_CHECK(s_App->mateTool.hoverSnapIsCorner);
    IM_CHECK(s_App->mateTool.hover.anchor.snapEdge.IsSet());
    IM_CHECK(s_App->mateTool.hover.anchor.snapEdge2.IsSet());
    IM_CHECK(s_App->mateTool.hover.anchor.snapEdge.name !=
             s_App->mateTool.hover.anchor.snapEdge2.name);
    // The frame really lands ON the corner, which is the point of all this.
    {
      const auto f = ui::ConnectorWorldFrame(*s_App, s_App->mateTool.hover);
      IM_CHECK(f.has_value());
      IM_CHECK_LE(glm::distance(glm::vec3((*f)[3]), corner), 1e-3f);
    }

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
  // Removing an occurrence, by right-clicking the thing you want gone.
  //
  // Reported from use 2026-09-01: "there is no option to delete something in
  // an assembly that I can see". There was one — a button in the "Selected
  // occurrence" editor, below the list, reachable only after selecting a row
  // and off the bottom of the panel on any real assembly. Clicked here on the
  // ROW, which is where a person looks, and clicked rather than called
  // because the complaint was that it could not be found.
  t = IM_REGISTER_TEST(engine, "assembly",
                       "an_occurrence_is_deleted_from_its_"
                       "own_row");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield(2);
    const uint64_t partId = s_App->parts[0].id;

    ui::SetActivePart(*s_App, ui::AddAssembly(*s_App, "Rig"));
    for (int i = 0; i < 2; ++i) {
      Occurrence o;
      o.name = "Block " + std::to_string(i + 1);
      o.partId = partId;
      o.position = glm::vec3(static_cast<float>(i) * 40.0f, 0.0f, 0.0f);
      s_App->assembly->AddOccurrence(o);
    }
    ctx->Yield(4);
    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), (size_t)2);
    const uint64_t doomed = s_App->assembly->GetOccurrences().front().id;

    const std::string row = "//###Model/###occ_" + std::to_string(doomed);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield(2);
    ctx->ItemClick("//$FOCUSED/Delete occurrence###occ_delete");
    ctx->Yield(3);

    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), (size_t)1);
    IM_CHECK(s_App->assembly->FindById(doomed) == nullptr);
  };

  // Ctrl-click several rows, press Delete, and they all go.
  //
  // The selection has been a SET since U1d and the viewport has ctrl-clicked
  // into it all along; the LIST was the half that still forced one at a time,
  // so removing four parts was four right-clicks. Delete is wired in the
  // modal-key chain rather than the keymap on purpose (Keymap.h: Escape,
  // Enter, Delete and the arrows resolve against armed tools and text fields
  // across ~15 sites), which is why it is PRESSED here against a real
  // assembly tab rather than dispatched as a command.
  t = IM_REGISTER_TEST(engine, "assembly",
                       "ctrl_click_selects_several_and_"
                       "delete_removes_them");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield(2);
    const uint64_t partId = s_App->parts[0].id;

    ui::SetActivePart(*s_App, ui::AddAssembly(*s_App, "Rig"));
    std::vector<uint64_t> ids;
    for (int i = 0; i < 4; ++i) {
      Occurrence o;
      o.name = "Block " + std::to_string(i + 1);
      o.partId = partId;
      o.position = glm::vec3(static_cast<float>(i) * 40.0f, 0.0f, 0.0f);
      ids.push_back(s_App->assembly->AddOccurrence(o));
    }
    ctx->Yield(4);
    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), (size_t)4);

    const auto row = [&](size_t i) {
      return "//###Model/###occ_" + std::to_string(ids[i]);
    };
    ctx->ItemClick(row(0).c_str());
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedOccurrenceIds(*s_App).size(), (size_t)1);

    // Ctrl-click ADDS rather than replacing.
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->ItemClick(row(1).c_str());
    ctx->Yield();
    ctx->ItemClick(row(2).c_str());
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedOccurrenceIds(*s_App).size(), (size_t)3);
    // And removes: ctrl-clicking a selected row takes it back out.
    ctx->ItemClick(row(2).c_str());
    ctx->Yield();
    ctx->KeyUp(ImGuiMod_Ctrl);
    IM_CHECK_EQ(ui::SelectedOccurrenceIds(*s_App).size(), (size_t)2);

    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), (size_t)2);
    IM_CHECK(s_App->assembly->FindById(ids[0]) == nullptr);
    IM_CHECK(s_App->assembly->FindById(ids[1]) == nullptr);
    IM_CHECK(s_App->assembly->FindById(ids[2]) != nullptr);

    // One snapshot for the batch: undoing a two-part deletion is one Ctrl+Z.
    IM_CHECK(ui::UndoAssembly(*s_App));
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), (size_t)4);
  };

  // A fastener is placed by clicking a hole's rim (docs/parts/gui-plan.md).
  //
  // The whole verb: arm from the toolbar, click the rim in the viewport, and
  // the size and the length derive from what was clicked. Clicked all the way
  // through — including the rim, at a point computed from the fixture's own
  // geometry — because the thing being tested is that a person can reach it.
  t = IM_REGISTER_TEST(engine, "parts",
                       "a_fastener_is_placed_by_picking_a_rim");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();

    // A 60 x 60 x 10 plate with a 6.6 bore — an M6 normal clearance, which is
    // what makes the size derivable. Built through the document API: the
    // fixture is not what is under test.
    Document& doc = *Doc();
    doc.AddFeature(std::make_unique<BoxFeature>(
        "Plate", glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, 10.0));
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Bore", glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0, 0, 1), 3.3, 12.0));
    doc.AddFeature(std::make_unique<BooleanFeature>(
        "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
        BooleanOp::Subtract));
    doc.GetFeature(2)->SetMaterial("steel");
    doc.Regenerate();
    ctx->Yield(2);
    const uint64_t partId = s_App->parts[0].id;

    const size_t asmIdx = ui::AddAssembly(*s_App, "Joint");
    ui::SetActivePart(*s_App, asmIdx);
    for (int i = 0; i < 2; ++i) {
      Occurrence o;
      o.name = "Plate " + std::to_string(i + 1);
      o.partId = partId;
      o.fixed = true;
      o.position = glm::vec3(0.0f, 0.0f, i == 0 ? 0.0f : -10.0f);
      s_App->assembly->AddOccurrence(o);
    }
    ctx->Yield(4);
    const size_t before = s_App->assembly->GetOccurrences().size();
    IM_CHECK_EQ(before, (size_t)2);

    // Look at the top rim from an ANGLE, not down the bore. Head-on, all four
    // rims in the stack project onto the same circle and the picker can hand
    // back the far one — which is a real ambiguity, recorded in
    // known-bugs.md, and not what this test is about.
    const glm::vec3 rim(3.3f, 0.0f, 10.0f); // a point ON the top rim circle
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(rim);
    s_App->camera->SetUp(glm::vec3(0, 0, 1));
    s_App->camera->SetPosition(rim + glm::vec3(26.0f, -26.0f, 26.0f));
    ctx->Yield(3);

    // Arm it from the toolbar, and with nothing picked Place must be REFUSED
    // rather than absent or silently inert (U10) — the same rule that caught
    // File > Save doing nothing on this very tab.
    ToolbarClick(ctx, "###tb_fastener");
    ctx->Yield(2);
    IM_CHECK(s_App->fastenerTool.open);
    {
      const ImGuiTestItemInfo place =
          ctx->ItemInfo("//Fastener###ToolWindow/###fastener_place");
      IM_CHECK((place.ItemFlags & ImGuiItemFlags_Disabled) != 0);
    }

    // The rim, in the viewport.
    ctx->MouseMoveToPos(WorldToWindow(rim));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(3);
    IM_CHECK(s_App->fastenerTool.rimElement.IsSet());
    IM_CHECK(s_App->fastenerTool.rimOccurrence != 0);
    IM_CHECK(s_App->fastenerTool.ok);
    // 20 mm of plate, and 20 is a length an M6 is made in.
    IM_CHECK_STR_EQ(s_App->fastenerTool.summary.substr(0, 6).c_str(), "M6x20 ");

    // The joint options are addressable — buttons and checkboxes, never a
    // bare combo, which gathers with an empty label and cannot be reached.
    ctx->ItemClick("//Fastener###ToolWindow/###fastener_nut");
    ctx->Yield(2);
    IM_CHECK(s_App->fastenerTool.ok);
    IM_CHECK(s_App->fastenerTool.summary.find("nut") != std::string::npos);
    ctx->ItemClick("//Fastener###ToolWindow/###fastener_std_iso7380");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(s_App->fastenerTool.standard.c_str(), "iso7380");

    // The GHOST: a translucent joint in the hole BEFORE anything is placed,
    // because "ISO 7380" says nothing about what you are getting. Its count
    // tracks what is TICKED, which is the assertion that catches a preview
    // showing a bare screw while the checkboxes promise washers and a nut.
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->fastenerTool.plan.size(), (size_t)2); // screw + nut
    IM_CHECK_EQ(s_App->fastenerGhostInstances.size(),
                s_App->fastenerTool.plan.size());
    ctx->ItemClick("//Fastener###ToolWindow/###fastener_washer_head");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->fastenerTool.plan.size(), (size_t)3);
    IM_CHECK_EQ(s_App->fastenerGhostInstances.size(), (size_t)3);
    ctx->ItemClick("//Fastener###ToolWindow/###fastener_washer_head");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->fastenerGhostInstances.size(), (size_t)2);

    ctx->ItemClick("//Fastener###ToolWindow/###fastener_place");
    ctx->Yield(4);
    // Placing clears the pick and re-arms, so the preview goes with it —
    // a ghost still hanging in the hole next to the real screw would read
    // as a double-placement.
    IM_CHECK(s_App->fastenerGhostInstances.empty());
    // The screw and its nut, and the session is STILL OPEN with the rim
    // cleared and the pick re-armed: holes come in patterns.
    IM_CHECK_EQ(s_App->assembly->GetOccurrences().size(), before + 2);
    IM_CHECK(s_App->fastenerTool.open);
    IM_CHECK(!s_App->fastenerTool.rimElement.IsSet());
    IM_CHECK_EQ(s_App->fastenerTool.placed, 1);

    ctx->ItemClick("//Fastener###ToolWindow/###fastener_done");
    ctx->Yield(2);
    IM_CHECK(!s_App->fastenerTool.open);
    // And it goes when the session does — a ghost that outlives its tool
    // haunts whatever document you switch to next.
    ctx->Yield(2);
    IM_CHECK(s_App->fastenerGhostInstances.empty());

    // And the other half: a stack that moved under a placed screw has to be
    // ASKED about, because deriving a length from other documents' geometry
    // cannot be a live binding (A2). Until this button existed the question
    // could only be put over the script API, which means in practice nobody
    // put it.
    const auto showPanel = [&](const char* title) {
      ImGuiWindow* w = ImGui::FindWindowByName(title);
      IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
      ctx->ItemClick(w->TabId);
      ctx->Yield(2);
    };
    showPanel(ui::kPanelModel);
    ctx->ItemClick("//###Model/Bill of materials###bom_header");
    ctx->Yield(2);
    ctx->ItemClick("//###Model/Check fasteners###bom_check_fasteners");
    ctx->Yield(2);
    IM_CHECK(s_App->fastenerChecked);
    IM_CHECK(s_App->fastenerChecks.empty()); // nothing has moved yet

    for (Occurrence& o : s_App->assembly->GetOccurrences())
      if (o.name == "Plate 2")
        o.position.z -= 15.0f;
    s_App->assembly->BumpVersion();
    ctx->Yield(3);
    ctx->ItemClick("//###Model/Check fasteners###bom_check_fasteners");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->fastenerChecks.size(), (size_t)1);
    IM_CHECK(s_App->fastenerChecks[0].find("too short") != std::string::npos);
  };

  // A project variable is edited from an assembly tab, by clicking.
  //
  // Reported 2026-09-01: File > Save did nothing on an assembly tab because
  // it guarded on `state.document`, which is NULL there. The Variables panel
  // had the same guard and the same shape of bug — project variables live on
  // the AppState, are undoable across tabs and drive every part, and the
  // panel went blank on two of the three kinds of tab with no reason given.
  // Clicked rather than called, because the whole defect was that the widget
  // was not drawn.
  t = IM_REGISTER_TEST(engine, "variables",
                       "project_scope_is_editable_on_an_"
                       "assembly_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();

    const auto showPanel = [&](const char* title) {
      ImGuiWindow* w = ImGui::FindWindowByName(title);
      IM_CHECK_SILENT(w != nullptr && w->DockNode != nullptr);
      ctx->ItemClick(w->TabId);
      ctx->Yield(2);
    };

    // Onto an assembly tab, where state.document is null by construction.
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(2);
    IM_CHECK(s_App->document == nullptr);

    // The panel is not blank: its one button works and the row it creates is
    // addressable and editable, from here.
    showPanel(ui::kPanelVariables);
    ctx->ItemClick("//###Variables/+ Add variable");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->projectParameters.size(), (size_t)1);
    ctx->ItemInputValue("//###Variables/###vars_table/###var_name_project_var1",
                        "span");
    ctx->Yield(2);
    ctx->ItemInputValue("//###Variables/###vars_table/###var_expr_project_span",
                        "42");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->projectParameters.size(), (size_t)1);
    IM_CHECK_STR_EQ(s_App->projectParameters[0].name.c_str(), "span");
    IM_CHECK_STR_EQ(s_App->projectParameters[0].expression.c_str(), "42");

    showPanel(ui::kPanelModel); // or the next test starts on the wrong tab
  };

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
