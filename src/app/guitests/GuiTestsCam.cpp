#include "app/guitests/GuiTestsCommon.h"
#include "ui/DocumentSlots.h"
#include "ui/MeasureActions.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include "cad/FaceSelector.h" // Ray
#include "core/cam/CamMachine.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/AssemblyActions.h" // the placement gizmo
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/DragMath.h" // GizmoScale
#include "ui/JobActions.h"
#include "ui/ListSelection.h"
#include "ui/MachineActions.h"

#include <chrono>
#include <filesystem>

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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    Job()->FindSetup(setup.id)->operations.push_back(op);
    ctx->Yield(2);
    for (const Body& b : CamPart()->GetBodies())
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
    IM_CHECK(Job()->FindSetup(setup.id)->topFace.IsSet());
    IM_CHECK(!s_App->camToolpaths.empty());
    // Now throw the document away.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ctx->Yield();
    ctx->MenuClick("File/New");
    ctx->Yield(3);
    // The whole JOB went with it, not just its setups: since v73 a job is a
    // document of its own, so File > New leaves none at all. Leaving one
    // behind is what made the panel offer to post G-code for a part that no
    // longer existed.
    IM_CHECK(Job() == nullptr);
    IM_CHECK(s_App->camToolpaths.empty());
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK_EQ(s_App->camActiveSetup, (uint64_t)0);
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0);
  };

  t = IM_REGISTER_TEST(engine, "cam", "pick_top_face_sets_z_zero");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    // Says so rather than assuming 0 — and says it on the SETUP, so a job
    // reopened from its own file still knows where Z zero came from.
    IM_CHECK(!Job()->FindSetup(setup.id)->topFace.IsSet());

    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ctx->Yield();
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);

    // A SIDE face used to be refused here. It is not any more: calling a face
    // the top is the statement that the part is clamped with it up, so the
    // pick clamps it and takes Z zero from it — one click where the refusal
    // cost three. It must SAY that it re-clamped.
    const Body& body = CamPart()->GetBodies()[0];
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
      IM_CHECK(Job()->FindSetup(setup.id)->topFace.IsSet());
      IM_CHECK(!cam::FrameIsSquare(*Job()->FindSetup(setup.id)));
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
    IM_CHECK(Job()->FindSetup(setup.id)->topFace.IsSet());
    IM_CHECK_EQ(s_App->slotPick.featureId, (uint64_t)0); // slot disarmed
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK(std::abs(live->origin.z - 12.0) < 1e-6); // the plate's top
    // The pick says where Z zero is, and NOTHING about how much material sits
    // above the part — it used to zero that, which threw away an allowance
    // already typed and made "Face the top" refuse right after the pick meant
    // to set the job up (docs/cam/dogfood-round.md, round 2).
    IM_CHECK(std::abs(live->stock.margin.z - 1.0) < 1e-9);
    const cam::DerivedOp face = cam::DeriveOperation(
        *live, CamPart()->GetBodies(), cam::OpType::Face, cam::Target{});
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    // A job with a toolpath, so the whole panel draws — Save G-code only
    // appears once something has been generated.
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    op.stepdown = 4.0;
    Job()->FindSetup(setup.id)->operations.push_back(op);
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
    // The setup row's combo is the MACHINE since round two; the post dialect
    // moved under "Stock and safety" as one of the machine's own fields.
    // Save G-code moved to the Generate window (docs/ui/cam-panel-tidy-
    // plan.md); the panel's run line is the widget that took its place.
    const ImGuiID machineCombo = panel->GetID("###cam_machine");
    const ImGuiID runStatus = panel->GetID("###cam_run_status");
    IM_CHECK(machineCombo != runStatus);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, "//CAM###CamPanel");
    bool sawCombo = false, sawRun = false;
    for (const ImGuiTestItemInfo& item : items) {
      sawCombo |= item.ID == machineCombo;
      sawRun |= item.ID == runStatus;
    }
    IM_CHECK(sawCombo); // the machine…
    IM_CHECK(sawRun);   // …and the run line, as two separate widgets
    // ...and the post combo is still reachable, under its header.
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_stock");
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists("###cam_post"));
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    for (int i = 0; i < 2; i++) {
      cam::Operation op;
      op.id = Job()->AllocateId();
      op.name = "Op " + std::to_string(i);
      op.type = cam::OpType::Profile;
      op.topZ = 13.0;
      op.bottomZ = -1.0;
      Job()->FindSetup(setup.id)->operations.push_back(op);
    }
    s_App->camToolOpen = true;
    ctx->Yield(4);
    IM_CHECK_EQ(Job()->FindSetup(setup.id)->operations.size(), (size_t)2);

    const uint64_t first = Job()->FindSetup(setup.id)->operations[0].id;
    // The row PushID's the operation's id as an int, which "$$<n>" addresses.
    // Removed from its row's menu, as a feature is: the rows lost their x
    // buttons to the feature list's gestures (docs/ui/cam-panel-tidy-plan.md).
    const std::string ref = "//CAM###ToolWindow/$$" +
                            std::to_string(static_cast<int>(first)) +
                            "/###op_row";
    ctx->ItemClick(ref.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###op_delete");
    ctx->Yield(3);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK(live->operations[0].id != first);
    s_App->camToolOpen = false;
  };

  // Operations are a LIST like the others (ListSelection.h): click selects,
  // Shift ranges, right-click offers "Delete N operations", and the Delete
  // key removes the selection. They had an x button per row and no selection
  // at all.
  t = IM_REGISTER_TEST(engine, "cam",
                       "operations_select_like_a_list_and_delete_together");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 4; i++) {
      cam::Operation op;
      op.id = Job()->AllocateId();
      op.name = "Op " + std::to_string(i);
      op.type = cam::OpType::Profile;
      op.topZ = 13.0;
      op.bottomZ = -1.0;
      Job()->FindSetup(setup.id)->operations.push_back(op);
      ids.push_back(op.id);
    }
    s_App->camToolOpen = true;
    ctx->Yield(4);
    const auto row = [&](size_t i) {
      return "//CAM###ToolWindow/$$" +
             std::to_string(static_cast<int>(ids[i])) + "/###op_row";
    };
    const auto picked = [&]() {
      return ui::SelectedIds(*s_App, ui::SelectionItem::Kind::Operation);
    };
    const auto ops = [&]() { return Job()->FindSetup(setup.id)->operations; };

    ctx->ItemClick(row(0).c_str());
    ctx->Yield();
    IM_CHECK_EQ(picked().size(), (size_t)1);
    ctx->KeyDown(ImGuiMod_Shift);
    ctx->ItemClick(row(2).c_str());
    ctx->KeyUp(ImGuiMod_Shift);
    ctx->Yield();
    IM_CHECK_EQ(picked().size(), (size_t)3);

    // Right-click inside the selection keeps it; the item counts it.
    ctx->ItemClick(row(1).c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    IM_CHECK_EQ(picked().size(), (size_t)3);
    IM_CHECK_STR_EQ(ctx->ItemInfo("//$FOCUSED/###op_delete").DebugLabel,
                    "Delete 3 operations###op_delete");
    ctx->ItemClick("//$FOCUSED/###op_delete");
    ctx->Yield(3);
    IM_CHECK_EQ(ops().size(), (size_t)1);
    IM_CHECK_EQ(ops()[0].id, ids[3]);
    IM_CHECK_EQ(picked().size(), (size_t)0);

    // The Delete key takes the last one.
    ctx->ItemClick(row(3).c_str());
    ctx->Yield();
    IM_CHECK_EQ(picked().size(), (size_t)1);
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield(3);
    IM_CHECK_EQ(ops().size(), (size_t)0);
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    MakeJob(); // the window is a job-tab session; without one it closes itself
    ctx->Yield(2);
    s_App->camToolOpen = true;
    ctx->Yield(4);
    ImGuiWindow* win = ImGui::FindWindowByName("CAM###ToolWindow");
    IM_CHECK(win != nullptr);
    ui::FitCameraToDocument(*s_App);
    if (s_App->camera)
      s_App->camera
          ->StopAnimation(); // a fit ANIMATES; the click needs it landed
    ctx->Yield(20);

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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    MakeJob(); // the tool window is a job-tab session (v73)
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(3);

    // 3-axis: the spindle points down the world's -Z, so the camera looks
    // that way whatever it was doing before.
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_spindle"); // the job bar's own button
    ctx->Yield(150); // AlignToFace animates; wait for it to land
    glm::vec3 fwd = glm::normalize(s_App->camera->GetForward());
    IM_CHECK(std::abs(fwd.z + 1.0f) < 0.02f); // looking along -Z
    IM_CHECK(std::abs(fwd.x) < 0.02f);
    IM_CHECK(std::abs(fwd.y) < 0.02f);

    // Indexed: the table has turned 90 degrees about X, so the spindle now
    // looks down the part's +Y. Verifying against the WORLD axes here would
    // pass for a job cut in the wrong frame.
    Job()->FindSetup(setup.id)->rotationA = 90.0;
    ctx->Yield(2);
    s_App->camera->SnapToView(glm::normalize(glm::vec3(1, 1, 0.2f)),
                              glm::vec3(0, 1, 0));
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_spindle");
    ctx->Yield(150);
    fwd = glm::normalize(s_App->camera->GetForward());
    // A job tab draws SETUP space, so the spindle is world +Z whatever the
    // part is clamped like — there is no frame left to come back through.
    const glm::vec3 want(0.0f, 0.0f, -1.0f);
    IM_CHECK(glm::length(fwd - want) < 0.03f);
  };

  // Picking a face as "the top" IS the statement that the part is clamped
  // with it up, so the pick lays it up rather than refusing. And a verb stays
  // armed after it lands: one verb is almost never one face.
  t = IM_REGISTER_TEST(engine, "cam", "top_face_clamps_and_verbs_stay_armed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Upright panel again: its big faces are the ones worth machining and
    // none of them is level as modelled.
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    const Body& body = CamPart()->GetBodies()[0];
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
    IM_CHECK(cam::FrameIsSquare(*Job()->FindSetup(setup.id)));
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(s_App->slotPick.miss.empty());
    IM_CHECK(!cam::FrameIsSquare(*live)); // laid flat by the same click
    IM_CHECK(live->topFace.IsSet());      // …and Z zero came from it
    IM_CHECK(!s_App->camNote.empty());    // and it SAID so

    // Picking the same face again is idempotent: no second re-clamp.
    const glm::dquat after = live->frame;
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(glm::length(live->frame - after) < 1e-9);

    // A verb stays armed after it lands, so a part with several holes is
    // several clicks and not several verb-presses.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamPocketFloor);
    IM_CHECK(!s_App->slotPick.prompt.empty()); // the pill survives the pick

    // …and a second face lands a second operation with no verb press.
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(setup.id)->operations.size(), (size_t)2);
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
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    const Body& body = CamPart()->GetBodies()[0];
    ElementRef big;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && n.y < -0.99f)
        big = ElementRef{body.createdBy, name};
    }
    IM_CHECK(big.IsSet());
    {
      cam::Setup* live = Job()->FindSetup(setup.id);
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
      live->safeZ = cam::DeriveSafeZ(*live, CamPart()->GetBodies());
      const cam::DerivedOp derived = cam::DeriveOperation(
          *live, CamPart()->GetBodies(), cam::OpType::Profile, {});
      IM_CHECK(derived.ok);
      cam::Operation op = derived.op;
      op.id = Job()->AllocateId();
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
    // It must be the spindle -- and a job tab DRAWS setup space (v73/J6), so
    // the spindle is world +Z whatever the part is clamped like. This used
    // to want the setup frame applied to +Z, which is where the spindle
    // points in MODEL space: it passed only while the simulation was
    // wrongly multiplying by setup-to-model, standing the stock upright
    // through a part lying flat.
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(!cam::FrameIsSquare(*live)); // the frame is real, and unused
    const glm::vec3 drawn = glm::normalize(glm::vec3((*m)[2]));
    const glm::vec3 want(0.0f, 0.0f, 1.0f);
    ctx->LogInfo("drawn  %.4f %.4f %.4f", drawn.x, drawn.y, drawn.z);
    ctx->LogInfo("spindle %.4f %.4f %.4f", want.x, want.y, want.z);
    IM_CHECK(glm::length(drawn - want) < 1e-4f);
    // …and it is a pure rotation: no scale, no shear, nothing view-derived.
    for (int c = 0; c < 3; c++)
      IM_CHECK(std::abs(glm::length(glm::vec3((*m)[c])) - 1.0f) < 1e-4f);
    IM_CHECK(std::abs((*m)[3][3] - 1.0f) < 1e-6f);

    // And it is OPAQUE. A ghosted instance draws alpha-blended with depth
    // writes off, so it occludes nothing — you see its own far wall through
    // its near one, which is what made the glyph read as a shape lying on its
    // side that half-followed the camera. `ghost` used to be sticky across
    // SetInstanceMaterial, so anything ever ghosted stayed that way.
    IM_CHECK(!s_App->meshRenderer->IsInstanceGhost(s_App->camToolInstance));
    s_App->meshRenderer->SetInstanceGhost(s_App->camToolInstance, 0.3f);
    IM_CHECK(s_App->meshRenderer->IsInstanceGhost(s_App->camToolInstance));
    s_App->meshRenderer->SetMeshMaterial(s_App->camToolInstance,
                                         glm::vec3(0.8f),
                                         glm::vec4(0.0f, 0.6f, 0.3f, 0.04f));
    IM_CHECK(!s_App->meshRenderer->IsInstanceGhost(s_App->camToolInstance));

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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    // A fresh setup already has an origin, DERIVED — not (0,0,0), which is a
    // literal sitting where the defaults rule wants a derivation.
    cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(cam::RecomputeOrigin(*live, CamPart()->GetBodies()));
    glm::dvec3 lo(0.0), hi(0.0);
    IM_CHECK(cam::StockBounds(*live, CamPart()->GetBodies(), lo, hi));
    IM_CHECK(std::abs(live->origin.x - lo.x) < 1e-9);
    IM_CHECK(std::abs(live->origin.y - lo.y) < 1e-9);
    IM_CHECK(std::abs(live->origin.z - hi.z) < 1e-9);

    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen(
        "###cam_stock"); // OPEN, not toggle (a header keeps its state)
    ctx->Yield(2);

    // Move zero to the stock's centre by clicking, and the origin follows.
    Job()->FindSetup(setup.id)->originAt = cam::OriginAt::StockCentre;
    IM_CHECK(cam::RecomputeOrigin(*Job()->FindSetup(setup.id),
                                  CamPart()->GetBodies()));
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(std::abs(live->origin.x - (lo.x + hi.x) * 0.5) < 1e-6);

    // And the FILE measures from it: post a real operation and read the
    // extremes back. This is the assertion that fails if the origin goes back
    // to being a label.
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    // Generate is a toolbar tool with its own window now
    // (docs/ui/cam-panel-tidy-plan.md); closed again so it cannot take the
    // clicks of what follows.
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(2);
    IM_CHECK(s_App->camGenerateOpen);
    s_App->camGenerateOpen = false;
    ctx->SetRef("//CAM###CamPanel");
    ctx->Yield(5);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(!live->operations.empty());
    if (!s_App->camToolpaths.empty()) {
      std::string gcode, error;
      IM_CHECK(cam::PostProcess(live->machine.post, *live, s_App->camToolpaths,
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

    // The Model panel belongs to a PART tab; step off the job before handing
    // the shared dock node back (v73).
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
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
      cam::Setup* live = Job()->FindSetup(setup.id);
      live->tools = {big, small};
      for (int i = 0; i < 2; i++) {
        cam::Operation op;
        op.id = Job()->AllocateId();
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

    cam::Setup* live = Job()->FindSetup(setup.id);
    const uint64_t firstId = live->operations[0].id;
    const uint64_t secondId = live->operations[1].id;

    // Point the first operation at the smaller cutter, by clicking: a
    // double-click on its row opens its editor, as a feature's does, and the
    // tool buttons are there (they were an inline "Advanced" tree).
    const std::string rowA = "**/$$" + std::to_string(firstId) + "/###op_row";
    ctx->ItemDoubleClick(rowA.c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, firstId);
    ctx->ItemClick("//###ToolWindow/**/###optool_t2");
    ctx->Yield(3);
    s_App->camEditOp = 0;
    s_App->camToolOpen = false;
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations[0].toolNumber, 2);
    // And the stepdown followed the cutter: half of 3, not half of 6. A pass
    // derived for a 6 mm tool is a full-diameter plunge for a 3 mm one.
    IM_CHECK(std::abs(live->operations[0].stepdown - 1.5) < 1e-6);
    // The one nobody retooled is untouched.
    IM_CHECK_EQ(live->operations[1].toolNumber, 1);
    IM_CHECK(std::abs(live->operations[1].stepdown - 3.0) < 1e-6);

    // Now drag the second operation above the first.
    // The row is its own drag handle, as a feature's is.
    const std::string dragA = "**/$$" + std::to_string(firstId) + "/###op_row";
    const std::string dragB = "**/$$" + std::to_string(secondId) + "/###op_row";
    ctx->ItemDragAndDrop(dragB.c_str(), dragA.c_str());
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)2);
    IM_CHECK_EQ(live->operations[0].id, secondId);
    IM_CHECK_EQ(live->operations[1].id, firstId);

    // Hand the shared dock node back before anything can fail below. The
    // Model panel belongs to a PART tab, so step off the job first -- since
    // v73 this test ends on one, and its tab has no model tree to click.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // S2 (docs/cam/setup-frame.md): the case every other fixture in this file
  // dodges by being modelled face-up. A panel that stands upright — an
  // imported speaker baffle, and every part that arrives from STEP or a
  // parametric import in the orientation it has in the assembly — has no level
  // face worth zeroing on. Laying one flat is a statement about the FIXTURE, so
  // it must work on a 3-axis machine and put no A word in the file.
  t = IM_REGISTER_TEST(engine, "cam", "upright_panel_lays_flat");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // 210 wide, 16 THICK, 400 tall: standing on edge, like a real baffle.
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    s_App->camToolOpen = true;
    ctx->Yield(3);

    // Find the big face — the one anyone would machine. Its normal is along
    // Y, so no amount of 2.5D can touch it as the part is modelled.
    const Body& body = CamPart()->GetBodies()[0];
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
    const cam::Setup* laid = Job()->FindSetup(setup.id);
    IM_CHECK(!cam::FrameIsSquare(*laid));
    IM_CHECK(!laid->machine.hasRotaryAxis);
    IM_CHECK(s_App->slotPick.miss.empty());

    // And now the SAME face is the top face, with a Z zero on it.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    IM_CHECK(s_App->slotPick.miss.empty());
    IM_CHECK(Job()->FindSetup(setup.id)->topFace.IsSet());

    // A real operation on it, and a file with no A word in it.
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(5);
    const cam::Setup* job = Job()->FindSetup(setup.id);
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    // A job with no setups: since v73 the job is the document CAM works on,
    // and it arrives empty. The claim under test is unchanged -- the first
    // ACTION makes the setup, not opening the window.
    IM_CHECK(MakeJob()->GetSetups().empty());
    ctx->Yield(2);

    // Pressing the verb is not an edit: the window opens, the pick arms, and
    // the document is as it was.
    ToolbarClick(ctx, "###tb_job_top_face");
    ctx->Yield(3);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(s_App->camVerb == ui::AppState::CamVerb::TopFace);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK(Job()->GetSetups().empty());

    // The first pick is. (The viewport click routes here through
    // SlotPickClicked; calling it directly keeps the test off the camera.)
    ui::SelectionItem top;
    top.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f)
          top.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(top.element.IsSet());
    ui::CamFacePicked(*s_App, top);
    ctx->Yield(3);

    IM_CHECK_EQ(Job()->GetSetups().size(), (size_t)1);
    const cam::Setup& made = Job()->GetSetups()[0];
    IM_CHECK(made.topFace.IsSet());
    IM_CHECK(std::abs(made.origin.z - 12.0) < 1e-6); // the plate's top
    IM_CHECK(s_App->slotPick.miss.empty());          // it took, not refused
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "verbs_two_clicks_to_a_pocket");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A plate with a pocket cleared from its top down to z = 12.
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = CamPart()->GetFeature(0)->GetId();
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 12), 30, 20, 20));
    const uint64_t voidId = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Pocket", plate, voidId, BooleanOp::Subtract));
    ctx->Yield(2);

    // Click 1: the verb, on the JOB bar. No setup exists yet, and the verb
    // makes one rather than sending the user to the panel.
    MakeJob();
    ctx->Yield(2);
    IM_CHECK(Job() != nullptr);
    IM_CHECK(Job()->GetSetups().empty());
    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield(2);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(s_App->camVerb == ui::AppState::CamVerb::Pocket);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamPocketFloor);

    // Click 2: the face. (The viewport click routes here through
    // SlotPickClicked; calling it directly keeps the test off the camera.)
    // First the post's barrel, which must be refused in words rather than
    // silently turned into a depth. Since R10 a vertical cylinder IS a wall
    // (that is how round openings are cut), so the refusal is the pocket
    // wall-guard's "click the pocket's FLOOR", not the old "curved".
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Post", glm::vec3(200, 0, 0), glm::vec3(0, 0, 1), 5.0, 10.0));
    ctx->Yield(2);
    for (const Body& b : CamPart()->GetBodies()) {
      if (b.createdBy !=
          CamPart()->GetFeature(CamPart()->FeatureCount() - 1)->GetId())
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
    for (const Body& b : CamPart()->GetBodies())
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

    // Closing the session drops the decision-time drawings (the ghost) and
    // keeps the backplot: the job survives the window that made it. The
    // STOCK stays too, since round two -- it is the thing the parts are
    // placed in, drawn on every job tab.
    ctx->ItemClick("//Clear a pocket###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK_EQ(s_App->camGhostOp, (uint64_t)0);
    std::vector<PrimVertex> afterPrims;
    std::vector<LineVertex3D> afterQuads;
    ui::AppendCamVisualisation(*s_App, afterPrims, afterQuads);
    IM_CHECK(afterQuads.size() < quads.size()); // the ghost's walls are gone
    IM_CHECK(!afterQuads.empty());              // the stock's are not
    std::vector<PrimVertex> backplot;
    ui::AppendCamToolpathOverlay(*s_App, backplot);
    IM_CHECK(!backplot.empty());
  };

  // R15: the same two clicks for the third verb, and the refusal that keeps
  // it honest — a flat end mill cannot break an edge, and saying so beats
  // ploughing a square step where a chamfer was asked for.
  t = IM_REGISTER_TEST(engine, "cam", "adaptive_clears_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/ui/long-operations.md, phase 1: generating an adaptive operation
    // does not block a frame. The same pocket the two-clicks test derives,
    // on the panel's REAL path (deferred, which the suite otherwise turns
    // off), watched frame by frame while the passes cut.
    if (!cam::AdaptiveAvailable())
      return;
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = CamPart()->GetFeature(0)->GetId();
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 12), 30, 20, 20));
    const uint64_t voidId = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Pocket", plate, voidId, BooleanOp::Subtract));
    ctx->Yield(2);
    MakeJob();
    ctx->Yield(2);
    s_App->camDeferPasses = true; // the real default, off for the suite
    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield(2);
    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - 12.0f) < 1e-4f)
          floor.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(floor.element.IsSet());

    // The pick returns at once: the toolpath is pending and a job is cutting.
    const auto picked = std::chrono::steady_clock::now();
    ui::CamFacePicked(*s_App, floor);
    const double pickMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - picked)
                              .count();
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].pending);
    IM_CHECK(ui::BackgroundJobRunning(*s_App, "cam"));
    ctx->LogInfo("pick returned in %.1f ms", pickMs);

    // Frames keep coming while the passes cut, and none of them is held.
    // The bound is generous against the idle pacer (a resting loop runs at
    // ~10 Hz, so a frame CAN legitimately take ~100 ms); what it excludes is
    // a frame that ran a pass, which on this pocket is several hundred ms.
    int framesWhileRunning = 0;
    double worstMs = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    while (ui::BackgroundJobRunning(*s_App, "cam")) {
      const auto f0 = std::chrono::steady_clock::now();
      ctx->Yield();
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - f0)
                            .count();
      worstMs = std::max(worstMs, ms);
      framesWhileRunning++;
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60))
        break;
    }
    ctx->LogInfo("%d frames while clearing, worst %.1f ms", framesWhileRunning,
                 worstMs);
    IM_CHECK(framesWhileRunning >= 2);
    IM_CHECK(worstMs < 250.0);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "cam"));

    // What landed is what a synchronous generate says.
    ctx->Yield(2);
    IM_CHECK(!ui::CamToolpathsPending(*s_App));
    const cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    const std::vector<cam::Toolpath> sync =
        cam::GenerateAll(*live, ui::JobBodies(*s_App));
    IM_CHECK_EQ(sync.size(), s_App->camToolpaths.size());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK_EQ(sync[0].moves.size(), s_App->camToolpaths[0].moves.size());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "surfacing_runs_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/ui/long-operations.md, phase 5. `GenerateContext`'s comment called
    // adaptive clearing "the one expensive step"; that stopped being true at
    // v118, and a finishing pass ran INLINE until this. The rotary rolling
    // pin took seventeen minutes with the app stopped -- single `ui` frames
    // of 10 s and 72 s on the frame HUD.
    //
    // The twin of adaptive_clears_off_the_frame, and deliberately so: the
    // properties that matter are the same three, and a second mechanism that
    // had to be watched a different way would be a second mechanism.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 60, 60, 20));
    const uint64_t block = CamPart()->GetFeature(0)->GetId();
    CamPart()->AddFeature(
        std::make_unique<SphereFeature>("Dome", glm::vec3(30, 30, 20), 18.0));
    const uint64_t dome = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>("Domed", block, dome,
                                                           BooleanOp::Union));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    cam::Setup* live0 = Job()->FindSetup(setup.id);
    IM_CHECK(live0 != nullptr);
    live0->tools[0].type = cam::ToolType::Ball; // a cone cannot finish
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.type = cam::OpType::SurfaceParallel;
    op.toolNumber = live0->tools[0].number;
    op.topZ = 38.0;
    op.bottomZ = 0.0;
    op.stepover = 0.6; // coarse: this case is about the frame, not the finish
    live0->operations.push_back(op);
    // A SECOND one, because the rolling pin found what one alone cannot: when
    // the first pass lands, the refresh that follows moves the next one from
    // the queue onto the worker, and the batch used to be judged finished on
    // the queues alone -- counters reset under a running pass ("1 / 0"), and
    // the verification fired onto a job whose last pass had not landed.
    cam::Operation op2 = op;
    op2.id = Job()->AllocateId();
    op2.stepover = 0.7;
    live0->operations.push_back(op2);

    s_App->camDeferPasses = true; // the real default, off for the suite
    const auto asked = std::chrono::steady_clock::now();
    // What the Generate button does (Commands.cpp, "job.generate"): refresh,
    // then ask for the verdict -- which, with the passes pending, is recorded
    // as owed and fired when the last one lands.
    ui::RefreshCamToolpaths(*s_App);
    ui::RunCamVerification(*s_App);
    const double askMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - asked)
                             .count();
    ctx->LogInfo("generate returned in %.1f ms", askMs);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].pending);
    IM_CHECK(ui::CamToolpathsPending(*s_App));
    IM_CHECK(ui::BackgroundJobRunning(*s_App, "cam"));
    // ...and it says which pass, so the row can be read rather than watched.
    const ui::CamPassProgress progress = ui::CamPassProgressOf(*s_App);
    IM_CHECK(progress.running);
    IM_CHECK(progress.total >= 1);
    IM_CHECK(progress.label.find("Finishing") != std::string::npos);

    int frames = 0;
    double worstMs = 0.0;
    bool countsSane = true;
    const auto t0 = std::chrono::steady_clock::now();
    // Through the passes AND the verification that follows them: both are
    // off the frame now, and it was the second that froze the rolling pin
    // once the first had been moved -- minutes of rotary carve, run in the
    // continuation that fires when the last pass lands.
    while (ui::BackgroundJobRunning(*s_App, "cam") ||
           ui::BackgroundJobRunning(*s_App, "cam-verify")) {
      if (const ui::CamPassProgress p = ui::CamPassProgressOf(*s_App);
          p.running && (p.total == 0 || p.done >= p.total))
        countsSane = false;
      const auto f0 = std::chrono::steady_clock::now();
      ctx->Yield();
      worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - f0)
                                      .count());
      frames++;
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120))
        break;
    }
    ctx->LogInfo("%d frames while surfacing and verifying, worst %.1f ms",
                 frames, worstMs);
    IM_CHECK(countsSane);
    IM_CHECK(frames >= 2);
    // The same bound the clearing twin uses: generous against the idle pacer
    // (a resting loop is ~10 Hz), tight enough to exclude a frame that ran
    // the pass.
    IM_CHECK(worstMs < 250.0);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "cam"));

    // What landed is what a synchronous generate says -- the cache is not
    // allowed to be a different answer, only a faster one.
    ctx->Yield(2);
    IM_CHECK(!ui::CamToolpathsPending(*s_App));
    const cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    const std::vector<cam::Toolpath> sync =
        cam::GenerateAll(*live, ui::JobBodies(*s_App));
    IM_CHECK_EQ(sync.size(), s_App->camToolpaths.size());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK_EQ(sync[0].moves.size(), s_App->camToolpaths[0].moves.size());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
    // ...and the verdict landed, rather than the placeholder that says one is
    // coming.
    IM_CHECK(!s_App->camVerdict.empty());
    for (const cam::VerdictLine& l : s_App->camVerdict)
      IM_CHECK(l.text.find("Verifying") == std::string::npos &&
               l.text.find("Not verified yet") == std::string::npos);
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "a_rotary_op_spirals_from_the_panel");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/cam/rotary-continuous-plan.md. The pattern is a radio pair on a
    // rotary op's Advanced row, offered because the capability table says
    // the type has it -- and choosing "spiral" there has to turn a raster
    // whose part turns only BETWEEN passes into one that turns while it cuts.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Bar", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 12.0, 30.0));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("On the trunnion");
    s_App->camActiveSetup = added.id;
    ctx->Yield(2);
    cam::Setup* setup = Job()->FindSetup(added.id);
    IM_CHECK(setup != nullptr);
    setup->machine.hasRotaryAxis = true;
    setup->safeZ = 30.0;
    setup->stock.margin = {0.0, 1.0, 1.0};
    setup->tools[0].type = cam::ToolType::Ball;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.type = cam::OpType::SurfaceRotary;
    op.toolNumber = setup->tools[0].number;
    op.stepover = 0.6;
    setup->operations.push_back(op);
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    // The op's settings open in its editor on a double-click.
    const std::string row = "**/$$" + std::to_string(op.id) + "/###op_row";
    ctx->ItemDoubleClick(row.c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, op.id);
    ctx->ItemClick("//###ToolWindow/**/spiral###pattern_spiral");
    ctx->Yield(3);
    setup = Job()->FindSetup(added.id);
    IM_CHECK(setup->operations[0].rotaryPattern == cam::RotaryPattern::Spiral);

    ui::RefreshCamToolpaths(*s_App);
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    size_t turningCuts = 0;
    const auto& moves = s_App->camToolpaths[0].moves;
    for (size_t i = 1; i < moves.size(); i++)
      if (moves[i].kind == cam::Move::Kind::Cut &&
          std::abs(moves[i].a - moves[i - 1].a) > 1e-9)
        turningCuts++;
    ctx->LogInfo("%zu cuts turn the part", turningCuts);
    IM_CHECK(turningCuts > 10);

    // Helical is the third radio, and only it shows the lead: a number the
    // other two patterns do not read is not offered for them.
    IM_CHECK(!ctx->ItemExists("//###ToolWindow/**/lead mm###lead"));
    ctx->ItemClick("//###ToolWindow/**/helical###pattern_helical");
    ctx->Yield(3);
    setup = Job()->FindSetup(added.id);
    IM_CHECK(setup->operations[0].rotaryPattern == cam::RotaryPattern::Helical);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/**/lead mm###lead"));
    s_App->camEditOp = 0;
    s_App->camToolOpen = false;
    ctx->Yield(2);

    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  t = IM_REGISTER_TEST(engine, "cam", "a_rotary_part_turns_as_it_plays");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/cam/rotary-plan.md, "The picture". A program whose A moves used to
    // be REFUSED by the simulator -- honestly, while its only stock was a
    // height map. It plays now, and the part TURNS: one matrix, R_x(A) at the
    // cursor, on the ghosted bodies, their outlines and the stock, while the
    // tool stays where the machine keeps it. Read off the RENDERER's own
    // instance state rather than trusted from the code path.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 15.0, 40.0));
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Waist", glm::vec3(12, 0, 0), glm::vec3(1, 0, 0), 15.0, 16.0));
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Necked", CamPart()->GetFeature(0)->GetId(),
        CamPart()->GetFeature(1)->GetId(), BooleanOp::Subtract));
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Journal", glm::vec3(12, 0, 0), glm::vec3(1, 0, 0), 11.0, 16.0));
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Shaft", CamPart()->GetFeature(2)->GetId(),
        CamPart()->GetFeature(3)->GetId(), BooleanOp::Union));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("On the trunnion");
    s_App->camActiveSetup = added.id;
    ctx->Yield(2);
    cam::Setup* setup = Job()->FindSetup(added.id);
    IM_CHECK(setup != nullptr);
    setup->machine.hasRotaryAxis = true;
    setup->safeZ = 30.0;
    setup->stock.margin = {0.0, 1.0, 1.0};
    setup->tools[0].type = cam::ToolType::Ball;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.type = cam::OpType::SurfaceRotary;
    op.toolNumber = setup->tools[0].number;
    op.stepover = 0.6; // coarse: this case is about the picture
    setup->operations.push_back(op);
    ui::RefreshCamToolpaths(*s_App);
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].error.empty());

    ToolbarClick(ctx, "###tb_job_simulate");
    ctx->Yield(3);
    ctx->LogInfo("refusal: '%s'", s_App->camPlaybackRefusal.c_str());
    IM_CHECK(s_App->camPlaybackOpen); // plays, rather than refusing
    IM_CHECK(s_App->camTimeline.rotary);
    IM_CHECK(!ctx->ItemExists("//Cannot simulate/###cam_refusal_close"));

    // Somewhere the part has turned well away from zero.
    cam::SimCursor at{s_App->camTimeline.Segments() / 2, 0.5};
    for (size_t seg = at.seg; seg < s_App->camTimeline.Segments(); seg++)
      if (std::abs(s_App->camTimeline.AngleAt({seg, 0.5})) > 30.0) {
        at = {seg, 0.5};
        break;
      }
    ui::SeekCamPlayback(*s_App, at);
    ctx->Yield(3);
    const double a = s_App->camTimeline.AngleAt(s_App->camCursor);
    ctx->LogInfo("A at the cursor: %.1f deg", a);
    IM_CHECK(std::abs(a) > 30.0);
    IM_CHECK(s_App->camRotarySim.RemovedVolume() > 0.0);
    IM_CHECK(s_App->stockRenderer != nullptr &&
             s_App->stockRenderer->HasMesh());

    // The part, TURNED by exactly that angle about the rotary axis: a point
    // at +Y must land where R_x(A) puts it.
    IM_CHECK(!s_App->documentMeshes.empty());
    const std::optional<glm::mat4> xf =
        s_App->meshRenderer->GetInstanceTransform(
            s_App->documentMeshes[0].meshId);
    IM_CHECK(xf.has_value());
    const glm::vec3 got = glm::vec3((*xf) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const float ar = glm::radians(static_cast<float>(a));
    const glm::vec3 want(0.0f, std::cos(ar), std::sin(ar));
    ctx->LogInfo("+Y went to (%.3f %.3f %.3f), want (%.3f %.3f %.3f)", got.x,
                 got.y, got.z, want.x, want.y, want.z);
    IM_CHECK(glm::length(got - want) < 1e-3f);

    // Playing it: frames keep coming while the stock is carved and re-meshed
    // as a tube, which is the cost the plan named as the one to measure.
    s_App->camPlaying = true;
    s_App->camPlaybackSpeed = 20.0f;
    double worstMs = 0.0;
    int frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3) &&
           s_App->camPlaying) {
      const auto f0 = std::chrono::steady_clock::now();
      ctx->Yield();
      worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - f0)
                                      .count());
      frames++;
    }
    s_App->camPlaying = false;
    ctx->LogInfo("%d frames playing, worst %.1f ms", frames, worstMs);
    IM_CHECK(frames >= 10);

    // And it all goes back: the part stands still once the playback ends.
    ui::EndCamPlayback(*s_App);
    ctx->Yield(4);
    IM_CHECK(!s_App->camPlaybackOpen);
    IM_CHECK(!s_App->documentMeshes.empty());
    const std::optional<glm::mat4> back =
        s_App->meshRenderer->GetInstanceTransform(
            s_App->documentMeshes[0].meshId);
    IM_CHECK(back.has_value());
    const glm::vec3 still =
        glm::vec3((*back) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    IM_CHECK(glm::length(still - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f);
  };

  t = IM_REGISTER_TEST(engine, "cam", "simulate_says_why_at_the_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // "The simulation button still does nothing." It did something: it
    // refused, and wrote why into `camPlaybackRefusal` -- which the CAM panel
    // printed below a screenful of verdict lines, out of sight of the toolbar
    // button that was pressed. From there a refusal is indistinguishable from
    // a dead button. It answers at the click now, in a modal like the
    // import's.
    //
    // Any refusal will do for the gate; the plainest is a job with nothing
    // to play.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    MakeJob();
    ctx->Yield(2);
    IM_CHECK(!ctx->ItemExists("//Cannot simulate/###cam_refusal_close"));

    ToolbarClick(ctx, "###tb_job_simulate");
    ctx->Yield(3);
    IM_CHECK(!s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camPlaybackRefusal.empty());
    ctx->LogInfo("refused: %s", s_App->camPlaybackRefusal.c_str());
    // The modal is up, and it is the thing on screen that says why.
    IM_CHECK(ctx->ItemExists("//Cannot simulate/###cam_refusal_close"));

    // ...and it goes away when asked, rather than trapping the app.
    ctx->ItemClick("//Cannot simulate/###cam_refusal_close");
    ctx->Yield(3);
    IM_CHECK(!ctx->ItemExists("//Cannot simulate/###cam_refusal_close"));
  };

  t = IM_REGISTER_TEST(engine, "cam", "verbs_two_clicks_to_an_edge_break");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    ctx->Yield(2);

    MakeJob();
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_edge");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamEdgeWall);

    // A wall of the plate — any vertical face.
    ui::SelectionItem wall;
    wall.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
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
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = CamPart()->GetFeature(0)->GetId();
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 14), 30, 20, 20));
    const uint64_t voidId = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Pocket", plate, voidId, BooleanOp::Subtract));
    ctx->Yield(2);

    // Same shape as the full pass: same operations, same order, same moves.
    const auto matchesFullPass = [&](const char* when) {
      const cam::Setup* live = ui::ActiveCamSetup(*s_App);
      IM_CHECK(live != nullptr);
      const std::vector<cam::Toolpath> full =
          cam::GenerateAll(*live, CamPart()->GetBodies());
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

    MakeJob();
    ctx->Yield(2);

    // Two operations, through the verbs: a pocket on the floor, then the
    // part cut out.
    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - 14.0f) < 1e-4f)
          floor.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(floor.element.IsSet());
    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield();
    ui::CamFacePicked(*s_App, floor);
    ctx->Yield(2);
    matchesFullPass("after the first operation");
    IM_CHECK_EQ(s_App->camToolpaths.size(), (size_t)1);

    ui::SelectionItem wall;
    wall.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (b.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.01f)
          wall.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(wall.element.IsSet());
    ToolbarClick(ctx, "###tb_job_profile");
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

    // And a delete, from a row's menu in the tool window.
    const size_t before = s_App->camToolpaths.size();
    ctx->ItemClick("//###ToolWindow/**/###op_row", ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###op_delete");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camToolpaths.size(), before - 1);
    matchesFullPass("after deleting one");
    s_App->camToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "cam", "generate_is_a_tool_with_a_window");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/ui/cam-panel-tidy-plan.md. The toolbar's gears used to generate
    // SILENTLY, with the verdict landing in the docked panel below the
    // operations; now they open the run where they were pressed -- the
    // verdict, the output options and Save G-code -- like Simulate.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    const cam::DerivedOp d =
        cam::DeriveOperation(*Job()->FindSetup(setup.id), ui::JobBodies(*s_App),
                             cam::OpType::Profile, {});
    IM_CHECK(d.ok);
    cam::Operation op = d.op;
    op.id = Job()->AllocateId();
    Job()->FindSetup(setup.id)->operations.push_back(op);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(3);
    IM_CHECK(s_App->camGenerateOpen);
    IM_CHECK(!s_App->camToolpaths.empty());
    ctx->SetRef("//###CamGenerate");
    IM_CHECK(ctx->ItemExists("###cam_generate")); // Regenerate
    IM_CHECK(ctx->ItemExists("###cam_split"));
    IM_CHECK(ctx->ItemExists("###cam_save_gcode"));

    // Save, from the window: beside the project, named for it and the setup.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_cam_generate_test";
    std::filesystem::create_directories(dir);
    s_App->projectPath = (dir / "plate.dsgn").string();
    ctx->ItemClick("###cam_save_gcode");
    ctx->Yield(2);
    ctx->LogInfo("wrote: %s", s_App->camLastPost.c_str());
    IM_CHECK(std::filesystem::exists(s_App->camLastPost));
    std::filesystem::remove_all(dir);
    s_App->projectPath.clear();

    // Closed by its own close box, like any window.
    s_App->camGenerateOpen = false;
    ctx->Yield(2);
    IM_CHECK(ImGui::FindWindowByName("###CamGenerate") == nullptr ||
             !ImGui::FindWindowByName("###CamGenerate")->Active);
  };

  t = IM_REGISTER_TEST(engine, "cam", "an_op_row_has_the_feature_gestures");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // Rows in the part feature list's style: the menu disables (and the row
    // dims), a double-click edits in the tool window.
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    Job()->FindSetup(setup.id)->operations.push_back(op);
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    const std::string row = "**/$$" + std::to_string(op.id) + "/###op_row";

    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###op_enable");
    ctx->Yield(2);
    IM_CHECK(!Job()->FindSetup(setup.id)->operations[0].enabled);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###op_enable");
    ctx->Yield(2);
    IM_CHECK(Job()->FindSetup(setup.id)->operations[0].enabled);

    ctx->ItemDoubleClick(row.c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, op.id);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###op_name"));
    ctx->ItemClick("//###ToolWindow/###cam_edit_done");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, (uint64_t)0);
    IM_CHECK(!s_App->camToolOpen);

    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  t = IM_REGISTER_TEST(engine, "cam", "panel_generates_and_backplots");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);

    cam::Setup& setup = MakeJob()->AddSetup("Side A");
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
    cam::Setup* live = Job()->FindSetup(setup.id);
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
    ctx->ItemOpen(
        "###cam_tools"); // OPEN, not toggle (a header keeps its state)
    ctx->Yield();
    ctx->ItemClick("###cam_add_tool");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(3);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);

    // Generate: toolpaths exist, and the backplot draws them.
    // Generate is a toolbar tool with its own window now
    // (docs/ui/cam-panel-tidy-plan.md); closed again so it cannot take the
    // clicks of what follows.
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(2);
    IM_CHECK(s_App->camGenerateOpen);
    s_App->camGenerateOpen = false;
    ctx->SetRef("//CAM###CamPanel");
    ctx->Yield(2);

    // Hand the shared dock node back to Model NOW, not at the end. A test
    // that fails partway never reaches its own cleanup, and the tab it left
    // selected covers the model tree for everything after it in the shard —
    // which is how a stale assertion here broke tree/context_rename two
    // tests later. Everything below asserts on app state, not on widgets.
    // Only when there IS one to hand back to: the Model panel belongs to a
    // part tab, and since v73 this test runs on a JOB tab, where the CAM
    // panel does not share a node with it (`ItemClick` on an absent tab is a
    // hard failure, not a no-op). The job tab itself goes away in
    // ResetAppForTest, which is what leaves the next test on a part.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      if (ctx->ItemInfo(modelWin->TabId, ImGuiTestOpFlags_NoError).ID != 0) {
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
    const cam::Setup* job = Job()->FindSetup(s_App->camActiveSetup);
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
    IM_CHECK(cam::PostProcess(live->machine.post, *live, s_App->camToolpaths,
                              {}, gcode, &error));
    IM_CHECK(gcode.find("G90 G91.1 G21") != std::string::npos);
    IM_CHECK(gcode.find("nan") == std::string::npos);
  };

  // The cutting simulation (docs/cam/simulation.md). Two things it must do:
  // show the block coming apart, and give everything back on the way out.
  t = IM_REGISTER_TEST(engine, "cam", "simulation_plays_and_gives_it_back");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    setup.safeZ = 20.0;
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    // Before anything is generated the button REFUSES, in words — the
    // defaults/refusals rule (docs/ui/u10-*). A transport that opens onto an
    // empty program would be worse than no button. Simulate is the job
    // toolbar's; the refusal is printed by the panel, and no transport
    // window appears.
    ToolbarClick(ctx, "###tb_job_simulate");
    ctx->Yield(2);
    IM_CHECK(!s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camPlaybackRefusal.empty());
    {
      ImGuiWindow* t =
          ImGui::FindWindowByName("Simulate the cut###CamTransport");
      IM_CHECK(t == nullptr || !t->WasActive);
    }
    // ...and says so AT THE CLICK now, in a modal, rather than only in the
    // panel below the verdict where a toolbar press cannot see it
    // (simulate_says_why_at_the_click). Closed before carrying on, as a
    // person would: it is modal because a refusal answers a press.
    IM_CHECK(ctx->ItemExists("//Cannot simulate/###cam_refusal_close"));
    ctx->ItemClick("//Cannot simulate/###cam_refusal_close");
    ctx->Yield(2);

    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_add_op");
    ctx->Yield(2);
    // Generate is a toolbar tool with its own window now
    // (docs/ui/cam-panel-tidy-plan.md); closed again so it cannot take the
    // clicks of what follows.
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(2);
    IM_CHECK(s_App->camGenerateOpen);
    s_App->camGenerateOpen = false;
    ctx->SetRef("//CAM###CamPanel");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());

    // A verb window open, asking for a face: Simulate closes it. Two
    // sessions over one block would be two claims on the same clicks.
    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield(2);
    IM_CHECK(s_App->camToolOpen);
    ToolbarClick(ctx, "###tb_job_simulate");
    ctx->Yield(3);
    IM_CHECK(s_App->camPlaybackOpen);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(s_App->slotPick.featureId != ui::kCamToolSlot);
    IM_CHECK(!s_App->camTimeline.Empty());
    // The transport is a FLOATING window over the viewport, inside it, along
    // the bottom — not a section of the panel (2026-09-02).
    {
      ImGuiWindow* t =
          ImGui::FindWindowByName("Simulate the cut###CamTransport");
      IM_CHECK(t != nullptr && t->WasActive);
      IM_CHECK(t->Pos.x >= s_App->viewportPos.x);
      IM_CHECK(t->Pos.x + t->Size.x <=
               s_App->viewportPos.x + s_App->viewportSize.x + 1.0f);
      IM_CHECK(t->Pos.y + t->Size.y <=
               s_App->viewportPos.y + s_App->viewportSize.y + 1.0f);
      IM_CHECK(t->Pos.y > s_App->viewportPos.y + s_App->viewportSize.y * 0.5f);
    }
    ctx->SetRef("//Simulate the cut###CamTransport");
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
    // Only when there IS one to hand back to: the Model panel belongs to a
    // part tab, and since v73 this test runs on a JOB tab, where the CAM
    // panel does not share a node with it (`ItemClick` on an absent tab is a
    // hard failure, not a no-op). The job tab itself goes away in
    // ResetAppForTest, which is what leaves the next test on a part.
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      if (ctx->ItemInfo(modelWin->TabId, ImGuiTestOpFlags_NoError).ID != 0) {
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

  // A job holds more than one part, and you can say so by clicking. Until
  // this section existed the only door was the RPC, which by this project's
  // own rule makes a feature half-built.
  t = IM_REGISTER_TEST(engine, "cam", "parts_and_placement_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    // A second part in the project, so the job has something to add.
    const size_t second = ui::AddPart(*s_App, "Spacer");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Blk", glm::vec3(0, 0, 0), 20, 20, 12));
    ctx->Yield(2);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    (void)second;

    cam::Setup& setup = MakeJob()->AddSetup("Sheet");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1); // the part it was made
                                                      // for

    // Reach the panel the way a user does: click its dock tab (a tab item's
    // id IS the window's TabId).
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_parts");
    ctx->Yield(2);

    // Add the second part by clicking its button.
    ctx->ItemClick("**/###cam_part_add");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)2);

    // Place it: the button appears per unplaced part, so clicking the first
    // one places the part that has none yet.
    ctx->ItemClick("**/###cam_place");
    ctx->Yield(2);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->placements.size(), (size_t)1);

    // And it names a part the job actually holds — a placement for anything
    // else would put a ghost in the stock.
    IM_CHECK(Job()->FindPart(live->placements[0].jobPartId) != nullptr);

    // Removing a part takes its placement with it.
    ctx->ItemClick("**/###cam_part_remove");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1);
    IM_CHECK(Job()->FindSetup(setup.id)->placements.size() <= 1);

    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      if (ctx->ItemInfo(modelWin->TabId, ImGuiTestOpFlags_NoError).ID != 0) {
        ctx->ItemClick(modelWin->TabId);
        ctx->Yield(2);
      }
  };

  // A click on a job tab knows WHICH placed part it landed on. Two parts in
  // one job routinely share a feature id and an element name -- two boxes
  // modelled independently both have feature 1 and a face "1/face5" -- so
  // without this the target resolves to whichever body answers first, which
  // is the wrong part, cut for real.
  t = IM_REGISTER_TEST(engine, "cam", "a_click_knows_which_part_it_hit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    ui::AddPart(*s_App, "B");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    cam::Setup& setup = MakeJob()->AddSetup("Sheet");
    s_App->camActiveSetup = setup.id;
    const uint64_t second = Job()->AddPart(s_App->parts[1].id, "B");
    // 400 mm apart, so a click can tell them apart on screen.
    cam::Placement pl;
    pl.id = Job()->AllocateId();
    pl.jobPartId = second;
    pl.pos = {400.0, 0.0};
    Job()->FindSetup(setup.id)->placements.push_back(pl);
    Job()->BumpVersion();
    ctx->Yield(4);

    // Both parts really do collide on both halves of an ElementRef.
    const std::vector<Body>& placed = ui::JobPlacedBodies(*s_App);
    IM_CHECK_EQ(placed.size(), (size_t)2);
    IM_CHECK_EQ(placed[0].createdBy, placed[1].createdBy);
    IM_CHECK(placed[0].jobPartId != placed[1].jobPartId);

    // Frame it deterministically rather than through a fit, the way every
    // other viewport-click test does: a fit animates, and an earlier test's
    // orbit leaks in through the shared camera.
    glm::vec3 lo, hi;
    placed[1].shape.GetBoundingBox(lo, hi);
    const glm::vec3 centre = (lo + hi) * 0.5f;
    const float radius = glm::length(hi - lo) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(centre);
    s_App->camera->SetUp(glm::vec3(0.0f, 0.0f, 1.0f));
    s_App->camera->SetPosition(
        centre + glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * radius * 5.0f);
    ctx->Yield(4);

    // Click the SECOND part, the one 400 mm along.
    const ImVec2 at = WorldToWindow(centre);
    ctx->MouseMoveToPos(at);
    ctx->MouseClick(0);
    ctx->Yield(3);

    bool sawFace = false;
    for (const ui::SelectionItem& item : s_App->selection.items) {
      if (item.kind != ui::SelectionItem::Kind::Face)
        continue;
      sawFace = true;
      IM_CHECK_EQ(item.jobPartId, placed[1].jobPartId);
    }
    IM_CHECK(sawFace);
  };

  // The backplot is drawn ON the part, whatever the part is clamped like.
  //
  // It used to be built in setup space and shown in MODEL space, because the
  // viewport drew the model. Since the job tab draws its parts LAID OUT
  // (v73/J6) that undoes exactly the transform the viewport just applied, so
  // a laid-flat job drew its toolpath floating beside the part it belongs to.
  t = IM_REGISTER_TEST(engine, "cam", "backplot_lands_on_a_clamped_part");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // An upright panel: nothing is machinable until it is laid down, which is
    // what makes the frame non-identity.
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    // Lay the big face up, the way picking it as the top face does.
    const Body& body = ui::JobBodies(*s_App)[0];
    glm::vec3 up(0.0f);
    double best = 0.0;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (body.shape.FacePlane(index, o, n, u) && std::abs(n.y) > best) {
        best = std::abs(n.y);
        up = n;
      }
    }
    IM_CHECK(best > 0.9);
    cam::Setup* live = Job()->FindSetup(setup.id);
    live->frame = cam::SolveFaceUp(glm::dvec3(up), live->frame);
    IM_CHECK(!cam::FrameIsSquare(*live));
    live->safeZ = cam::DeriveSafeZ(*live, ui::JobBodies(*s_App));
    (void)cam::RecomputeOrigin(*live, ui::JobBodies(*s_App));
    const cam::DerivedOp derived = cam::DeriveOperation(
        *live, ui::JobBodies(*s_App), cam::OpType::Profile, cam::Target{});
    IM_CHECK(derived.ok);
    cam::Operation op = derived.op;
    op.id = Job()->AllocateId();
    live->operations.push_back(op);
    ui::RefreshCamToolpaths(*s_App);
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolpaths.empty());

    std::vector<PrimVertex> prims;
    ui::AppendCamToolpathOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());

    // Where the part is DRAWN, and where the backplot is drawn. A profile
    // runs around the part, so the two must share a neighbourhood.
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const Body& b : ui::JobPlacedBodies(*s_App)) {
      glm::vec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      lo = glm::min(lo, bmin);
      hi = glm::max(hi, bmax);
    }
    glm::vec3 plo(FLT_MAX), phi(-FLT_MAX);
    for (const PrimVertex& v : prims) {
      plo = glm::min(plo, v.posThis);
      phi = glm::max(phi, v.posThis);
    }
    // MATCHING extents, not merely overlapping ones: a profile runs around
    // the part, so its box is the part's box plus a tool radius. "Inside the
    // part's box" passes with the backplot collapsed onto one axis, which is
    // exactly what the wrong transform produces.
    const float pad = 30.0f; // tool radius + stock margin
    for (int a = 0; a < 2; a++) {
      IM_CHECK(std::abs(plo[a] - lo[a]) < pad);
      IM_CHECK(std::abs(phi[a] - hi[a]) < pad);
    }

    // And the SIMULATION is drawn in the same space. It had the same bug the
    // same week: the stock and the cutter were multiplied by setup-to-model,
    // which stood the block upright through a part lying flat. The world IS
    // setup space on a job tab, so the cutter sits exactly where the timeline
    // says, with nothing in between.
    IM_CHECK(ui::BeginCamPlayback(*s_App));
    ui::SeekCamPlayback(*s_App, s_App->camTimeline.CursorAtDistance(
                                    s_App->camTimeline.totalDistance * 0.5));
    ctx->Yield(2);
    IM_CHECK(s_App->camToolInstance != 0);
    const std::optional<glm::mat4> toolXf =
        s_App->meshRenderer->GetInstanceTransform(s_App->camToolInstance);
    IM_CHECK(toolXf.has_value());
    const glm::vec3 drawnAt = glm::vec3((*toolXf)[3]);
    const glm::vec3 cursorAt(s_App->camTimeline.PositionAt(s_App->camCursor));
    IM_CHECK(glm::distance(drawnAt, cursorAt) < 1e-3f);
    // ...and where the part is: a profile's cutter is never far off the box.
    IM_CHECK(drawnAt.x > lo.x - pad && drawnAt.x < hi.x + pad);
    IM_CHECK(drawnAt.y > lo.y - pad && drawnAt.y < hi.y + pad);
    ui::EndCamPlayback(*s_App);
    ctx->Yield(2);
  };

  // A job tab has its own toolbar (2026-09-02): the verbs, one button each,
  // where the part bar has its features. It used to draw the PART bar —
  // thirty disabled modelling tools — and the CAM panel used to exist on
  // every kind of tab. Same three properties the part bar's tests hold it
  // to: the row fits, every id is present, no two buttons wear one glyph.
  t = IM_REGISTER_TEST(engine, "cam", "job_tab_has_its_own_toolbar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    // On the PART: the part bar, the part context, and no CAM panel at all.
    IM_CHECK_EQ(ui::ActiveContext(*s_App), keymap::Context::Part);
    ctx->SetRef("//##DockHost");
    IM_CHECK(ctx->ItemExists("**/###tb_box"));
    IM_CHECK(!ctx->ItemExists("**/###tb_job_pocket"));
    {
      ImGuiWindow* cam = ImGui::FindWindowByName(ui::kPanelCam);
      IM_CHECK(cam == nullptr || !cam->WasActive);
    }

    // On the JOB: the other way round, and the CAM panel comes forward on
    // its own — it shares the left node, and the tab that was in front on
    // the part is not the one this tab is about.
    ctx->SetRef("//##MainMenuBar");
    MakeJob();
    ctx->Yield(8);
    IM_CHECK(Job() != nullptr);
    IM_CHECK_EQ(ui::ActiveContext(*s_App), keymap::Context::Job);
    ctx->SetRef("//##DockHost");
    IM_CHECK(!ctx->ItemExists("**/###tb_box"));
    static const char* const kJobTools[] = {
        "###tb_job_top_face", "###tb_job_lay_face_up", "###tb_job_spindle",
        "###tb_job_pocket",   "###tb_job_profile",     "###tb_job_drill",
        "###tb_job_edge",     "###tb_job_whole",       "###tb_job_face",
        "###tb_job_generate", "###tb_job_simulate",    "###tb_job_fit",
        "###tb_job_measure",
    };
    std::unordered_map<std::string, std::string> byGlyph;
    for (const char* id : kJobTools) {
      const ImGuiTestItemInfo info =
          ctx->ItemInfo((std::string("**/") + id).c_str());
      IM_CHECK_SILENT(info.ID != 0);
      const std::string label = info.DebugLabel;
      const size_t hash = label.find("###");
      IM_CHECK_NO_RET(hash != std::string::npos && hash > 0);
      if (hash == std::string::npos || hash == 0)
        continue;
      const auto [it, fresh] = byGlyph.emplace(label.substr(0, hash), id);
      if (!fresh)
        ctx->LogError("%s and %s share one glyph", it->second.c_str(), id);
      IM_CHECK_NO_RET(fresh);
    }
    // The rotary index only on a machine that has the axis.
    IM_CHECK(!ctx->ItemExists("**/###tb_job_index_face"));
    ImGuiWindow* bar = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
      if (std::string(w->Name).find("##ToolbarRow") != std::string::npos) {
        bar = w;
        break;
      }
    IM_CHECK(bar != nullptr);
    IM_CHECK(bar->ContentSize.x <= bar->InnerRect.GetWidth() + 1.0f);
    {
      ImGuiWindow* cam = ImGui::FindWindowByName(ui::kPanelCam);
      IM_CHECK(cam != nullptr && cam->WasActive);
      IM_CHECK(cam->DockNode != nullptr &&
               cam->DockNode->SelectedTabId == cam->TabId);
    }

    // A verb button is the verb: it opens THAT verb's window and arms its
    // pick, lit while armed; pressing it again puts it away.
    ToolbarClick(ctx, "###tb_job_profile");
    ctx->Yield(3);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(s_App->camVerb == ui::AppState::CamVerb::Profile);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamProfileOutline);
    IM_CHECK(ImGui::FindWindowByName("Cut it out###ToolWindow") != nullptr);
    ToolbarClick(ctx, "###tb_job_profile");
    ctx->Yield(3);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(s_App->slotPick.featureId != ui::kCamToolSlot);
  };

  // A verb derives for the LAST TOOL USED, and says which. Every number a
  // verb makes is a fraction of the cutter, so the tool has to be visible
  // where the verb is pressed — and the tool you just cut with is the one in
  // the spindle, so it is the default, across sessions, because the answer
  // is read from the job's operations and not from the session.
  t = IM_REGISTER_TEST(engine, "cam", "verb_defaults_to_the_last_tool");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    {
      cam::Setup* live = Job()->FindSetup(setup.id);
      cam::Tool small = live->tools[0];
      small.number = 2;
      small.name = "small";
      small.diameter = 3.0;
      live->tools.push_back(small);
    }
    ctx->Yield(2);
    // Nothing cut yet: the first tool.
    IM_CHECK_EQ(ui::VerbTool(*s_App, *Job()->FindSetup(setup.id)), 1);

    // The whole part, cut with the tool in the window: T2. The verb needs no
    // pick, so the cut is made at once with the default, and choosing the
    // other tool in ITS window re-derives that cut rather than asking for a
    // remove-and-redo.
    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(s_App->camVerb == ui::AppState::CamVerb::Whole);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK_EQ(live->operations[0].toolNumber, 1);
    const uint64_t made = live->operations[0].id;
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_verb_t2");
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)1); // re-derived, not added
    IM_CHECK_EQ(live->operations[0].id, made);
    IM_CHECK_EQ(live->operations[0].toolNumber, 2);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camToolpaths[0].operationId == made);
    IM_CHECK(s_App->camToolpaths[0].error.empty());

    // A NEW session — the choice forgotten — still answers T2: it is what
    // the newest operation cuts with.
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    s_App->camVerbTool = 0;
    IM_CHECK_EQ(ui::VerbTool(*s_App, *live), 2);
    ToolbarClick(ctx, "###tb_job_face");
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->operations.size(), (size_t)2);
    IM_CHECK_EQ(live->operations[1].toolNumber, 2);
    ctx->ItemClick("//Face the top###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
  };

  // The backplot is DRAWN on a job tab. Every CAM contributor in UI.cpp was
  // gated on `state.document`, which a job tab does not have, so the one
  // kind of tab CAM is drawn on since v73 drew no backplot, no stock box and
  // no ghost — and every test kept passing, because they call the
  // contributors directly and never asked the renderer what it was handed.
  t = IM_REGISTER_TEST(engine, "cam",
                       "backplot_reaches_the_renderer_on_a_job_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    IM_CHECK(s_App->document == nullptr); // a job tab: no part document
    IM_CHECK(s_App->sketchRenderer != nullptr);
    ctx->Yield(2);
    const size_t before = s_App->sketchRenderer->PrimitiveCount();

    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camShowToolpath);
    // What the renderer HOLDS grew by the backplot, not just what the
    // contributor would produce if asked.
    std::vector<PrimVertex> backplot;
    ui::AppendCamToolpathOverlay(*s_App, backplot);
    IM_CHECK(!backplot.empty());
    IM_CHECK(s_App->sketchRenderer->PrimitiveCount() >=
             before + backplot.size());
    const size_t withPath = s_App->sketchRenderer->PrimitiveCount();

    // ...and hiding the path takes it back out. Measured against what the
    // renderer HELD with the path shown, not against `before + backplot` --
    // that margin only held while the backplot was enormous, so collapsing
    // collinear runs for the frame budget broke a test that was really only
    // asserting "a toolpath is big".
    s_App->camShowToolpath = false;
    ctx->Yield(2);
    IM_CHECK(s_App->sketchRenderer->PrimitiveCount() < withPath);
    s_App->camShowToolpath = true;
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
  };

  // A verb whose operation would cut NOTHING is refused, not added. The
  // derivation only sees what it can see (a wall, a depth); the generator
  // finds the rest — a 6 mm drill asked for a 4 mm hole — and its answer
  // used to arrive as a red line under a dead row the user then had to
  // notice and remove. Twice, in the screenshot that prompted this. (The
  // 8 mm hole that screenshot showed is BORED since R3; a hole the tool
  // cannot fit in is the refusal that remains.)
  t = IM_REGISTER_TEST(engine, "cam", "a_dead_operation_is_refused_not_added");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    const uint64_t plate = CamPart()->GetFeature(0)->GetId();
    // A 4 mm hole through it: narrower than the 6 mm tool.
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Hole", glm::vec3(30, 20, -1), glm::vec3(0, 0, 1), 2.0, 14.0));
    const uint64_t hole = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Drilled", plate, hole, BooleanOp::Subtract));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job"); // arrives with a 6 mm flat
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);

    ui::SelectionItem wall;
    wall.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : CamPart()->GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::vec3 o(0.0f), n(0.0f), u(0.0f);
        if (!b.shape.FacePlane(index, o, n, u)) // curved: the hole's wall
          wall.element = ElementRef{b.createdBy, name};
      }
    IM_CHECK(wall.element.IsSet());

    ToolbarClick(ctx, "###tb_job_drill");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.slot, (int)ui::PickSlot::CamDrillHole);
    ui::CamFacePicked(*s_App, wall);
    ctx->Yield(2);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live->operations.empty()); // refused, not a dead row
    IM_CHECK(s_App->camToolpaths.empty());
    IM_CHECK(!s_App->slotPick.miss.empty());
    ctx->LogInfo("refused: %s", s_App->slotPick.miss.c_str());
    IM_CHECK(s_App->slotPick.miss.find("4 mm") != std::string::npos);
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot); // still armed
    ToolbarClick(ctx, "###tb_job_drill");                     // put it away
    ctx->Yield(2);
  };

  // A number typed into a CAM field TAKES on a job tab. Every ExprDrag field
  // evaluated against `state.document->Symbols()` behind a null check that
  // refused to commit, and a job tab has no document: the tool diameter
  // accepted "8", then snapped back to 6.000 on Enter, with the error sent to
  // a prompt bar the tab does not draw.
  t = IM_REGISTER_TEST(engine, "cam", "tool_size_can_be_typed_on_a_job_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(4);
    IM_CHECK(s_App->document == nullptr);
    IM_CHECK(std::abs(Job()->FindSetup(setup.id)->tools[0].diameter - 6.0) <
             1e-9);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_tools");
    ctx->Yield(2);
    // A tool's fields are edited in the tool window, from its row
    // (docs/ui/consistency-round-plan.md).
    ctx->ItemDoubleClick("tools/###cam_tool_1");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditTool, 1);
    // ItemInputValue IS the gesture: Ctrl+click the drag, type, Enter.
    ctx->ItemInputValue("//###ToolWindow/###dia", "8");
    ctx->Yield(3);
    IM_CHECK(std::abs(Job()->FindSetup(setup.id)->tools[0].diameter - 8.0) <
             1e-6);
    // ...and an expression, which is what the field is for.
    ctx->ItemInputValue("//###ToolWindow/###dia", "5*2");
    ctx->Yield(3);
    IM_CHECK(std::abs(Job()->FindSetup(setup.id)->tools[0].diameter - 10.0) <
             1e-6);
    ctx->ItemClick("//###ToolWindow/###cam_tool_editor_close");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditTool, 0);
    IM_CHECK(!s_App->camToolOpen);
  };

  // The 3D finishing verbs (docs/cam/surface-verbs-plan.md) were written with
  // their picks and derivations and never given a button. Clicked here, on a
  // curved face that every other verb refuses.
  t = IM_REGISTER_TEST(engine, "cam", "the_surface_verbs_take_a_curved_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Bar", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 12.0, 30.0));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("Three axis");
    s_App->camActiveSetup = added.id;
    Job()->FindSetup(added.id)->tools[0].type = cam::ToolType::Ball;
    ctx->Yield(2);

    // The bar's round side: the one face with no plane.
    const Body& body = CamPart()->GetBodies()[0];
    ui::SelectionItem side;
    side.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (!body.shape.FacePlane(index, o, n, u)) {
        side.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    IM_CHECK(side.element.IsSet());

    // No 4th axis: the rotary verbs are not on the bar.
    ctx->SetRef("//##DockHost");
    IM_CHECK_EQ(
        ctx->ItemInfo("**/###tb_job_rotary_finish", ImGuiTestOpFlags_NoError)
            .ID,
        (ImGuiID)0);

    const struct {
      const char* button;
      cam::OpType type;
    } verbs[] = {{"###tb_job_surface", cam::OpType::SurfaceParallel},
                 {"###tb_job_walls", cam::OpType::SurfaceWaterline}};
    for (const auto& v : verbs) {
      ToolbarClick(ctx, v.button);
      ctx->Yield();
      IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
      IM_CHECK_EQ(s_App->slotPick.slot,
                  static_cast<int>(ui::PickSlot::CamSurfaceRegion));
      ui::CamFacePicked(*s_App, side);
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->slotPick.miss, std::string());
      const cam::Setup* live = Job()->FindSetup(added.id);
      IM_CHECK(!live->operations.empty());
      IM_CHECK(live->operations.back().type == v.type);
      ctx->KeyPress(ImGuiKey_Escape); // disarm before the next verb
      ctx->Yield();
    }
    IM_CHECK_EQ(Job()->FindSetup(added.id)->operations.size(), (size_t)2);
    for (const cam::Toolpath& tp : s_App->camToolpaths)
      IM_CHECK_EQ(tp.error, std::string());

    s_App->camToolOpen = false;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // The rotary verbs: no pick, only on a machine with the axis, a rough in
  // layers that leaves a skin and a finish that takes it.
  t = IM_REGISTER_TEST(engine, "cam", "the_rotary_verbs_rough_then_finish");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Bar", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 12.0, 30.0));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("On the trunnion");
    s_App->camActiveSetup = added.id;
    cam::Setup* setup = Job()->FindSetup(added.id);
    setup->machine.hasRotaryAxis = true;
    setup->safeZ = 30.0;
    setup->stock.margin = {0.0, 3.0, 3.0};
    setup->tools[0].type = cam::ToolType::Ball;
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_job_rotary_rough");
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_rotary_finish");
    ctx->Yield(2);
    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)2);
    if (setup->operations.size() == 2) {
      const cam::Operation& rough = setup->operations[0];
      const cam::Operation& finish = setup->operations[1];
      IM_CHECK(rough.type == cam::OpType::SurfaceRotary);
      IM_CHECK(finish.type == cam::OpType::SurfaceRotary);
      IM_CHECK_GT(rough.stepdown, 0.0);     // layers
      IM_CHECK_GT(rough.stockToLeave, 0.0); // a skin
      IM_CHECK_EQ(finish.stepdown, 0.0);    // one pass, to the part
      IM_CHECK_EQ(finish.stockToLeave, 0.0);
    }
    IM_CHECK_EQ(s_App->camToolpaths.size(), (size_t)2);
    for (const cam::Toolpath& tp : s_App->camToolpaths) {
      IM_CHECK_EQ(tp.error, std::string());
      IM_CHECK(!tp.moves.empty());
    }

    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // Floating windows share one placement (ui/FloatingWindow.h). Generate used
  // to anchor at exactly the tool window's corner, so opening it while an op
  // or a tool was being edited put it on top of that editor.
  t = IM_REGISTER_TEST(engine, "cam", "generate_opens_below_the_tool_window");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    ui::EditCamTool(*s_App, setup.tools[0].number);
    s_App->camGenerateOpen = true;
    ctx->Yield(4);
    ImGuiWindow* tool = ImGui::FindWindowByName("###ToolWindow");
    ImGuiWindow* gen = ImGui::FindWindowByName("###CamGenerate");
    IM_CHECK(tool != nullptr && tool->WasActive);
    IM_CHECK(gen != nullptr && gen->WasActive);
    // Below it, with no overlap.
    IM_CHECK_GE(gen->Pos.y, tool->Pos.y + tool->Size.y);
    // And with the tool window gone, it moves back up to the corner.
    ctx->ItemClick("//###ToolWindow/###cam_tool_editor_close");
    ctx->Yield(4);
    IM_CHECK_LT(gen->Pos.y, tool->Pos.y + tool->Size.y);
    s_App->camGenerateOpen = false;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // A tool is a row: Delete is in its menu, and REFUSED while an operation
  // cuts with it. There was no way to delete a tool from a setup at all.
  t = IM_REGISTER_TEST(engine, "cam",
                       "a_tool_deletes_from_its_row_unless_in_use");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    cam::Tool spare = setup.tools[0];
    spare.number = 2;
    spare.name = "spare";
    setup.tools.push_back(spare);
    cam::Operation op;
    op.id = 77;
    op.name = "Pocket A";
    op.toolNumber = 1;
    setup.operations.push_back(op);
    ctx->Yield(4);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_tools");
    ctx->Yield(2);

    // T1 is in use: the refusal names the operation, and nothing goes.
    std::string why;
    IM_CHECK(!ui::RemoveCamTool(*s_App, 1, &why));
    IM_CHECK(why.find("Pocket A") != std::string::npos);
    IM_CHECK_EQ(Job()->FindSetup(setup.id)->tools.size(), (size_t)2);

    // T2 is not: its row's menu deletes it.
    ctx->ItemClick("tools/###cam_tool_2", ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_tool_delete");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(setup.id)->tools.size(), (size_t)1);
    IM_CHECK_EQ(Job()->FindSetup(setup.id)->tools[0].number, 1);

    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // Measure works on a job tab. The button was greyed out there: the gate
  // asked for a part or an assembly, and the element resolution knew only
  // those two sources — while the pick pipeline under it already knew the
  // job's bodies. A job pick is measured against ITS part's bodies, the way
  // an occurrence pick is, because two parts in one job share element names.
  t = IM_REGISTER_TEST(engine, "cam", "measure_works_on_a_job_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    MakeJob();
    ctx->Yield(2);
    IM_CHECK(s_App->document == nullptr);
    IM_CHECK(ui::CommandEnabled(*s_App, "view.measure"));
    ToolbarClick(ctx, "###tb_job_measure");
    ctx->Yield(2);
    IM_CHECK(s_App->measure.active);

    // The two X faces, 60 mm apart.
    const Body& body = ui::JobBodies(*s_App)[0];
    std::string px, nx;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (!body.shape.FacePlane(index, o, n, u))
        continue;
      if (n.x > 0.99f)
        px = name;
      else if (n.x < -0.99f)
        nx = name;
    }
    IM_CHECK(!px.empty() && !nx.empty());
    ui::MeasureState::Element a =
        ui::MeasureState::Element::OfFace(ElementRef{body.createdBy, px});
    ui::MeasureState::Element b =
        ui::MeasureState::Element::OfFace(ElementRef{body.createdBy, nx});
    a.jobPartId = b.jobPartId = body.jobPartId;
    IM_CHECK(a.jobPartId != 0);
    ui::MeasureElementClicked(*s_App, a);
    ctx->Yield();
    ctx->LogInfo("one face: %s", s_App->measure.readout.c_str());
    IM_CHECK(!s_App->measure.readout.empty());
    IM_CHECK(s_App->measure.readout.find("Could not") == std::string::npos);
    ui::MeasureElementClicked(*s_App, b);
    ctx->Yield();
    ctx->LogInfo("two faces: %s", s_App->measure.readout.c_str());
    IM_CHECK(s_App->measure.readout.rfind("Distance:", 0) == 0);
    IM_CHECK(s_App->measure.readout.find("60") != std::string::npos);
    s_App->measure = ui::MeasureState{};
  };

  // Preferences > Machines is the store (docs/cam/job-document.md, round
  // two): New writes a file, a rename moves it, an edit rewrites it, Delete
  // removes it. The GUI runner points XDG_CONFIG_HOME at a scratch dir, so
  // this is a scratch store; the test still cleans up by name.
  t = IM_REGISTER_TEST(engine, "cam", "machines_are_kept_in_preferences");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    for (const char* stale : {"Machine 1", "Test router"})
      (void)ui::DeleteMachine(*s_App, stale);
    const size_t before = s_App->machines.size();
    ctx->MenuClick("Edit/###edit_preferences");
    ctx->Yield();
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemOpen("###sec_Machines");
    ctx->Yield();
    ctx->ItemClick("###mach_new");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->machines.size(), before + 1);
    IM_CHECK(cam::FindMachine(s_App->machines, "Machine 1") != nullptr);
    const std::filesystem::path dir(ui::MachineDir());
    IM_CHECK(std::filesystem::exists(dir / "machine-1.json"));

    // Rename moves the file.
    ctx->ItemInputValue("###mach_name", "Test router");
    ctx->Yield(3);
    IM_CHECK(cam::FindMachine(s_App->machines, "Test router") != nullptr);
    IM_CHECK(cam::FindMachine(s_App->machines, "Machine 1") == nullptr);
    IM_CHECK(std::filesystem::exists(dir / "test-router.json"));
    IM_CHECK(!std::filesystem::exists(dir / "machine-1.json"));

    // An edit rewrites it: typed, so the field's expression path is what is
    // tested, and read back from DISK rather than from the struct. ONE axis
    // of the work area first: the file used to drop the whole area until
    // all three were stated, so the X just typed snapped back to zero.
    ctx->ItemInputValue("###mach_wa_x", "600");
    ctx->Yield(3);
    ctx->ItemInputValue("###mach_rapid", "3000");
    ctx->Yield(3);
    ctx->ItemClick("###mach_rotary");
    ctx->Yield(3);
    const auto fromDisk = DocumentIO::LoadMachines(ui::MachineDir());
    const cam::Machine* saved = cam::FindMachine(fromDisk, "Test router");
    IM_CHECK(saved != nullptr);
    IM_CHECK(std::abs(saved->rapidFeed - 3000.0) < 1e-9);
    IM_CHECK(saved->hasRotaryAxis);
    IM_CHECK(std::abs(saved->travelMax.x - 600.0) < 1e-9);
    IM_CHECK(!saved->HasWorkArea()); // Y and Z still unstated, and kept so

    ctx->ItemClick("###mach_delete");
    ctx->Yield(2);
    IM_CHECK(cam::FindMachine(s_App->machines, "Test router") == nullptr);
    IM_CHECK(!std::filesystem::exists(dir / "test-router.json"));
    IM_CHECK_EQ(s_App->machines.size(), before);
    s_App->settingsWindowOpen = false;
  };

  // A setup names a machine and EMBEDS a copy; the panel says when the copy
  // and the store disagree, and offers both directions. Every reference
  // surveyed reconciles this quietly (docs/cam/job-document.md, round two).
  t = IM_REGISTER_TEST(engine, "cam",
                       "a_setup_names_a_machine_and_says_when_it_differs");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    (void)ui::DeleteMachine(*s_App, "Test shop router");
    cam::Machine mine;
    mine.name = "Test shop router";
    mine.travelMax = {600.0, 400.0, 80.0};
    mine.rapidFeed = 2500.0;
    IM_CHECK(ui::SaveMachine(*s_App, mine));

    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(3);
    IM_CHECK(cam::MatchMachine(s_App->machines, setup.machine) ==
             cam::MachineMatch::Unnamed);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_machine");
    ctx->ItemClick("//$FOCUSED/Test shop router");
    ctx->Yield(2);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live->machine == mine);
    IM_CHECK(std::abs(live->rapidFeed - 2500.0) < 1e-9); // seeded
    IM_CHECK(cam::MatchMachine(s_App->machines, live->machine) ==
             cam::MachineMatch::Global);
    IM_CHECK(!ctx->ItemExists("###cam_machine_use_mine"));

    // Edit the JOB'S copy: it now differs, and the panel offers mine back.
    ctx->ItemOpen("###cam_stock");
    ctx->Yield();
    ctx->ItemClick("###cam_arcs");
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(!live->machine.fitArcs);
    IM_CHECK(cam::MatchMachine(s_App->machines, live->machine) ==
             cam::MachineMatch::Differs);
    IM_CHECK(ctx->ItemExists("###cam_machine_use_mine"));
    ctx->ItemClick("###cam_machine_use_mine");
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(live->machine == mine);

    // The store loses it: the job keeps its copy and says so; Save it puts
    // it back.
    IM_CHECK(ui::DeleteMachine(*s_App, "Test shop router"));
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(cam::MatchMachine(s_App->machines, live->machine) ==
             cam::MachineMatch::Missing);
    IM_CHECK(live->machine.name == "Test shop router"); // the copy survives
    ctx->ItemClick("###cam_machine_save");
    ctx->Yield(2);
    IM_CHECK(cam::MatchMachine(s_App->machines, live->machine) ==
             cam::MachineMatch::Global);
    IM_CHECK(ui::DeleteMachine(*s_App, "Test shop router"));
  };

  // The stock and the machine's work area are drawn on a job tab whether or
  // not a verb window is open (round two, M2); the work area turns red and
  // the post refuses when the job does not fit it. The machine is never
  // grown to fit.
  t = IM_REGISTER_TEST(engine, "cam", "stock_and_work_area_are_always_drawn");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Panel", glm::vec3(0, 0, 0), 880, 380, 18));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolOpen); // no session

    std::vector<PrimVertex> prims;
    std::vector<LineVertex3D> quads;
    ui::AppendCamVisualisation(*s_App, prims, quads);
    IM_CHECK(!quads.empty()); // the stock's walls, with no session open
    const size_t stockOnly = prims.size();

    // A machine with a work area adds its box...
    cam::Setup* live = Job()->FindSetup(setup.id);
    live->machine.name = "Test big router";
    live->machine.travelMin = {0.0, 0.0, -100.0};
    live->machine.travelMax = {1200.0, 800.0, 0.0};
    ctx->Yield(2);
    prims.clear();
    quads.clear();
    ui::AppendCamVisualisation(*s_App, prims, quads);
    IM_CHECK(prims.size() > stockOnly);
    IM_CHECK(cam::MachineWarning(*live, ui::JobBodies(*s_App)).empty());

    // ...and a machine too small WARNS: the cut is made, as far as the
    // machine reaches, and the verdict says how far that is.
    live->machine.travelMax = {600.0, 800.0, 0.0};
    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(s_App->camRefusal.empty());
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    bool warned = false;
    for (const cam::VerdictLine& line : s_App->camVerdict)
      warned |= line.level == cam::VerdictLine::Level::Warn &&
                line.text.find("600") != std::string::npos &&
                line.text.find("beyond") != std::string::npos;
    IM_CHECK(warned);
    IM_CHECK(live->machine.travelMax.x == 600.0); // not grown
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
  };

  // A job is made the way an assembly is (round two, M3): New Job on the
  // tab bar, then the panel's setup step -- machine, stock, parts. With no
  // machine defined the step REFUSES, visibly, and points at Preferences
  // (u10); with one it is chosen for you. The part bar's CAM button is gone.
  t = IM_REGISTER_TEST(engine, "cam", "a_job_is_made_from_the_tab_bar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    (void)ui::DeleteMachine(*s_App, "Test tab router");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    ctx->SetRef("//##DockHost");
    IM_CHECK(!ctx->ItemExists("**/###tb_cam")); // gone from the part bar

    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New CAM Job##addjob");
    ctx->Yield(4);
    IM_CHECK(s_App->job != nullptr);
    IM_CHECK(s_App->document == nullptr);
    IM_CHECK(Job()->GetSetups().empty());
    IM_CHECK(Job()->GetParts().empty());

    // No machine: the step refuses. The button is there and disabled, the
    // reason is said, and nothing was made.
    const bool hadMachines = !s_App->machines.empty();
    ctx->SetRef("//CAM###CamPanel");
    if (!hadMachines) {
      IM_CHECK(ctx->ItemExists("###cam_new_machine_prefs"));
      IM_CHECK(!ctx->ItemExists("###cam_new_machine"));
      ctx->ItemClick("###cam_add_setup", ImGuiMouseButton_Left,
                     ImGuiTestOpFlags_NoError);
      ctx->Yield(2);
      IM_CHECK(Job()->GetSetups().empty());
    }

    // With a machine, it is chosen for you; a part comes in; the setup is
    // made on that machine, with the sheet's thickness from the part.
    cam::Machine mine;
    mine.name = "Test tab router";
    mine.travelMax = {600.0, 400.0, 80.0};
    IM_CHECK(ui::SaveMachine(*s_App, mine));
    ctx->Yield(2);
    IM_CHECK(ctx->ItemExists("###cam_new_machine"));
    ctx->ItemClick("###cam_new_machine");
    ctx->ItemClick("//$FOCUSED/Test tab router");
    ctx->Yield();
    IM_CHECK_EQ(s_App->camNewSetup.machine, std::string("Test tab router"));
    // The plate is the only part: one Add button, PushID'd by its slot id.
    uint64_t plateSlot = 0;
    for (const ui::PartSlot& slot : s_App->parts)
      if (slot.kind == ui::DocKind::Part)
        plateSlot = slot.id;
    IM_CHECK(plateSlot != 0);
    ctx->ItemClick(("$$" + std::to_string(static_cast<int>(plateSlot)) +
                    "/###cam_new_part_add")
                       .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1);
    ctx->ItemClick("###cam_new_stock_fixed");
    ctx->Yield(2);
    IM_CHECK(std::abs(s_App->camNewSetup.sheet.x - 600.0f) < 1e-3f); // the bed
    IM_CHECK(std::abs(s_App->camNewSetup.sheet.z - 12.0f) < 1e-3f);  // the part
    ctx->ItemClick("###cam_add_setup");
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->GetSetups().size(), (size_t)1);
    const cam::Setup& made = Job()->GetSetups()[0];
    IM_CHECK(made.machine == mine);
    IM_CHECK(made.stock.mode == cam::StockMode::Fixed);
    IM_CHECK(std::abs(made.stock.size.x - 600.0) < 1e-6);
    IM_CHECK(std::abs(made.stock.size.z - 12.0) < 1e-6);

    // ...and the verbs work on it: the whole flow, no part-bar button.
    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->GetSetups()[0].operations.size(), (size_t)1);
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    IM_CHECK(ui::DeleteMachine(*s_App, "Test tab router"));
  };

  // The machine's tool library (M4): edited in Preferences, COPIED onto a
  // new setup on that machine, and offered first by the panel's Add tool.
  t = IM_REGISTER_TEST(engine, "cam",
                       "a_setup_takes_its_tools_from_the_machine");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    (void)ui::DeleteMachine(*s_App, "Test tooled router");
    cam::Machine mine;
    mine.name = "Test tooled router";
    IM_CHECK(ui::SaveMachine(*s_App, mine));

    // Two tools into its library through Preferences; read back from disk.
    ctx->MenuClick("Edit/###edit_preferences");
    ctx->Yield();
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemOpen("###sec_Machines");
    ctx->Yield();
    for (size_t i = 0; i < s_App->machines.size(); i++)
      if (s_App->machines[i].name == "Test tooled router")
        s_App->machineEditIndex = static_cast<int>(i);
    ctx->Yield(2);
    ctx->ItemClick("###mach_add_tool");
    ctx->Yield(2);
    ctx->ItemClick("###mach_add_tool");
    ctx->Yield(2);
    ctx->ItemInputValue("$$2/###libdia", "8");
    ctx->Yield(3);
    s_App->settingsWindowOpen = false;
    ctx->Yield();
    const auto fromDisk = DocumentIO::LoadMachines(ui::MachineDir());
    const cam::Machine* saved =
        cam::FindMachine(fromDisk, "Test tooled router");
    IM_CHECK(saved != nullptr);
    IM_CHECK_EQ(saved->library.size(), (size_t)2);
    IM_CHECK(std::abs(saved->library[1].diameter - 8.0) < 1e-9);

    // A setup made on it starts with that table, not the default 6 mm.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    MakeJob();
    ctx->Yield(3);
    s_App->camNewSetup.machine = "Test tooled router";
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_add_setup");
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->GetSetups().size(), (size_t)1);
    const cam::Setup& made = Job()->GetSetups()[0];
    IM_CHECK_EQ(made.tools.size(), (size_t)2);
    IM_CHECK(std::abs(made.tools[1].diameter - 8.0) < 1e-9);
    // ...and the verb's tool row shows both, defaulting to the first.
    IM_CHECK_EQ(ui::VerbTool(*s_App, made), 1);
    IM_CHECK(ui::DeleteMachine(*s_App, "Test tooled router"));
  };

  // A placement is dragged in the viewport (round two, M5): the occurrence
  // gizmo's X/Y arrows on the selected job part move it in the stock, the Z
  // arc spins it. Driven through the same state machine the mouse drives,
  // with rays aimed at the handle, so the camera stays out of it.
  t = IM_REGISTER_TEST(engine, "cam", "a_placement_is_dragged_in_the_viewport");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    const Body& body = ui::JobBodies(*s_App)[0];
    IM_CHECK(body.jobPartId != 0);

    // Nothing selected: no handles.
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK(s_App->viewportHandles.empty());

    // Select the part (a Body item, as a click on it would leave).
    ui::SelectionItem item;
    item.kind = ui::SelectionItem::Kind::Body;
    item.id = body.createdBy;
    item.jobPartId = body.jobPartId;
    s_App->selection.items = {item};
    ctx->Yield(2);
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK_EQ(s_App->viewportHandles.size(), (size_t)3); // X, Y, spin
    // The handles sit on the part: at its top centre.
    const glm::vec3 origin = s_App->viewportHandles[0].origin;
    IM_CHECK(std::abs(origin.x - 30.0f) < 1e-3f);
    IM_CHECK(std::abs(origin.y - 20.0f) < 1e-3f);
    IM_CHECK(std::abs(origin.z - 12.0f) < 1e-3f);

    // Grab the X arrow with a ray straight down onto its middle, drag it
    // 25 mm along X, release.
    const float L = ui::GizmoScale(*s_App);
    Ray grab;
    grab.origin = origin + glm::vec3(0.5f * L, 0.0f, 100.0f);
    grab.direction = {0.0f, 0.0f, -1.0f};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    IM_CHECK(s_App->gizmoDrag.active);
    IM_CHECK_EQ(s_App->gizmoDrag.jobPartId, body.jobPartId);
    IM_CHECK_EQ(s_App->gizmoDrag.axis, 0);
    IM_CHECK(!s_App->gizmoDrag.rotate);
    Ray moved = grab;
    moved.origin.x += 25.0f;
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    // No Yield between move and release: a frame in between runs the
    // viewport's own gizmo pass, which reads the REAL mouse (up) and ends
    // the drag itself -- correct for a user, wrong for a scripted one.
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK_EQ(live->placements.size(), (size_t)1); // made on grab
    IM_CHECK(std::abs(live->placements[0].pos.x - 25.0) < 0.5);
    IM_CHECK(std::abs(live->placements[0].pos.y) < 1e-6);
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    IM_CHECK(!s_App->gizmoDrag.active);
    ctx->Yield(2);
    // The drawn part followed the drag.
    glm::vec3 bmin, bmax;
    ui::JobPlacedBodies(*s_App)[0].shape.GetBoundingBox(bmin, bmax);
    IM_CHECK(std::abs(bmin.x - 25.0f) < 0.5f);
    // ...and the handles moved with it.
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK(std::abs(s_App->viewportHandles[0].origin.x - 55.0f) < 0.5f);

    // SNAPPING: on a fixed 200 x 100 sheet, a drag that leaves the part's
    // far edge a hair short of the sheet's edge lands it exactly there.
    {
      cam::Setup* s = Job()->FindSetup(setup.id);
      s->stock.mode = cam::StockMode::Fixed;
      s->stock.size = {200.0, 100.0, 12.0};
      s->placements[0].pos = {0.0, 0.0};
      s_App->job->BumpVersion();
    }
    ctx->Yield(2);
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    const glm::vec3 o2 = s_App->viewportHandles[0].origin;
    Ray grab2;
    grab2.origin = o2 + glm::vec3(0.5f * L, 0.0f, 100.0f);
    grab2.direction = {0.0f, 0.0f, -1.0f};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab2, true, true, false));
    Ray nearRay = grab2;
    nearRay.origin.x += 139.0f; // the part's far edge would sit at 199, 1 short
    IM_CHECK(ui::HandleGizmoInput(*s_App, nearRay, false, true, false));
    ctx->LogInfo("gizmo scale %.2f, snapped x %.3f", L,
                 Job()->FindSetup(setup.id)->placements[0].pos.x);
    IM_CHECK(std::abs(Job()->FindSetup(setup.id)->placements[0].pos.x - 140.0) <
             1e-3); // snapped: far edge ON the sheet's edge at 200
    IM_CHECK(ui::HandleGizmoInput(*s_App, nearRay, false, false, true));
    ctx->Yield(2);

    // ...and a spin to the common angles: 88 degrees becomes 90.
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    const glm::vec3 o3 = s_App->viewportHandles[0].origin;
    // Grabbed BETWEEN the arrows (135 degrees): on the +X axis the arc and
    // the X arrow coincide, and the arrow wins the tie.
    const float R = 0.78f * L;
    const float a0 = glm::radians(135.0f), a1 = glm::radians(135.0f + 88.0f);
    Ray arc;
    arc.origin = o3 + glm::vec3(R * std::cos(a0), R * std::sin(a0), 100.0f);
    arc.direction = {0.0f, 0.0f, -1.0f};
    IM_CHECK(ui::HandleGizmoInput(*s_App, arc, true, true, false));
    IM_CHECK(s_App->gizmoDrag.rotate);
    Ray turned = arc;
    turned.origin = o3 + glm::vec3(R * std::cos(a1), R * std::sin(a1), 100.0f);
    IM_CHECK(ui::HandleGizmoInput(*s_App, turned, false, true, false));
    IM_CHECK(
        std::abs(std::abs(Job()->FindSetup(setup.id)->placements[0].rotZdeg) -
                 90.0) < 1e-6);
    IM_CHECK(ui::HandleGizmoInput(*s_App, turned, false, false, true));
    ctx->Yield(2);
    s_App->selection.Clear();
  };

  // A job edit undoes (router-round.md R7): a verb, a placement drag and a
  // panel field each become one step, and redo brings each back. Nothing on
  // a job tab could be undone before this.
  t = IM_REGISTER_TEST(engine, "cam", "a_job_edit_undoes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    // The id, not a reference: undo REPLACES the job, and a reference into
    // the old one is exactly the dangling read that crashed this test first.
    const uint64_t setupId = MakeJob()->AddSetup("Job").id;
    s_App->camActiveSetup = setupId;
    ctx->Yield(3);
    IM_CHECK(!ui::CanGlobalUndo(*s_App));

    // A verb: one step.
    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(setupId)->operations.size(), (size_t)1);
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    IM_CHECK(ui::CanGlobalUndo(*s_App));
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(setupId)->operations.empty());
    IM_CHECK(s_App->camToolpaths.empty()); // the picture followed
    ui::GlobalRedo(*s_App);
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(setupId)->operations.size(), (size_t)1);
    IM_CHECK(!s_App->camToolpaths.empty());

    // A placement drag: one step for the whole drag.
    const Body& body = ui::JobBodies(*s_App)[0];
    ui::SelectionItem item;
    item.kind = ui::SelectionItem::Kind::Body;
    item.id = body.createdBy;
    item.jobPartId = body.jobPartId;
    s_App->selection.items = {item};
    ctx->Yield(2);
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK(!s_App->viewportHandles.empty());
    const glm::vec3 origin = s_App->viewportHandles[0].origin;
    const float L = ui::GizmoScale(*s_App);
    Ray grab;
    grab.origin = origin + glm::vec3(0.5f * L, 0.0f, 100.0f);
    grab.direction = {0.0f, 0.0f, -1.0f};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    Ray moved = grab;
    moved.origin.x += 20.0f;
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(setupId)->placements.size(), (size_t)1);
    s_App->selection.Clear();
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(setupId)->placements.empty());
    IM_CHECK_EQ(Job()->FindSetup(setupId)->operations.size(), (size_t)1);

    // A panel field: typed, one step, through the guard the panel's every
    // edit passes through.
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_stock");
    ctx->Yield(2);
    const double safeBefore = Job()->FindSetup(setupId)->safeZ;
    ctx->ItemInputValue("###cam_safez", "77");
    ctx->Yield(4);
    IM_CHECK(std::abs(Job()->FindSetup(setupId)->safeZ - 77.0) < 1e-6);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(std::abs(Job()->FindSetup(setupId)->safeZ - safeBefore) < 1e-6);
  };

  // The terminal panel (docs/ui/terminal-panel.md). Off by default, docked
  // right when on, and — the part that decides whether it feels safe — the
  // keyboard goes to it ONLY while the pointer is over it.
#endif
}
