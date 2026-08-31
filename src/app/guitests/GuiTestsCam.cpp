#include "app/guitests/GuiTestsCommon.h"

#ifdef HAVE_CAM
#include "render/StockRenderer.h"
#include "ui/CamPlayback.h"
#endif

// Cam GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterCamGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
#ifdef HAVE_CAM
  t = IM_REGISTER_TEST(engine, "cam", "new_document_takes_the_job_with_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    cam::Operation op;
    op.id = Doc()->AllocateId();
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    Doc()->FindCamSetup(setup.id)->operations.push_back(op);
    ctx->Yield(2);
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f) {
          ui::SelectionItem top;
          top.kind = ui::SelectionItem::Kind::Face;
          top.element = ElementRef{b.createdBy, name};
          ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top");
          ui::CamFacePicked(*s_App, top);
        }
      }
    ctx->Yield(2);
    IM_CHECK(Doc()->FindCamSetup(setup.id)->topFace.IsSet());
    IM_CHECK(!s_App->camToolpaths.empty());
    // Now throw the document away.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ctx->Yield();
    ctx->MenuClick("File/New");
    ctx->Yield(3);
    IM_CHECK(Doc()->GetCamSetups().empty()); // the whole job went with it
    IM_CHECK(s_App->camToolpaths.empty());
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK_EQ(s_App->camActiveSetup, (uint64_t)0);
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0);
  };

  t = IM_REGISTER_TEST(engine, "cam", "pick_top_face_sets_z_zero");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    // Says so rather than assuming 0 — and says it on the SETUP, so a job
    // reopened from its own file still knows where Z zero came from.
    IM_CHECK(!Doc()->FindCamSetup(setup.id)->topFace.IsSet());

    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ctx->Yield();
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);

    // A SIDE face used to be refused here. It is not any more: calling a face
    // the top is the statement that the part is clamped with it up, so the
    // pick clamps it and takes Z zero from it — one click where the refusal
    // cost three. It must SAY that it re-clamped.
    const Body& body = Doc()->GetBodies()[0];
    ui::SelectionItem side;
    side.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.5f) {
        side.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    if (side.element.IsSet()) {
      ui::CamFacePicked(*s_App, side);
      ctx->Yield(2);
      IM_CHECK(s_App->slotPick.miss.empty());
      IM_CHECK(Doc()->FindCamSetup(setup.id)->topFace.IsSet());
      IM_CHECK(!cam::FrameIsSquare(*Doc()->FindCamSetup(setup.id)));
      IM_CHECK(!s_App->camNote.empty()); // "laid the part flat on that face"
    }

    // …and picking the plate's own top face puts it back, and sets Z zero.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::SelectionItem top;
    top.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && n.z > 0.99f) {
        top.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    IM_CHECK(top.element.IsSet());
    ui::CamFacePicked(*s_App, top);
    ctx->Yield(2);
    IM_CHECK(Doc()->FindCamSetup(setup.id)->topFace.IsSet());
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0); // slot disarmed
    const cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK(std::abs(live->origin.z - 12.0) < 1e-6); // the plate's top
    // The pick says where Z zero is, and NOTHING about how much material sits
    // above the part — it used to zero that, which threw away an allowance
    // already typed and made "Face the top" refuse right after the pick meant
    // to set the job up (docs/cam/dogfood-round.md, round 2).
    IM_CHECK(std::abs(live->stock.margin.z - 1.0) < 1e-9);
    const cam::DerivedOp face = cam::DeriveOperation(
        *live, Doc()->GetBodies(), cam::OpType::Face, ElementRef{});
    IM_CHECK(face.ok); // so the first verb on a rough board just works
  };

  // No two visible widgets may share an ImGuiID. ImGui detects this itself
  // and says so in a popup — which is a popup the USER gets, not a test
  // failure, so nothing caught "Save G-code" and the post-dialect combo both
  // answering to ###cam_post for as long as they both existed. It only became
  // visible when the button moved above the operations list and the two were
  // on screen at once (docs/cam/dogfood-round.md).
  //
  // ui_items cannot see this: GatherItems is keyed BY id, so a collision
  // shows up as one entry, not two. ImGui's own detection is the check.
  t = IM_REGISTER_TEST(engine, "cam", "panel_widgets_have_distinct_ids");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    // A job with a toolpath, so the whole panel draws — Save G-code only
    // appears once something has been generated.
    cam::Operation op;
    op.id = Doc()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    op.stepdown = 4.0;
    Doc()->FindCamSetup(setup.id)->operations.push_back(op);
    ui::RefreshCamToolpaths(*s_App);
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());

    IM_CHECK(ImGui::FindWindowByName("CAM###CamPanel") != nullptr);
    ctx->WindowFocus("//CAM###CamPanel");
    ctx->Yield(4);

    // Both widgets must be REACHABLE. Two items sharing an id is invisible to
    // a listing — the test engine gathers by id, so a collision shows up as
    // one entry rather than two, which is why nothing noticed. Asking for
    // each of them by name is the check that does not depend on ImGui's own
    // detection, which only fires while the mouse happens to be over one of
    // the pair.
    ImGuiWindow* panel = ImGui::FindWindowByName("CAM###CamPanel");
    IM_CHECK(panel != nullptr);
    const ImGuiID postCombo = panel->GetID("###cam_post");
    const ImGuiID saveButton = panel->GetID("###cam_save_gcode");
    IM_CHECK(postCombo != saveButton);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "//CAM###CamPanel");
    bool sawCombo = false, sawSave = false;
    for (const ImGuiTestItemInfo& item : items) {
      sawCombo |= item.ID == postCombo;
      sawSave |= item.ID == saveButton;
    }
    IM_CHECK(sawCombo); // the post dialect…
    IM_CHECK(sawSave);  // …and Save G-code, as two separate widgets
  };

  // An operation can be removed from the window that makes them. Removing was
  // only possible in the docked panel, which is a tab behind Model — while
  // every operation is created in the floating verb window. Adding three by
  // mistake (because the first two silently cut nothing) and being unable to
  // take any of them back is how a job ends up with duplicates in it
  // (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "an_operation_can_be_removed_where_made");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    for (int i = 0; i < 2; i++) {
      cam::Operation op;
      op.id = Doc()->AllocateId();
      op.name = "Op " + std::to_string(i);
      op.type = cam::OpType::Profile;
      op.topZ = 13.0;
      op.bottomZ = -1.0;
      Doc()->FindCamSetup(setup.id)->operations.push_back(op);
    }
    s_App->camToolOpen = true;
    ctx->Yield(4);
    IM_CHECK_EQ(Doc()->FindCamSetup(setup.id)->operations.size(), (size_t)2);

    const uint64_t first = Doc()->FindCamSetup(setup.id)->operations[0].id;
    // The row PushID's the operation's id as an int, which "$$<n>" addresses.
    const std::string ref = "//CAM###ToolWindow/$$" +
                            std::to_string(static_cast<int>(first)) +
                            "/###rmop";
    ctx->ItemClick(ref.c_str());
    ctx->Yield(3);
    const cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK(live->operations[0].id != first);
    s_App->camToolOpen = false;
  };

  // A tool session belongs to the document it is editing. File > New used to
  // leave every one of them open on a document that no longer existed —
  // including a feature-editor session naming a feature that had just been
  // discarded. The one that bit was camToolOpen: it is a TOGGLE, so a stale
  // one turned OFF at the next click on the button meant to turn it on, and
  // the failure surfaced in an unrelated test (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "fit_keeps_the_model_clear_of_the_tool");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    s_App->camToolOpen = true;
    ctx->Yield(4);
    ImGuiWindow* win = ImGui::FindWindowByName("CAM###ToolWindow");
    IM_CHECK(win != nullptr);
    ui::FitCameraToDocument(*s_App);
    ctx->Yield(4);

    // Where the part's centre lands on screen. It must sit left of the
    // window's left edge, not under it.
    const glm::vec2 screen =
        ui::WorldToScreen(*s_App, glm::vec3(30.0f, 20.0f, 6.0f));
    ctx->LogInfo("part centre at x=%.0f, window starts at x=%.0f", screen.x,
                 win->Pos.x);
    IM_CHECK(screen.x < win->Pos.x);
    s_App->camToolOpen = false;
  };

  // The floating tool window FOLLOWS the viewport. Anchoring it once, on the
  // appearing frame, placed it against a size AlwaysAutoResize had not
  // computed yet, and any later layout change left it stranded over the
  // middle of the model it is asking you to click (dogfooding round 2).
  t = IM_REGISTER_TEST(engine, "cam", "tool_window_follows_the_viewport");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    s_App->camToolOpen = true;
    ctx->Yield(4); // one frame to size it, one to place it against that size

    const auto rightEdge = [&]() {
      ImGuiWindow* w = ImGui::FindWindowByName("CAM###ToolWindow");
      return w != nullptr ? w->Pos.x + w->Size.x : -1.0f;
    };
    const float want = s_App->viewportPos.x + s_App->viewportSize.x - 12.0f;
    IM_CHECK(rightEdge() > 0.0f);
    IM_CHECK(std::abs(rightEdge() - want) < 2.0f);

    // Now move the viewport under it: the terminal panel docks into the
    // central node, so turning it on genuinely narrows the 3D view. The
    // window must come with it rather than stay where it was.
    const bool hadTerminal = s_App->settings.terminalPanel;
    s_App->settings.terminalPanel = true;
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(8);
    const float narrowed = s_App->viewportPos.x + s_App->viewportSize.x - 12.0f;
    IM_CHECK(narrowed < want - 10.0f); // the viewport really did move
    IM_CHECK(std::abs(rightEdge() - narrowed) < 2.0f);

    s_App->settings.terminalPanel = hadTerminal;
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(8);
    IM_CHECK(std::abs(rightEdge() - (s_App->viewportPos.x +
                                     s_App->viewportSize.x - 12.0f)) < 2.0f);
    s_App->camToolOpen = false;
  };

  // The view a machinist works in. The app's up is +Y and CAM's is +Z, so the
  // plan view — down the spindle — is filed under "Front" in Standard Views
  // and the entry called "Top" shows the part edge-on. CAM offers the view it
  // means, in the frame THIS setup cuts in (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "look_down_the_spindle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    // 3-axis: the spindle points down the world's -Z, so the camera looks
    // that way whatever it was doing before.
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ctx->ItemClick("//CAM###ToolWindow/###cam_view_spindle");
    ctx->Yield(150); // AlignToFace animates; wait for it to land
    glm::vec3 fwd = glm::normalize(s_App->camera->GetForward());
    IM_CHECK(std::abs(fwd.z + 1.0f) < 0.02f); // looking along -Z
    IM_CHECK(std::abs(fwd.x) < 0.02f);
    IM_CHECK(std::abs(fwd.y) < 0.02f);

    // Indexed: the table has turned 90 degrees about X, so the spindle now
    // looks down the part's +Y. Verifying against the WORLD axes here would
    // pass for a job cut in the wrong frame.
    Doc()->FindCamSetup(setup.id)->rotationA = 90.0;
    ctx->Yield(2);
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ctx->ItemClick("//CAM###ToolWindow/###cam_view_spindle");
    ctx->Yield(150);
    fwd = glm::normalize(s_App->camera->GetForward());
    const glm::dvec3 spindle = cam::FromSetupSpace(
        *Doc()->FindCamSetup(setup.id), glm::dvec3(0, 0, 1));
    const glm::vec3 want = -glm::normalize(glm::vec3(spindle));
    IM_CHECK(glm::length(fwd - want) < 0.03f);
    s_App->camToolOpen = false; // the toolbar button TOGGLES: do not leak it
  };

  // Picking a face as "the top" IS the statement that the part is clamped
  // with it up, so the pick lays it up rather than refusing. And a verb stays
  // armed after it lands: one verb is almost never one face.
  t = IM_REGISTER_TEST(engine, "cam", "top_face_clamps_and_verbs_stay_armed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Upright panel again: its big faces are the ones worth machining and
    // none of them is level as modelled.
    Doc()->AddFeature(std::make_unique<BoxFeature>("Baffle", glm::vec3(0, 0, 0),
                                                   210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    const Body& body = Doc()->GetBodies()[0];
    ui::SelectionItem big;
    big.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && n.y < -0.99f)
        big.element = ElementRef{body.createdBy, name};
    }
    IM_CHECK(big.element.IsSet());

    // ONE pick: it clamps the part flat AND takes Z zero from that face. This
    // used to be a refusal the user had to translate into two more clicks.
    IM_CHECK(cam::FrameIsSquare(*Doc()->FindCamSetup(setup.id)));
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    const cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(s_App->slotPick.miss.empty());
    IM_CHECK(!cam::FrameIsSquare(*live)); // laid flat by the same click
    IM_CHECK(live->topFace.IsSet());      // …and Z zero came from it
    IM_CHECK(!s_App->camNote.empty());    // and it SAID so

    // Picking the same face again is idempotent: no second re-clamp.
    const glm::dquat after = live->frame;
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(glm::length(live->frame - after) < 1e-9);

    // A verb stays armed after it lands, so a part with several holes is
    // several clicks and not several verb-presses.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamPocketFloor);
    IM_CHECK(!s_App->slotPick.prompt.empty()); // the pill survives the pick

    // …and a second face lands a second operation with no verb press.
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    IM_CHECK_EQ(Doc()->FindCamSetup(setup.id)->operations.size(), (size_t)2);
    s_App->slotPick = {};
    s_App->camToolOpen = false;
  };

  // Where the CUTTER points, asserted against what the RENDERER holds rather
  // than against the caller's arithmetic. Reported three times from the real
  // job as "the cutter is not perpendicular to the cutting surface — it almost
  // follows the camera", and twice defended with a screenshot, which is not
  // evidence about a matrix. `GetInstanceTransform` exists for exactly this.
  t = IM_REGISTER_TEST(engine, "cam", "tool_glyph_points_down_the_spindle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A panel standing on edge, so the fixture frame is a real rotation and
    // not the identity every other fixture in this file happens to have.
    Doc()->AddFeature(std::make_unique<BoxFeature>("Baffle", glm::vec3(0, 0, 0),
                                                   210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    const Body& body = Doc()->GetBodies()[0];
    ElementRef big;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && n.y < -0.99f)
        big = ElementRef{body.createdBy, name};
    }
    IM_CHECK(big.IsSet());
    {
      cam::Setup* live = Doc()->FindCamSetup(setup.id);
      cam::Tool t1;
      t1.number = 1;
      t1.name = "8 mm";
      t1.diameter = 8.0;
      t1.feedCut = 1200.0;
      t1.feedPlunge = 400.0;
      t1.spindleRpm = 18000.0;
      live->tools = {t1};
      live->frame = cam::SolveFaceUp({0.0, -1.0, 0.0}, live->frame);
      IM_CHECK(!cam::FrameIsSquare(*live));
      live->safeZ = cam::DeriveSafeZ(*live, Doc()->GetBodies());
      const cam::DerivedOp derived = cam::DeriveOperation(
          *live, Doc()->GetBodies(), cam::OpType::Profile, {});
      IM_CHECK(derived.ok);
      cam::Operation op = derived.op;
      op.id = Doc()->AllocateId();
      live->operations.push_back(op);
    }
    ui::RefreshCamToolpaths(*s_App);
    ctx->Yield(3);
    IM_CHECK(!s_App->camToolpaths.empty());

    IM_CHECK(ui::BeginCamPlayback(*s_App));
    ui::SeekCamPlayback(*s_App, s_App->camTimeline.End());
    ctx->Yield(4); // SyncCamPlayback runs per frame
    IM_CHECK(s_App->camToolInstance != 0);
    IM_CHECK(s_App->meshRenderer != nullptr);

    const std::optional<glm::mat4> m =
        s_App->meshRenderer->GetInstanceTransform(s_App->camToolInstance);
    IM_CHECK(m.has_value());
    // The glyph is revolved about ITS OWN +Z (CamStockMesh::BuildToolMesh),
    // so column 2 of the model matrix is where the tool points in the world.
    // It must be the spindle: the setup frame applied to +Z.
    const cam::Setup* live = Doc()->FindCamSetup(setup.id);
    const glm::dvec3 spindle =
        cam::SetupToModel(*live) * glm::dvec3(0.0, 0.0, 1.0);
    const glm::vec3 drawn = glm::normalize(glm::vec3((*m)[2]));
    const glm::vec3 want = glm::normalize(glm::vec3(spindle));
    ctx->LogInfo("drawn  %.4f %.4f %.4f", drawn.x, drawn.y, drawn.z);
    ctx->LogInfo("spindle %.4f %.4f %.4f", want.x, want.y, want.z);
    IM_CHECK(glm::length(drawn - want) < 1e-4f);
    // …and it is a pure rotation: no scale, no shear, nothing view-derived.
    for (int c = 0; c < 3; c++)
      IM_CHECK(std::abs(glm::length(glm::vec3((*m)[c])) - 1.0f) < 1e-4f);
    IM_CHECK(std::abs((*m)[3][3] - 1.0f) < 1e-6f);

    ui::EndCamPlayback(*s_App);
    ctx->Yield(2);
  };

  // W2/W5 (docs/cam/stock-and-origin.md): the corner the operator touches off
  // on, chosen by clicking, and honoured by the file. Before v68 `origin` was
  // a label the post never read, so "Z zero at 16.000" was a claim no line of
  // the g-code kept.
  t = IM_REGISTER_TEST(engine, "cam", "work_origin_is_where_it_says");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    // A fresh setup already has an origin, DERIVED — not (0,0,0), which is a
    // literal sitting where the defaults rule wants a derivation.
    cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(cam::RecomputeOrigin(*live, Doc()->GetBodies()));
    glm::dvec3 lo(0.0), hi(0.0);
    IM_CHECK(cam::StockBounds(*live, Doc()->GetBodies(), lo, hi));
    IM_CHECK(std::abs(live->origin.x - lo.x) < 1e-9);
    IM_CHECK(std::abs(live->origin.y - lo.y) < 1e-9);
    IM_CHECK(std::abs(live->origin.z - hi.z) < 1e-9);

    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_stock"); // open Stock and safety
    ctx->Yield(2);

    // Move zero to the stock's centre by clicking, and the origin follows.
    Doc()->FindCamSetup(setup.id)->originAt = cam::OriginAt::StockCentre;
    IM_CHECK(cam::RecomputeOrigin(*Doc()->FindCamSetup(setup.id),
                                  Doc()->GetBodies()));
    ctx->Yield(2);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(std::abs(live->origin.x - (lo.x + hi.x) * 0.5) < 1e-6);

    // And the FILE measures from it: post a real operation and read the
    // extremes back. This is the assertion that fails if the origin goes back
    // to being a label.
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    ctx->ItemClick("###cam_generate");
    ctx->Yield(5);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(!live->operations.empty());
    if (!s_App->camToolpaths.empty()) {
      std::string gcode, error;
      IM_CHECK(cam::PostProcess(live->post, *live, s_App->camToolpaths,
                                ui::PostOpts(*live), gcode, &error));
      const cam::GProgram file = cam::ParseGcode(gcode);
      const cam::GProgram back = cam::ProgramInSetupSpace(gcode, *live);
      IM_CHECK(file.moves.size() == back.moves.size());
      bool sawOffset = false;
      for (size_t i = 0; i < file.moves.size(); i++)
        if (!file.moves[i].rapid &&
            std::abs(back.moves[i].to.x - file.moves[i].to.x) > 1e-6)
          sawOffset = true;
      IM_CHECK(sawOffset); // the file is NOT in model coordinates any more
    }

    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // Which cutter an operation uses, and in what order they cut. Both were
  // unreachable by clicking: `toolNumber` was READ to print a diameter and
  // never written, so "use a cutter of 5.46 mm or less" was a refusal naming
  // a remedy the panel could not perform; and the operations list had no
  // reorder at all, though the order is the cutting sequence.
  t = IM_REGISTER_TEST(engine, "cam", "operation_tool_and_order");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    {
      cam::Tool big;
      big.number = 1;
      big.name = "6 mm flat";
      big.diameter = 6.0;
      cam::Tool small = big;
      small.number = 2;
      small.name = "3 mm flat";
      small.diameter = 3.0;
      cam::Setup* live = Doc()->FindCamSetup(setup.id);
      live->tools = {big, small};
      for (int i = 0; i < 2; i++) {
        cam::Operation op;
        op.id = Doc()->AllocateId();
        op.name = i == 0 ? "First" : "Second";
        op.type = cam::OpType::Profile;
        op.toolNumber = 1;
        op.topZ = 13.0;
        op.bottomZ = -1.0;
        op.stepdown = cam::DerivedStepdown(big); // nobody has stated one
        live->operations.push_back(op);
      }
    }
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    cam::Setup* live = Doc()->FindCamSetup(setup.id);
    const uint64_t firstId = live->operations[0].id;
    const uint64_t secondId = live->operations[1].id;

    // Point the first operation at the smaller cutter, by clicking.
    const std::string adv =
        "**/$$" + std::to_string(firstId) + "/Advanced###adv";
    ctx->ItemClick(adv.c_str());
    ctx->Yield(2);
    // Only one Advanced is open, so exactly one T2 button is visible.
    ctx->ItemClick("**/###optool_t2");
    ctx->Yield(3);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK_EQ(live->operations[0].toolNumber, 2);
    // And the stepdown followed the cutter: half of 3, not half of 6. A pass
    // derived for a 6 mm tool is a full-diameter plunge for a 3 mm one.
    IM_CHECK(std::abs(live->operations[0].stepdown - 1.5) < 1e-6);
    // The one nobody retooled is untouched.
    IM_CHECK_EQ(live->operations[1].toolNumber, 1);
    IM_CHECK(std::abs(live->operations[1].stepdown - 3.0) < 1e-6);

    // Now drag the second operation above the first.
    const std::string dragA = "**/$$" + std::to_string(firstId) + "/###drag";
    const std::string dragB = "**/$$" + std::to_string(secondId) + "/###drag";
    ctx->ItemDragAndDrop(dragB.c_str(), dragA.c_str());
    ctx->Yield(3);
    live = Doc()->FindCamSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)2);
    IM_CHECK_EQ(live->operations[0].id, secondId);
    IM_CHECK_EQ(live->operations[1].id, firstId);

    // Hand the shared dock node back before anything can fail below.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // S2 (docs/cam/setup-frame.md): the case every other fixture in this file
  // dodges by being modelled face-up. A panel that stands upright — an
  // imported speaker baffle, and every part that arrives from STEP or Onshape
  // in the orientation it has in the assembly — has no level face worth
  // zeroing on. Laying one flat is a statement about the FIXTURE, so it must
  // work on a 3-axis machine and put no A word in the file.
  t = IM_REGISTER_TEST(engine, "cam", "upright_panel_lays_flat");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // 210 wide, 16 THICK, 400 tall: standing on edge, like a real baffle.
    Doc()->AddFeature(std::make_unique<BoxFeature>("Baffle", glm::vec3(0, 0, 0),
                                                   210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    // Find the big face — the one anyone would machine. Its normal is along
    // Y, so no amount of 2.5D can touch it as the part is modelled.
    const Body& body = Doc()->GetBodies()[0];
    ui::SelectionItem big;
    big.kind = ui::SelectionItem::Kind::Face;
    double best = 0.0;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (!body.shape.FacePlane(index, o, n, u))
        continue;
      const double area = std::abs(n.y); // the two Y-facing faces are the big
      if (area > best) {                 // ones on a panel this shape
        best = area;
        big.element = ElementRef{body.createdBy, name};
      }
    }
    IM_CHECK(big.element.IsSet());

    // Clamp it with that face up, EXPLICITLY. Picking it as the top face does
    // the same thing in one click (top_face_clamps_and_verbs_stay_armed);
    // this path is the one that re-clamps WITHOUT re-answering Z zero, which
    // is what a flip job needs. No rotary axis is declared, and none is
    // needed: a clamp can hold a part any way round.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamLayFaceUp, "Click the face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    const cam::Setup* laid = Doc()->FindCamSetup(setup.id);
    IM_CHECK(!cam::FrameIsSquare(*laid));
    IM_CHECK(!laid->hasRotaryAxis);
    IM_CHECK(s_App->slotPick.miss.empty());

    // And now the SAME face is the top face, with a Z zero on it.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    IM_CHECK(s_App->slotPick.miss.empty());
    IM_CHECK(Doc()->FindCamSetup(setup.id)->topFace.IsSet());

    // A real operation on it, and a file with no A word in it.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(5);
    const cam::Setup* job = Doc()->FindCamSetup(setup.id);
    IM_CHECK(!job->operations.empty());
    IM_CHECK(!s_App->camToolpaths.empty());
    for (const cam::Toolpath& tp : s_App->camToolpaths)
      for (const cam::Move& m : tp.moves)
        IM_CHECK(!m.hasA);
    s_App->camToolOpen = false; // the toolbar button TOGGLES: do not leak it
  };

  // G2-G4 (docs/cam/gui.md): the verbs. Two clicks — the verb, then the face
  // — produce a machinable operation with nothing typed, the viewport shows
  // what will be removed, and the verdict arrives without being asked for.
  // The CAM window is usable on a part that has no CAM job — which is the
  // only state it can be in the first time anyone opens it. The machining
  // verbs already made a setup on demand; the two picks in the Zero section
  // answered "add a setup first" into a field this window does not draw, so
  // pressing "Pick top face" on a fresh part and clicking a face did nothing
  // at all, visibly or otherwise (docs/cam/dogfood-round.md).
  t = IM_REGISTER_TEST(engine, "cam", "the_first_pick_makes_the_setup");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    IM_CHECK(Doc()->GetCamSetups().empty());

    // Opening the window is not an edit: look and close, and the document is
    // as it was.
    ToolbarClick(ctx, "###tb_cam");
    ctx->Yield(3);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(Doc()->GetCamSetups().empty());

    // The first pick is. (The viewport click routes here through
    // SlotPickClicked; calling it directly keeps the test off the camera.)
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ctx->Yield();
    ui::SelectionItem top;
    top.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f)
          top.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(top.element.IsSet());
    ui::CamFacePicked(*s_App, top);
    ctx->Yield(3);

    IM_CHECK_EQ(Doc()->GetCamSetups().size(), (size_t)1);
    const cam::Setup& made = Doc()->GetCamSetups()[0];
    IM_CHECK(made.topFace.IsSet());
    IM_CHECK(std::abs(made.origin.z - 12.0) < 1e-6); // the plate's top
    IM_CHECK(s_App->slotPick.miss.empty());          // it took, not refused
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "verbs_two_clicks_to_a_pocket");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A plate with a pocket cleared from its top down to z = 12.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 12), 30, 20, 20));
    const uint64_t voidId = Doc()->GetFeature(1)->GetId();
    Doc()->AddFeature(std::make_unique<BooleanFeature>("Pocket", plate, voidId,
                                                       BooleanOp::Subtract));
    ctx->Yield(2);

    // Click 1: the verb. The toolbar opens the session; no setup exists yet,
    // and the verb makes one rather than sending the user to the panel.
    ToolbarClick(ctx, "###tb_cam");
    ctx->Yield(2);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(Doc()->GetCamSetups().empty());
    ctx->ItemClick("//CAM###ToolWindow/###cam_verb_pocket");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamPocketFloor);

    // Click 2: the face. (The viewport click routes here through
    // SlotPickClicked; calling it directly keeps the test off the camera.)
    // First the post's barrel, which must be refused in words rather than
    // silently turned into a depth. Since R10 a vertical cylinder IS a wall
    // (that is how round openings are cut), so the refusal is the pocket
    // wall-guard's "click the pocket's FLOOR", not the old "curved".
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Post", glm::vec3(200, 0, 0), glm::vec3(0, 0, 1), 5.0, 10.0));
    ctx->Yield(2);
    for (const Body& b : Doc()->GetBodies()) {
      if (b.createdBy != Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetId())
        continue;
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u))
          continue; // planar: not the barrel
        ui::SelectionItem curved;
        curved.kind = ui::SelectionItem::Kind::Face;
        curved.element = ElementRef{b.createdBy, name};
        ui::CamFacePicked(*s_App, curved);
        IM_CHECK(!s_App->slotPick.miss.empty());
        IM_CHECK(s_App->slotPick.miss.find("FLOOR") != std::string::npos);
        break;
      }
      break;
    }
    // The slot stays armed through a refusal: the next click is almost always
    // the face they meant, and re-pressing the verb first is a wasted step.
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);

    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - 12.0f) < 1e-4f)
          floor.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(floor.element.IsSet());
    ui::CamFacePicked(*s_App, floor);
    ctx->Yield(2);

    // An operation with every depth derived: the floor is the face, the top
    // is the stock, the stepdown is half the cutter. Nothing was typed.
    const cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    const cam::Operation& op = live->operations[0];
    // 9 mm deep with a 6 mm cutter, so the verb chose adaptive clearing —
    // one verb, the strategy a consequence of the geometry (C8). Without the
    // LGPL library this build clears it concentrically instead.
    IM_CHECK(op.type == (cam::AdaptiveAvailable() ? cam::OpType::Adaptive
                                                  : cam::OpType::Pocket));
    IM_CHECK(std::abs(op.bottomZ - 12.0) < 1e-4);
    IM_CHECK(op.topZ > op.bottomZ);
    IM_CHECK_EQ(op.targets.size(), (size_t)1);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
    // STILL armed on success: one verb is almost never one face, so the slot
    // survives the pick and the next click is the next hole. It used to
    // disarm, which cost a verb press per hole. Escape or the verb again puts
    // it away — `top_face_clamps_and_verbs_stay_armed` gates the loop.
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    s_App->slotPick = {}; // and this test is done with it

    // G4: the verdict arrived by itself, in words — and for a job that cuts
    // the part correctly it says nothing ALARMING. The panel checks on a
    // 0.4 mm grid and the RPC defaulted to 0.25, and a cell centre landing on
    // the corner of an island counted the island's whole height as a gouge:
    // "Cuts 6.00 mm INTO the part — that is scrap" about a part the RPC's own
    // check called clean (docs/cam/dogfood-round.md, round 2). Leftover is a
    // WARN and expected here — nothing has cut the outside yet.
    IM_CHECK(!s_App->camVerdict.empty());
    for (const cam::VerdictLine& line : s_App->camVerdict) {
      IM_CHECK_SILENT(line.level != cam::VerdictLine::Level::Bad);
      if (line.level == cam::VerdictLine::Level::Bad)
        ctx->LogError("verdict: %s", line.text.c_str());
    }

    // G3: the picture. While the session is open the stock box and the
    // removed-volume ghost are drawn; the ghost's walls are translucent
    // surfaces, so they land in the quad channel.
    std::vector<PrimVertex> prims;
    std::vector<LineVertex3D> quads;
    ui::AppendCamVisualisation(*s_App, prims, quads);
    IM_CHECK(!prims.empty());
    IM_CHECK(!quads.empty());
    // The ghost spans the cut, top to bottom — it is the material coming off,
    // not an outline of the floor.
    float lowest = 1e9f, highest = -1e9f;
    for (const LineVertex3D& v : quads) {
      lowest = std::min(lowest, v.position.z);
      highest = std::max(highest, v.position.z);
    }
    IM_CHECK(lowest <= (float)op.bottomZ + 1e-3f);
    IM_CHECK(highest >= (float)op.topZ - 1e-3f);

    // Closing the session drops the decision-time drawings and keeps the
    // backplot: the job survives the window that made it.
    ctx->ItemClick("//CAM###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolOpen);
    std::vector<PrimVertex> afterPrims;
    std::vector<LineVertex3D> afterQuads;
    ui::AppendCamVisualisation(*s_App, afterPrims, afterQuads);
    IM_CHECK(afterQuads.empty()); // no stock box, no ghost
    std::vector<PrimVertex> backplot;
    ui::AppendCamToolpathOverlay(*s_App, backplot);
    IM_CHECK(!backplot.empty());
  };

  // R15: the same two clicks for the third verb, and the refusal that keeps
  // it honest — a flat end mill cannot break an edge, and saying so beats
  // ploughing a square step where a chamfer was asked for.
  t = IM_REGISTER_TEST(engine, "cam", "verbs_two_clicks_to_an_edge_break");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_cam");
    ctx->Yield(2);
    ctx->ItemClick("//CAM###ToolWindow/###cam_verb_edge");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamEdgeWall);

    // A wall of the plate — any vertical face.
    ui::SelectionItem wall;
    wall.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.01f)
          wall.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(wall.element.IsSet());

    // The setup arrives with a 6 mm FLAT tool, so the first pick refuses in
    // words and names the two tools that would work.
    ui::CamFacePicked(*s_App, wall);
    ctx->Yield(2);
    IM_CHECK(!s_App->slotPick.miss.empty());
    IM_CHECK(s_App->slotPick.miss.find("ball") != std::string::npos);
    cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    IM_CHECK(live->operations.empty()); // refused, not half-made

    // Make it a ball nose and pick again: one operation, every number
    // derived from the wall and the tool.
    live->tools[0].type = cam::ToolType::Ball;
    ctx->Yield();
    ui::CamFacePicked(*s_App, wall);
    ctx->Yield(2);
    live = ui::ActiveCamSetup(*s_App);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    const cam::Operation& op = live->operations[0];
    IM_CHECK(op.type == cam::OpType::EdgeRound);
    IM_CHECK(std::abs(op.topZ - 20.0) < 1e-4);    // the wall's own top
    IM_CHECK(std::abs(op.edgeSize - 3.0) < 1e-4); // the ball's radius
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
    s_App->camToolOpen = false;
  };

  // The cached toolpaths are now maintained ONE OPERATION AT A TIME (adding,
  // deleting and editing each touch their own entry), so the property that
  // makes the cache safe is that it always equals a full pass. Assert exactly
  // that after each kind of edit — a stale entry is the failure this class of
  // optimisation invites.
  t = IM_REGISTER_TEST(engine, "cam", "incremental_toolpaths_never_go_stale");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 14), 30, 20, 20));
    const uint64_t voidId = Doc()->GetFeature(1)->GetId();
    Doc()->AddFeature(std::make_unique<BooleanFeature>("Pocket", plate, voidId,
                                                       BooleanOp::Subtract));
    ctx->Yield(2);

    // Same shape as the full pass: same operations, same order, same moves.
    const auto matchesFullPass = [&](const char* when) {
      const cam::Setup* live = ui::ActiveCamSetup(*s_App);
      IM_CHECK(live != nullptr);
      const std::vector<cam::Toolpath> full =
          cam::GenerateAll(*live, Doc()->GetBodies());
      ctx->LogDebug("%s: cached %d, full %d", when,
                    (int)s_App->camToolpaths.size(), (int)full.size());
      IM_CHECK_EQ(s_App->camToolpaths.size(), full.size());
      for (size_t i = 0; i < full.size(); i++) {
        IM_CHECK_EQ(s_App->camToolpaths[i].operationId, full[i].operationId);
        IM_CHECK_EQ(s_App->camToolpaths[i].moves.size(), full[i].moves.size());
        IM_CHECK(std::abs(s_App->camToolpaths[i].CutLength() -
                          full[i].CutLength()) < 1e-6);
      }
    };

    ToolbarClick(ctx, "###tb_cam");
    ctx->Yield(2);

    // Two operations, through the verbs: a pocket on the floor, then the
    // part cut out.
    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - 14.0f) < 1e-4f)
          floor.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(floor.element.IsSet());
    ctx->ItemClick("//CAM###ToolWindow/###cam_verb_pocket");
    ctx->Yield();
    ui::CamFacePicked(*s_App, floor);
    ctx->Yield(2);
    matchesFullPass("after the first operation");
    IM_CHECK_EQ(s_App->camToolpaths.size(), (size_t)1);

    ui::SelectionItem wall;
    wall.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : Doc()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.01f)
          wall.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(wall.element.IsSet());
    ctx->ItemClick("//CAM###ToolWindow/###cam_verb_profile");
    ctx->Yield();
    ui::CamFacePicked(*s_App, wall); // the part's own side: cut it out
    ctx->Yield(2);
    matchesFullPass("after the second");

    // An edit to ONE operation's parameters: the panel's path, called the
    // way the panel calls it.
    {
      cam::Setup* live = ui::ActiveCamSetup(*s_App);
      IM_CHECK(live != nullptr);
      IM_CHECK(live->operations.size() >= (size_t)2);
      cam::Operation& first = live->operations[0];
      first.stepdown = std::max(0.5, first.stepdown * 0.5);
      ui::RefreshCamToolpath(*s_App, first.id);
      ctx->Yield();
      matchesFullPass("after editing one operation");
    }

    // And a delete, through the tool window's own button.
    const size_t before = s_App->camToolpaths.size();
    ctx->ItemClick("//CAM###ToolWindow/**/x###rmop");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camToolpaths.size(), before - 1);
    matchesFullPass("after deleting one");
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "panel_generates_and_backplots");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);

    cam::Setup& setup = Doc()->AddCamSetup("Side A");
    setup.safeZ = 20.0;
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    // Reach the panel the way a user does: click its dock tab. Model owns the
    // shared node (CAM used to steal it by appearing without
    // NoFocusOnAppearing, which hid the model tree), so the tab click is a
    // required step rather than a formality. A tab item's id IS the window's
    // TabId; FocusWindow deliberately does not select dock tabs.
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);

    // The panel's "Add operation" derives its depths from the model rather
    // than defaulting to zeros — the u10 rule: a default that cuts nothing
    // (or everything) is a bug.
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    cam::Setup* live = Doc()->FindCamSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    const cam::Operation& op = live->operations[0];
    IM_CHECK(op.topZ > op.bottomZ);
    // The STOCK top (plate 12 + 1 mm of margin), not the model top: material
    // sits above the part, and a cut that starts below it leaves a skin.
    IM_CHECK(std::abs(op.topZ - 13.0) < 1e-6);
    IM_CHECK(op.stepdown > 0.0);

    // A second tool AND a second operation: tool numbers and operation ids
    // are both small ints, so PushID(int) put them in one id space and ImGui
    // raised "2 visible items with conflicting ID" in a modal. One of each
    // never collides — which is why the first version of this test passed
    // while the panel was broken in front of the user.
    ctx->ItemClick("###cam_tools"); // expand the Tools header
    ctx->Yield();
    ctx->ItemClick("###cam_add_tool");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(3);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);

    // Generate: toolpaths exist, and the backplot draws them.
    ctx->ItemClick("###cam_generate");
    ctx->Yield(2);

    // Hand the shared dock node back to Model NOW, not at the end. A test
    // that fails partway never reaches its own cleanup, and the tab it left
    // selected covers the model tree for everything after it in the shard —
    // which is how a stale assertion here broke tree/context_rename two
    // tests later. Everything below asserts on app state, not on widgets.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel)) {
      ctx->ItemClick(modelWin->TabId);
      ctx->Yield(2);
    }
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);

    std::vector<PrimVertex> prims;
    ui::AppendCamToolpathOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());
    // The tool is drawn to scale where it enters — the "will my cutter fit"
    // picture (docs/cam/gui.md). Its circle sits INSIDE the path's extent, so
    // the check is its geometry: vertices one tool radius from the entry
    // point. (A bounding-box check passed for the wrong reason and was
    // replaced.)
    glm::dvec3 entry(0.0);
    for (const cam::Move& m : s_App->camToolpaths[0].moves)
      if (m.kind == cam::Move::Kind::Cut) {
        entry = m.to;
        break;
      }
    const cam::Setup* job = Doc()->FindCamSetup(s_App->camActiveSetup);
    IM_CHECK(job != nullptr && !job->tools.empty());
    const float radius = (float)(job->tools.front().diameter * 0.5);
    int onRing = 0;
    for (const PrimVertex& v : prims) {
      const float dx = v.posThis.x - (float)entry.x;
      const float dy = v.posThis.y - (float)entry.y;
      if (std::abs(std::sqrt(dx * dx + dy * dy) - radius) < 0.05f)
        onRing++;
    }
    IM_CHECK(onRing > 20); // a 32-segment circle, drawn as thick lines
    // Turning the picture off turns it off — a backplot that cannot be
    // hidden buries the model it is drawn over.
    s_App->camShowToolpath = false;
    std::vector<PrimVertex> none;
    ui::AppendCamToolpathOverlay(*s_App, none);
    IM_CHECK(none.empty());
    s_App->camShowToolpath = true;

    // The toolpath the overlay drew is the one the post would consume: same
    // vector, so the picture cannot disagree with the file.
    std::string gcode, error;
    IM_CHECK(cam::PostProcess(live->post, *live, s_App->camToolpaths, {}, gcode,
                              &error));
    IM_CHECK(gcode.find("G90 G91.1 G21") != std::string::npos);
    IM_CHECK(gcode.find("nan") == std::string::npos);
  };

  // The cutting simulation (docs/cam/simulation.md). Two things it must do:
  // show the block coming apart, and give everything back on the way out.
  t = IM_REGISTER_TEST(engine, "cam", "simulation_plays_and_gives_it_back");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = Doc()->AddCamSetup("Job");
    setup.safeZ = 20.0;
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    // Before anything is generated the button REFUSES, in words, where it
    // is — the defaults/refusals rule (docs/ui/u10-*). A transport that
    // opens onto an empty program would be worse than no button.
    ctx->ItemClick("###cam_sim_start");
    ctx->Yield(2);
    IM_CHECK(!s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camPlaybackRefusal.empty());

    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    ctx->ItemClick("###cam_generate");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());

    ctx->ItemClick("###cam_sim_start");
    ctx->Yield(2);
    IM_CHECK(s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camTimeline.Empty());
    // Nothing is cut at the start, and the whole program cuts something.
    IM_CHECK(s_App->camStockSim.RemovedVolume() < 1e-9);
    ui::SeekCamPlayback(*s_App, s_App->camTimeline.End());
    ctx->Yield(2);
    const double removed = s_App->camStockSim.RemovedVolume();
    IM_CHECK(removed > 100.0);
    // And it scrubs BACK — a height map cannot be un-cut, so this only
    // passes because seeking backwards re-runs from the pristine block.
    ui::SeekCamPlayback(*s_App, {0, 0.0});
    ctx->Yield(2);
    IM_CHECK(s_App->camStockSim.RemovedVolume() < 1e-9);

    // The path drawn while a playback is open IS the file being played.
    ui::SeekCamPlayback(*s_App, s_App->camTimeline.CursorAtDistance(
                                    s_App->camTimeline.totalDistance * 0.5));
    ctx->Yield(2);
    std::vector<PrimVertex> prims;
    ui::AppendCamToolpathOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());

    // And there is a PICTURE, not just a cursor: a surface for the half-cut
    // block, and the cutter standing in it. Asserting the renderers hold real
    // geometry is what a screenshot would be checking anyway, without a
    // reference image to rot.
    IM_CHECK(s_App->stockRenderer != nullptr);
    IM_CHECK(s_App->stockRenderer->HasMesh());
    IM_CHECK(s_App->camToolInstance != 0);

    // And it is aimed at the SAME rectangle as the rest of the scene. The 3D
    // view is a sub-region between the docked panels; a renderer left out of
    // the list in UI.cpp draws full-window while the camera aspect is the
    // sub-region, so its geometry lands at a different scale and offset than
    // everything drawn around it. That shipped: the stock came out not
    // parallel to the part with its cuts shifted sideways, while the cutter —
    // a mesh-renderer instance, correctly aimed — sat right where it belonged.
    //
    // Only a WINDOWED run can catch it. Under --offscreen there are no panels,
    // so the sub-region is the whole framebuffer and the bug is invisible,
    // which is exactly how it got past a screenshot review.
    const ImGuiIO& vpIo = ImGui::GetIO();
    const glm::vec2 vpScale(vpIo.DisplayFramebufferScale.x,
                            vpIo.DisplayFramebufferScale.y);
    const glm::vec2 wantPos = s_App->viewportPos * vpScale;
    const glm::vec2 wantSize = s_App->viewportSize * vpScale;
    const glm::ivec4 got = s_App->stockRenderer->Viewport();
    IM_CHECK_EQ(got.x, (int)wantPos.x);
    IM_CHECK_EQ(got.y, (int)wantPos.y);
    IM_CHECK_EQ(got.z, (int)wantSize.x);
    IM_CHECK_EQ(got.w, (int)wantSize.y);

    // Step forward one move, then back: the buttons a person uses to look at
    // the move a verdict named.
    const cam::SimCursor mid = s_App->camCursor;
    ctx->ItemClick("###cam_sim_step");
    ctx->Yield(2);
    IM_CHECK(mid < s_App->camCursor);
    ctx->ItemClick("###cam_sim_stepback");
    ctx->Yield(2);
    IM_CHECK(s_App->camCursor < mid || s_App->camCursor == mid);
    // Skipping to the next operation lands ON an operation boundary.
    ctx->ItemClick("###cam_sim_nextop");
    ctx->Yield(2);
    IM_CHECK(mid < s_App->camCursor);

    // Hand the dock node back before the assertions that could fail late.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel)) {
      ctx->ItemClick(modelWin->TabId);
      ctx->Yield(2);
    }

    // Closing gives everything back: no session state, no stock mesh, and no
    // tool instance left in the renderer. This is the U11 class — a mode that
    // hides things and forgets to put them back.
    ui::EndCamPlayback(*s_App);
    ctx->Yield(3);
    IM_CHECK(!s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camPlaying);
    IM_CHECK(s_App->camTimeline.Empty());
    IM_CHECK_EQ(s_App->camToolInstance, (uint32_t)0);
    if (s_App->stockRenderer != nullptr)
      IM_CHECK(!s_App->stockRenderer->HasMesh());
    // The backplot is back to the plain one.
    std::vector<PrimVertex> after;
    ui::AppendCamToolpathOverlay(*s_App, after);
    IM_CHECK(!after.empty());
  };

  // The terminal panel (docs/ui/terminal-panel.md). Off by default, docked
  // right when on, and — the part that decides whether it feels safe — the
  // keyboard goes to it ONLY while the pointer is over it.
#endif
}
