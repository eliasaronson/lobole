// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/guitests/GuiTestsCommon.h"
#include "ui/DocumentSlots.h"
#include "ui/MeasureActions.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include "cad/FaceSelector.h" // Ray
#include "core/cam/CamMachine.h"
#include "core/cam/CamToolpath.h"
#include "core/features/HoleFeature.h"
#include "core/io/Serialization.h"
#include "ui/AssemblyActions.h" // the placement gizmo
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamTool.h" // CamStockBox
#include "ui/DragMath.h" // GizmoScale
#include "ui/JobActions.h"
#include "ui/ListSelection.h"
#include "ui/MachineActions.h"
#include "ui/ProjectActions.h"    // SaveProject, OpenProject
#include "ui/SelectionPipeline.h" // ArmedPrompt

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <chrono>
#include <filesystem>

#ifdef HAVE_CAM
#include "render/StockRenderer.h"
#include "ui/CamPlayback.h"
#endif

#ifdef HAVE_CAM
namespace {
// The CAM panel is a tree (docs/ui/panels.md#cam-job-tab-the-cam-panel): a
// setup is a row, drawn under PushID("setup<id>"), and its verbs are in its
// menu. Right-click it and click one of them.
std::string CamSetupRow(uint64_t setupId) {
  return "//CAM###CamPanel/**/###cam_setup_" + std::to_string(setupId);
}
void CamSetupMenu(ImGuiTestContext* ctx, uint64_t setupId, const char* item) {
  ctx->ItemClick(CamSetupRow(setupId).c_str(), ImGuiMouseButton_Right);
  ctx->Yield();
  ctx->ItemClick((std::string("//$FOCUSED/") + item).c_str());
  ctx->Yield(2);
}
// The verb window's operations and verdict scroll in a child region of their
// own (docs/cam/gui.md#verb-windows). A child window's path is mangled, so it
// is reached through WindowInfo, and a row is then "$$<op id>/###op_row".
void RefToolWindowList(ImGuiTestContext* ctx, const char* window) {
  ctx->SetRef(
      ctx->WindowInfo((std::string("//") + window + "/cam_tool_list").c_str())
          .Window);
}
// A job part's row, and one of its menu's items.
std::string CamPartRow(uint64_t jobPartId) {
  return "//CAM###CamPanel/parts/###cam_part_" + std::to_string(jobPartId);
}
void CamPartMenu(ImGuiTestContext* ctx, uint64_t jobPartId, const char* item) {
  ctx->ItemClick(CamPartRow(jobPartId).c_str(), ImGuiMouseButton_Right);
  ctx->Yield();
  ctx->ItemClick((std::string("//$FOCUSED/") + item).c_str());
  ctx->Yield(2);
}
} // namespace
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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
    // to set the job up (docs/cam/gui.md#verb-windows).
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
  // on screen at once (docs/cam/gui.md#the-cam-panel).
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
    // The panel is a tree now (docs/ui/panels.md#cam-job-tab-the-cam-panel):
    // Parts, Setups and Tools headers, the setup a row whose tail is the run
    // status (the line that replaced Save G-code), its operation under it.
    // The machine and the post dialect are the setup's settings, in the tool
    // window.
    ctx->SetRef("//CAM###CamPanel");
    for (const char* id :
         {"###cam_parts", "###cam_setups", "###cam_tools", "###cam_parts_add",
          "###cam_setups_add", "###cam_tools_add"})
      IM_CHECK(ctx->ItemExists(id));
    IM_CHECK(ctx->ItemExists("**/###cam_run_status"));
    IM_CHECK(ctx->ItemExists(
        ("**/$$" + std::to_string(static_cast<int>(op.id)) + "/###op_row")
            .c_str()));
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);
    // Its settings: the machine and the post, two separate widgets.
    ctx->ItemDoubleClick(CamSetupRow(setup.id).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditSetup, setup.id);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###cam_machine"));
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###cam_post"));
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###cam_safez"));
    ctx->ItemClick("//###ToolWindow/###cam_setup_editor_close");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditSetup, (uint64_t)0);
    IM_CHECK(!s_App->camToolOpen);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);
  };

  // An operation can be removed from the window that makes them. Removing was
  // only possible in the docked panel, which is a tab behind Model — while
  // every operation is created in the floating verb window. Adding three by
  // mistake (because the first two silently cut nothing) and being unable to
  // take any of them back is how a job ends up with duplicates in it
  // (docs/cam/gui.md#verb-windows).
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
    // buttons to the feature list's gestures
    // (docs/ui/panels.md#cam-job-tab-the-cam-panel).
    const std::string ref =
        "$$" + std::to_string(static_cast<int>(first)) + "/###op_row";
    RefToolWindowList(ctx, "CAM###ToolWindow");
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
    RefToolWindowList(ctx, "CAM###ToolWindow");
    const auto row = [&](size_t i) {
      return "$$" + std::to_string(static_cast<int>(ids[i])) + "/###op_row";
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
  // the failure surfaced in an unrelated test (docs/cam/gui.md#verb-windows).
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
  // means, in the frame THIS setup cuts in (docs/cam/gui.md#the-job-toolbar).
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
    // AlignToFace animates; wait for it to land.
    WaitFor(ctx, "the view to land",
            [] { return !s_App->camera->IsAnimating(); });
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
    WaitFor(ctx, "the view to land",
            [] { return !s_App->camera->IsAnimating(); });
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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

  // The tool window keeps the width its content asks for, and the next
  // session does not inherit it. Clamping the window inside the app AFTER
  // Begin moved it without moving the region its content was laid out in, so
  // the list (a child as wide as the window) reached past the edge by the
  // clamp's step; the window took that as its width and grew 97 px a frame
  // across the viewport. The child then kept that width through the
  // reappearing window's remeasure, and the next session opened 786 px wide
  // (docs/ui/panels.md#floating-windows).
  t = IM_REGISTER_TEST(engine, "cam", "tool_window_keeps_its_own_width");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    const uint64_t setupId = setup.id;
    s_App->camActiveSetup = setupId;
    s_App->camToolOpen = true;
    ctx->Yield(8);
    const auto window = [] {
      return ImGui::FindWindowByName("CAM###ToolWindow");
    };
    IM_CHECK(window() != nullptr);
    const float plain = window()->Size.x;
    const float rightEdge =
        s_App->viewportPos.x + s_App->viewportSize.x - 12.0f;

    const Body& body = CamPart()->GetBodies()[0];
    ui::SelectionItem big;
    big.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (body.shape.FacePlane(index, o, n, u) && n.y < -0.99f)
        big.element = ElementRef{body.createdBy, name};
    }
    IM_CHECK(big.element.IsSet());
    ui::ArmCamPick(*s_App, ui::PickSlot::CamTopFace, "Click the top face");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    ui::ArmCamPick(*s_App, ui::PickSlot::CamPocketFloor, "Click a floor");
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(3);
    ui::CamFacePicked(*s_App, big);
    ctx->Yield(6);
    IM_CHECK_EQ(Job()->FindSetup(setupId)->operations.size(), (size_t)2);

    // Settled: the same width frame after frame, still at the viewport's
    // top-right corner.
    const float settled = window()->Size.x;
    ctx->Yield(10);
    ctx->LogInfo("plain %.0f px, with operations %.0f px, then %.0f px", plain,
                 settled, window()->Size.x);
    IM_CHECK(std::abs(window()->Size.x - settled) < 1.0f);
    IM_CHECK(std::abs(window()->Pos.x + window()->Size.x - rightEdge) < 2.0f);

    // Closed, the list emptied, opened again: as wide as it first was.
    s_App->slotPick = {};
    s_App->camToolOpen = false;
    ctx->Yield(3);
    Job()->FindSetup(setupId)->operations.clear();
    ui::MarkJobEdited(*s_App);
    ui::RefreshCamToolpaths(*s_App);
    s_App->camVerdict.clear();
    s_App->camNote.clear();
    s_App->camToolOpen = true;
    ctx->Yield(8);
    ctx->LogInfo("reopened %.0f px", window()->Size.x);
    IM_CHECK(std::abs(window()->Size.x - plain) < 2.0f);
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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

  // Work origin (docs/cam/setup.md#work-origin): the corner the operator
  // touches off on, chosen by clicking, and honoured by the file. Before v68
  // `origin` was a label the post never read, so "Z zero at 16.000" was a claim
  // no line of the g-code kept.
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

    // Move zero to the stock's centre, and the origin follows.
    Job()->FindSetup(setup.id)->originAt = cam::OriginAt::StockCentre;
    IM_CHECK(cam::RecomputeOrigin(*Job()->FindSetup(setup.id),
                                  CamPart()->GetBodies()));
    ctx->Yield(2);
    live = Job()->FindSetup(setup.id);
    IM_CHECK(std::abs(live->origin.x - (lo.x + hi.x) * 0.5) < 1e-6);

    // And the FILE measures from it: post a real operation and read the
    // extremes back. This is the assertion that fails if the origin goes back
    // to being a label.
    CamSetupMenu(ctx, setup.id, "###cam_add_op");
    ctx->Yield(2);
    // Generate is a toolbar tool with its own window now
    // (docs/ui/panels.md#cam-job-tab-the-cam-panel); closed again so it cannot
    // take the clicks of what follows.
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

  // docs/cam/setup.md#fixturing-frame-and-rotary-index: the case every other
  // fixture in this file dodges by being modelled face-up. A panel that stands
  // upright — an imported speaker baffle, and every part that arrives from STEP
  // or a parametric import in the orientation it has in the assembly — has no
  // level face worth zeroing on. Laying one flat is a statement about the
  // FIXTURE, so it must work on a 3-axis machine and put no A word in the file.
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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

  // The verbs (docs/cam/gui.md#verb-windows). Two clicks — the verb, then the
  // face — produce a machinable operation with nothing typed, the viewport
  // shows what will be removed, and the verdict arrives without being asked
  // for. The CAM window is usable on a part that has no CAM job — which is the
  // only state it can be in the first time anyone opens it. The machining
  // verbs already made a setup on demand; the two picks in the Zero section
  // answered "add a setup first" into a field this window does not draw, so
  // pressing "Pick top face" on a fresh part and clicking a face did nothing
  // at all, visibly or otherwise (docs/cam/gui.md#verb-windows).
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
    // check called clean (docs/cam/simulation.md#the-verifier). Leftover is a
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
    // docs/ui/long-operations.md#cam-toolpath-passes: generating an adaptive
    // operation does not block a frame. The same pocket the two-clicks test
    // derives, on the panel's REAL path (deferred, which the suite otherwise
    // turns off), watched frame by frame while the passes cut.
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
      // What the frame COST, not how long it waited for a core
      // (MainThreadCpuMs): a loaded machine stretches every frame.
      const double f0 = MainThreadCpuMs();
      ctx->Yield();
      worstMs = std::max(worstMs, MainThreadCpuMs() - f0);
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

  t = IM_REGISTER_TEST(engine, "cam", "a_profile_is_cut_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/cam/README.md#background-work: "Add operation" on a helically fluted
    // pin held the frame for two minutes, sectioning the part at every
    // depth pass inline. The panel's REAL path (deferred, which the suite
    // otherwise turns off): the click returns, the sections are cut on the
    // worker, and what lands is what a synchronous generate says.
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("Side A");
    added.safeZ = 20.0;
    s_App->camActiveSetup = added.id;
    ctx->Yield(2);
    s_App->camDeferPasses = true; // the real default, off for the suite
    s_App->camOutlineCache.Clear();
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    CamSetupMenu(ctx, added.id, "###cam_add_op");

    int frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (ui::BackgroundJobRunning(*s_App, "cam") &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
      ctx->Yield();
      frames++;
    }
    ctx->LogInfo("%d frames while sectioning", frames);
    ctx->Yield(2);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "cam"));
    IM_CHECK(!ui::CamToolpathsPending(*s_App));
    // Only the worker puts an outline in the cache when passes are deferred,
    // so an entry here says the sections were cut off the frame.
    IM_CHECK_EQ(s_App->camOutlineCache.Size(), (size_t)1);

    const cam::Setup* live = ui::ActiveCamSetup(*s_App);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK(live->operations[0].type == cam::OpType::Profile);
    const std::vector<cam::Toolpath> sync =
        cam::GenerateAll(*live, ui::JobBodies(*s_App));
    IM_CHECK_EQ(sync.size(), s_App->camToolpaths.size());
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK_EQ(sync[0].moves.size(), s_App->camToolpaths[0].moves.size());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);
  };

  t = IM_REGISTER_TEST(engine, "cam",
                       "add_operation_on_the_trunnion_is_rotary");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/cam/operations.md#derivation--what-a-click-means: the button added a
    // 3-axis cut-out to a job cut on the trunnion -- a contour round the dowel
    // from its top to its bottom. A setup that already cuts on the 4th axis
    // gets a pass that does too.
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
    CamSetupMenu(ctx, added.id, "###cam_add_op");
    setup = Job()->FindSetup(added.id);
    IM_CHECK(setup != nullptr);
    IM_CHECK_EQ(setup->operations.size(), (size_t)2);
    IM_CHECK(setup->operations[1].type == cam::OpType::SurfaceRotary);
    IM_CHECK(s_App->camRefusal.empty());
  };

  t = IM_REGISTER_TEST(engine, "cam", "surfacing_runs_off_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // docs/ui/long-operations.md#cam-toolpath-passes. `GenerateContext`'s
    // comment called adaptive clearing "the one expensive step"; that stopped
    // being true at v118, and a finishing pass ran INLINE until this. The
    // rotary rolling pin took seventeen minutes with the app stopped -- single
    // `ui` frames of 10 s and 72 s on the frame HUD.
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
      const double f0 = MainThreadCpuMs(); // the frame's cost, not its wait
      ctx->Yield();
      worstMs = std::max(worstMs, MainThreadCpuMs() - f0);
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
    // docs/cam/rotary.md#patterns-rotary_pattern. The pattern is a radio pair
    // on a rotary op's Advanced row, offered because the capability table says
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
    // Between centres, not in jaws: a 30 mm bar in the default 15 mm grip
    // leaves too little length to tell a spiral from a raster.
    setup->chuck = 0;
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
    // docs/cam/rotary.md#the-rotary-picture. A program whose A moves used to
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
    // Playing posts the file, and a rotary whose installation is unstated
    // is refused there (docs/cam/rotary.md#how-the-rotary-is-installed):
    // stated as the identity, and nothing holding the part.
    setup->machine.rotary.along = 'x';
    setup->machine.rotary.chuckEnd = -1;
    setup->machine.rotary.word = 'A';
    setup->machine.rotary.topAxis = 'y';
    setup->machine.rotary.topSign = -1;
    setup->chuck = 0;
    setup->tailstock = 0;
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
    double worstMs = 0.0, totalMs = 0.0;
    int frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3) &&
           s_App->camPlaying) {
      const double f0 = MainThreadCpuMs();
      ctx->Yield();
      const double cost = MainThreadCpuMs() - f0;
      worstMs = std::max(worstMs, cost);
      totalMs += cost;
      frames++;
    }
    s_App->camPlaying = false;
    ctx->LogInfo("%d frames playing, %.1f ms of main-thread CPU each, worst "
                 "%.1f ms",
                 frames, frames > 0 ? totalMs / frames : 0.0, worstMs);
    // Interactive: a frame of playback costs under 300 ms of the main thread
    // on average, so more than three a second on a core of its own. This
    // counted frames in 3 s of WALL time (>= 10, the same rate), which a
    // loaded machine fails with frames that cost nothing and waited for a
    // core.
    IM_CHECK(frames >= 2);
    IM_CHECK(totalMs / frames < 300.0);

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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
    // docs/ui/panels.md#cam-job-tab-the-cam-panel. The toolbar's gears used to
    // generate SILENTLY, with the verdict landing in the docked panel below the
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

  // docs/cam/gui.md#whose-results-are-on-screen. The Generate window's
  // verdict, its cutting length and the file it saved were GLOBAL, "cleared
  // on any edit" -- and opening another job is not an edit, so job B's window
  // went on showing job A's verdict lines, A's millimetres of cutting and A's
  // saved path. Each job's run is its own: B shows none of A's, and A's comes
  // back with A.
  t = IM_REGISTER_TEST(engine, "cam", "another_job_shows_its_own_run");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    const size_t partTab = static_cast<size_t>(s_App->activePart);
    const uint64_t partId = s_App->parts[partTab].id;

    // Job A: a profile round the plate, into a clamp, so the run has a
    // verdict, a fixture hit and a file.
    cam::Setup& made = MakeJob()->AddSetup("Top");
    const size_t jobA = static_cast<size_t>(s_App->activePart);
    const uint64_t sid = made.id;
    s_App->camActiveSetup = sid;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    made.operations.push_back(op);
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    CamSetupMenu(ctx, sid, "###cam_fixture_add_menu");
    ctx->ItemClick("//###Menu_00/###cam_fixture_box");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(sid)->fixtures.size(), (size_t)1);

    // Job B, on the same part: a setup with nothing in it yet. Its ids
    // count from the same start as A's, which is how A's results could pass
    // for B's.
    const size_t jobB = ui::CreateJobForPart(*s_App, partId, "Other job");
    cam::Setup& other = Job()->AddSetup("Back");
    (void)cam::RecomputeOrigin(other, ui::JobBodies(*s_App));
    s_App->camActiveSetup = other.id;
    ctx->Yield(2);
    ui::SetActivePart(*s_App, jobA);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camActiveSetup, sid);

    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(3);
    IM_CHECK(s_App->camGenerateOpen);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(!s_App->camVerdict.empty());
    IM_CHECK(s_App->camFixtureHit.has_value());
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_cam_own_run_test";
    std::filesystem::create_directories(dir);
    s_App->projectPath = (dir / "plate.dsgn").string();
    ctx->ItemClick("//###CamGenerate/###cam_save_gcode");
    ctx->Yield(2);
    IM_CHECK(std::filesystem::exists(s_App->camLastPost));
    const std::vector<cam::VerdictLine> verdictA = s_App->camVerdict;
    const std::vector<glm::dvec3> leftoverA = s_App->camLeftover;
    const glm::dvec3 hitA = s_App->camFixtureHit.value_or(glm::dvec3(0.0));
    const std::string postA = s_App->camLastPost;
    const size_t pathsA = s_App->camToolpaths.size();

    // To B, by its tab.
    ui::SetActivePart(*s_App, jobB);
    ctx->Yield(3);
    // The window stayed open (it is a tool, like Simulate), and shows B.
    IM_CHECK(s_App->camGenerateOpen);
    IM_CHECK(ImGui::FindWindowByName("###CamGenerate") != nullptr &&
             ImGui::FindWindowByName("###CamGenerate")->Active);
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK(s_App->camLastPost.empty());
    IM_CHECK(s_App->camToolpaths.empty()); // "N mm of cutting" was A's
    IM_CHECK(ctx->ItemExists("//###CamGenerate/###cam_generate"));
    IM_CHECK(!ctx->ItemExists("//###CamGenerate/###cam_save_gcode"));
    // ...and the viewport's markers, drawn from the same state.
    IM_CHECK(s_App->camLeftover.empty());
    IM_CHECK(!s_App->camFixtureHit.has_value());
    {
      std::vector<PrimVertex> prims;
      ui::AppendCamToolpathOverlay(*s_App, prims);
      IM_CHECK(prims.empty()); // A's backplot is not drawn over B
    }

    // Back to A: its own run, still true -- nothing was edited.
    ui::SetActivePart(*s_App, jobA);
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->camActiveSetup, sid);
    IM_CHECK_EQ(s_App->camToolpaths.size(), pathsA);
    IM_CHECK_EQ(s_App->camVerdict.size(), verdictA.size());
    for (size_t i = 0; i < std::min(verdictA.size(), s_App->camVerdict.size());
         i++)
      IM_CHECK(s_App->camVerdict[i].text == verdictA[i].text);
    IM_CHECK_EQ(s_App->camLeftover.size(), leftoverA.size());
    IM_CHECK(s_App->camFixtureHit.has_value() && *s_App->camFixtureHit == hitA);
    IM_CHECK(s_App->camLastPost == postA);
    IM_CHECK(ctx->ItemExists("//###CamGenerate/###cam_save_gcode"));

    // A tab that is not a job shows nothing of either.
    ui::SetActivePart(*s_App, partTab);
    ctx->Yield(2);
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK(s_App->camToolpaths.empty());

    // An edit to A's part while A is away makes its verdict about a part
    // that no longer exists: it does not come back, and the file it wrote
    // still does.
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Boss", glm::vec3(0, 0, 12), 10, 10, 5));
    ctx->Yield(2);
    ui::SetActivePart(*s_App, jobA);
    ctx->Yield(3);
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK(!s_App->camFixtureHit.has_value());
    IM_CHECK(s_App->camLastPost == postA);

    std::filesystem::remove_all(dir);
    s_App->projectPath.clear();
    s_App->camGenerateOpen = false;
    ui::SetActivePart(*s_App, partTab);
    ctx->Yield(2);
  };

  // The same within one job: the verdict is about ONE setup. Making another
  // the active one used to clear it -- so coming back meant generating again
  // -- and left the saved path standing, naming the other setup's file.
  t = IM_REGISTER_TEST(engine, "cam", "another_setup_shows_its_own_run");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    const uint64_t top = MakeJob()->AddSetup("Top").id;
    const uint64_t back = Job()->AddSetup("Back").id;
    std::map<uint64_t, uint64_t> opOf;
    for (const uint64_t s : {top, back}) {
      // Derived as a setup made in the panel is, so making one active moves
      // nothing (which would be an edit, and end every kept verdict).
      (void)cam::RecomputeOrigin(*Job()->FindSetup(s), ui::JobBodies(*s_App));
      cam::Operation op;
      op.id = Job()->AllocateId();
      op.name = s == top ? "Outline" : "Face";
      op.type = cam::OpType::Profile;
      op.topZ = 13.0;
      op.bottomZ = s == top ? -1.0 : 6.0;
      Job()->FindSetup(s)->operations.push_back(op);
      opOf[s] = op.id;
    }
    s_App->camActiveSetup = top;
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "designer_cam_own_setup_test";
    std::filesystem::create_directories(dir);
    s_App->projectPath = (dir / "plate.dsgn").string();

    // Generate and save the top.
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(3);
    IM_CHECK(!s_App->camVerdict.empty());
    ctx->ItemClick("//###CamGenerate/###cam_save_gcode");
    ctx->Yield(2);
    IM_CHECK(s_App->camLastPost.find("Top") != std::string::npos);
    const std::vector<cam::VerdictLine> verdictTop = s_App->camVerdict;
    const std::string postTop = s_App->camLastPost;

    // A click on the back's row: the window is the back's now.
    ctx->ItemClick(CamSetupRow(back).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->camActiveSetup, back);
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK(s_App->camLastPost.empty());
    IM_CHECK_EQ(s_App->camToolpaths.size(), (size_t)1);
    if (!s_App->camToolpaths.empty())
      IM_CHECK_EQ(s_App->camToolpaths[0].operationId, opOf[back]);

    // Its own run, and saved under its own name.
    ctx->ItemClick("//###CamGenerate/###cam_generate");
    ctx->Yield(3);
    IM_CHECK(!s_App->camVerdict.empty());
    ctx->ItemClick("//###CamGenerate/###cam_save_gcode");
    ctx->Yield(2);
    IM_CHECK(s_App->camLastPost.find("Back") != std::string::npos);
    const std::string postBack = s_App->camLastPost;
    const size_t verdictBack = s_App->camVerdict.size();

    // Each comes back with its setup.
    ctx->ItemClick(CamSetupRow(top).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->camActiveSetup, top);
    IM_CHECK(s_App->camLastPost == postTop);
    IM_CHECK_EQ(s_App->camVerdict.size(), verdictTop.size());
    for (size_t i = 0;
         i < std::min(verdictTop.size(), s_App->camVerdict.size()); i++)
      IM_CHECK(s_App->camVerdict[i].text == verdictTop[i].text);
    if (!s_App->camToolpaths.empty())
      IM_CHECK_EQ(s_App->camToolpaths[0].operationId, opOf[top]);
    ctx->ItemClick(CamSetupRow(back).c_str());
    ctx->Yield(3);
    IM_CHECK(s_App->camLastPost == postBack);
    IM_CHECK_EQ(s_App->camVerdict.size(), verdictBack);

    // An edit to the job -- the top's operation, made from the back --
    // leaves the top's verdict about a job that is gone: it does not come
    // back.
    Job()->FindSetup(top)->operations[0].bottomZ = 2.0;
    ui::MarkJobEdited(*s_App);
    ctx->ItemClick(CamSetupRow(top).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->camActiveSetup, top);
    IM_CHECK(s_App->camVerdict.empty());
    IM_CHECK(s_App->camLastPost == postTop);

    std::filesystem::remove_all(dir);
    s_App->projectPath.clear();
    s_App->camGenerateOpen = false;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // Workholding by clicking (docs/cam/setup.md#workholding-fixtures): a
  // keep-out box from the panel, placed from the stock; its row checks, moves
  // and edits it; and Verify names the clamp the profile runs into.
  // docs/cam/gui.md#gizmos: a fixture moves with the arrows parts
  // move with, its own faces snap to the stock's, and one put down on the
  // stock's top RESTS there -- seated, following the stock. Rays are handed
  // to HandleGizmoInput, as a_placement_is_dragged_in_the_viewport does.
  t = IM_REGISTER_TEST(engine, "cam", "a_clamp_is_dragged_onto_the_stock");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    // The clamp: a bar, a nose pad under one end, a stud at the other.
    const size_t clampSlot = ui::AddPart(*s_App, "Step clamp");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 10), 100, 20, 8));
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Pad", glm::vec3(80, 0, 5), 20, 20, 5));
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Stud", glm::vec3(5, 5, -40), 10, 10, 50));
    ctx->Yield(2);
    const uint64_t clampDoc = s_App->parts[clampSlot].id;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    cam::Setup& made = MakeJob()->AddSetup("Clamped");
    const uint64_t sid = made.id;
    s_App->camActiveSetup = sid;
    ctx->Yield(2);
    const auto live = [&]() -> cam::Setup& { return *Job()->FindSetup(sid); };
    cam::Fixture clamp;
    clamp.id = Job()->AllocateId();
    clamp.sourceDocId = clampDoc;
    clamp.name = "Step clamp";
    // Standing on the bed off the -X side: its pad hangs over nothing.
    IM_CHECK(cam::PlaceFixture(live(), ui::JobBodies(*s_App),
                               ui::FixtureModelBodies(*s_App, clamp), 0.0,
                               cam::FixtureSide::MinusX, clamp.pos));
    live().fixtures.push_back(clamp);
    glm::dvec3 lo, hi;
    IM_CHECK(cam::StockBounds(live(), ui::JobBodies(*s_App), lo, hi));

    // No editor, no handles; its editor open, X, Y, Z and a spin.
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK(s_App->viewportHandles.empty());
    ui::EditCamFixture(*s_App, clamp.id);
    ctx->Yield(2);
    // Where handle `k` is: X, Y and Z arrows, then the spin -- four, or a
    // NaN that fails every check after it.
    const auto handle = [&](size_t k) -> glm::vec3 {
      s_App->viewportHandles.clear();
      ui::PublishGizmoHandles(*s_App);
      if (s_App->viewportHandles.size() != 4)
        return glm::vec3(std::numeric_limits<float>::quiet_NaN());
      return s_App->viewportHandles[k].origin;
    };
    s_App->viewportHandles.clear();
    ui::PublishGizmoHandles(*s_App);
    IM_CHECK_EQ(s_App->viewportHandles.size(), (size_t)4);
    const float L = ui::GizmoScale(*s_App);

    // X: 20 mm in. The pad's -X face lands on the stock's -X side (a snap),
    // its +X face 20 mm in over the top -- but still in the air.
    glm::vec3 o = handle(0);
    Ray grab{o + glm::vec3(0.5f * L, 0.0f, 100.0f), {0.0f, 0.0f, -1.0f}};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    IM_CHECK(s_App->gizmoDrag.active);
    IM_CHECK_EQ(s_App->gizmoDrag.fixtureId, clamp.id);
    Ray moved = grab;
    moved.origin.x += 20.0f;
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    ctx->LogInfo("snap: %s", s_App->gizmoDrag.snapLabel.c_str());
    IM_CHECK(s_App->gizmoDrag.snapLabel.find("on the stock's -X side") !=
             std::string::npos);
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    IM_CHECK(!live().FindFixture(clamp.id)->seat.has_value()); // in the air

    // Z: down to a hair above the top. The pad's underside snaps onto it,
    // and put down there the clamp RESTS on the stock.
    o = handle(2);
    const cam::Fixture* f = live().FindFixture(clamp.id);
    const double padZ =
        (cam::FixtureTransform(*f) * glm::dvec4(90, 10, 5, 1)).z;
    grab = Ray{o + glm::vec3(0.0f, -100.0f, 0.5f * L), {0.0f, 1.0f, 0.0f}};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    moved = grab;
    moved.origin.z -= static_cast<float>(padZ - hi.z - 0.3);
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    ctx->LogInfo("snap: %s", s_App->gizmoDrag.snapLabel.c_str());
    IM_CHECK(s_App->gizmoDrag.snapLabel == "Pad's underside on the stock top");
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    ctx->Yield(2);
    f = live().FindFixture(clamp.id);
    IM_CHECK(f->seat.has_value());
    IM_CHECK(f->seat->edge == cam::StockEdge::MinusX);
    IM_CHECK(std::abs(f->seat->overMm - 20.0) < 1e-3);
    IM_CHECK(std::abs((cam::FixtureTransform(*f) * glm::dvec4(90, 10, 5, 1)).z -
                      hi.z) < 1e-6);

    // The stock grows: resting on it, the clamp goes with it.
    live().stock.margin.x += 5.0;
    live().stock.margin.z += 3.0;
    ctx->Yield(3);
    glm::dvec3 lo2, hi2;
    IM_CHECK(cam::StockBounds(live(), ui::JobBodies(*s_App), lo2, hi2));
    f = live().FindFixture(clamp.id);
    const glm::dvec4 padIn =
        cam::FixtureTransform(*f) * glm::dvec4(100, 10, 5, 1);
    IM_CHECK(std::abs(padIn.z - hi2.z) < 1e-3);
    IM_CHECK(std::abs(padIn.x - (lo2.x + 20.0)) < 1e-3);

    // Z up 25 mm: off the top, it stands on its own again.
    o = handle(2);
    grab = Ray{o + glm::vec3(0.0f, -100.0f, 0.5f * L), {0.0f, 1.0f, 0.0f}};
    IM_CHECK(ui::HandleGizmoInput(*s_App, grab, true, true, false));
    moved = grab;
    moved.origin.z += 25.0f;
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, true, false));
    IM_CHECK(ui::HandleGizmoInput(*s_App, moved, false, false, true));
    IM_CHECK(!live().FindFixture(clamp.id)->seat.has_value());
    ctx->ItemClick("//###ToolWindow/**/###cam_fixture_editor_close");
    ctx->Yield(2);
  };

  // docs/cam/gui.md#milling-a-thread: a thread on the part is milled from
  // its setup's menu, driven by the Thread; the op editor takes its passes.
  t = IM_REGISTER_TEST(engine, "cam", "a_thread_is_milled_from_the_setup_menu");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    Document* part = CamPart();
    // A 40 x 40 x 10 plate, a 3/8-16 UNC tapped hole through it, and a
    // Thread of class 2B on the hole's bore.
    auto pad =
        std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
    pad->AddRectangle({-20, -20}, {20, 20});
    part->AddFeature(std::move(pad));
    part->AddFeature(std::make_unique<ExtrudeFeature>(
        "Pad", part->GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
    auto centre = std::make_unique<SketchFeature>(
        "Centre", SketchPlane(glm::vec3(0, 0, 10), glm::vec3(0, 0, 1),
                              glm::vec3(1, 0, 0)));
    centre->AddEntity(sketch::Point{{0, 0}, 1});
    part->AddFeature(std::move(centre));
    auto hole = std::make_unique<HoleFeature>(
        "Tripod", part->GetFeature(2)->GetId(), HoleKind::Simple, 7.9375, 10.0);
    hole->through = true;
    hole->fitThread = "3/8-16 UNC";
    hole->fitKind = "tapped";
    part->AddFeature(std::move(hole));
    ctx->Yield(2);
    ElementRef bore;
    for (const auto& [index, name] : part->GetBodies().at(0).elementMap.names)
      if (name.find("/bore") != std::string::npos)
        bore = ElementRef{part->GetBodies()[0].createdBy, name};
    IM_CHECK(bore.IsSet());
    part->AddFeature(std::make_unique<ThreadFeature>(
        "Tripod thread", bore, "3/8-16 UNC-2B", 25.4 / 16.0));
    ctx->Yield(2);

    cam::Setup& made = MakeJob()->AddSetup("Tapped");
    const uint64_t sid = made.id;
    made.stock.bottomMargin = 0.0;
    cam::Tool mill;
    mill.number = 2;
    mill.name = "6 mm thread mill";
    mill.diameter = 6.0;
    mill.type = cam::ToolType::ThreadMill;
    cam::FillThreadMillShape(mill, false);
    made.tools.push_back(mill);
    s_App->camActiveSetup = sid;
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    const auto live = [&]() -> cam::Setup& { return *Job()->FindSetup(sid); };

    // Add operation > Mill a thread: the job's one thread, by its name.
    ctx->ItemClick(CamSetupRow(sid).c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_add_thread");
    ctx->Yield(2);
    // A submenu is a window of its own, "<label>###Menu_00".
    ctx->ItemClick("//###Menu_00/###cam_thread_0");
    ctx->Yield(3);
    IM_CHECK_EQ(live().operations.size(), (size_t)1);
    const cam::Operation& op = live().operations[0];
    IM_CHECK(op.type == cam::OpType::ThreadMill);
    IM_CHECK_EQ(op.toolNumber, 2); // the thread mill, not tool 1
    IM_CHECK(op.climb && op.leadArcs);
    IM_CHECK_EQ(op.targets.size(), (size_t)1);
    const uint64_t opId = op.id;

    // Cut: the helix is declared, as the verifier will read it.
    const std::vector<cam::Toolpath> paths =
        ui::GenerateCamToolpathsNow(*s_App, live());
    IM_CHECK_EQ(paths.size(), (size_t)1);
    IM_CHECK(paths[0].error.empty());
    IM_CHECK_EQ(paths[0].threads.size(), (size_t)1);
    IM_CHECK(!paths[0].arcs.empty());

    // The op editor takes its passes, and not its depths (the thread's).
    ctx->ItemDoubleClick(
        ("**/$$" + std::to_string(static_cast<int>(opId)) + "/###op_row")
            .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, opId);
    ctx->ItemInputValue("//###ToolWindow/**/###threadpasses", 2);
    ctx->Yield(3);
    IM_CHECK_EQ(live().operations[0].threadPasses, 2);
    ctx->ItemClick("//###ToolWindow/###cam_edit_done");
    ctx->Yield(2);
  };

  t = IM_REGISTER_TEST(engine, "cam", "a_clamp_is_a_row_the_verdict_names");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& made = MakeJob()->AddSetup("Clamped");
    const uint64_t sid = made.id;
    s_App->camActiveSetup = sid;
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 13.0;
    op.bottomZ = -1.0;
    made.operations.push_back(op);
    ctx->Yield(2);
    if (ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam))
      ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    // Fixtures are under their setup, added from its menu.
    CamSetupMenu(ctx, sid, "###cam_fixture_add_menu");
    ctx->ItemClick("//###Menu_00/###cam_fixture_box");
    ctx->Yield(2);
    const auto live = [&]() -> cam::Setup& { return *Job()->FindSetup(sid); };
    IM_CHECK_EQ(live().fixtures.size(), (size_t)1);
    const uint64_t id = live().fixtures[0].id;
    glm::dvec3 lo, hi;
    IM_CHECK(cam::StockBounds(live(), ui::JobBodies(*s_App), lo, hi));
    // Against -X on the bed, a quarter of the stock's width, taller than it
    // -- and safe Z now clears it.
    const cam::Fixture& f0 = live().fixtures[0];
    IM_CHECK(f0.IsBox());
    IM_CHECK(std::abs(f0.pos.x + f0.box.x - lo.x) < 1e-6);
    IM_CHECK(std::abs(f0.pos.z - lo.z) < 1e-6);
    IM_CHECK(live().safeZ > f0.pos.z + f0.box.z);

    const std::string row = "**/###cam_fixture_" + std::to_string(id);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_fixture_check");
    ctx->Yield(2);
    IM_CHECK(!live().fixtures[0].enabled);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_fixture_check");
    ctx->Yield(2);
    IM_CHECK(live().fixtures[0].enabled);

    // Moved through the menu: on the stock's top.
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_fixture_move");
    ctx->Yield(2);
    // A submenu is a window of its own, "<label>###Menu_00" -- so its id is
    // the depth's, not the label's.
    ctx->ItemClick("//###Menu_00/###cam_fixture_to_top");
    ctx->Yield(2);
    IM_CHECK(std::abs(live().fixtures[0].pos.z - hi.z) < 1e-6);

    // Edited in the tool window, and back against -X from there.
    ctx->ItemDoubleClick(row.c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditFixture, id);
    IM_CHECK(ctx->ItemExists("//###ToolWindow/###cam_fixture_editor_close"));
    ctx->ItemClick("//###ToolWindow/###cam_fixture_mx");
    ctx->Yield(2);
    IM_CHECK(std::abs(live().fixtures[0].pos.z - lo.z) < 1e-6);
    ctx->ItemClick("//###ToolWindow/###cam_fixture_editor_close");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditFixture, (uint64_t)0);

    // The profile runs round the plate, into the clamp: the verdict says
    // which one, and marks where.
    ui::RefreshCamToolpaths(*s_App);
    ui::RunCamVerification(*s_App);
    ctx->Yield(2);
    bool named = false;
    for (const cam::VerdictLine& l : s_App->camVerdict)
      named = named || l.text.find("fixture \"" + live().fixtures[0].name +
                                   "\"") != std::string::npos;
    IM_CHECK(named);
    IM_CHECK(s_App->camFixtureHit.has_value());

    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
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
    CamSetupMenu(ctx, setup.id, "###cam_add_op");
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
    ctx->ItemClick("###cam_tools_add");
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_add_tool");
    ctx->Yield();
    ctx->SetRef("//CAM###CamPanel");
    CamSetupMenu(ctx, setup.id, "###cam_add_op");
    ctx->Yield(3);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);

    // Generate: toolpaths exist, and the backplot draws them.
    // Generate is a toolbar tool with its own window now
    // (docs/ui/panels.md#cam-job-tab-the-cam-panel); closed again so it cannot
    // take the clicks of what follows.
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
    // picture (docs/cam/gui.md#what-the-viewport-draws). Its circle sits INSIDE
    // the path's extent, so the check is its geometry: vertices one tool radius
    // from the entry point. (A bounding-box check passed for the wrong reason
    // and was replaced.)
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
    CamSetupMenu(ctx, setup.id, "###cam_add_op");
    // Generate is a toolbar tool with its own window now
    // (docs/ui/panels.md#cam-job-tab-the-cam-panel); closed again so it cannot
    // take the clicks of what follows.
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

  // docs/cam/gui.md#the-cam-panel: one Add menu per section, and
  // each offers what it should. The part menu lists the project parts the
  // job does not hold; the fixture menu lists the box and the same parts,
  // and says when nothing holds the stock.
  t = IM_REGISTER_TEST(engine, "cam", "add_menus_offer_the_right_parts");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    ui::AddPart(*s_App, "Spacer");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Blk", glm::vec3(0, 0, 0), 20, 20, 12));
    const size_t jaw = ui::AddPart(*s_App, "Vise jaw");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Jaw", glm::vec3(0, 0, 0), 80, 10, 25));
    ctx->Yield(2);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    cam::Setup& made = MakeJob()->AddSetup("Sheet");
    const uint64_t sid = made.id;
    s_App->camActiveSetup = sid;
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_parts");
    ctx->Yield(2);

    // The two parts the job does not hold, and not the one it does: the
    // Parts header's "+", and the setup menu's Add fixture.
    const auto countIn = [&](const char* window, const char* item) {
      ImGuiTestItemList items;
      ctx->GatherItems(&items, window, 2);
      int n = 0;
      for (int i = 0; i < items.GetSize(); i++)
        if (std::strstr(items[i]->DebugLabel, item) != nullptr)
          n++;
      return n;
    };
    ctx->ItemClick("###cam_parts_add");
    ctx->Yield(2);
    IM_CHECK_EQ(countIn("//$FOCUSED", "###cam_part_add"), 2);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    CamSetupMenu(ctx, sid, "###cam_fixture_add_menu");
    IM_CHECK_EQ(countIn("//###Menu_00", "###cam_fixture_add"), 2);

    // Hold it with the jaw, from the menu's part group.
    ctx->ItemClick(("//###Menu_00/$$" +
                    std::to_string(static_cast<int>(s_App->parts[jaw].id)) +
                    "/###cam_fixture_add")
                       .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(sid)->fixtures.size(), (size_t)1);
    IM_CHECK(Job()->FindSetup(sid)->fixtures[0].name == "Vise jaw");
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

    // Add the second part from the header's "+".
    ctx->ItemClick("###cam_parts_add");
    ctx->Yield(2);
    // The popup's items sit under the part's id (PushID), as the op rows do.
    ctx->ItemClick(("//$FOCUSED/$$" +
                    std::to_string(static_cast<int>(s_App->parts[second].id)) +
                    "/###cam_part_add")
                       .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)2);

    // Place it, from its row's menu.
    const uint64_t added = Job()->GetParts().back().id;
    CamPartMenu(ctx, added, "###cam_place");
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live != nullptr);
    IM_CHECK_EQ(live->placements.size(), (size_t)1);

    // And it names the part it was placed for -- a placement for anything
    // else would put a ghost in the stock.
    IM_CHECK_EQ(live->placements[0].jobPartId, added);

    // A double-click edits the placement in the tool window.
    ctx->ItemDoubleClick(CamPartRow(added).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditPart, added);
    ctx->ItemInputValue("//###ToolWindow/###cam_place_x", "12");
    ctx->Yield(3);
    IM_CHECK(std::abs(Job()->FindSetup(setup.id)->placements[0].pos.x - 12.0) <
             1e-6);
    ctx->ItemClick("//###ToolWindow/###cam_part_editor_close");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditPart, (uint64_t)0);

    // Removing a part takes its placement with it.
    CamPartMenu(ctx, added, "###cam_part_remove");
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1);
    IM_CHECK(Job()->FindSetup(setup.id)->placements.size() <= 1);

    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      if (ctx->ItemInfo(modelWin->TabId, ImGuiTestOpFlags_NoError).ID != 0) {
        ctx->ItemClick(modelWin->TabId);
        ctx->Yield(2);
      }
  };

  // Setups are rows of a tree, each a disclosure over its operations and
  // fixtures (docs/ui/panels.md#cam-job-tab-the-cam-panel). They were a
  // combo, so a job's structure, setup by setup, was never on screen at once.
  t = IM_REGISTER_TEST(engine, "cam", "two_setups_are_open_at_once");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    const uint64_t top = MakeJob()->AddSetup("Top").id;
    const uint64_t back = Job()->AddSetup("Back").id;
    std::vector<uint64_t> ops;
    for (const uint64_t sid : {top, back}) {
      cam::Operation op;
      op.id = Job()->AllocateId();
      op.name = sid == top ? "Outline" : "Face";
      op.type = cam::OpType::Profile;
      op.topZ = 13.0;
      op.bottomZ = -1.0;
      Job()->FindSetup(sid)->operations.push_back(op);
      ops.push_back(op.id);
    }
    s_App->camActiveSetup = top;
    ctx->Yield(2);
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    const auto opRow = [&](size_t i) {
      return "**/$$" + std::to_string(static_cast<int>(ops[i])) + "/###op_row";
    };
    const auto isOpen = [&](uint64_t sid) {
      const ImGuiTestItemInfo info = ctx->ItemInfo(
          ("**/###cam_setup_open_" + std::to_string(sid)).c_str());
      return info.ID != 0 &&
             (info.StatusFlags & ImGuiItemStatusFlags_Opened) != 0;
    };
    // The active setup is open, the other closed; opening it is not
    // activating it.
    IM_CHECK(isOpen(top));
    IM_CHECK(!isOpen(back));
    IM_CHECK(ctx->ItemExists(opRow(0).c_str()));
    IM_CHECK(!ctx->ItemExists(opRow(1).c_str()));
    ctx->ItemOpen(("**/###cam_setup_open_" + std::to_string(back)).c_str());
    ctx->Yield(2);
    IM_CHECK(isOpen(top));
    IM_CHECK(isOpen(back));
    IM_CHECK_EQ(s_App->camActiveSetup, top);
    IM_CHECK(ctx->ItemExists(opRow(0).c_str()));
    IM_CHECK(ctx->ItemExists(opRow(1).c_str()));
    // The run status is the ACTIVE setup's, and only one row carries it.
    IM_CHECK(ctx->ItemExists("**/###cam_run_status"));

    // A click on the other row makes it the active setup; the first stays
    // open, and both operations stay listed.
    ctx->ItemClick(CamSetupRow(back).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camActiveSetup, back);
    IM_CHECK(isOpen(top));
    IM_CHECK(isOpen(back));
    // An operation of the setup that is not active makes it active when it is
    // edited: its settings are the active setup's.
    ctx->ItemDoubleClick(opRow(0).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camActiveSetup, top);
    IM_CHECK_EQ(s_App->camEditOp, ops[0]);
    ctx->ItemClick("//###ToolWindow/###cam_edit_done");
    ctx->Yield(2);
    IM_CHECK(!ImGui::GetCurrentContext()->DebugDrawIdConflicts);

    // Rename and Duplicate, from the menu.
    CamSetupMenu(ctx, back, "###cam_setup_rename");
    ctx->KeyCharsReplaceEnter("Underside");
    ctx->Yield(2);
    IM_CHECK(Job()->FindSetup(back)->name == "Underside");
    CamSetupMenu(ctx, back, "###cam_setup_duplicate");
    IM_CHECK_EQ(Job()->GetSetups().size(), (size_t)3);
    const cam::Setup& copy = Job()->GetSetups().back();
    IM_CHECK_EQ(copy.operations.size(), (size_t)1);
    IM_CHECK(copy.operations[0].id != ops[1]);
    IM_CHECK_EQ(s_App->camActiveSetup, copy.id);
  };

  // The chess undersides job, by hand (docs/cam/gui.md#the-cam-panel): ONE
  // part in two configurations -- a blank, and the same blank with a pocket
  // -- added twice, placed side by side, and the pocketed one cut. A job
  // part's configuration had no door but the RPC.
  t = IM_REGISTER_TEST(engine, "cam", "one_part_in_two_configurations");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 20));
    const uint64_t plate = CamPart()->GetFeature(0)->GetId();
    CamPart()->AddFeature(std::make_unique<BoxFeature>(
        "Void", glm::vec3(15, 10, 12), 30, 20, 20));
    const uint64_t voidId = CamPart()->GetFeature(1)->GetId();
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Pocket", plate, voidId, BooleanOp::Subtract));
    const uint64_t pocketId = CamPart()->GetFeature(2)->GetId();
    const uint64_t blank = [&] {
      Configuration& c = CamPart()->AddConfiguration("blank");
      c.suppressedFeatures = {voidId, pocketId};
      return c.id;
    }();
    const uint64_t pocketed = CamPart()->AddConfiguration("pocketed").id;
    ctx->Yield(2);
    const uint64_t sourceId = s_App->parts[0].id;
    const uint64_t sid = MakeJob()->AddSetup("Undersides").id;
    s_App->camActiveSetup = sid;
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)1);
    const uint64_t first = Job()->GetParts()[0].id;
    IM_CHECK_EQ(Job()->GetParts()[0].configuration, (uint64_t)0);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");

    // The first one is the blank: its row's Configuration submenu, one undo
    // step.
    ctx->ItemClick(CamPartRow(first).c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_part_cfg");
    ctx->Yield(2);
    ctx->ItemClick(
        ("//###Menu_00/###cam_part_cfg_" + std::to_string(blank)).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindPart(first)->configuration, blank);
    IM_CHECK_EQ(ui::JobBodies(*s_App).size(), (size_t)1); // no pocket, one box
    IM_CHECK(ui::CanGlobalUndo(*s_App));
    ui::GlobalUndo(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindPart(first)->configuration, (uint64_t)0);
    ui::GlobalRedo(*s_App);
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindPart(first)->configuration, blank);

    // The second, from the Parts header's "+": the part is a submenu of its
    // configurations, since the job may cut it in more than one.
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_parts_add");
    ctx->Yield(2);
    ctx->ItemClick(("//$FOCUSED/$$" +
                    std::to_string(static_cast<int>(sourceId)) +
                    "/###cam_part_add_cfgs")
                       .c_str());
    ctx->Yield(2);
    ctx->ItemClick(
        ("//###Menu_00/###cam_part_add_cfg_" + std::to_string(pocketed))
            .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->GetParts().size(), (size_t)2);
    const uint64_t second = Job()->GetParts()[1].id;
    IM_CHECK_EQ(Job()->GetParts()[1].sourceDocId, sourceId);
    IM_CHECK_EQ(Job()->GetParts()[1].configuration, pocketed);

    // Side by side: each placed from its row, the second moved in its editor.
    CamPartMenu(ctx, first, "###cam_place");
    CamPartMenu(ctx, second, "###cam_place");
    ctx->ItemDoubleClick(CamPartRow(second).c_str());
    ctx->Yield(2);
    ctx->ItemInputValue("//###ToolWindow/###cam_place_x", "80");
    ctx->Yield(3);
    ctx->ItemClick("//###ToolWindow/###cam_part_editor_close");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(sid)->placements.size(), (size_t)2);
    IM_CHECK(
        cam::PlacementRefusal(*Job()->FindSetup(sid), ui::JobBodies(*s_App))
            .empty());

    // Cut the pocketed one: the pocket verb, and its floor -- which only the
    // second part has.
    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield(2);
    ui::SelectionItem floor;
    floor.kind = ui::SelectionItem::Kind::Face;
    for (const Body& b : ui::JobBodies(*s_App))
      for (const auto& [index, name] : b.elementMap.names) {
        glm::dvec3 o(0.0), n(0.0), u(0.0);
        if (b.jobPartId == second && b.shape.FacePlane(index, o, n, u) &&
            n.z > 0.99f && std::abs(o.z - 12.0f) < 1e-4f) {
          floor.element = ElementRef{b.createdBy, name};
          floor.jobPartId = second;
        }
      }
    IM_CHECK(floor.element.IsSet());
    ui::CamFacePicked(*s_App, floor);
    ctx->Yield(2);
    ctx->ItemClick("//###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
    const cam::Setup* live = Job()->FindSetup(sid);
    IM_CHECK_EQ(live->operations.size(), (size_t)1);
    IM_CHECK_EQ(live->operations[0].targets.size(), (size_t)1);
    IM_CHECK_EQ(live->operations[0].targets[0].jobPartId, second);

    // Generated: a toolpath that cuts, no refusal.
    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(2);
    const auto t0 = std::chrono::steady_clock::now();
    while ((ui::BackgroundJobRunning(*s_App, "cam") ||
            ui::CamToolpathsPending(*s_App)) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
      ctx->Yield();
    s_App->camGenerateOpen = false;
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camToolpaths.size(), (size_t)1);
    IM_CHECK(s_App->camToolpaths[0].error.empty());
    IM_CHECK(s_App->camToolpaths[0].CutLength() > 0.0);

    // A configuration the part no longer has is said, not substituted.
    IM_CHECK(CamPart() != nullptr);
    Document* src = ui::JobSourceDocument(*s_App, sourceId);
    IM_CHECK(src != nullptr);
    IM_CHECK(src->RemoveConfiguration(blank));
    ctx->Yield(2);
    std::string why;
    IM_CHECK(ui::JobPartDocument(*s_App, *Job()->FindPart(first), &why) ==
             nullptr);
    IM_CHECK(!why.empty());
    IM_CHECK(ctx->ItemExists(CamPartRow(first).c_str()));
    // ...and a door the RPC shares refuses one the part does not have.
    std::string refused;
    IM_CHECK(!ui::SetJobPartConfiguration(*s_App, first, blank, &refused));
    IM_CHECK(refused.find("pocketed") != std::string::npos);
    IM_CHECK(ui::SetJobPartConfiguration(*s_App, first, 0, &refused));
    ctx->Yield(2);
    IM_CHECK(ui::JobPartDocument(*s_App, *Job()->FindPart(first)) != nullptr);
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
    glm::dvec3 lo, hi;
    placed[1].shape.GetBoundingBox(lo, hi);
    const glm::vec3 centre = (lo + hi) * 0.5;
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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
    glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
    for (const Body& b : ui::JobPlacedBodies(*s_App)) {
      glm::dvec3 bmin, bmax;
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
    IM_CHECK_EQ(s_App->sketchRenderer->CachedPrimitiveCount(), (size_t)0);

    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(3);
    IM_CHECK(!s_App->camToolpaths.empty());
    IM_CHECK(s_App->camShowToolpath);
    // What the renderer HOLDS is the backplot, not just what the contributor
    // would produce if asked. It lives in the renderer's cached channel
    // (docs/cam/gui.md#what-the-viewport-draws), so that is where to look.
    const ui::CamBackplotRef backplot = ui::CamBackplot(*s_App);
    IM_CHECK(backplot.verts != nullptr && !backplot.verts->empty());
    IM_CHECK_EQ(s_App->sketchRenderer->CachedPrimitiveCount(),
                backplot.verts->size());

    // ...and hiding the path takes it back out.
    s_App->camShowToolpath = false;
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sketchRenderer->CachedPrimitiveCount(), (size_t)0);
    s_App->camShowToolpath = true;
    ctx->ItemClick("//Cut out the whole part###ToolWindow/###cam_tool_done");
    ctx->Yield(2);
  };

  // docs/cam/gui.md#what-the-viewport-draws: the backplot was cached and then
  // copied twice per frame -- into the primitive list, and into the frame's
  // vertex buffer -- which on the rolling pin was 93% of an idle frame. An
  // idle frame now copies nothing: the renderer holds the SAME list the
  // panel built, and uploads it only when it changes.
  t = IM_REGISTER_TEST(engine, "cam", "backplot_is_not_copied_per_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    s_App->camActiveSetup = setup.id;
    ctx->Yield(2);
    ToolbarClick(ctx, "###tb_job_whole");
    ctx->Yield(4);
    IM_CHECK(!s_App->camToolpaths.empty());
    const ui::CamBackplotRef backplot = ui::CamBackplot(*s_App);
    IM_CHECK(backplot.verts != nullptr && !backplot.verts->empty());
    // Shared, not copied: the renderer holds the panel's own list.
    IM_CHECK(s_App->sketchRenderer->CachedPrimitives() == backplot.verts.get());

    const size_t uploads = s_App->sketchRenderer->CachedUploads();
    ctx->Yield(10);
    IM_CHECK(s_App->sketchRenderer->CachedPrimitives() == backplot.verts.get());
    // At most one upload per frame in flight catching up, then none.
    ctx->LogInfo("%zu uploads over 10 idle frames",
                 s_App->sketchRenderer->CachedUploads() - uploads);
    IM_CHECK(s_App->sketchRenderer->CachedUploads() - uploads <= 2);
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
        glm::dvec3 o(0.0), n(0.0), u(0.0);
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
    // (docs/ui/panels.md#cam-job-tab-the-cam-panel).
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

  // The 3D finishing verbs (docs/cam/gui.md#the-job-toolbar) were written with
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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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

  // Engrave from the viewport (docs/cam/gui.md#engrave-from-the-viewport):
  // the job tab draws a part's labels where the part sits, a click on one
  // derives an engraving, a second label joins the same op, and it
  // generates with nothing refused.
  t = IM_REGISTER_TEST(engine, "cam", "a_label_is_engraved_by_clicking_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Document* part = CamPart();
    part->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 40, 30, 20));
    auto sk = std::make_unique<SketchFeature>(
        "Marks", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f)));
    sketch::Text a;
    a.anchor = {4.0f, 18.0f};
    a.text = "a1";
    a.font = "hershey";
    a.height = 6.0;
    const uint32_t first = sk->AddEntity(a);
    sketch::Text b = a;
    b.anchor = {4.0f, 6.0f};
    b.text = "b2";
    const uint32_t second = sk->AddEntity(b);
    part->AddFeature(std::move(sk));
    const uint64_t sketchId = part->GetFeature(1)->GetId();
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("Top");
    s_App->camActiveSetup = added.id;
    cam::Setup* setup = Job()->FindSetup(added.id);
    setup->tools[0].type = cam::ToolType::VBit;
    setup->tools[0].diameter = 6.0;
    setup->tools[0].tipAngleDeg = 90.0;
    // The stock's top IS the engraved surface: nothing to face first.
    setup->stock.margin.z = 0.0;
    setup->safeZ = 30.0; // clear of the 20 mm plate
    ctx->Yield(3);

    ToolbarClick(ctx, "###tb_job_fit"); // the part in view, to click on
    ctx->Yield(3);
    ToolbarClick(ctx, "###tb_job_engrave");
    ctx->Yield();
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    IM_CHECK_EQ(s_App->slotPick.slot,
                static_cast<int>(ui::PickSlot::CamEngraveCurve));
    IM_CHECK_EQ(s_App->camRefusal, std::string());

    // The label is DRAWN where the part sits, and a click on it is a pick:
    // a point of its first stroke, laid out as the job tab lays out the
    // part, is where the pick finds it.
    const cam::JobPart& jp = Job()->GetParts().front();
    const cam::SketchCurves curves = cam::SketchEntityCurves(
        *part, ElementRef{sketchId, "g" + std::to_string(first)});
    IM_CHECK(curves.why.empty() && !curves.strokes.empty());
    const glm::dvec3 model = curves.strokes.front().pts.front();
    const glm::dvec4 laid =
        cam::BodyToSetup(*Job()->FindSetup(added.id), jp.id) *
        glm::dvec4(model, 1.0);
    const glm::vec2 at =
        ui::WorldToScreen(*s_App, glm::vec3(laid.x, laid.y, laid.z));
    ui::SelectionItem hit;
    IM_CHECK(ui::CamEngraveCurveAt(*s_App, at, glm::vec3(0.0f),
                                   glm::vec3(0.0f, 0.0f, -1.0f), -1.0f, hit));
    IM_CHECK(hit.kind == ui::SelectionItem::Kind::SketchCurve);
    IM_CHECK_EQ(hit.element.featureId, sketchId);
    IM_CHECK_EQ(hit.jobPartId, jp.id);

    ui::SelectionItem label;
    label.kind = ui::SelectionItem::Kind::SketchCurve;
    label.element = ElementRef{sketchId, "g" + std::to_string(first)};
    label.jobPartId = jp.id;
    ui::CamFacePicked(*s_App, label);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.miss, std::string());
    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)1);
    // The second label joins the same engraving.
    label.element.name = "g" + std::to_string(second);
    ui::CamFacePicked(*s_App, label);
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->slotPick.miss, std::string());
    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)1);
    if (setup->operations.size() == 1) {
      const cam::Operation& op = setup->operations[0];
      IM_CHECK(op.type == cam::OpType::Engrave);
      IM_CHECK_EQ(op.targets.size(), (size_t)2);
      // A label's groove is its cap height / 8.
      IM_CHECK(std::abs(op.engraveWidth - 6.0 / 8.0) < 1e-9);
    }
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();

    ToolbarClick(ctx, "###tb_job_generate");
    ctx->Yield(3);
    IM_CHECK(!s_App->camToolpaths.empty());
    for (const cam::Toolpath& tp : s_App->camToolpaths) {
      IM_CHECK_EQ(tp.error, std::string());
      IM_CHECK(tp.skipped.empty());
      IM_CHECK(!tp.grooves.empty());
    }
    for (const cam::VerdictLine& line : s_App->camVerdict) {
      ctx->LogInfo("verdict: %s", line.text.c_str());
      IM_CHECK(line.level != cam::VerdictLine::Level::Bad);
    }

    s_App->camGenerateOpen = false;
    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // Without a V-bit the verb refuses where it was pressed, naming the tools
  // the setup has, and arms nothing
  // (docs/ui/tool-sessions.md#defaults-and-refusals).
  t = IM_REGISTER_TEST(engine, "cam", "engrave_without_a_v_bit_refuses");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Document* part = CamPart();
    part->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 40, 30, 20));
    auto sk = std::make_unique<SketchFeature>(
        "Marks", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f)));
    sketch::Text a;
    a.anchor = {4.0f, 10.0f};
    a.text = "a1";
    a.font = "hershey";
    a.height = 6.0;
    (void)sk->AddEntity(a);
    part->AddFeature(std::move(sk));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("Top");
    s_App->camActiveSetup = added.id;
    IM_CHECK(Job()->FindSetup(added.id)->tools[0].type != cam::ToolType::VBit);
    ctx->Yield(2);

    ToolbarClick(ctx, "###tb_job_engrave");
    ctx->Yield(2);
    IM_CHECK(s_App->slotPick.featureId != ui::kCamToolSlot);
    IM_CHECK(s_App->camToolOpen);
    IM_CHECK(s_App->camRefusal.find("V-bit") != std::string::npos);
    IM_CHECK(s_App->camRefusal.find("T1") != std::string::npos);
    IM_CHECK(Job()->FindSetup(added.id)->operations.empty());
    // Where it was pressed: the verb's own window says it.
    IM_CHECK(ImGui::FindWindowByName("###ToolWindow") != nullptr);

    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
    s_App->camRefusal.clear();
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // Indices from picks (docs/cam/rotary.md#one-operation-at-several-indices):
  // a square bar in the rotary with a recess in each side. Four floors,
  // picked with one verb, are ONE pocket at 0/90/180/270 -- the setup's
  // own index for the one facing up, the index that turns it up for each of
  // the others -- and its settings show them as chips.
  t = IM_REGISTER_TEST(engine, "cam", "four_floors_picked_are_one_op");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Document* part = CamPart();
    part->AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, -12, -12),
                                                  60.0, 24.0, 24.0));
    uint64_t body = part->GetFeature(0)->GetId();
    const glm::vec3 recesses[4][2] = {
        {{22, -5, 8}, {16, 10, 4}},   // +Z: faces up at A0
        {{22, 8, -5}, {16, 4, 10}},   // +Y
        {{22, -5, -12}, {16, 10, 4}}, // -Z
        {{22, -12, -5}, {16, 4, 10}}, // -Y
    };
    for (const auto& r : recesses) {
      part->AddFeature(
          std::make_unique<BoxFeature>("Recess", r[0], r[1].x, r[1].y, r[1].z));
      const uint64_t tool = part->GetFeature(part->FeatureCount() - 1)->GetId();
      part->AddFeature(std::make_unique<BooleanFeature>("Cut", body, tool,
                                                        BooleanOp::Subtract));
      body = part->GetFeature(part->FeatureCount() - 1)->GetId();
    }
    ctx->Yield(2);
    IM_CHECK_EQ(part->GetBodies().size(), (size_t)1);

    // The four floors: flat, 8 mm off the axis, facing round it.
    const Body& bar = part->GetBodies()[0];
    std::vector<std::pair<glm::vec3, ui::SelectionItem>> floors;
    for (const auto& [index, name] : bar.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (!bar.shape.FacePlane(index, o, n, u) || std::abs(n.x) > 0.1f ||
          std::abs(std::abs(glm::dot(o, n)) - 8.0f) > 1e-3f)
        continue;
      ui::SelectionItem f;
      f.kind = ui::SelectionItem::Kind::Face;
      f.element = ElementRef{bar.createdBy, name};
      floors.push_back({n, f});
    }
    IM_CHECK_EQ(floors.size(), (size_t)4);
    // Picked +Z first, the one already facing the spindle.
    std::sort(floors.begin(), floors.end(), [](const auto& l, const auto& r) {
      return l.first.z > r.first.z;
    });

    MakeJob();
    cam::Setup& added = Job()->AddSetup("On the trunnion");
    s_App->camActiveSetup = added.id;
    cam::Setup* setup = Job()->FindSetup(added.id);
    setup->machine.hasRotaryAxis = true;
    setup->heldIn = cam::Setup::HeldIn::Rotary;
    setup->chuck = 0;
    setup->safeZ = 30.0;
    setup->stock.margin = {0.0, 1.0, 1.0};
    setup->tools[0].diameter = 4.0;
    ctx->Yield(3);

    ToolbarClick(ctx, "###tb_job_pocket");
    ctx->Yield();
    for (const auto& [n, f] : floors) {
      ui::CamFacePicked(*s_App, f);
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->slotPick.miss, std::string());
    }
    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)1);
    if (setup->operations.size() != 1) {
      s_App->camToolOpen = false;
      ui::SetActivePart(*s_App, 0);
      return;
    }
    const uint64_t opId = setup->operations[0].id;
    IM_CHECK(setup->operations[0].indexA ==
             (std::vector<double>{0.0, 90.0, 180.0, 270.0}));
    IM_CHECK_EQ(setup->operations[0].targets.size(), (size_t)4);
    for (const cam::Toolpath& tp : s_App->camToolpaths) {
      IM_CHECK_EQ(tp.error, std::string());
      for (const std::string& why : tp.skipped)
        ctx->LogError("skipped: %s", why.c_str());
      IM_CHECK(tp.skipped.empty());
    }
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();

    // The settings: a chip per index, and the typed list behind "edit".
    ui::EditCamOperation(*s_App, opId);
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    for (const char* chip : {"**/###index_chip_0", "**/###index_chip_90",
                             "**/###index_chip_180", "**/###index_chip_270"})
      IM_CHECK(ctx->ItemInfo(chip, ImGuiTestOpFlags_NoError).ID != 0);
    IM_CHECK_EQ(ctx->ItemInfo("**/###index_a", ImGuiTestOpFlags_NoError).ID,
                (ImGuiID)0);
    ctx->ItemClick("**/###index_chip_270");
    ctx->Yield(2);
    IM_CHECK(Job()->FindSetup(added.id)->operations[0].indexA ==
             (std::vector<double>{0.0, 90.0, 180.0}));
    ctx->ItemClick("**/###index_edit");
    ctx->Yield(2);
    IM_CHECK(ctx->ItemInfo("**/###index_a", ImGuiTestOpFlags_NoError).ID != 0);
    ctx->ItemClick("**/###index_edit");
    ctx->Yield();

    s_App->camEditOp = 0;
    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
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

  // Rough, finish, clean up from the toolbar
  // (docs/cam/operations.md#rough-a-surface-and-rest-machining): a dome on a
  // plate, whose ring where they meet a 6 mm ball cannot reach.
  t = IM_REGISTER_TEST(engine, "cam",
                       "rough_finish_and_clean_up_from_the_toolbar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Document* part = CamPart();
    part->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 50, 50, 12));
    part->AddFeature(
        std::make_unique<SphereFeature>("Dome", glm::vec3(25, 25, 12), 10.0));
    part->AddFeature(std::make_unique<BooleanFeature>(
        "Domed plate", part->GetFeature(0)->GetId(),
        part->GetFeature(1)->GetId(), BooleanOp::Union));
    ctx->Yield(2);
    IM_CHECK_EQ(part->GetBodies().size(), (size_t)1);
    cam::Setup& added = MakeJob()->AddSetup("Relief");
    s_App->camActiveSetup = added.id;
    cam::Setup* setup = Job()->FindSetup(added.id);
    setup->tools[0].type = cam::ToolType::Ball;
    setup->tools[0].diameter = 6.0;
    cam::Tool small = setup->tools[0];
    small.number = 2;
    small.name = "2 mm ball";
    small.diameter = 2.0;
    setup->tools.push_back(small);
    ctx->Yield(2);

    // Rough: no pick.
    ToolbarClick(ctx, "###tb_job_surface_rough");
    ctx->Yield(2);
    // Finish: pick the dome.
    const Body& body = CamPart()->GetBodies()[0];
    ui::SelectionItem dome;
    dome.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (!body.shape.FacePlane(index, o, n, u)) {
        dome.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    IM_CHECK(dome.element.IsSet());
    ToolbarClick(ctx, "###tb_job_surface");
    ctx->Yield();
    ui::CamFacePicked(*s_App, dome);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    // Clean up, with the small ball chosen as the verb's window would.
    s_App->camVerbTool = 2;
    ToolbarClick(ctx, "###tb_job_rest");
    ctx->Yield(2);

    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)3);
    if (setup->operations.size() == 3) {
      const cam::Operation& rough = setup->operations[0];
      const cam::Operation& finish = setup->operations[1];
      const cam::Operation& rest = setup->operations[2];
      IM_CHECK(rough.type == cam::OpType::SurfaceParallel);
      IM_CHECK_GT(rough.stepdown, 0.0);
      IM_CHECK_GT(rough.stockToLeave, 0.0);
      IM_CHECK(finish.type == cam::OpType::SurfaceParallel);
      IM_CHECK_EQ(finish.stepdown, 0.0);
      IM_CHECK_EQ(rest.toolNumber, 2);
      IM_CHECK_EQ(rest.restFromTool, 1);
    }
    IM_CHECK_EQ(s_App->camRefusal, std::string());
    for (const cam::Toolpath& tp : s_App->camToolpaths)
      IM_CHECK_EQ(tp.error, std::string());

    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // A rotary pass bounded ALONG the axis by a picked face, from the verb's
  // window, and back to the whole length.
  t = IM_REGISTER_TEST(engine, "cam", "a_rotary_pass_limits_to_a_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Document* part = CamPart();
    part->AddFeature(std::make_unique<CylinderFeature>(
        "Big", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 12.0, 15.0));
    part->AddFeature(std::make_unique<CylinderFeature>(
        "Small", glm::vec3(15, 0, 0), glm::vec3(1, 0, 0), 8.0, 15.0));
    part->AddFeature(std::make_unique<BooleanFeature>(
        "Stepped", part->GetFeature(0)->GetId(), part->GetFeature(1)->GetId(),
        BooleanOp::Union));
    ctx->Yield(2);
    cam::Setup& added = MakeJob()->AddSetup("On the trunnion");
    s_App->camActiveSetup = added.id;
    cam::Setup* setup = Job()->FindSetup(added.id);
    setup->machine.hasRotaryAxis = true;
    // Between centres: a 30 mm bar in the default 15 mm grip leaves too
    // little to tell a face's span from the whole length.
    setup->chuck = 0;
    setup->safeZ = 30.0;
    setup->stock.margin = {0.0, 1.0, 1.0};
    setup->tools[0].type = cam::ToolType::Ball;
    ctx->Yield(2);

    const auto cutSpan = [&]() {
      double x0 = 1e9, x1 = -1e9;
      for (const cam::Toolpath& tp : s_App->camToolpaths)
        for (const cam::Move& m : tp.moves)
          if (m.kind == cam::Move::Kind::Cut) {
            x0 = std::min(x0, m.to.x);
            x1 = std::max(x1, m.to.x);
          }
      return x1 - x0;
    };
    ToolbarClick(ctx, "###tb_job_rotary_finish");
    ctx->Yield(2);
    IM_CHECK_EQ(Job()->FindSetup(added.id)->operations.size(), (size_t)1);
    const double whole = cutSpan();
    IM_CHECK_GT(whole, 25.0);

    ctx->ItemClick("//###ToolWindow/###cam_verb_limit");
    ctx->Yield();
    IM_CHECK_EQ(s_App->slotPick.featureId, ui::kCamToolSlot);
    const Body& body = CamPart()->GetBodies()[0];
    ui::SelectionItem round;
    round.kind = ui::SelectionItem::Kind::Face;
    for (const auto& [index, name] : body.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (!body.shape.FacePlane(index, o, n, u)) {
        round.element = ElementRef{body.createdBy, name};
        break;
      }
    }
    IM_CHECK(round.element.IsSet());
    ui::CamFacePicked(*s_App, round);
    ctx->Yield(2);
    setup = Job()->FindSetup(added.id);
    IM_CHECK_EQ(setup->operations.size(), (size_t)1); // limited, not added
    IM_CHECK_EQ(setup->operations[0].targets.size(), (size_t)1);
    const double limited = cutSpan();
    ctx->LogInfo("whole %.1f mm, limited %.1f mm", whole, limited);
    IM_CHECK_LT(limited, 17.0); // one of the two 15 mm faces

    ctx->ItemClick("//###ToolWindow/###cam_verb_whole");
    ctx->Yield(2);
    IM_CHECK(Job()->FindSetup(added.id)->operations[0].targets.empty());
    IM_CHECK_GT(cutSpan(), 25.0);

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
      glm::dvec3 o(0.0), n(0.0), u(0.0);
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

  // Preferences > Machines is the store
  // (docs/cam/setup.md#machine-profiles): New writes a file, a rename moves it,
  // an edit rewrites it, Delete removes it. The GUI runner points
  // XDG_CONFIG_HOME at a scratch dir, so this is a scratch store; the test
  // still cleans up by name.
  t = IM_REGISTER_TEST(engine, "cam", "machines_are_kept_in_preferences");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    for (const char* stale : {"Machine 1", "Test router"})
      (void)ui::DeleteMachine(*s_App, stale);
    const size_t before = s_App->machines.size();
    ctx->MenuClick("Edit/###edit_preferences");
    ctx->Yield();
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemClick("###cat_Machines");
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

  // The CAM panel's "Preferences › Machines" opens the window ON Machines,
  // wherever it was left, and search finds a machine by name and opens it
  // in the form (docs/ui/settings.md#the-window).
  t = IM_REGISTER_TEST(engine, "cam", "the_machine_button_opens_on_machines");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    (void)ui::DeleteMachine(*s_App, "Test search mill");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New CAM Job##addjob");
    ctx->Yield(4);
    IM_CHECK(s_App->job != nullptr);
    // The button shows while the store is empty; the in-memory mirror is
    // emptied for the test and reloaded from the store after it.
    s_App->machines.clear();
    // Left on another page, as a person would have.
    ui::OpenPreferences(*s_App, "Keyboard");
    ctx->Yield(2);
    s_App->settingsWindowOpen = false;
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemClick("###cam_new_machine_prefs");
    ctx->Yield(2);
    IM_CHECK(s_App->settingsWindowOpen);
    IM_CHECK_EQ(s_App->settingsCategory, std::string("Machines"));
    IM_CHECK(ctx->ItemExists("//###SettingsWindow/###mach_new"));
    ui::ReloadMachines(*s_App);

    cam::Machine mine;
    mine.name = "Test search mill";
    IM_CHECK(ui::SaveMachine(*s_App, mine));
    s_App->machineEditIndex = -1;
    ui::OpenPreferences(*s_App, "Appearance");
    ctx->Yield(2);
    ctx->SetRef("//###SettingsWindow");
    ctx->ItemInputValue("###set_search", "search mill");
    ctx->Yield(2);
    ctx->ItemClick("###find_Machines_Test search mill");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->settingsCategory, std::string("Machines"));
    IM_CHECK(s_App->settingsSearch.empty());
    IM_CHECK(s_App->machineEditIndex >= 0);
    IM_CHECK_EQ(
        s_App->machines[static_cast<size_t>(s_App->machineEditIndex)].name,
        std::string("Test search mill"));
    IM_CHECK(ctx->ItemExists("###mach_name"));
    (void)ui::DeleteMachine(*s_App, "Test search mill");
    s_App->settingsWindowOpen = false;
  };

  // A setup names a machine and EMBEDS a copy; the panel says when the copy
  // and the store disagree, and offers both directions. Every reference
  // surveyed reconciles this quietly (docs/cam/setup.md#machine-profiles).
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
    // The machine is one of the setup's settings, in the tool window.
    ctx->ItemDoubleClick(CamSetupRow(setup.id).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditSetup, setup.id);
    ctx->SetRef("//###ToolWindow");
    ctx->ItemClick("###cam_machine");
    ctx->ItemClick("//$FOCUSED/Test shop router");
    ctx->Yield(2);
    const cam::Setup* live = Job()->FindSetup(setup.id);
    IM_CHECK(live->machine == mine);
    IM_CHECK(std::abs(live->rapidFeed - 2500.0) < 1e-9); // seeded
    IM_CHECK(cam::MatchMachine(s_App->machines, live->machine) ==
             cam::MachineMatch::Global);
    IM_CHECK(!ctx->ItemExists("###cam_machine_use_mine"));

    // Edit the JOB'S copy: it now differs, and the settings offer mine back.
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
    ctx->ItemClick("###cam_setup_editor_close");
    ctx->Yield(2);
    IM_CHECK(!s_App->camToolOpen);
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
    ctx->ItemClick("###cat_Machines");
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
    glm::dvec3 bmin, bmax;
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
    ctx->ItemDoubleClick(CamSetupRow(setupId).c_str());
    ctx->Yield(2);
    const double safeBefore = Job()->FindSetup(setupId)->safeZ;
    ctx->ItemInputValue("//###ToolWindow/###cam_safez", "77");
    ctx->Yield(4);
    IM_CHECK(std::abs(Job()->FindSetup(setupId)->safeZ - 77.0) < 1e-6);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(std::abs(Job()->FindSetup(setupId)->safeZ - safeBefore) < 1e-6);
    ctx->ItemClick("//###ToolWindow/###cam_setup_editor_close");
    ctx->Yield(2);
  };

  // Delete reaches every row the CAM panel lists
  // (docs/ui/selection.md#lists-one-gesture-one-delete): a setup, a part, a
  // fixture and a tool are each clicked and deleted with the KEY, each as one
  // undo step that brings back what went -- as an operation always was, and
  // as a feature is. Before, only their menus could remove them.
  t = IM_REGISTER_TEST(engine, "cam", "delete_reaches_every_row_kind");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    const size_t second = ui::AddPart(*s_App, "Spacer");
    s_App->document->AddFeature(
        std::make_unique<BoxFeature>("Blk", glm::vec3(0, 0, 0), 20, 20, 12));
    ctx->Yield(2);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    cam::JobDocument* job = MakeJob();
    job->AddPart(s_App->parts[second].id, "Spacer");
    job->BumpVersion();
    const uint64_t spacer = job->GetParts().back().id;
    cam::Setup& a = job->AddSetup("A");
    const uint64_t aId = a.id;
    for (int n = 2; n <= 3; n++) {
      cam::Tool spare = a.tools.front();
      spare.number = n;
      spare.name = "spare " + std::to_string(n);
      a.tools.push_back(spare);
    }
    cam::Fixture clamp;
    clamp.id = job->AllocateId();
    clamp.name = "Clamp";
    clamp.box = glm::dvec3(10.0, 10.0, 30.0);
    clamp.pos = glm::dvec3(-20.0, 0.0, 0.0);
    a.fixtures.push_back(clamp);
    const uint64_t bId = job->AddSetup("B").id;
    s_App->camActiveSetup = aId;
    ctx->Yield(3);

    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_tools");
    ctx->Yield(2);

    const auto undoDepth = [] { return s_App->jobHistory.undo.size(); };
    // Click a row, press Delete: gone, in one step, and back on Ctrl+Z.
    const auto clickDelete = [&](const std::string& row) {
      ctx->ItemClick(row.c_str());
      ctx->Yield();
      const size_t depth = undoDepth();
      ctx->KeyPress(ImGuiKey_Delete);
      ctx->Yield(3);
      return undoDepth() - depth;
    };

    // A tool (the active setup's).
    IM_CHECK_EQ(clickDelete("tools/###cam_tool_2"), (size_t)1);
    IM_CHECK(Job()->FindSetup(aId)->FindTool(2) == nullptr);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(aId)->FindTool(2) != nullptr);

    // Two tools, Ctrl-clicked: both go, as ONE step.
    ctx->ItemClick("tools/###cam_tool_2");
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->ItemClick("tools/###cam_tool_3");
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    IM_CHECK_EQ(
        ui::SelectedIds(*s_App, ui::SelectionItem::Kind::CamTool).size(),
        (size_t)2);
    size_t depth = undoDepth();
    ctx->KeyPress(ImGuiKey_Delete);
    ctx->Yield(3);
    IM_CHECK_EQ(undoDepth() - depth, (size_t)1);
    IM_CHECK_EQ(Job()->FindSetup(aId)->tools.size(), (size_t)1);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(aId)->tools.size(), (size_t)3);

    // A fixture.
    IM_CHECK_EQ(clickDelete("**/###cam_fixture_" + std::to_string(clamp.id)),
                (size_t)1);
    IM_CHECK(Job()->FindSetup(aId)->fixtures.empty());
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(aId)->fixtures.size(), (size_t)1);

    // A part.
    IM_CHECK_EQ(clickDelete(CamPartRow(spacer)), (size_t)1);
    IM_CHECK(Job()->FindPart(spacer) == nullptr);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindPart(spacer) != nullptr);

    // A setup -- the one that is not active, so the click makes it so.
    IM_CHECK_EQ(clickDelete(CamSetupRow(bId)), (size_t)1);
    IM_CHECK(Job()->FindSetup(bId) == nullptr);
    IM_CHECK(Job()->FindSetup(aId) != nullptr);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(bId) != nullptr);

    // A plain click on one kind lets go of the others: a setup clicked a
    // moment ago does not go with the tool Delete was pressed for.
    ctx->ItemClick(CamSetupRow(aId).c_str());
    ctx->Yield();
    ctx->ItemOpen("###cam_tools");
    ctx->Yield();
    IM_CHECK_EQ(clickDelete("tools/###cam_tool_3"), (size_t)1);
    IM_CHECK(Job()->FindSetup(aId) != nullptr);
    IM_CHECK(Job()->FindSetup(aId)->FindTool(3) == nullptr);

    s_App->selection.Clear();
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // Deleting an operation or a tool from the PANEL is one undo step. Both
  // snapshotted the job themselves and were then recorded again by the guard
  // the panel draws under: one Ctrl+Z brought back nothing.
  t = IM_REGISTER_TEST(engine, "cam", "a_delete_from_the_panel_is_one_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Job");
    const uint64_t sid = setup.id;
    cam::Tool spare = setup.tools.front();
    spare.number = 2;
    spare.name = "spare";
    setup.tools.push_back(spare);
    cam::Operation op;
    op.id = Job()->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 12.0;
    op.bottomZ = -1.0;
    setup.operations.push_back(op);
    s_App->camActiveSetup = sid;
    ctx->Yield(3);
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_tools");
    ctx->Yield(2);
    const auto undoDepth = [] { return s_App->jobHistory.undo.size(); };

    size_t depth = undoDepth();
    ctx->ItemClick(
        ("**/$$" + std::to_string(static_cast<int>(op.id)) + "/###op_row")
            .c_str(),
        ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###op_delete");
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(sid)->operations.empty());
    IM_CHECK_EQ(undoDepth() - depth, (size_t)1);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK_EQ(Job()->FindSetup(sid)->operations.size(), (size_t)1);

    depth = undoDepth();
    ctx->ItemClick("tools/###cam_tool_2", ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###cam_tool_delete");
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(sid)->FindTool(2) == nullptr);
    IM_CHECK_EQ(undoDepth() - depth, (size_t)1);
    ui::GlobalUndo(*s_App);
    ctx->Yield(3);
    IM_CHECK(Job()->FindSetup(sid)->FindTool(2) != nullptr);

    s_App->selection.Clear();
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // The operation, tool and fixture editors in the tool window: a typed
  // field is one Ctrl+Z, and so is a whole drag (docs/cam/gui.md#undo). They
  // drew under no guard, so a changed depth, cutter or clamp size could not
  // be taken back -- their edits bumped the counter with nothing to record it.
  t = IM_REGISTER_TEST(engine, "cam", "an_editor_field_undoes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::JobDocument* job = MakeJob();
    cam::Setup& setup = job->AddSetup("Job");
    const uint64_t sid = setup.id;
    cam::Operation op;
    op.id = job->AllocateId();
    op.name = "Outline";
    op.type = cam::OpType::Profile;
    op.topZ = 12.0;
    op.bottomZ = -1.0;
    setup.operations.push_back(op);
    cam::Fixture clamp;
    clamp.id = job->AllocateId();
    clamp.name = "Clamp";
    clamp.box = glm::dvec3(10.0, 10.0, 30.0);
    clamp.pos = glm::dvec3(-20.0, 0.0, 0.0);
    setup.fixtures.push_back(clamp);
    s_App->camActiveSetup = sid;
    ctx->Yield(3);
    ImGuiWindow* camWin = ImGui::FindWindowByName(ui::kPanelCam);
    IM_CHECK(camWin != nullptr && camWin->DockNode != nullptr);
    ctx->ItemClick(camWin->TabId);
    ctx->Yield(2);
    ctx->SetRef("//CAM###CamPanel");
    ctx->ItemOpen("###cam_tools");
    ctx->Yield(2);

    const auto undoDepth = [] { return s_App->jobHistory.undo.size(); };
    const auto undo = [&] {
      ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
      ctx->Yield(3);
    };
    // Typed, then dragged: each exactly one step, each undone by one Ctrl+Z.
    const auto typedAndDragged = [&](const char* field, const char* typed,
                                     double want, auto read) {
      const double before = read();
      size_t depth = undoDepth();
      ctx->ItemInputValue(field, typed);
      ctx->Yield(3);
      IM_CHECK(std::abs(read() - want) < 1e-6);
      IM_CHECK_EQ(undoDepth() - depth, (size_t)1);
      undo();
      IM_CHECK(std::abs(read() - before) < 1e-6);

      // A drag held over several frames, each of which moves the value: the
      // test engine's own ItemDragWithDelta lands in one frame, and would
      // pass with a step per frame.
      depth = undoDepth();
      ctx->MouseMove(field);
      const ImVec2 start = ImGui::GetIO().MousePos;
      ctx->MouseDown(ImGuiMouseButton_Left);
      double last = read();
      int moves = 0;
      for (int k = 1; k <= 4; k++) {
        ctx->MouseMoveToPos(ImVec2(start.x + 15.0f * k, start.y));
        ctx->Yield(2);
        if (std::abs(read() - last) > 1e-9)
          moves++;
        last = read();
      }
      ctx->MouseUp(ImGuiMouseButton_Left);
      ctx->Yield(3);
      IM_CHECK_GT(moves, 1); // the value moved on more than one frame
      IM_CHECK(std::abs(read() - before) > 1e-6);
      IM_CHECK_EQ(undoDepth() - depth, (size_t)1);
      undo();
      IM_CHECK(std::abs(read() - before) < 1e-6);
    };

    // The operation's top.
    ctx->ItemDoubleClick(
        ("**/$$" + std::to_string(static_cast<int>(op.id)) + "/###op_row")
            .c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditOp, op.id);
    typedAndDragged("//###ToolWindow/**/###top", "20", 20.0,
                    [&] { return Job()->FindSetup(sid)->operations[0].topZ; });
    ctx->ItemClick("//###ToolWindow/###cam_edit_done");
    ctx->Yield(2);

    // The tool's diameter.
    ctx->ItemDoubleClick("tools/###cam_tool_1");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditTool, 1);
    typedAndDragged("//###ToolWindow/**/###dia", "4", 4.0, [&] {
      return Job()->FindSetup(sid)->FindTool(1)->diameter;
    });
    ctx->ItemClick("//###ToolWindow/###cam_tool_editor_close");
    ctx->Yield(2);

    // The fixture's spin.
    ctx->ItemDoubleClick(
        ("**/###cam_fixture_" + std::to_string(clamp.id)).c_str());
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->camEditFixture, clamp.id);
    typedAndDragged("//###ToolWindow/**/###cam_fixture_rot", "30", 30.0,
                    [&] { return Job()->FindSetup(sid)->fixtures[0].rotZdeg; });
    ctx->ItemClick("//###ToolWindow/###cam_fixture_editor_close");
    ctx->Yield(2);

    s_App->selection.Clear();
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    if (ImGuiWindow* modelWin = ImGui::FindWindowByName(ui::kPanelModel))
      ctx->ItemClick(modelWin->TabId);
    ctx->Yield(2);
  };

  // The verb window keeps a bounded height and scrolls
  // (docs/cam/gui.md#verb-windows). It grew with what it listed, and
  // Engrave's -- an operation per label, the verdict's lines -- grew down
  // over the curves it was asking to be clicked.
  t = IM_REGISTER_TEST(engine, "cam", "the_engrave_window_keeps_its_height");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 12));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Top");
    const uint64_t sid = setup.id;
    setup.tools[0].type = cam::ToolType::VBit;
    setup.tools[0].diameter = 6.0;
    setup.tools[0].tipAngleDeg = 90.0;
    for (int i = 0; i < 40; i++) {
      cam::Operation op;
      op.id = Job()->AllocateId();
      op.name = "Label " + std::to_string(i + 1);
      op.type = cam::OpType::Engrave;
      op.toolNumber = setup.tools[0].number;
      setup.operations.push_back(op);
    }
    s_App->camActiveSetup = sid;
    ctx->Yield(3);
    ToolbarClick(ctx, "###tb_job_engrave");
    ctx->Yield(2);
    for (int i = 0; i < 30; i++)
      s_App->camVerdict.push_back(
          {cam::VerdictLine::Level::Warn,
           "a line of the verdict, number " + std::to_string(i + 1)});
    ctx->Yield(4);
    ImGuiWindow* win = ImGui::FindWindowByName("Engrave###ToolWindow");
    IM_CHECK(win != nullptr);
    if (win == nullptr)
      return;
    // The list scrolls in a region of its own, so the prompt and Close stay
    // on screen.
    ImGuiWindow* list = nullptr;
    for (ImGuiWindow* child : win->DC.ChildWindows)
      if (std::string(child->Name).find("cam_tool_list") != std::string::npos)
        list = child;
    IM_CHECK(list != nullptr);
    if (list == nullptr)
      return;
    const float vp = s_App->viewportSize.y;
    ctx->LogInfo("engrave window %.0f px tall, its list %.0f of %.0f, "
                 "viewport %.0f",
                 win->Size.y, list->Size.y, list->ContentSize.y, vp);
    IM_CHECK(list->ContentSize.y > 0.4f * vp);   // more than fits...
    IM_CHECK_LE(list->Size.y, 0.4f * vp + 1.0f); // ...in a bounded list...
    IM_CHECK(list->ScrollMax.y > 0.0f);          // ...that scrolls to it
    IM_CHECK(ctx->ItemExists("//Engrave###ToolWindow/###cam_tool_done"));
    // ...and more of it does not grow the window by a pixel.
    const float tall = win->Size.y;
    for (int i = 0; i < 30; i++)
      s_App->camVerdict.push_back({cam::VerdictLine::Level::Bad,
                                   "and another, " + std::to_string(i + 1)});
    ctx->Yield(4);
    IM_CHECK(std::abs(win->Size.y - tall) < 1.0f);

    s_App->camVerdict.clear();
    s_App->camToolOpen = false;
    s_App->camVerb = ui::AppState::CamVerb::None;
    if (s_App->slotPick.featureId == ui::kCamToolSlot)
      s_App->slotPick = {};
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
  };

  // A job tab is framed to its parts AND its stock, on the first visit and
  // when the project opens on it (docs/cam/gui.md#what-the-viewport-draws).
  // Opening a project whose active tab was a job framed nothing at all, and the
  // fit left the stock out.
  t = IM_REGISTER_TEST(engine, "cam", "a_job_tab_frames_its_parts_and_stock");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    CamPart()->AddFeature(
        std::make_unique<BoxFeature>("Blk", glm::vec3(0, 0, 0), 20, 20, 10));
    ctx->Yield(2);
    cam::Setup& setup = MakeJob()->AddSetup("Wide");
    // A stock much bigger than the part, all round: framing the part alone
    // puts the stock's corners off the screen.
    setup.stock.mode = cam::StockMode::Relative;
    setup.stock.margin = glm::dvec3(60.0, 45.0, 5.0);
    s_App->camActiveSetup = setup.id;
    ctx->Yield(3);

    // Every corner of the stock and of the part lands inside the viewport,
    // and the job is not a speck in the middle of it.
    const auto framed = [&](const char* when) {
      if (s_App->camera)
        s_App->camera->FinishAnimation();
      ctx->Yield(2);
      glm::dvec3 lo(0.0), hi(0.0);
      IM_CHECK(ui::CamStockBox(*s_App, lo, hi));
      const glm::vec2 vp0 = s_App->viewportPos;
      const glm::vec2 vp1 = vp0 + s_App->viewportSize;
      glm::vec2 sLo(FLT_MAX), sHi(-FLT_MAX);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                          (c & 4) ? hi.z : lo.z);
        const glm::vec2 s = ui::WorldToScreen(*s_App, p);
        sLo = glm::min(sLo, s);
        sHi = glm::max(sHi, s);
      }
      ctx->LogInfo("%s: stock on screen %.0f,%.0f .. %.0f,%.0f in viewport "
                   "%.0f,%.0f .. %.0f,%.0f",
                   when, sLo.x, sLo.y, sHi.x, sHi.y, vp0.x, vp0.y, vp1.x,
                   vp1.y);
      IM_CHECK(sLo.x >= vp0.x - 1.0f && sLo.y >= vp0.y - 1.0f);
      IM_CHECK(sHi.x <= vp1.x + 1.0f && sHi.y <= vp1.y + 1.0f);
      const glm::vec2 span = sHi - sLo;
      IM_CHECK(span.x > 0.4f * s_App->viewportSize.x ||
               span.y > 0.4f * s_App->viewportSize.y);
    };

    // The first visit.
    const size_t jobTab = static_cast<size_t>(s_App->activePart);
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    s_App->parts[jobTab].everFramed = false;
    ui::SetActivePart(*s_App, jobTab);
    framed("first visit");

    // Opening a project with the job tab active.
    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_test_job_framing")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    if (s_App->camera) // look somewhere else entirely first
      s_App->camera->SetPosition(glm::vec3(900.0f, -700.0f, -500.0f));
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    IM_CHECK(s_App->job != nullptr);
    framed("opened on the job");
    std::filesystem::remove_all(dir);

    ctx->MenuClick("File/New");
    ctx->Yield(2);
  };

  // Frame time on a job tab (docs/rendering/frame-pacing.md#tests). A job tab
  // asked OCC for the tight box of every framed body three times a frame, and
  // on a turned blank's tori that is a search per face: the chess undersides
  // job ran at one frame a second doing nothing, and switching job tabs cost
  // 3-7 s. Nothing measured frame time, so a human found it. The bounds are
  // the main thread's CPU time over the `update` and `ui` phases, which
  // neither a software rasteriser nor a loaded machine moves (the wall-clock
  // phases it read before are stretched by every wait for a core), and sit
  // many times above a healthy frame and far below a broken one.
  t = IM_REGISTER_TEST(engine, "frametime", "job_tab_idle_and_switch");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A stepped shaft with its circular edges rounded: six toroidal faces,
    // the surfaces whose exact box is the expensive one.
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 15.0, 60.0));
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Journal", glm::vec3(60, 0, 0), glm::vec3(1, 0, 0), 10.0, 20.0));
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Shaft", CamPart()->GetFeature(0)->GetId(),
        CamPart()->GetFeature(1)->GetId(), BooleanOp::Union));
    CamPart()->AddFeature(std::make_unique<CylinderFeature>(
        "Spigot", glm::vec3(80, 0, 0), glm::vec3(1, 0, 0), 6.0, 10.0));
    CamPart()->AddFeature(std::make_unique<BooleanFeature>(
        "Stepped", CamPart()->GetFeature(2)->GetId(),
        CamPart()->GetFeature(3)->GetId(), BooleanOp::Union));
    ctx->Yield(2);
    IM_CHECK_EQ(CamPart()->GetBodies().size(), (size_t)1);
    std::vector<ElementRef> rims;
    {
      const std::vector<Body>& bodies = CamPart()->GetBodies();
      for (const auto& [index, name] : bodies[0].elementMap.edgeNames) {
        const ElementRef ref{bodies[0].createdBy, name};
        const auto eb = Measure::ElementBounds(bodies, ref);
        if (eb && eb->max.y - eb->min.y > 1.0f) // a circle, not the seam
          rims.push_back(ref);
      }
    }
    IM_CHECK_EQ(rims.size(), (size_t)6);
    CamPart()->AddFeature(std::make_unique<FilletFeature>("Rounds", rims, 1.5));
    ctx->Yield(2);
    IM_CHECK(CamPart()->GetFeature(5)->GetError().empty());
    const uint64_t partId = s_App->parts[s_App->activePart].id;
    const size_t partTab = static_cast<size_t>(s_App->activePart);

    // Jobs on it, each held in the rotary and indexed.
    const auto rotaryJob = [&](const char* name, double indexDeg) {
      ui::SetActivePart(*s_App, partTab);
      const size_t tab = ui::CreateJobForPart(*s_App, partId, name);
      cam::Setup& added = Job()->AddSetup(name);
      added.machine.hasRotaryAxis = true;
      added.machine.rotary.along = 'x';
      added.machine.rotary.chuckEnd = -1;
      added.machine.rotary.word = 'A';
      added.machine.rotary.topAxis = 'y';
      added.machine.rotary.topSign = -1;
      added.heldIn = cam::Setup::HeldIn::Rotary;
      added.rotationA = indexDeg;
      added.stock.margin = {0.0, 1.0, 1.0};
      s_App->camActiveSetup = added.id;
      return tab;
    };
    // One frame, and the main thread's CPU time over its update and ui
    // phases (FrameTimer::SetWorkCpuMs): what the app did, not how long it
    // waited for a core — a loaded machine stretches the WALL phases, and a
    // frame that computed nothing read 50 ms of them — and not what drawing
    // it took, which on llvmpipe is most of the frame's CPU (a whole idle
    // frame here is ~20 ms of main-thread CPU against ~0.5 ms of ui).
    const auto frameCpuMs = [ctx] {
      ctx->Yield();
      return s_App->frameTimer.Snapshot(1).workCpuMs;
    };
    const auto msSince = [](std::chrono::steady_clock::time_point t0) {
      return std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - t0)
          .count();
    };

    // The idle frame, at an index no quarter turn reaches: every framed copy
    // of the part is a new shape, so only the memos (CamTool.cpp's
    // CachedStockView, and the box remembered by shape and location) stand
    // between this frame and three exact boxes.
    const size_t indexed = rotaryJob("Indexed", 30.0);
    ctx->Yield(2);
    {
      // What one of those boxes costs here, for the log: computed, since
      // asked through GetBoundingBox a framing at this angle is now a memo
      // hit (CADShape::RememberTightBox).
      const CADShape cold =
          ui::JobBodies(*s_App).at(0).shape.Transformed(glm::rotate(
              glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(1, 0, 0)));
      glm::dvec3 lo(0.0), hi(0.0);
      const auto t0 = std::chrono::steady_clock::now();
      cold.ExactBoxUnmemoized(lo, hi);
      ctx->LogInfo("one cold exact box of the part: %.1f ms", msSince(t0));
    }
    ctx->Yield(20); // settled: the first visit's framing and meshing are done
    std::vector<double> idle;
    for (int i = 0; i < 30; i++)
      idle.push_back(frameCpuMs());
    std::sort(idle.begin(), idle.end());
    const double idleMedian = idle[idle.size() / 2];
    ctx->LogInfo("idle job tab: median %.2f ms of CPU a frame, worst %.2f ms",
                 idleMedian, idle.back());
    // Healthy is under a millisecond; the regression was a second.
    IM_CHECK_LE(idleMedian, 15.0);

    // Switching. Two more jobs framed by quarter turns, which carry their
    // source's box: returning to one pays nothing. The first visit to each is
    // behind us before the measuring starts.
    const size_t flats = rotaryJob("Flats", 90.0);
    ctx->Yield(10);
    const size_t flipped = rotaryJob("Flipped", 180.0);
    ctx->Yield(10);
    double worstQuarter = 0.0, worstIndexed = 0.0;
    for (int round = 0; round < 3; round++)
      for (const size_t tab : {flats, flipped, indexed}) {
        ui::SetActivePart(*s_App, tab);
        double worst = 0.0;
        for (int i = 0; i < 6; i++)
          worst = std::max(worst, frameCpuMs());
        ctx->LogInfo("to %s: worst frame %.1f ms of CPU",
                     s_App->parts[tab].name.c_str(), worst);
        double& into = tab == indexed ? worstIndexed : worstQuarter;
        into = std::max(into, worst);
      }
    ctx->LogInfo("worst switch: %.1f ms quarter-turned, %.1f ms indexed 30",
                 worstQuarter, worstIndexed);
    IM_CHECK_LE(worstQuarter, 120.0);
    // A job at an angle no quarter turn reaches: its framed copies are new
    // shapes each visit, and paid their exact boxes again every time (210 ms
    // here, most of a second on the chess blanks) until the box was
    // remembered by shape identity and location. Now it costs what a
    // quarter-turned one does, and is held to the same bound.
    IM_CHECK_LE(worstIndexed, 120.0);
  };

  // The terminal panel (docs/ui/terminal.md). Off by default, docked
  // right when on, and — the part that decides whether it feels safe — the
  // keyboard goes to it ONLY while the pointer is over it.
#endif
}
