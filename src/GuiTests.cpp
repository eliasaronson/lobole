#include "GuiTests.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/features/BooleanFeature.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/Occurrence.h"
#include "core/Feature.h"
#include "core/features/FilletFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/Measure.h"
#include "core/io/Serialization.h"
#include "core/features/SketchFeature.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <glm/glm.hpp>

// Tests run inside the live application and reach it through the AppState.
static ui::AppState* s_App = nullptr;

static Document* Doc() { return s_App->document.get(); }

// Window position of a world point, mapped through the central-node viewport
// rect (where the 3D scene actually renders), matching the picking math.
static ImVec2 WorldToWindow(const glm::vec3& p) {
  const Camera& cam = *s_App->camera;
  glm::vec4 clip =
      cam.GetProjectionMatrix() * cam.GetViewMatrix() * glm::vec4(p, 1.0f);
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  const glm::vec2 pos = s_App->viewportPos;
  const glm::vec2 size = s_App->viewportSize;
  return ImVec2(pos.x + (ndc.x + 1.0f) * 0.5f * size.x,
                pos.y + (ndc.y + 1.0f) * 0.5f * size.y);
}

// Most tests want a committed feature: create via menu, then accept the
// preview (exercises the real accept path).
static void MenuCreateAccept(ImGuiTestContext* ctx, const char* menuPath) {
  ctx->SetRef("//##MainMenuBar");
  ctx->MenuClick(menuPath);
  ctx->Yield();
  ui::AcceptPendingFeature(*s_App);
  ctx->Yield();
}

void RegisterGuiTests(ImGuiTestEngine* engine, ui::AppState* state) {
  s_App = state;

  ImGuiTest* t = nullptr;

  t = IM_REGISTER_TEST(engine, "app", "boots_and_renders");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->Yield(3);
    IM_CHECK(ctx->UiContext->WithinFrameScope == false ||
             ctx->UiContext->FrameCount > 0);
  };

  t = IM_REGISTER_TEST(engine, "app", "core_windows_exist");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(ctx->GetWindowByRef("//Model") != nullptr);
    IM_CHECK(ctx->GetWindowByRef("//Properties") != nullptr);
  };

  t = IM_REGISTER_TEST(engine, "document", "parametric_box_undo_redo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(Doc() != nullptr);
    const size_t before = Doc()->FeatureCount();

    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), before + 1);

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

    MenuCreateAccept(ctx, "Create/Sphere");
    MenuCreateAccept(ctx, "Create/Cylinder");
    IM_CHECK_EQ(Doc()->FeatureCount(), before + 2);
    IM_CHECK_EQ(Doc()->GetBodies().size(), before + 2);

    ctx->MenuClick("Edit/Undo");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), before);
  };

  t = IM_REGISTER_TEST(engine, "document", "new_project_clears_features");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK(Doc()->FeatureCount() > 0);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "properties", "edit_box_width_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");

    auto* box = dynamic_cast<BoxFeature*>(Doc()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0);

    ctx->ItemClick("//Model/Box 1##0");
    // Labels-left rows (U4): the widget id is "##<label>", the visible label
    // is a separate Text item.
    ctx->ItemInputValue("//Properties/##Width", 5.0f);
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

  t = IM_REGISTER_TEST(engine, "shortcuts", "undo_redo_keyboard");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "middle_drag_rotates_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");

    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->Yield();

    const glm::vec3 posAfter = s_App->camera->GetPosition();
    IM_CHECK(glm::length(posAfter - posBefore) > 0.1f);
    // Rotation keeps the distance to the target
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "ctrl_middle_drag_pans_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const glm::vec3 targetBefore = s_App->camera->GetTarget();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseDown(ImGuiMouseButton_Middle);
    ctx->MouseMoveToPos(ImVec2(center.x + 100.0f, center.y + 60.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();

    // Pan moves the orbit target
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.05f);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "ctrl_mid_drag_switches_to_pan");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    // Pressing Ctrl after the middle button is already down must switch the
    // drag from rotate to pan
    const glm::vec3 targetBefore = s_App->camera->GetTarget();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Middle); // starts as rotate
    ctx->KeyDown(ImGuiMod_Ctrl);             // ...now becomes pan
    ctx->MouseMoveToPos(ImVec2(center.x + 90.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Middle);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();

    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.05f);
  };

  t = IM_REGISTER_TEST(engine, "viewport", "scroll_zooms");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ctx->MouseMoveToPos(ImVec2(size.x * 0.5f, size.y * 0.5f));
    ctx->MouseWheelY(2.0f); // scroll up = zoom in
    ctx->Yield();

    IM_CHECK(s_App->camera->GetDistance() < distBefore);
  };

  // Trackpad navigation: Alt+left-drag orbits (middle-button substitute) and
  // the click never reaches picking; Shift+scroll pans instead of zooming.
  t = IM_REGISTER_TEST(engine, "viewport", "alt_left_drag_orbits");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->camera->StopAnimation();
    ctx->Yield();
    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Alt);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Alt);
    ctx->Yield();

    IM_CHECK(glm::length(s_App->camera->GetPosition() - posBefore) > 0.1f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None); // drag ended
  };

  t = IM_REGISTER_TEST(engine, "viewport", "shift_scroll_pans");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const glm::vec3 targetBefore = s_App->camera->GetTarget();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ctx->MouseMoveToPos(ImVec2(size.x * 0.5f, size.y * 0.5f));
    ctx->KeyDown(ImGuiMod_Shift);
    ctx->MouseWheelY(2.0f);
    ctx->KeyUp(ImGuiMod_Shift);
    ctx->Yield();

    // Pan moves the orbit target and keeps the distance.
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.01f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.01f * distBefore);
  };

  // Entering simulation or sketch mode drops an armed measure tool — it owns
  // the viewport while active and those modes have no toggle to disarm it.
  t = IM_REGISTER_TEST(engine, "measure", "cleared_on_mode_entry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");

    s_App->measure.active = true;
    ui::EnterSimulation(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    ui::ExitSimulation(*s_App);
    ctx->Yield();

    s_App->measure.active = true;
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    s_App->sketchEdit = ui::SketchEditState{}; // leave sketch edit
    // Entering the sketch aligned the camera with an animation; don't leak it
    // into the next test's camera framing (it overrides setters until done).
    s_App->camera->StopAnimation();
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "viewport", "click_selects_facing_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    ctx->Yield();

    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 bodyCenter = (min + max) * 0.5f;

    // File/New does not reset the camera, so the orbit/pan/zoom left by earlier
    // viewport tests leaks in (the default box is off-origin, so it can even
    // project behind a docked panel). Frame it deterministically: a clean iso
    // direction, centred on the box, far enough back (~2x the fit distance)
    // that the box stays small and the deselect click below lands on empty
    // space rather than the box itself.
    const float radius = glm::length(max - min) * 0.5f;
    s_App->camera->SetTarget(bodyCenter);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        bodyCenter + glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * radius * 5.0f);
    ctx->Yield();

    // Click the +X face's center pixel: the ray hits that face's interior, a
    // full half-extent from every edge. (The body-center pixel is unusable
    // since U1b: on a diagonal view it lines up with the near corner, whose
    // edges correctly outrank the face in the unified pick.)
    const glm::vec3 faceCenter(max.x, bodyCenter.y, bodyCenter.z);
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    const Face* face = s_App->faceSelector->GetSelectedFace();
    IM_CHECK(face != nullptr);

    // Selected face must face the camera and sit on the near side of center
    glm::vec3 normal =
        s_App->faceSelector->GetFaceNormal(face->meshId, face->faceIndex);
    glm::vec3 toCamera =
        glm::normalize(s_App->camera->GetPosition() - face->center);
    IM_CHECK(glm::dot(normal, toCamera) > 0.3f);

    // Clicking empty space clears the selection
    ctx->MouseMoveToPos(ImVec2(WorldToWindow(bodyCenter).x,
                               ImGui::GetMainViewport()->Size.y * 0.92f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(s_App->faceSelector->GetSelectedFace() == nullptr);
  };

  t = IM_REGISTER_TEST(engine, "sketch", "front_plane_enters_edit");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    IM_CHECK(!s_App->sketchEdit.IsEditing());

    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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

  t = IM_REGISTER_TEST(engine, "sketch", "draw_line_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Top Plane");
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);

    // Entering a sketch aligns the camera to the plane over a 1s animation. Wait
    // for it to settle before projecting click coordinates — otherwise
    // SketchUVToScreen reads a mid-flight camera and the clicks land off target.
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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

    // Extrude from the toolbar → preview, then accept
    ctx->SetRef("//Sketch");
    ctx->ItemClick("Extrude...");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App)); // extrude is a pending preview
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();

    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetFeature(1)->TypeName() == "Extrude");
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() > 1.0); // rect x default dist
  };

  t = IM_REGISTER_TEST(engine, "preview", "box_accept_commits_once");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();

    ctx->MenuClick("Create/Box");
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
    ctx->MenuClick("File/New");
    ctx->Yield();

    ctx->MenuClick("Create/Box");
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
    ctx->MenuClick("File/New");
    ctx->Yield();

    ctx->MenuClick("Create/Box");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    // While pending, Ctrl+Z cancels the preview rather than touching history
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK(!ui::HasPendingFeature(*s_App));
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0);
  };

  // Direct-manipulation preview handles (docs/ui/direct-manipulation.md):
  // drag the viewport arrow to set the pending extrude's distance.
  t = IM_REGISTER_TEST(engine, "handles", "extrude_arrow_drag_sets_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    ctx->SetRef("//Sketch");
    ctx->ItemClick("Extrude...");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));
    auto* ext =
        dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(ext->distanceExpr.empty());

    // Frame deterministically from an iso direction so the extrude axis is
    // draggable on screen (facing the plane it would be view-parallel). The
    // target sits below the profile so the handle projects in the upper half
    // of the viewport, clear of the bottom-center preview dialog.
    sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const glm::vec3 N = glm::normalize(sketch->plane.GetNormal());
    const glm::vec3 C = sketch->plane.PlaneToWorld(glm::vec2(2.0f, 1.0f));
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(C + glm::vec3(0.0f, -3.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(C + glm::vec3(0.0f, -3.0f, 0.0f) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) * 12.0f);
    ctx->Yield(2);

    // Grab the arrow mid-shaft and pull one world unit further out.
    const double d0 = ext->distance;
    const float L =
        0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
    const glm::vec3 anchor = C + N * static_cast<float>(d0);
    const glm::vec3 grab = anchor + N * (0.45f * L);
    ctx->MouseMoveToPos(WorldToWindow(grab));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(grab + N * 1.0f));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active); // the click grabbed the handle
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - (d0 + 1.0)) < 0.05);
    IM_CHECK(ui::HasPendingFeature(*s_App)); // still previewing

    // Esc mid-drag restores the grab-time value and keeps the preview.
    const double d1 = ext->distance;
    const glm::vec3 anchor1 = C + N * static_cast<float>(d1);
    ctx->MouseMoveToPos(WorldToWindow(anchor1 + N * (0.45f * L)));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(anchor1 + N * (0.45f * L + 1.0f)));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - d1) < 1e-6);
    IM_CHECK(ui::HasPendingFeature(*s_App)); // Esc consumed by the drag only

    // A click away from the handle falls through to picking: no value change,
    // preview still pending.
    ctx->MouseMoveToPos(ImVec2(s_App->viewportPos.x + 30.0f,
                               s_App->viewportPos.y + 30.0f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ext = dynamic_cast<ExtrudeFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(ext != nullptr);
    IM_CHECK(std::abs(ext->distance - d1) < 1e-6);
    IM_CHECK(ui::HasPendingFeature(*s_App));

    // Accept commits the dragged value as one undoable feature.
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(s_App->commands->CanUndo());
  };

  t = IM_REGISTER_TEST(engine, "handles", "fillet_marker_drag_sets_radius");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    ctx->Yield();

    // Preview a fillet on a known edge (the picking flow is covered by the
    // fillet tests; this one is about the handle).
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.edgeNames.empty());
    const ElementRef ref{body.createdBy,
                         body.elementMap.edgeNames.begin()->second};
    ui::BeginPreviewFeature(
        *s_App, std::make_unique<FilletFeature>("Fillet",
                                                std::vector<ElementRef>{ref},
                                                0.4));
    ctx->Yield(2);
    auto* fil =
        dynamic_cast<FilletFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(fil != nullptr);

    // Recompute the handle's anchor/direction the way PreviewHandles does:
    // the picked edge resolved on the blend's INPUT body (the displayed body
    // is already blended and renames its edges), pulled away from the body
    // center ⟂ to the edge.
    const std::vector<Body>* input = Doc()->CachedBodiesAfter(0);
    IM_CHECK(input != nullptr);
    const auto eb = Measure::ElementBounds(*input, ref);
    IM_CHECK(eb.has_value());
    const glm::vec3 P = (eb->min + eb->max) * 0.5f;
    const glm::vec3 T = glm::normalize(eb->max - eb->min);
    glm::vec3 bmin, bmax;
    (*input)[0].shape.GetBoundingBox(bmin, bmax);
    const glm::vec3 bodyCenter = (bmin + bmax) * 0.5f;
    glm::vec3 out = P - bodyCenter;
    out -= glm::dot(out, T) * T;
    IM_CHECK(glm::length(out) > 1e-4f);
    out = glm::normalize(out);

    // Frame from iso, target below the body so the handle clears the dialog.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(bodyCenter + glm::vec3(0.0f, -2.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        bodyCenter + glm::vec3(0.0f, -2.0f, 0.0f) +
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
            (glm::length(bmax - bmin) * 2.5f));
    ctx->Yield(2);

    const float L =
        0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
    const double r0 = fil->radius;
    ctx->MouseMoveToPos(WorldToWindow(P + out * (0.45f * L)));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(WorldToWindow(P + out * (0.45f * L + 0.3f)));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    fil = dynamic_cast<FilletFeature*>(ui::ResolvePendingFeature(*s_App));
    IM_CHECK(fil != nullptr);
    IM_CHECK(std::abs(fil->radius - (r0 + 0.3)) < 0.05);

    ui::CancelPendingFeature(*s_App);
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "rib", "menu_rib_previews_and_adds_volume");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    // Box + an open-line profile above its top (the test_rib.cpp setup).
    Doc()->AddFeature(std::make_unique<BoxFeature>("Base", glm::vec3(0), 10.0,
                                                   10.0, 10.0));
    auto sk = std::make_unique<SketchFeature>(
        "P", SketchPlane(glm::vec3(5, 5, 15), glm::vec3(0, 0, -1),
                         glm::vec3(1, 0, 0)));
    sk->AddEntity(sketch::Line{{-3, 0}, {3, 0}});
    Doc()->AddFeature(std::move(sk));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    const double v0 = Doc()->GetBodies()[0].shape.Volume();

    ctx->MenuClick("Create/Rib");
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
    ctx->MenuClick("File/New");
    ctx->Yield();
    Doc()->AddFeature(std::make_unique<CylinderFeature>(
        "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 4.0, 20.0));
    ctx->Yield();

    ctx->MenuClick("Create/Thread");
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

    auto* th =
        dynamic_cast<ThreadFeature*>(ui::ResolvePendingFeature(*s_App));
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
  t = IM_REGISTER_TEST(engine, "pattern", "circular_preview_param_edits_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    ctx->Yield();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Pattern");
    ctx->Yield();
    IM_CHECK(ui::HasPendingFeature(*s_App));

    ctx->SetRef("//Pattern###PreviewDialog");
    ctx->ComboClick("##Type/Circular");
    ctx->Yield(2);
    {
      auto* pat =
          dynamic_cast<PatternFeature*>(ui::ResolvePendingFeature(*s_App));
      IM_CHECK(pat != nullptr);
      IM_CHECK_EQ((int)pat->kind, (int)PatternKind::Circular);
    }
    ctx->ComboClick("##Axis/Z");
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
    ctx->MenuClick("File/New");
    ctx->Yield();
    Doc()->AddFeature(std::make_unique<BoxFeature>("Wide", glm::vec3(0), 10.0,
                                                   4.0, 4.0));
    ctx->Yield();
    const double v0 = Doc()->GetBodies()[0].shape.Volume();

    ctx->MenuClick("Create/Pattern");
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

  t = IM_REGISTER_TEST(engine, "view", "planes_autohide_with_geometry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::EffectivePlaneVisibility(*s_App)); // empty part → planes shown

    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK(!ui::EffectivePlaneVisibility(*s_App)); // has a body → hidden
  };

  // S1 shadows: toggling the map off/on and sectioning while it renders is
  // exactly where barrier/layout bugs crash the device — assert we survive
  // several frames of each with a body in the scene.
  t = IM_REGISTER_TEST(engine, "view", "shadow_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showShadows = false;
    ctx->Yield(3);
    s_App->showShadows = true;
    ctx->Yield(3);
    s_App->sectionPlane = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // cut on XY
    ctx->Yield(5);
    s_App->sectionPlane = glm::vec4(0.0f);
    ctx->Yield(3);
    IM_CHECK(s_App->showShadows);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  // S2 SSAO: same barrier/layout stress as the shadow test — the prepass +
  // AO + blur chain toggled off/on while sectioned, over several frames.
  t = IM_REGISTER_TEST(engine, "view", "ssao_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showAmbientOcclusion = false;
    ctx->Yield(3);
    s_App->sectionPlane = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // cut on XY
    ctx->Yield(3);
    s_App->showAmbientOcclusion = true;
    ctx->Yield(5);
    s_App->sectionPlane = glm::vec4(0.0f);
    ctx->Yield(3);
    IM_CHECK(s_App->showAmbientOcclusion);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  // S3 render-quality switches: each change makes the main loop recreate
  // GPU targets behind wait-idle and rebind descriptors — the crash surface
  // is exactly that recreation, so flip everything with frames in between.
  t = IM_REGISTER_TEST(engine, "view", "render_quality_switches_survive");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->ssaoFullRes = true; // half -> full AO targets
    ctx->Yield(3);
    s_App->shadowMapSize = 1024; // shadow map recreation
    ctx->Yield(3);
    s_App->ssaoFullRes = false; // back to defaults, both paths again
    s_App->shadowMapSize = 2048;
    ctx->Yield(3);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  t = IM_REGISTER_TEST(engine, "constraints", "coincident_merges_points");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    bool ok = ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Coincident, 0.0);
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{0, 0}, {4, 1.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    s_App->sketchEdit.selected = {{l0, 0}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App,
                                       sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& ln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f);
  };

  // Clicking a line's body (not an endpoint) selects the whole entity, so a
  // whole-line constraint like Horizontal can then be applied.
  t = IM_REGISTER_TEST(engine, "pick", "select_line_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 1}, {2, 1.6f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0)); // Execute replaced it

    // Pick the segment midpoint (far from both endpoints) → whole-line body.
    glm::vec2 midScreen =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 1.3f));
    ui::SketchPick pick = ui::PickSketchEntity(*s_App, *sk, midScreen, 12.0f);
    IM_CHECK(pick.hit);
    IM_CHECK(pick.body);
    IM_CHECK_EQ(pick.ref.entity, l0);

    // Body selection enables + applies Horizontal.
    s_App->sketchEdit.selected = {pick.ref};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Horizontal,
                                       0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& hln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(hln.a.y - hln.b.y) < 1e-3f);
  };

  // A click near a vertex prefers the endpoint over the body.
  t = IM_REGISTER_TEST(engine, "pick", "prefers_endpoint");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0)); // Execute replaced it

    glm::vec2 endScreen =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2.0f, 0.0f));
    ui::SketchPick pick = ui::PickSketchEntity(*s_App, *sk, endScreen, 12.0f);
    IM_CHECK(pick.hit);
    IM_CHECK(!pick.body); // endpoint, not body
    IM_CHECK_EQ(pick.ref.entity, l0);
    IM_CHECK_EQ((int)pick.ref.which, 1); // endpoint b
  };

  // Selection precision: a click near a line's endpoint picks the POINT (not
  // the line body, even though it is just as close); a repeated click at the
  // same spot cycles to the line body (the U1 same-spot convention).
  t = IM_REGISTER_TEST(engine, "pick", "endpoint_cycles_to_line");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    // Off the datum axes so they are not pick candidates at the endpoint.
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0.5f}, {2, 0.5f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    // Pin the camera: the zoom left by earlier tests changes what falls
    // inside the pixel pick tolerances (e.g. the X axis 0.5 UV away).
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(0.0f, 0.0f, 8.0f));
    ctx->Yield(2);
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Select;

    // Click at endpoint b: the point wins over the line body under it.
    glm::vec2 end = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 0.5f));
    ctx->MouseMoveToPos(ImVec2(end.x, end.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK_EQ(s_App->sketchEdit.selected[0].entity, l0);
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which, 1);
    IM_CHECK(s_App->sketchDrag.mode == ui::SketchDragSession::Mode::Point);

    // Same spot again: cycles to the line body.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK_EQ(s_App->sketchEdit.selected[0].entity, l0);
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which, 0);
    IM_CHECK(s_App->sketchDrag.mode == ui::SketchDragSession::Mode::LineBody);

    // Once more wraps back to the endpoint.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which, 1);
    IM_CHECK(s_App->sketchDrag.mode == ui::SketchDragSession::Mode::Point);

    ctx->KeyPress(ImGuiKey_Escape); // clear selection
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Trim tool: clicking the middle span of a crossed line removes just that
  // piece, end to end through the viewport handler.
  t = IM_REGISTER_TEST(engine, "trim", "removes_middle_span");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t h = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}}); // horizontal
    sk->AddEntity(sketch::Line{{-1, -1}, {-1, 1}});            // crosses at x=-1
    sk->AddEntity(sketch::Line{{1, -1}, {1, 1}});              // crosses at x=1
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
  t = IM_REGISTER_TEST(engine, "fillet", "fillet_corner_via_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    IM_CHECK_EQ(sk->entities.size(), (size_t)3); // 2 lines + fillet arc
    IM_CHECK_EQ(arcs, 1);
  };

  // A new sketch defaults to the Select tool; letter keys switch tools (Onshape
  // keymap: g = rectangle, m = trim).
  t = IM_REGISTER_TEST(engine, "keys", "tool_switch");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select); // default
    ctx->KeyPress(ImGuiKey_L);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Line);
    ctx->KeyPress(ImGuiKey_G);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Rectangle);
    ctx->KeyPress(ImGuiKey_M);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Trim);
  };

  // `h` applies Horizontal to the current selection (constraint keymap).
  t = IM_REGISTER_TEST(engine, "keys", "constraint_on_selection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {4, 0.5f}}); // slightly tilted
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    s_App->sketchEdit.selected = {{l, 0}};
    ctx->KeyPress(ImGuiKey_H);
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    int nh = 0;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Horizontal)
        nh++;
    IM_CHECK_EQ(nh, 1);
    const auto& ln = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l)]);
    IM_CHECK(std::abs(ln.a.y - ln.b.y) < 1e-3f); // solved flat
  };

  // Dragging a box over empty space selects the entities it encloses (window).
  t = IM_REGISTER_TEST(engine, "select", "box_selects_enclosed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    // Box corners in empty space (well off the geometry) enclosing both lines.
    glm::vec2 a = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1, -1));
    glm::vec2 b = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(2, 2));
    ctx->MouseMoveToPos(ImVec2(a.x, a.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(b.x, b.y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)2);
  };

  // Escape backs out one level per press: tool → clear selection → leave edit.
  t = IM_REGISTER_TEST(engine, "select", "escape_cascade");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    uint32_t l = sk->AddEntity(sketch::Line{{0, 0}, {1, 0}});
    s_App->sketchEdit.tool = ui::SketchTool::Circle;
    s_App->sketchEdit.selected = {{l, 0}};
    ctx->KeyPress(ImGuiKey_Escape); // 1: tool → Select
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.tool == ui::SketchTool::Select);
    IM_CHECK(!s_App->sketchEdit.selected.empty());
    ctx->KeyPress(ImGuiKey_Escape); // 2: clear selection
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.selected.empty());
    IM_CHECK(s_App->sketchEdit.IsEditing());
    ctx->KeyPress(ImGuiKey_Escape); // 3: leave sketch edit
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.IsEditing());
  };

  // Drawing a near-horizontal line auto-adds a Horizontal constraint and snaps
  // it flat.
  t = IM_REGISTER_TEST(engine, "infer", "horizontal_snaps");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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

    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, 0.5f)); // onto the point
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t target = sk->AddEntity(sketch::Point{{1.5f, 1.8f}}); // shares X below
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = true;

    // Line from (-1.5,-1) to (1.5,-1): the end shares X (1.5) with the point but
    // is well away in Y → vertical alignment, not coincident.
    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(-1.5f, -1.0f));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.5f, -1.0f));
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
  t = IM_REGISTER_TEST(engine, "prompt", "staged_tool_steps");
  t->TestFunc = [](ImGuiTestContext*) {
    ui::SketchEditState e;
    e.activeSketchId = 1; // in sketch edit

    e.tool = ui::SketchTool::Select;
    IM_CHECK(ui::SketchToolPrompt(e).text.empty()); // Select is quiet

    e.tool = ui::SketchTool::Line;
    e.hasPendingPoint = false;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 1);
    e.hasPendingPoint = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(e).step, 2);

    ui::SketchEditState m;
    m.tool = ui::SketchTool::Mirror;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 1);      // collecting geometry
    IM_CHECK_EQ(ui::SketchToolPrompt(m).stepCount, 2);
    m.mirrorAxisPhase = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(m).step, 2);      // axis-pick step

    ui::SketchEditState el;
    el.tool = ui::SketchTool::Ellipse;
    el.hasPendingPoint = true;
    el.hasPendingPoint2 = true;
    IM_CHECK_EQ(ui::SketchToolPrompt(el).step, 3);     // third click
    IM_CHECK_EQ(ui::SketchToolPrompt(el).stepCount, 3);
  };

  // Guided mirror (N2): select geometry in-tool, Enter, then click the axis.
  t = IM_REGISTER_TEST(engine, "mirror", "guided_flow");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    sk->AddEntity(sketch::Line{{0.5f, 0.6f}, {1.5f, 0.6f}}); // geometry to mirror
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
    glm::vec2 g = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->sketchEdit.selected.empty());

    // Enter advances to the axis-pick step.
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(s_App->sketchEdit.mirrorAxisPhase);

    // Step 2: click the axis line → the mirror runs and adds a copy.
    glm::vec2 ax = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 0.0f));
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    glm::vec2 g = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(1.0f, 0.6f));
    ctx->MouseMoveToPos(ImVec2(g.x, g.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    glm::vec2 ax = ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, -1.2f));
    ctx->MouseMoveToPos(ImVec2(ax.x, ax.y));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();

    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    bool symToYAxis = false;
    for (const auto& c : sk->constraints)
      if (c.kind == sketch::ConstraintKind::Symmetric && c.c.entity == sketch::kAxisY)
        symToYAxis = true;
    IM_CHECK(symToYAxis); // mirrored across the Y axis
  };

  // With auto-constrain off, no constraints are inferred.
  t = IM_REGISTER_TEST(engine, "infer", "toggle_suppresses");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    uint32_t l1 = sk->AddEntity(sketch::Line{{2, 0}, {2, 3}}); // l1.a ~ l0.b
    sk->constraints.push_back(
        {sketch::ConstraintKind::Coincident, {l0, 1}, {l1, 0}, 0.0});
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
    const auto& moved = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l0)]);
    // Both endpoints translated up ~1; the neighbour's coincident end followed.
    IM_CHECK(moved.a.y > 0.5f && moved.b.y > 0.5f);
    const auto& nbr = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(l1)]);
    IM_CHECK(glm::length(nbr.a - moved.b) < 1e-2f);
    IM_CHECK(s_App->commands->CanUndo()); // one undoable edit
  };

  // 3D fillet via edge picking (M7/T5): create a box, start a fillet, click a
  // visible B-Rep edge, accept — the body loses volume along that edge.
  t = IM_REGISTER_TEST(engine, "fillet", "fillet_body_edge_via_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
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
    ctx->MenuClick("Create/Fillet");
    ctx->Yield();
    IM_CHECK(s_App->edgePick.Active());
    auto* fil = dynamic_cast<FilletFeature*>(
        Doc()->FindFeatureById(s_App->edgePick.filletId));
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

    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->edgePick.Active()); // picking ends with the preview
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);
    IM_CHECK(Doc()->GetBodies()[0].shape.Volume() < before - 1e-6);
    IM_CHECK(s_App->commands->CanUndo()); // accept = one undoable action
  };

  // Feature tree affordances: context-menu rename (inline edit) and the
  // draggable rollback bar.
  t = IM_REGISTER_TEST(engine, "tree", "context_rename_and_rollback_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    MenuCreateAccept(ctx, "Create/Sphere");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)2);

    // Rename "Box 1" via right-click > Rename: the row becomes a focused
    // input; typing replaces the name, Enter commits (one undoable edit).
    ctx->ItemClick("//Model/Box 1##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Rename");
    ctx->KeyCharsReplaceEnter("Chassis");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->GetFeature(0)->GetName(), std::string("Chassis"));
    IM_CHECK(s_App->commands->CanUndo());

    // Drag the rollback bar (parked at the end) up above the sphere row:
    // regeneration stops at index 1 and the sphere body disappears.
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
    ctx->ItemDragWithDelta("//Model/##RollbackBar", ImVec2(0, -30));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->Rollback(), 1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)1);
    // Drag it back past the end: full history replays.
    ctx->ItemDragWithDelta("//Model/##RollbackBar", ImVec2(0, 60));
    ctx->Yield();
    IM_CHECK_EQ(Doc()->Rollback(), -1);
    IM_CHECK_EQ(Doc()->GetBodies().size(), (size_t)2);
  };

  // U1a: feature selection is a stable id, not an index — deleting an
  // earlier sibling must not move it (the old int-index selection pointed at
  // whatever feature slid into the vacated slot).
  t = IM_REGISTER_TEST(engine, "selection", "survives_sibling_delete");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    MenuCreateAccept(ctx, "Create/Sphere");
    const uint64_t sphereId = Doc()->GetFeature(1)->GetId();

    ctx->ItemClick("//Model/Sphere 2##1");
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Delete the box above it: the selection stays on the sphere.
    ctx->ItemClick("//Model/Box 1##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Delete");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Deleting the selected feature itself clears the selection (prune).
    ctx->ItemClick("//Model/Sphere 2##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Delete");
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), (uint64_t)0);
  };

  // U1b: the unified pick pipeline — face/body cycling on a repeated click,
  // edge picking outside any armed tool, Ctrl+click multi-select, Escape
  // clears.
  t = IM_REGISTER_TEST(engine, "selection", "pick_cycle_multiselect");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");

    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 center = (min + max) * 0.5f;
    const float radius = glm::length(max - min) * 0.5f;
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(center);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(
        center + glm::normalize(glm::vec3(1.0f, 0.6f, 1.0f)) * radius * 5.0f);
    ctx->Yield();

    const auto& items = s_App->selection.items;

    // Click the +X face's center pixel — face interior, no edge in range —
    // → one Face item. (The body-center pixel lines up with corner edges.)
    const glm::vec3 faceCenter(max.x, center.y, center.z);
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Face);

    // Same spot again: cycles to the face's body; once more wraps to face.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Body);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Face);

    // A visible edge midpoint (top edge at z-max): edges outrank faces.
    const glm::vec3 edgeMid((min.x + max.x) * 0.5f, max.y, max.z);
    ctx->MouseMoveToPos(WorldToWindow(edgeMid));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)1);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Edge);

    // Ctrl+click back on the face: toggles it IN alongside the edge.
    ctx->MouseMoveToPos(WorldToWindow(faceCenter));
    ctx->Yield();
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    IM_CHECK_EQ(items.size(), (size_t)2);
    IM_CHECK(items[0].kind == ui::SelectionItem::Kind::Edge);
    IM_CHECK(items[1].kind == ui::SelectionItem::Kind::Face);

    // Escape clears.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(s_App->selection.Empty());
  };

  // U3: dialog pick slots — arm the Boolean's Target/Tool slots and fill
  // them by clicking bodies in the viewport (overriding the defaults, which
  // are the two most-recent producers).
  t = IM_REGISTER_TEST(engine, "slots", "boolean_via_viewport_picks");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    MenuCreateAccept(ctx, "Create/Box");
    MenuCreateAccept(ctx, "Create/Box");
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
    ctx->MenuClick("Create/Boolean");
    ctx->Yield();
    IM_CHECK(s_App->pendingFeatureId != 0);
    const uint64_t box1Id = Doc()->GetFeature(0)->GetId();
    const uint64_t box2Id = box2->GetId();

    // Arm Target, click box 1's TOP face; arm Tool, click box 2's —
    // top faces project high on screen, clear of the bottom-center dialog.
    ctx->ItemClick("//Boolean###PreviewDialog/###slot1"); // Target
    IM_CHECK(s_App->slotPick.Active());
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(-3.7f, 2.0f, 1.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK(!s_App->slotPick.Active()); // single slot disarms on pick
    ctx->ItemClick("//Boolean###PreviewDialog/###slot2"); // Tool
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

  // U2: region picking — arm the extrude's Profiles slot and toggle one of
  // a two-region sketch's profiles by clicking inside it in the viewport.
  t = IM_REGISTER_TEST(engine, "region", "pick_one_of_two_regions");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    auto sk = std::make_unique<SketchFeature>("Profiles",
                                              SketchPlane(PlaneType::XY));
    const uint32_t c1 = sk->AddEntity(sketch::Circle{{0, 0}, 1.0});
    sk->AddEntity(sketch::Circle{{5, 0}, 1.0});
    Doc()->AddFeature(std::move(sk));
    const uint64_t skId = Doc()->GetFeature(0)->GetId();
    Doc()->AddFeature(std::make_unique<ExtrudeFeature>(
        "Pad", skId, 2.0, ExtrudeMode::NewBody));
    Doc()->Regenerate();
    const uint64_t extId = Doc()->GetFeature(1)->GetId();
    ui::SelectFeature(*s_App, extId);

    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(2.5f, 0.0f, 0.0f));
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(glm::vec3(2.5f, 0.5f, 12.0f));
    ctx->Yield();

    // Arm the Profiles slot (committed feature → Properties panel).
    ctx->ItemClick("//Properties/###slot7");
    IM_CHECK(s_App->slotPick.Active());

    // Click inside circle 1's region (the sketch-plane point, not the body).
    ctx->MouseMoveToPos(WorldToWindow(glm::vec3(0.0f, 0.0f, 0.0f)));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    auto* ext =
        dynamic_cast<ExtrudeFeature*>(Doc()->FindFeatureById(extId));
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
  t = IM_REGISTER_TEST(engine, "palette", "ctrl_k_creates_box");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_K);
    ctx->Yield();
    IM_CHECK(s_App->paletteOpen);
    ctx->KeyChars("box");
    ctx->Yield();
    ctx->KeyPress(ImGuiKey_Enter);
    ctx->Yield();
    IM_CHECK(!s_App->paletteOpen);
    IM_CHECK(s_App->pendingFeatureId != 0); // Box preview began
    ui::AcceptPendingFeature(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
  };

  // Dimension visuals: a new distance dim is born with a non-zero label
  // offset (legible placement), its pill drags to a new spot (writing the
  // serialized labelOffset), and the drag neither moves nor draws geometry
  // underneath (the dimHot gate).
  t = IM_REGISTER_TEST(engine, "dims", "label_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    for (int i = 0; i < 600 && s_App->camera->IsAnimating(); i++)
      ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    s_App->sketchEdit.tool = ui::SketchTool::Line;
    s_App->sketchEdit.autoConstrain = false;
    glm::vec2 p0 = ui::SketchUVToScreen(*s_App, sk->plane,
                                        glm::vec2(-1.5f, -0.5f));
    glm::vec2 p1 = ui::SketchUVToScreen(*s_App, sk->plane,
                                        glm::vec2(1.5f, -0.5f));
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
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Distance, 3.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0)); // replaced
    IM_CHECK_EQ(sk->constraints.size(), (size_t)1);
    const glm::vec2 seeded = sk->constraints[0].labelOffset;
    IM_CHECK_GT(glm::length(seeded), 0.1f);

    // The label pill sits at the dimension line's midpoint: mid + offset.
    const glm::vec2 labelUV = glm::vec2(0.0f, -0.5f) + seeded;
    const glm::vec2 labelScr =
        ui::SketchUVToScreen(*s_App, sk->plane, labelUV);
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    clickUV({-0.5f, 0}); // place the label
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
    const auto& la = std::get<sketch::Line>(sk->entities[sk->IndexOfEntity(lA)]);
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    IM_CHECK_LE(std::abs(glm::dot(off, d)), 0.02f); // ⟂ to the span
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Distance, 3.0));
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
    IM_CHECK_EQ(sk->entities.size(), (size_t)1); // l0 gone
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    addCon({{p0, 0}}, CK::Fixed, 0.0);                       // [0]
    addCon({{p0, 0}, {p1, 0}}, CK::DistanceX, 2.0);          // [1]
    addCon({{l0, 0}}, CK::Fixed, 0.0);                       // [2]
    addCon({{l0, 1}}, CK::Fixed, 0.0);                       // [3]
    addCon({{pp, 0}, {l0, 0}}, CK::DistancePL, 0.7);         // [4]
    addCon({{lA, 0}}, CK::Fixed, 0.0);                       // [5]
    addCon({{lA, 1}}, CK::Fixed, 0.0);                       // [6]
    addCon({{lB, 0}}, CK::Fixed, 0.0);                       // [7]
    addCon({{lA, 0}, {lB, 0}}, CK::Angle, 0.46364761);       // [8]
    addCon({{lD, 0}, {lD, 1}}, CK::Distance, 3.0);           // [9]
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
    IM_CHECK_EQ(sk->constraints[1].a.entity, p0); // NOT swapped
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
      const auto& lb = std::get<sketch::Line>(
          sk->entities[sk->IndexOfEntity(lB)]);
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
      const auto& ld = std::get<sketch::Line>(
          sk->entities[sk->IndexOfEntity(lD)]);
      IM_CHECK_LE(std::abs(glm::length(ld.b - ld.a) - 2.0f), 1e-3f);
    }

    // Dimension-on-create with a negative typed value: same rule — the
    // created radius is |v|.
    ctx->KeyPress(ImGuiKey_C);
    ctx->Yield();
    {
      glm::vec2 c = ui::SketchUVToScreen(*s_App, sk->plane,
                                         glm::vec2(0.3f, -0.4f));
      glm::vec2 r = ui::SketchUVToScreen(*s_App, sk->plane,
                                         glm::vec2(0.6f, -0.4f));
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Distance, 3.0));
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
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
    IM_CHECK(s_App->sketchEdit.hasPendingPoint); // staged state untouched
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
    ctx->KeyPress(ImGuiKey_Escape); // leave sketch edit
    ctx->Yield();
  };

  // Center rectangle (Phase 3.6): Shift+G arms it; centre + corner clicks
  // build 4 lines plus a centre point anchored by a symmetric-about-point
  // constraint — dimensioning the width grows the rectangle symmetrically.
  t = IM_REGISTER_TEST(engine, "sketch", "center_rectangle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
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
      if (c.kind == sketch::ConstraintKind::Symmetric &&
          c.c.entity == pointId)
        anchored = true;
    IM_CHECK(anchored);

    // Pin the centre, dimension the width: symmetric growth about it.
    s_App->sketchEdit.tool = ui::SketchTool::Select;
    s_App->sketchEdit.selected = {{pointId, 0}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App, sketch::ConstraintKind::Fixed,
                                       0.0));
    ctx->Yield();
    s_App->sketchEdit.selected = {{l0, 0}, {l0, 1}};
    IM_CHECK(ui::ApplySketchConstraint(*s_App,
                                       sketch::ConstraintKind::Distance, 3.0));
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
  t = IM_REGISTER_TEST(engine, "tabs", "folder_flyout");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    ui::AddPart(*s_App, "Bolt");
    s_App->parts.back().folder = "Imported";
    ui::AddPart(*s_App, "Nut");
    s_App->parts.back().folder = "Imported";
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    // One flyout button for the folder; clicking lists the parts.
    ctx->ItemClick("//##PartTabs/Imported (2)##folderImported");
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/Nut##fpart2");
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

  // Assembly measure: two occurrences of a 2mm box, 10mm apart — measuring
  // a top face on each reads the 8mm gap in world space; re-clicking one
  // deselects it (occurrence is part of the element's identity).
  t = IM_REGISTER_TEST(engine, "measure", "assembly_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box"); // 2x2x2 at the origin
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const uint64_t partSlotId = s_App->parts[0].id;

    ui::AddAssembly(*s_App, "Asm");
    ctx->Yield();
    IM_CHECK(s_App->assembly != nullptr);
    Occurrence o1;
    o1.partId = partSlotId;
    o1.fixed = true;
    const uint64_t occ1 = s_App->assembly->AddOccurrence(o1);
    Occurrence o2;
    o2.partId = partSlotId;
    o2.position = glm::vec3(10.0f, 0.0f, 0.0f);
    const uint64_t occ2 = s_App->assembly->AddOccurrence(o2);
    s_App->assembly->BumpVersion();
    ctx->Yield(3); // resync builds the occurrence meshes

    // Slightly-elevated view centered between the boxes; top faces clear.
    const glm::vec3 cA = (min + max) * 0.5f;
    const glm::vec3 mid = cA + glm::vec3(5.0f, 0.0f, 0.0f);
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(mid);
    s_App->camera->SetUp(glm::vec3(0.0f, 1.0f, 0.0f));
    s_App->camera->SetPosition(mid +
                               glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f)) *
                                   glm::length(max - min) * 6.0f);
    ctx->Yield();

    s_App->measure = ui::MeasureState{};
    s_App->measure.active = true;
    ctx->Yield();

    // Top-face centers of each occurrence (world space).
    const glm::vec3 topA(cA.x, max.y, cA.z);
    const glm::vec3 topB = topA + glm::vec3(10.0f, 0.0f, 0.0f);
    ctx->MouseMoveToPos(WorldToWindow(topA));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);
    IM_CHECK_EQ(s_App->measure.picked[0].occurrenceId, occ1);

    ctx->MouseMoveToPos(WorldToWindow(topB));
    ctx->Yield();
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)2);
    IM_CHECK_EQ(s_App->measure.picked[1].occurrenceId, occ2);
    IM_CHECK(s_App->measure.hasDistanceLine);
    const float gap =
        glm::length(s_App->measure.distB - s_App->measure.distA);
    IM_CHECK_LE(std::abs(gap - (10.0f - (max.x - min.x))), 1e-3f);

    // Same face, same occurrence → toggles OFF (identity includes the
    // occurrence, so this must not add a third element).
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
  };
}
