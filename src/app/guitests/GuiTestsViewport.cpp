#include "app/guitests/GuiTestsCommon.h"
#include "ui/AssemblyActions.h"
#include "ui/MeasureOverlay.h"
#include "ui/SimulationActions.h"

// Viewport GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterViewportGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "viewport", "middle_drag_rotates_camera");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");

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

  // Right-drag navigates like the middle button (no-middle-button / trackpad
  // path): plain orbits, +Ctrl pans. Left stays selection.
  t = IM_REGISTER_TEST(engine, "viewport", "right_drag_orbits");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    s_App->camera->StopAnimation();
    ctx->Yield();
    const glm::vec3 posBefore = s_App->camera->GetPosition();
    const float distBefore = s_App->camera->GetDistance();

    ImVec2 size = ImGui::GetMainViewport()->Size;
    ImVec2 center(size.x * 0.5f, size.y * 0.5f);
    ctx->MouseMoveToPos(center);
    ctx->MouseDown(ImGuiMouseButton_Right);
    ctx->MouseMoveToPos(ImVec2(center.x + 80.0f, center.y + 50.0f));
    ctx->MouseUp(ImGuiMouseButton_Right);
    ctx->Yield();

    // Orbit moved the eye but kept the distance (not a zoom), and the drag
    // released cleanly on the right button.
    IM_CHECK(glm::length(s_App->camera->GetPosition() - posBefore) > 0.1f);
    IM_CHECK(std::abs(s_App->camera->GetDistance() - distBefore) <
             0.05f * distBefore);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None);

    // +Ctrl mid-scheme = pan: moves the orbit target, keeps distance.
    const glm::vec3 targetBefore = s_App->camera->GetTarget();
    ctx->MouseMoveToPos(center);
    ctx->KeyDown(ImGuiMod_Ctrl);
    ctx->MouseDown(ImGuiMouseButton_Right);
    ctx->MouseMoveToPos(ImVec2(center.x + 60.0f, center.y));
    ctx->MouseUp(ImGuiMouseButton_Right);
    ctx->KeyUp(ImGuiMod_Ctrl);
    ctx->Yield();
    IM_CHECK(glm::length(s_App->camera->GetTarget() - targetBefore) > 0.01f);
    IM_CHECK(s_App->viewportDrag == ui::ViewportDrag::None);
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
    ToolbarCreateAccept(ctx, "###tb_box");

    s_App->measure.active = true;
    ui::EnterSimulation(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    ui::ExitSimulation(*s_App);
    ctx->Yield();

    s_App->measure.active = true;
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
    s_App->sketchEdit = ui::SketchEditState{}; // leave sketch edit
    // Entering the sketch aligned the camera with an animation; don't leak it
    // into the next test's camera framing (it overrides setters until done).
    s_App->camera->StopAnimation();
    ctx->Yield();
  };

  // The report this workstream started from: two edges meeting at a corner
  // measured "Distance: 0.000 mm (nearest) / dx 0 dy 0 dz 0". True, and
  // useless — they touch, and the question was the 90 degrees between them.
  // The readout computes both and leads with whichever is not degenerate.
  t = IM_REGISTER_TEST(engine, "measure", "touching_leads_with_the_angle");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    const uint64_t id = Doc()->GetFeature(0)->GetId();

    // Two ADJACENT faces of the box: they share an edge, so the minimum
    // distance between them is exactly zero and the angle is the answer.
    const Body& body = Doc()->GetBodies()[0];
    std::string px, py, nx;
    for (const auto& [idx, name] : body.elementMap.names) {
      const auto f = Measure::DescribeFaceAt(body, idx);
      if (!f)
        continue;
      if (f->normal.x > 0.99f)
        px = name;
      else if (f->normal.x < -0.99f)
        nx = name;
      else if (f->normal.y > 0.99f)
        py = name;
    }
    IM_CHECK(!px.empty() && !py.empty() && !nx.empty());

    s_App->measure = ui::MeasureState{};
    s_App->measure.active = true;
    s_App->measure.picked.push_back({ElementRef{id, px}, false, 0});
    s_App->measure.picked.push_back({ElementRef{id, py}, false, 0});
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    const std::string touching = s_App->measure.readout;
    // The angle comes FIRST, and the zero is labelled as contact rather than
    // presented as a measurement.
    IM_CHECK(touching.rfind("Angle:", 0) == 0);
    IM_CHECK(touching.find("90") != std::string::npos);
    IM_CHECK(touching.find("perpendicular") != std::string::npos);
    IM_CHECK(touching.find("touching") != std::string::npos);
    IM_CHECK(touching.find("(nearest)") == std::string::npos);
    // Three zero deltas are a line that says nothing; it is suppressed.
    IM_CHECK(touching.find("dx ") == std::string::npos);

    // Two OPPOSITE faces: 2 mm apart and parallel, so the distance leads and
    // is labelled parallel — for parallel elements the minimum separation IS
    // the separation, everywhere, not "the nearest of many".
    s_App->measure.picked[1] = {ElementRef{id, nx}, false, 0};
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    const std::string apart = s_App->measure.readout;
    IM_CHECK(apart.rfind("Distance:", 0) == 0);
    IM_CHECK(apart.find("(parallel)") != std::string::npos);
    IM_CHECK(apart.find("dx ") != std::string::npos); // the breakdown is back
    IM_CHECK(apart.find("Angle:") != std::string::npos); // still reported
    IM_CHECK(apart.find("180") != std::string::npos);    // outward normals

    s_App->measure = ui::MeasureState{};
  };

  // M4: a measurement you can keep, a distance that says WHICH distance it
  // is, and a third pick that adds up instead of being refused.
  t = IM_REGISTER_TEST(engine, "measure", "pin_qualifiers_and_totals");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    const uint64_t id = Doc()->GetFeature(0)->GetId();
    const Body& body = Doc()->GetBodies()[0];
    std::string px, nx;
    for (const auto& [idx, name] : body.elementMap.names) {
      const auto f = Measure::DescribeFaceAt(body, idx);
      if (!f)
        continue;
      if (f->normal.x > 0.99f)
        px = name;
      else if (f->normal.x < -0.99f)
        nx = name;
    }
    IM_CHECK(!px.empty() && !nx.empty());

    s_App->measure = ui::MeasureState{};
    s_App->measure.active = true;
    s_App->measure.picked.push_back({ElementRef{id, px}, false, 0});
    s_App->measure.picked.push_back({ElementRef{id, nx}, false, 0});
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    // Opposite faces of a 2 mm box: 2 apart at the closest, and the readout
    // names which distance that is.
    IM_CHECK(s_App->measure.readout.find("(parallel)") != std::string::npos);

    // Maximum is a different number and says so. This is the clearance
    // question, and a minimum-only tool answers it plausibly and wrongly.
    s_App->measure.distanceKind = Measure::DistanceKind::Maximum;
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    IM_CHECK(s_App->measure.readout.find("(maximum)") != std::string::npos);
    const float far =
        glm::distance(s_App->measure.dimension.a, s_App->measure.dimension.b);
    s_App->measure.distanceKind = Measure::DistanceKind::Centres;
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    IM_CHECK(s_App->measure.readout.find("(centre to centre)") !=
             std::string::npos);
    const float mid =
        glm::distance(s_App->measure.dimension.a, s_App->measure.dimension.b);
    IM_CHECK(far > mid); // corner-to-corner beats centre-to-centre

    // Pin it: the measurement is kept, the picks are cleared for the next
    // one, and the tool stays armed.
    ui::PinMeasurement(*s_App);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.pinned.size(), (size_t)1);
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)0);
    IM_CHECK(s_App->measure.active);
    // A pin keeps the qualifier it was TAKEN with, not whatever the live
    // control says later.
    IM_CHECK(s_App->measure.pinned[0].kind == Measure::DistanceKind::Centres);
    s_App->measure.distanceKind = Measure::DistanceKind::Minimum;

    // ...and it draws.
    std::vector<PrimVertex> prims;
    ui::AppendMeasureOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());

    // Three picks TOTAL instead of refusing. "3 elements selected - keep two
    // for a distance" was a refusal with no reason and no alternative; weld
    // length and coating area are real questions a pair cannot answer.
    std::vector<std::string> edges;
    for (const auto& [idx, name] : body.elementMap.edgeNames) {
      (void)idx;
      edges.push_back(name);
      if (edges.size() == 3)
        break;
    }
    IM_CHECK_EQ(edges.size(), (size_t)3);
    for (const std::string& n : edges)
      s_App->measure.picked.push_back({ElementRef{id, n}, true, 0});
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    IM_CHECK(s_App->measure.readout.find("Total length of 3 edges") !=
             std::string::npos);

    // A face mixed in refuses, and says why an area plus a length is not a
    // quantity rather than printing one of them.
    s_App->measure.picked.push_back({ElementRef{id, px}, false, 0});
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    IM_CHECK(s_App->measure.readout.find("do not add up") != std::string::npos);

    // Deleting the measured geometry drops the pin rather than leaving a
    // stale dimension over a model that no longer has that face.
    s_App->measure.picked.clear();
    s_App->commands->Execute(std::make_unique<RemoveFeatureCommand>(0), *Doc());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->measure.pinned.size(), (size_t)0);

    s_App->measure = ui::MeasureState{};
    ctx->Yield();
  };

  // M3: the element under the cursor is measured live, against what is
  // already picked, and drawn dimmed — so you can sweep a face and read the
  // gap without committing, and see what a click is about to give you.
  //
  // This is GUI-only by nature: it exists under a hovering cursor, and an
  // RPC cannot leave one there (the test engine hands the mouse back to the
  // real pointer the moment its action ends). So the test IS the
  // verification, not a stand-in for one.
  t = IM_REGISTER_TEST(engine, "measure", "hover_previews_before_the_click");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
    glm::vec3 mn, mx;
    Doc()->GetBodies()[0].shape.GetBoundingBox(mn, mx);
    const glm::vec3 c = (mn + mx) * 0.5f;

    // Look down at the top face from a slight angle so it is clear to hit.
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(c);
    s_App->camera->SetUp(glm::vec3(0, 1, 0));
    s_App->camera->SetPosition(c + glm::normalize(glm::vec3(0.4f, 1.0f, 0.5f)) *
                                       glm::length(mx - mn) * 5.0f);
    ctx->Yield();

    s_App->measure = ui::MeasureState{};
    s_App->measure.active = true;
    ctx->Yield();

    // Nothing picked: hovering reads out that element's OWN measure. The
    // cheapest possible "what is this", and it costs no click.
    const glm::vec3 top(c.x, mx.y, c.z);
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->Yield(2);
    IM_CHECK(s_App->measure.hover.has);
    IM_CHECK(!s_App->measure.hover.readout.empty());
    IM_CHECK(s_App->measure.hover.readout.find("area") != std::string::npos);
    IM_CHECK(s_App->measure.picked.empty()); // hovering commits nothing

    // The cached key means a stationary cursor recomputes nothing. Clear the
    // answer by hand and yield: an unchanged key must leave it cleared.
    s_App->measure.hover.readout = "SENTINEL";
    ctx->Yield(2);
    IM_CHECK_STR_EQ(s_App->measure.hover.readout.c_str(), "SENTINEL");

    // Commit that one, then hover a DIFFERENT face: the preview becomes the
    // pair measurement, with a drawable dimension, still without a click.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);

    const glm::vec3 side(mx.x, c.y, c.z);
    ctx->MouseMoveToPos(WorldToWindow(side));
    ctx->Yield(2);
    IM_CHECK(s_App->measure.hover.has);
    IM_CHECK(!s_App->measure.hover.readout.empty());
    // Two adjacent faces of a box: touching, so the preview leads with the
    // angle exactly as the committed readout would.
    IM_CHECK(s_App->measure.hover.readout.rfind("Angle:", 0) == 0);
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1); // still uncommitted

    // The preview is DRAWN, and it is drawn as well as the committed
    // measurement rather than instead of it.
    std::vector<PrimVertex> prims;
    ui::AppendMeasureOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());

    // Hovering something already picked previews nothing: measuring an
    // element against itself is a zero nobody asked for.
    ctx->MouseMoveToPos(WorldToWindow(top));
    ctx->Yield(2);
    IM_CHECK(s_App->measure.hover.readout.empty());

    s_App->measure = ui::MeasureState{};
    ctx->Yield();
  };

  // ...and so does a tab switch. A measured element is an ElementRef into the
  // document that was on screen; carried to another tab it resolves against
  // nothing, while the readout keeps displaying the old distance over
  // geometry that was never measured. ResetActiveTabUiState cleared the
  // selection, slot picks, sketch edit and simulation mode, and missed this.
  t = IM_REGISTER_TEST(engine, "measure", "cleared_on_tab_switch");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    const uint64_t boxId = Doc()->GetFeature(0)->GetId();

    s_App->measure.active = true;
    s_App->measure.picked.push_back({ElementRef{boxId, "1/face0"}, false, 0});
    s_App->measure.picked.push_back({ElementRef{boxId, "1/face1"}, false, 0});
    ui::UpdateMeasureReadout(*s_App);
    ctx->Yield();
    IM_CHECK(!s_App->measure.readout.empty()); // it measured something

    ui::AddPart(*s_App, "Part 2");
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)0);
    IM_CHECK(s_App->measure.readout.empty());
    IM_CHECK(!s_App->measure.hasDistanceLine);
  };

  // Simulation is the one workspace that emits numbers a person would act on,
  // and its setup defaults were world literals: gravity along -Z (sideways,
  // since world up is +Y), a force direction chosen before the face was
  // picked, and a magnitude of 0 N that places a load doing nothing.

#ifdef HAVE_CAM
  t = IM_REGISTER_TEST(engine, "view", "standard_views_keep_the_pivot");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Modelled well away from the origin, which is where this went wrong.
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Plate", glm::vec3(200, 100, 50), 60, 40, 12));
    ctx->Yield(2);
    ui::FitCameraToDocument(*s_App);
    ctx->Yield(4);
    const glm::vec3 fitted = s_App->camera->GetTarget();
    IM_CHECK(glm::length(fitted) > 100.0f); // it really is off the origin

    ctx->MenuClick("View/Standard Views/Front");
    ctx->Yield(180); // AlignToFace animates
    ctx->LogInfo("target after Front: %.1f %.1f %.1f",
                 s_App->camera->GetTarget().x, s_App->camera->GetTarget().y,
                 s_App->camera->GetTarget().z);
    IM_CHECK(glm::length(s_App->camera->GetTarget() - fitted) < 1.0f);
    // …and it did turn: looking along -Z, the app's "Front".
    const glm::vec3 fwd = glm::normalize(s_App->camera->GetForward());
    IM_CHECK(std::abs(fwd.z + 1.0f) < 0.02f);
  };

  // Fitting the view with a tool window open puts the model in the band the
  // window LEAVES. The pan was the wrong way round — it pushed the model
  // under the very window the branch exists to clear — and nothing noticed,
  // because "the model moved sideways" looks the same either way until you
  // ask which side (docs/cam/dogfood-round.md).
#endif
  t = IM_REGISTER_TEST(engine, "viewport", "click_selects_facing_face");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
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
    s_App->camera->SetPosition(bodyCenter +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   radius * 5.0f);
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

  t = IM_REGISTER_TEST(engine, "section",
                       "assembly_cut_is_capped_and_centered");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // Modeled a metre from the origin: a world-origin plane misses it.
    Doc()->AddFeature(std::make_unique<BoxFeature>(
        "Block", glm::vec3(1000, 0, 0), 40, 20, 20));
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    for (int i = 0; i < 2; i++) {
      Occurrence occ;
      occ.name = i ? "B2" : "B1";
      occ.partId = s_App->parts[0].id;
      occ.fixed = true;
      occ.position = glm::vec3(0.0f, i * 100.0f, 0.0f);
      s_App->assembly->AddOccurrence(occ);
    }
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)0);

    // The toolbar button opens the tool AND seeds the first cut.
    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK(s_App->sectionToolOpen);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);

    // Derived, not zero: the plane sits inside the model's own span.
    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 p0 = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::vec3(p0);
    IM_CHECK(p0.w > glm::dot(bmin, n) && p0.w < glm::dot(bmax, n));

    // One cap per cut body per occurrence — the whole point.
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)2);

    // Reverse keeps the other side: same surface, opposite half-space.
    const uint64_t firstId = s_App->sections[0].id;
    ctx->SetRef("//Section###ToolWindow");
    ctx->ItemClick("**/###reverse");
    ctx->Yield(3);
    IM_CHECK(s_App->sections[0].reverse);
    const glm::vec4 p1 = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    IM_CHECK(glm::dot(glm::vec3(p1), n) < 0.0f);            // flipped normal
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)2); // still capped

    // A second cut, which must intersect the first (and trim its caps).
    ctx->ItemClick("###add");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)2);
    IM_CHECK(s_App->sections[1].id != firstId);
    IM_CHECK_GT(s_App->sectionCapMeshes.size(), (size_t)0);

    // Deleting a row takes its cut with it.
    ctx->ItemClick("###clear");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)0);
    IM_CHECK_EQ(s_App->sectionCapMeshes.size(), (size_t)0);
  };

  // The drag arrow, driven THROUGH THE VIEWPORT — a real mouse press at the
  // arrow's screen position, not a direct call into the handler.
  //
  // The first version of this test called ui::HandlePreviewHandleInput
  // itself and passed while the feature was broken for every user:
  // ViewportInput only routed clicks to the handle layer when a pending
  // FEATURE existed, so a section drag fell through to the picker and
  // selected whatever part sat behind the arrow. Testing the handler is not
  // testing the path.
  // P2: a cut can follow a datum plane and be tilted off it, and a source
  // that stops resolving stops the CUT rather than moving it somewhere else.
  t = IM_REGISTER_TEST(engine, "section", "datum_source_and_tilt");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
    auto datum = std::make_unique<DatumPlaneFeature>("D");
    datum->basePlane = PlaneType::XY;
    datum->offset = 6.0;
    Doc()->AddFeature(std::move(datum));
    ctx->Yield(2);
    // AFTER insertion: the document owns id assignment, so an id read from
    // the feature before AddFeature is not the one it ends up with.
    const uint64_t datumId = Doc()->GetFeature(1)->GetId();
    IM_CHECK(datumId != 0);

    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    ui::AppState::SectionView& sec = s_App->sections[0];

    // Point it at the datum: the cut plane lands ON the datum, not on the
    // world origin.
    sec.planeId = datumId;
    sec.offset = 0.0f;
    ctx->Yield();
    glm::vec4 p = ui::SectionPlaneOf(*s_App, sec);
    IM_CHECK(std::abs(p.w - 6.0f) < 1e-3f);
    IM_CHECK(std::abs(p.z - 1.0f) < 1e-3f);
    IM_CHECK(ui::SectionSourceResolved(*s_App, sec));

    // Tilt 30 degrees: the normal turns, and the plane still passes through
    // the point it was pivoted about (dot(anchor, n) == d). A tilt that also
    // slid the cut would break the second check.
    sec.tiltU = 30.0f;
    ctx->Yield();
    p = ui::SectionPlaneOf(*s_App, sec);
    IM_CHECK(std::abs(p.z - 1.0f) > 1e-3f); // no longer straight up
    const glm::vec3 anchor(0.0f, 0.0f, 6.0f);
    IM_CHECK(std::abs(glm::dot(anchor, glm::vec3(p)) - p.w) < 1e-3f);

    // Delete the datum: the reference dangles, so the cut stops and says so
    // rather than quietly cutting along Z=0.
    Doc()->RemoveFeature(1);
    ctx->Yield(2);
    IM_CHECK(!ui::SectionSourceResolved(*s_App, s_App->sections[0]));
    const ClipPlaneArray planes = ui::SectionPlanes(*s_App);
    IM_CHECK(planes[0].w == 0.0f && planes[0].x == 0.0f &&
             planes[0].y == 0.0f && planes[0].z == 0.0f);
    ui::ClearSections(*s_App);
    s_App->sectionToolOpen = false;
  };

  t = IM_REGISTER_TEST(engine, "section", "offset_arrow_drags_the_cut");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    const float before = s_App->sections[0].offset;
    s_App->selection.Clear();

    // Where the arrow is on screen: anchor + half its length along the normal.
    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 plane = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::normalize(glm::vec3(plane));
    const glm::vec3 c = (bmin + bmax) * 0.5f;
    const glm::vec3 anchor = c - n * (glm::dot(c, n) - plane.w);
    const float L = 0.9f * ui::GizmoScale(*s_App);
    const glm::vec2 grab = ui::WorldToScreen(*s_App, anchor + n * (0.5f * L));
    const glm::vec2 to = ui::WorldToScreen(*s_App, anchor + n * (2.0f * L));

    ctx->MouseMoveToPos(ImVec2(grab.x, grab.y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active); // the arrow took the click
    IM_CHECK(s_App->handleDrag.sectionId == s_App->sections[0].id);
    IM_CHECK(s_App->handleDrag.featureId == 0); // no feature involved
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);

    IM_CHECK(std::abs(s_App->sections[0].offset - before) > 1e-3f);
    IM_CHECK(!s_App->handleDrag.active);
    // ...and the click went to the ARROW, not through it to the geometry.
    IM_CHECK(s_App->selection.Empty());
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the document is untouched
  };

  // The same drag on an ASSEMBLY tab. It highlighted on hover and then did
  // nothing: the section arrow is the only direct-manipulation handle an
  // assembly has, and the block that routes clicks to it lived only in the
  // part path — so the click fell through to the picker.
  t = IM_REGISTER_TEST(engine, "section", "offset_arrow_drags_on_an_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::AddAssembly(*s_App, "Rig");
    ctx->Yield();
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly != nullptr); // we really are on an assembly tab

    ToolbarClick(ctx, "###tb_section");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->sections.size(), (size_t)1);
    const float before = s_App->sections[0].offset;
    s_App->selection.Clear();
    ui::SelectOccurrence(*s_App, 0); // no gizmo to compete for the click
    ctx->Yield();

    glm::vec3 bmin, bmax;
    IM_CHECK(ui::VisibleSceneBounds(*s_App, bmin, bmax));
    const glm::vec4 plane = ui::SectionPlaneOf(*s_App, s_App->sections[0]);
    const glm::vec3 n = glm::normalize(glm::vec3(plane));
    const glm::vec3 c = (bmin + bmax) * 0.5f;
    const glm::vec3 anchor = c - n * (glm::dot(c, n) - plane.w);
    const float L = 0.9f * ui::GizmoScale(*s_App);
    const glm::vec2 grab = ui::WorldToScreen(*s_App, anchor + n * (0.5f * L));
    const glm::vec2 to = ui::WorldToScreen(*s_App, anchor + n * (2.0f * L));

    // Hover first: the highlight and the grab must agree about what is there.
    ctx->MouseMoveToPos(ImVec2(grab.x, grab.y));
    ctx->Yield(2);
    IM_CHECK_EQ(ui::OwnerOf(s_App->hoveredHandle), ui::HandleOwner::Section);

    // A rebuild removes and re-adds every mesh (and recomputes an OCC
    // half-space boolean per body), so mesh ids moving mid-drag IS the lag:
    // on an assembly that ran for every part, every frame.
    IM_CHECK(!s_App->documentMeshes.empty());
    const uint32_t meshIdBefore = s_App->documentMeshes[0].meshId;

    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(to.x, to.y));
    ctx->Yield();
    IM_CHECK(s_App->handleDrag.active);
    IM_CHECK_EQ(s_App->handleDrag.sectionId, s_App->sections[0].id);
    // Mid-drag: the cut previews through the shader clip, and NOTHING was
    // rebuilt. The caps settle on release, exactly as they already did for
    // the tool window's offset slider.
    IM_CHECK_EQ(s_App->documentMeshes[0].meshId, meshIdBefore);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(3);
    IM_CHECK(std::abs(s_App->sections[0].offset - before) > 1e-3f);
    IM_CHECK(s_App->selection.Empty()); // the arrow took it, not the part
    // ...and on release the caps DO follow: the rebuild happened.
    IM_CHECK(s_App->documentMeshes[0].meshId != meshIdBefore);
  };

  // Inserting a SUB-ASSEMBLY through the UI. Until this landed the only way
  // to nest an assembly was the RPC — the insert popup filtered to
  // DocKind::Part and silently skipped everything else.

  // The bill of materials and exploded views (A4) shipped with no clicked
  // coverage at all, and a 2026-08-23 round found the BOM table CLIPPING its
  // own numbers at the width the dock ships at: a 385 kg dock totalled
  // "385169.06" grams in a 40 px column and rendered as "385", with every
  // unit mass out by a factor of ten to a thousand and nothing to say so.

  // A part placed three times — twice inside a sub-assembly, once directly —
  // so the BOM's flattening is gated too (A5 arrived after the BOM did).
  t = IM_REGISTER_TEST(engine, "handles", "extrude_arrow_drag_sets_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sketch = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sketch != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
    sketch->AddRectangle({0, 0}, {4, 2});
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();

    ToolbarClick(ctx, "###tb_sk_extrude");
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
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   12.0f);
    ctx->Yield(2);

    // Grab the arrow mid-shaft and pull one world unit further out.
    const double d0 = ext->distance;
    const float L = 0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
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
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + 30.0f, s_App->viewportPos.y + 30.0f));
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
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();

    // Preview a fillet on a known edge (the picking flow is covered by the
    // fillet tests; this one is about the handle).
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.edgeNames.empty());
    const ElementRef ref{body.createdBy,
                         body.elementMap.edgeNames.begin()->second};
    ui::BeginPreviewFeature(*s_App,
                            std::make_unique<FilletFeature>(
                                "Fillet", std::vector<ElementRef>{ref}, 0.4));
    ctx->Yield(2);
    auto* fil = dynamic_cast<FilletFeature*>(ui::ResolvePendingFeature(*s_App));
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
    s_App->camera->SetPosition(bodyCenter + glm::vec3(0.0f, -2.0f, 0.0f) +
                               glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)) *
                                   (glm::length(bmax - bmin) * 2.5f));
    ctx->Yield(2);

    const float L = 0.9f * std::max(0.13f * s_App->camera->GetDistance(), 0.1f);
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

  t = IM_REGISTER_TEST(engine, "view", "planes_autohide_with_geometry");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    IM_CHECK(ui::EffectivePlaneVisibility(*s_App)); // empty part → planes shown

    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK(!ui::EffectivePlaneVisibility(*s_App)); // has a body → hidden
  };

  // S1 shadows: toggling the map off/on and sectioning while it renders is
  // exactly where barrier/layout bugs crash the device — assert we survive
  // several frames of each with a body in the scene.
  t = IM_REGISTER_TEST(engine, "view", "shadow_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showShadows = false;
    ctx->Yield(3);
    s_App->showShadows = true;
    ctx->Yield(3);
    ui::AddSection(*s_App, 0); // cut on XY
    ctx->Yield(5);
    ui::ClearSections(*s_App);
    ctx->Yield(3);
    IM_CHECK(s_App->showShadows);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // app + body survived
  };

  // S2 SSAO: same barrier/layout stress as the shadow test — the prepass +
  // AO + blur chain toggled off/on while sectioned, over several frames.
  t = IM_REGISTER_TEST(engine, "view", "ssao_toggle_survives_section");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    s_App->showAmbientOcclusion = false;
    ctx->Yield(3);
    ui::AddSection(*s_App, 0); // cut on XY
    ctx->Yield(3);
    s_App->showAmbientOcclusion = true;
    ctx->Yield(5);
    ui::ClearSections(*s_App);
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
    ToolbarCreateAccept(ctx, "###tb_box");
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

  t = IM_REGISTER_TEST(engine, "pick", "select_line_body");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 1}, {2, 1.6f}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(0)); // Execute replaced it

    // Pick the segment midpoint (far from both endpoints) → whole-line body.
    glm::vec2 midScreen =
        ui::SketchUVToScreen(*s_App, sk->plane, glm::vec2(0.0f, 1.3f));
    ui::SketchPick pick = ui::PickSketchEntity(*s_App, *sk, midScreen, 12.0f);
    IM_CHECK(pick.hit);
    IM_CHECK(pick.body);
    IM_CHECK_EQ(pick.ref.entity, l0);

    // Body selection enables + applies Horizontal.
    s_App->sketchEdit.selected = {pick.ref};
    IM_CHECK(ui::ApplySketchConstraint(
        *s_App, sketch::ConstraintKind::Horizontal, 0.0));
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    const auto& hln = std::get<sketch::Line>(sk->entities[0]);
    IM_CHECK(std::abs(hln.a.y - hln.b.y) < 1e-3f);
  };

  // A click near a vertex prefers the endpoint over the body.
  t = IM_REGISTER_TEST(engine, "pick", "prefers_endpoint");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);

    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0}, {2, 0}});
    nlohmann::json after = DocumentIO::FeatureToJson(*sk);
    s_App->commands->Execute(
        std::make_unique<ModifyFeatureCommand>(0, before, after), *Doc());
    ctx->Yield();
    sk = dynamic_cast<SketchFeature*>(
        Doc()->GetFeature(0)); // Execute replaced it

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
    EnterSketchOnPlane(ctx, 0);
    ctx->Yield();
    auto* sk = dynamic_cast<SketchFeature*>(Doc()->GetFeature(0));
    IM_CHECK(sk != nullptr);
    nlohmann::json before = DocumentIO::FeatureToJson(*sk);
    // Off the datum axes so they are not pick candidates at the endpoint.
    uint32_t l0 = sk->AddEntity(sketch::Line{{-2, 0.5f}, {2, 0.5f}});
    sk->constraints.push_back(
        {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 4.0});
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

    // Same spot again: cycles to the dimensioned line body, which translates
    // the sketch rather than trying to resize the driving dimension. The body
    // records kWholeEntity, NOT 0 — a body pick and a vertex-0 pick both used
    // to store {id, 0}, so highlighting could not tell them apart and lit the
    // whole line whenever a vertex of it was picked.
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->sketchEdit.selected.size(), (size_t)1);
    IM_CHECK_EQ(s_App->sketchEdit.selected[0].entity, l0);
    IM_CHECK_EQ((int)s_App->sketchEdit.selected[0].which,
                (int)sketch::kWholeEntity);
    IM_CHECK(s_App->sketchDrag.mode ==
             ui::SketchDragSession::Mode::SketchTranslate);

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
  t = IM_REGISTER_TEST(engine, "select", "box_selects_enclosed");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
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

  // U8 polish: the Mirror collect phase box-selects like the Select tool,
  // and a tap in empty space advances to the axis pick.
  t = IM_REGISTER_TEST(engine, "select", "escape_cascade");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    EnterSketchOnPlane(ctx, 0);
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
  t = IM_REGISTER_TEST(engine, "selection", "survives_sibling_delete");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ToolbarCreateAccept(ctx, "###tb_sphere");
    const uint64_t sphereId = Doc()->GetFeature(1)->GetId();

    ctx->ItemClick("//###Model/Sphere 2##1");
    ctx->Yield();
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Delete the box above it: the selection stays on the sphere.
    ctx->ItemClick("//###Model/Box 1##0", ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Delete");
    ctx->Yield();
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);
    IM_CHECK_EQ(ui::SelectedFeatureId(*s_App), sphereId);

    // Deleting the selected feature itself clears the selection (prune).
    ctx->ItemClick("//###Model/Sphere 2##0", ImGuiMouseButton_Right);
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
    ToolbarCreateAccept(ctx, "###tb_box");

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
  t = IM_REGISTER_TEST(engine, "camera", "opening_an_assembly_frames_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A part far bigger than the default camera distance, in an assembly.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Beam", glm::vec3(0), 4000, 200, 300));
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    Occurrence occ;
    occ.name = "Beam";
    occ.partId = s_App->parts[0].id;
    occ.fixed = true;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_open_frame")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));

    // Reopen from scratch with the camera somewhere useless.
    ctx->MenuClick("File/New");
    ctx->Yield();
    s_App->camera->StopAnimation();
    s_App->camera->SetTarget(glm::vec3(0.0f));
    s_App->camera->SetDistance(5.0f); // inside anything of this size
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    ctx->Yield();

    // Framed: the camera stands off by something like the model's size,
    // rather than sitting inside it.
    IM_CHECK(s_App->assembly != nullptr);
    IM_CHECK(s_App->camera->GetDistance() > 1000.0f);
    std::filesystem::remove_all(dir);
  };

  // The camera is global, so arriving at a tab far bigger than the last one
  // used to put you inside it. The first visit to a tab frames it; after
  // that the framing is the user's.
  t = IM_REGISTER_TEST(engine, "camera", "first_visit_to_a_tab_frames_it");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A small part in tab 0 and a six-metre one in tab 1, saved and reopened
    // so both arrive unvisited — the state a project always opens in.
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Chip", glm::vec3(0), 8, 8, 8));
    const size_t big = ui::AddPart(*s_App, "Girder");
    ui::SetActivePart(*s_App, big);
    ctx->Yield();
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Span", glm::vec3(0), 6000, 300, 300));
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);

    const std::string dir =
        (std::filesystem::temp_directory_path() / "designer_tab_frame")
            .string();
    std::filesystem::remove_all(dir);
    std::string err;
    IM_CHECK(ui::SaveProject(*s_App, dir, &err));
    ctx->MenuClick("File/New");
    ctx->Yield();
    IM_CHECK(ui::OpenProject(*s_App, dir, &err));
    ctx->Yield(2);
    s_App->camera->FinishAnimation();

    // Tab 0 (the 8 mm chip) was framed by the open: close in.
    const float small = s_App->camera->GetDistance();
    IM_CHECK(small < 500.0f);

    // Switching to the girder frames it rather than leaving us inside.
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    IM_CHECK(s_App->camera->GetDistance() > 1000.0f);

    // Coming back a second time keeps whatever framing the user set.
    ui::SetActivePart(*s_App, 0);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    s_App->camera->SetDistance(42.0f);
    ui::SetActivePart(*s_App, 1);
    ctx->Yield(2);
    s_App->camera->FinishAnimation();
    IM_CHECK(std::abs(s_App->camera->GetDistance() - 42.0f) < 1.0f);
    std::filesystem::remove_all(dir);
  };

  // The Render modal on an assembly tab. Its own Render button carried a
  // part-only test (state.document && bodies), so on an assembly — where
  // `document` is null — it sat greyed out behind a working Render entry,
  // saying "Add a body to render" about an assembly full of parts.
  t = IM_REGISTER_TEST(engine, "render", "modal_enabled_on_an_assembly");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // a part with a body
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly != nullptr);

    // Empty assembly: the TOOLBAR button refuses — there is nothing to trace,
    // and opening a modal to say so would be theatre.
    ctx->SetRef("//##DockHost");
    IM_CHECK((ctx->ItemInfo("**/###tb_asm_render").ItemFlags &
              ImGuiItemFlags_Disabled) != 0);

    // With an occurrence in it, the modal opens AND its own Render button is
    // live. That button used to test `state.document && bodies`, which is
    // false on every assembly tab, so it sat greyed behind a working entry
    // saying "Add a body to render" about an assembly full of parts.
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    occ.fixed = true;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    IM_CHECK(s_App->assembly->OccurrenceCount() > 0);
    ToolbarClick(ctx, "###tb_asm_render");
    ctx->Yield(2);
    IM_CHECK(ctx->UiContext->OpenPopupStack.Size > 0); // the modal is up
    const bool rtReady =
        s_App->rayTracer != nullptr && s_App->rayTracer->IsInitialized();
    if (rtReady) {
      IM_CHECK((ctx->ItemInfo("//Render Still/###render_go").ItemFlags &
                ImGuiItemFlags_Disabled) == 0);
    } else {
      // No ray tracing on this GPU: the modal must blame the GPU, not the
      // model for being empty.
      IM_CHECK(ctx->ItemExists("//Render Still/###render_close"));
    }
    ctx->ItemClick("//Render Still/###render_close");
    ctx->Yield();
  };

  // A tab bar of names alone made a part, its drawing and a folder of them
  // look identical until you clicked. Each tab now carries its kind's glyph,
  // and the ids are "###" so they do not move when the label gains one.
  t = IM_REGISTER_TEST(engine, "measure", "assembly_distance");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // 2x2x2 at the origin
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
    const float gap = glm::length(s_App->measure.distB - s_App->measure.distA);
    IM_CHECK_LE(std::abs(gap - (10.0f - (max.x - min.x))), 1e-3f);

    // ...and it is actually DRAWN. This test asserted `hasDistanceLine` — a
    // variable — and passed for months while the assembly tab rendered
    // nothing at all: the primitive build sat inside `if (state.document)`,
    // which is null on every assembly tab, so the readout said "Distance:
    // 427.814 mm" over a viewport with no line in it and no highlight on the
    // picked edge. A claim that something is visible has to be asserted
    // against the pixels' input, not against the intent.
    std::vector<PrimVertex> prims;
    ui::AppendMeasureOverlay(*s_App, prims);
    IM_CHECK(!prims.empty());
    IM_CHECK(s_App->measure.dimension.valid);
    IM_CHECK(!s_App->measure.dimension.label.empty());

    // Same face, same occurrence → toggles OFF (identity includes the
    // occurrence, so this must not add a third element).
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield();
    IM_CHECK_EQ(s_App->measure.picked.size(), (size_t)1);

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield();
    IM_CHECK(!s_App->measure.active);
  };

  // Turning the terminal preference off and on again must give a working
  // shell. ShutdownTerminal terminated the PTY but kept the TerminalState,
  // whose `everStarted` latch (there to stop the shell respawning after the
  // user presses Stop) then blocked every future start — a panel that could
  // never open a shell again until the app restarted. The same one-shot trap
  // the chooser this panel replaced used to have.
  t = IM_REGISTER_TEST(engine, "handles", "hover_highlights_the_arrow");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    // A pending FILLET has the direct-manipulation handle (its radius); a
    // pending box does not — ComputeHandleSpec only serves features with a
    // headline scalar (extrude distance, blend radius, revolve angle).
    ToolbarCreateAccept(ctx, "###tb_box");
    ctx->Yield();
    const Body& body = Doc()->GetBodies()[0];
    IM_CHECK(!body.elementMap.edgeNames.empty());
    const ElementRef ref{body.createdBy,
                         body.elementMap.edgeNames.begin()->second};
    ui::BeginPreviewFeature(*s_App,
                            std::make_unique<FilletFeature>(
                                "Fillet", std::vector<ElementRef>{ref}, 0.4));
    ctx->Yield(2);
    IM_CHECK(ui::HasViewportHandle(*s_App));
    IM_CHECK(!s_App->viewportHandles.empty());
    const ui::ViewportHandle h = s_App->viewportHandles[0];
    IM_CHECK_EQ(ui::OwnerOf(h.id), ui::HandleOwner::Feature);

    // Nothing under the cursor to start with (the mouse is not on the arrow).
    ctx->MouseMoveToPos(WorldToWindow(h.origin - h.dir * (h.length * 6.0f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, (uint64_t)0);

    // On the shaft: hovered, and it is THIS handle, not merely "something".
    ctx->MouseMoveToPos(WorldToWindow(h.origin + h.dir * (h.length * 0.6f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, h.id);

    // Moving off clears it — a highlight that never turns off is worse than
    // none, because it stops meaning "you can grab this".
    ctx->MouseMoveToPos(WorldToWindow(h.origin - h.dir * (h.length * 6.0f)));
    ctx->Yield(2);
    IM_CHECK_EQ(s_App->hoveredHandle, (uint64_t)0);
    ui::CancelPendingFeature(*s_App);
  };

  // Per-axis, which is the whole reason the shared handle carries its own
  // colour: hovering the gizmo's X arrow must identify X.
  t = IM_REGISTER_TEST(engine, "handles", "gizmo_hover_names_the_axis");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box");
    ui::AddAssembly(*s_App, "Rig");
    ctx->Yield();
    Occurrence occ;
    occ.name = "Block";
    occ.partId = s_App->parts[0].id;
    s_App->assembly->AddOccurrence(occ);
    ctx->Yield(2);
    ui::SelectOccurrence(*s_App, s_App->assembly->GetOccurrences()[0].id);
    ctx->Yield(2);

    // Three translate arrows (and their arcs) are published, one per axis.
    IM_CHECK(s_App->viewportHandles.size() >= 3);
    int axesSeen = 0;
    for (const ui::ViewportHandle& h : s_App->viewportHandles)
      if (ui::OwnerOf(h.id) == ui::HandleOwner::Gizmo &&
          h.kind == ui::HandleKind::Linear)
        axesSeen++;
    IM_CHECK_EQ(axesSeen, 3);

    for (const ui::ViewportHandle& h : s_App->viewportHandles) {
      if (ui::OwnerOf(h.id) != ui::HandleOwner::Gizmo ||
          h.kind != ui::HandleKind::Linear)
        continue;
      ctx->MouseMoveToPos(WorldToWindow(h.origin + h.dir * (h.length * 0.6f)));
      ctx->Yield(2);
      // The axis under the cursor is the one reported — not the first one.
      IM_CHECK_EQ(ui::GizmoAxisOf(ui::SubOf(s_App->hoveredHandle)),
                  ui::GizmoAxisOf(ui::SubOf(h.id)));
    }

    // And the highlight reaches the DRAWN output, not just the state: the
    // same overlay, built with and without a hover, must differ — and only
    // in the hovered axis's own lines.
    const uint64_t xId =
        ui::HandleId(ui::HandleOwner::Gizmo, ui::GizmoSub(0, /*rotate=*/false));
    const auto overlayWith = [&](uint64_t hovered) {
      s_App->hoveredHandle = hovered;
      std::vector<LineVertex3D> lines;
      s_App->viewportHandles.clear(); // rebuilt by the call below
      ui::BuildGizmoOverlay(*s_App, lines);
      return lines;
    };
    const std::vector<LineVertex3D> plain = overlayWith(0);
    const std::vector<LineVertex3D> hoveredX = overlayWith(xId);
    IM_CHECK_EQ(plain.size(), hoveredX.size()); // geometry is untouched
    int brighter = 0;
    for (size_t i = 0; i < plain.size(); i++)
      if (plain[i].color != hoveredX[i].color) {
        brighter++;
        // Brighter, and still recognisably the axis's own colour: X stays
        // red rather than turning into one shared accent.
        IM_CHECK(hoveredX[i].color.r > plain[i].color.r);
        IM_CHECK(hoveredX[i].color.r > hoveredX[i].color.g);
      }
    IM_CHECK(brighter > 0);
  };

  // Reported: clicking a POINT in a sketch highlighted the point and one of
  // the lines next to it. A body pick and a vertex pick both stored {id, 0},
  // so the drawing code — which could only compare the entity — lit the whole
  // line whenever one of its vertices was selected.
}
