#include "GuiTests.h"

#include "Scene.h"
#include "core/Document.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// Tests run inside the live application, so they reach application state
// through these pointers rather than through main.cpp's TU-local globals.
static Scene* s_Scene = nullptr;
static Document* s_Document = nullptr;

void RegisterGuiTests(ImGuiTestEngine* engine, Scene* scene,
                      Document* document) {
  s_Scene = scene;
  s_Document = document;

  ImGuiTest* t = nullptr;

  t = IM_REGISTER_TEST(engine, "app", "boots_and_renders");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->Yield(3);
    IM_CHECK(ctx->UiContext->WithinFrameScope == false ||
             ctx->UiContext->FrameCount > 0);
  };

  t = IM_REGISTER_TEST(engine, "app", "core_windows_exist");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(ctx->GetWindowByRef("//Scene") != nullptr);
    IM_CHECK(ctx->GetWindowByRef("//Properties") != nullptr);
  };

  t = IM_REGISTER_TEST(engine, "menu", "sketch_mode_shows_toolbar");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(ctx->GetWindowByRef("//Sketch Tools") == nullptr);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Sketch/Sketch on Front Plane");
    ctx->Yield();
    IM_CHECK(ctx->GetWindowByRef("//Sketch Tools") != nullptr);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Sketch/Exit Sketch");
    ctx->Yield();
  };

  t = IM_REGISTER_TEST(engine, "document", "parametric_box_undo_redo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(s_Document != nullptr);
    const size_t before = s_Document->FeatureCount();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Box");
    ctx->Yield();
    IM_CHECK_EQ(s_Document->FeatureCount(), before + 1);
    IM_CHECK_EQ(s_Document->GetBodies().size(), before + 1);

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(s_Document->FeatureCount(), before);
    IM_CHECK_EQ(s_Document->GetBodies().size(), before);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    IM_CHECK_EQ(s_Document->FeatureCount(), before + 1);
    IM_CHECK_EQ(s_Document->GetBodies().size(), before + 1);
  };

  t = IM_REGISTER_TEST(engine, "menu", "create_box_adds_scene_object");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(s_Scene != nullptr);
    const size_t before = s_Scene->GetObjects().size();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Primitives (Legacy)/Box");
    ctx->Yield();

    IM_CHECK_EQ(s_Scene->GetObjects().size(), before + 1);
  };

  t = IM_REGISTER_TEST(engine, "menu", "create_two_boxes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(s_Scene != nullptr);
    const size_t before = s_Scene->GetObjects().size();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Primitives (Legacy)/Box");
    ctx->MenuClick("Create/Primitives (Legacy)/Sphere");
    ctx->Yield();

    IM_CHECK_EQ(s_Scene->GetObjects().size(), before + 2);
  };
}
