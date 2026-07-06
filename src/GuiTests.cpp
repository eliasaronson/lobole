#include "GuiTests.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "core/Document.h"
#include "core/Feature.h"
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
    ctx->ItemInputValue("//Properties/Width", 5.0f);
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

  t = IM_REGISTER_TEST(engine, "viewport", "click_selects_facing_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    MenuCreateAccept(ctx, "Create/Box");
    ctx->Yield();

    // Click on the projected body center: the ray must hit the camera-facing
    // face of the box
    glm::vec3 min, max;
    Doc()->GetBodies()[0].shape.GetBoundingBox(min, max);
    const glm::vec3 bodyCenter = (min + max) * 0.5f;

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

  t = IM_REGISTER_TEST(engine, "sketch", "front_plane_shows_toolbar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    IM_CHECK(ctx->GetWindowByRef("//Sketch Tools") == nullptr);

    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    // A SketchFeature was added and the toolbar appeared
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK(Doc()->GetFeature(0)->TypeName() == "Sketch");
    IM_CHECK(ctx->GetWindowByRef("//Sketch Tools") != nullptr);

    ctx->SetRef("//Sketch Tools");
    ctx->ItemClick("Exit Sketch");
    ctx->Yield(2);
    // The window object lingers in ImGui's list; check it is no longer shown
    ImGuiWindow* toolbar = ctx->GetWindowByRef("//Sketch Tools");
    IM_CHECK(toolbar == nullptr || !toolbar->WasActive);
  };

  t = IM_REGISTER_TEST(engine, "sketch", "draw_line_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Sketch/Sketch on Top Plane");
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);

    // Line tool is the default; two viewport clicks place a line
    ImVec2 size = ImGui::GetMainViewport()->Size;
    ctx->MouseMoveToPos(ImVec2(size.x * 0.45f, size.y * 0.45f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(size.x * 0.55f, size.y * 0.55f));
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
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
    sketch->AddEntity(sketch::Rectangle{{0, 0}, {4, 2}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    // Extrude from the toolbar → preview, then accept
    ctx->SetRef("//Sketch Tools");
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
}
