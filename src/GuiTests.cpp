#include "GuiTests.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
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

    // Click on the projected body center: the ray must hit the camera-facing
    // face of the box
    ctx->MouseMoveToPos(WorldToWindow(bodyCenter));
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

  t = IM_REGISTER_TEST(engine, "view", "planes_autohide_with_geometry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::EffectivePlaneVisibility(*s_App)); // empty part → planes shown

    MenuCreateAccept(ctx, "Create/Box");
    IM_CHECK(!ui::EffectivePlaneVisibility(*s_App)); // has a body → hidden
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
}
