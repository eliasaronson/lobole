#include "GuiTests.h"

#include "core/Document.h"
#include "core/Feature.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// Tests run inside the live application; the getter resolves the current
// document each time since File > New / Open swap the object out.
static std::function<Document*()> s_GetDocument;

void RegisterGuiTests(ImGuiTestEngine* engine,
                      std::function<Document*()> getDocument) {
  s_GetDocument = std::move(getDocument);

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
    Document* doc = s_GetDocument();
    IM_CHECK(doc != nullptr);
    const size_t before = doc->FeatureCount();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Box");
    ctx->Yield();
    IM_CHECK_EQ(doc->FeatureCount(), before + 1);
    IM_CHECK_EQ(doc->GetBodies().size(), before + 1);

    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(doc->FeatureCount(), before);
    IM_CHECK_EQ(doc->GetBodies().size(), before);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    IM_CHECK_EQ(doc->FeatureCount(), before + 1);
    IM_CHECK_EQ(doc->GetBodies().size(), before + 1);
  };

  t = IM_REGISTER_TEST(engine, "document", "create_sphere_and_cylinder");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    Document* doc = s_GetDocument();
    IM_CHECK(doc != nullptr);
    const size_t before = doc->FeatureCount();

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Sphere");
    ctx->MenuClick("Create/Cylinder");
    ctx->Yield();
    IM_CHECK_EQ(doc->FeatureCount(), before + 2);
    IM_CHECK_EQ(doc->GetBodies().size(), before + 2);

    ctx->MenuClick("Edit/Undo");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    IM_CHECK_EQ(doc->FeatureCount(), before);
  };

  t = IM_REGISTER_TEST(engine, "document", "new_project_clears_features");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    IM_CHECK(s_GetDocument() != nullptr);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Create/Box");
    ctx->Yield();
    IM_CHECK(s_GetDocument()->FeatureCount() > 0);

    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK_EQ(s_GetDocument()->FeatureCount(), (size_t)0);
    IM_CHECK_EQ(s_GetDocument()->GetBodies().size(), (size_t)0);
  };

  t = IM_REGISTER_TEST(engine, "properties", "edit_box_width_undo");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Box");
    ctx->Yield();

    Document* doc = s_GetDocument();
    IM_CHECK(doc != nullptr);
    auto* box = dynamic_cast<BoxFeature*>(doc->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0);

    // Select the feature in the Model panel, edit its width in Properties
    ctx->ItemClick("//Model/Box 1##0");
    ctx->ItemInputValue("//Properties/Width", 5.0f);
    ctx->Yield();

    box = dynamic_cast<BoxFeature*>(s_GetDocument()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 5.0);
    IM_CHECK_EQ(s_GetDocument()->GetBodies().size(), (size_t)1);

    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("Edit/Undo");
    ctx->Yield();
    box = dynamic_cast<BoxFeature*>(s_GetDocument()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 2.0);

    ctx->MenuClick("Edit/Redo");
    ctx->Yield();
    box = dynamic_cast<BoxFeature*>(s_GetDocument()->GetFeature(0));
    IM_CHECK(box != nullptr);
    IM_CHECK_EQ(box->width, 5.0);
  };

  t = IM_REGISTER_TEST(engine, "shortcuts", "undo_redo_keyboard");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->MenuClick("Create/Box");
    ctx->Yield();
    IM_CHECK_EQ(s_GetDocument()->FeatureCount(), (size_t)1);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield();
    IM_CHECK_EQ(s_GetDocument()->FeatureCount(), (size_t)0);

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
    ctx->Yield();
    IM_CHECK_EQ(s_GetDocument()->FeatureCount(), (size_t)1);
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
}
