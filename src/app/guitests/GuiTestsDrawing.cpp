#include "app/guitests/GuiTestsCommon.h"
#include "core/drawing/DrawingProjection.h" // ProjectPoint
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/DrawingAnnotate.h"
#include "ui/DrawingSheet.h"

#include <chrono>

namespace {

// A plate (80 wide, 30 tall seen from the front) and a drawing tab holding
// its front view, projected. Returns the view's id.
uint64_t PlateOnASheet(ImGuiTestContext* ctx) {
  ctx->SetRef("//##MainMenuBar");
  Doc()->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0.0), 80.0, 60.0, 30.0));
  ctx->Yield(2);
  const uint64_t partId = s_App->parts[s_App->activePart].id;
  ui::SetActivePart(*s_App, ui::AddDrawing(*s_App, "Sheet"));
  ctx->Yield(3);
  DrawingView v;
  v.name = "front";
  v.sourceDocId = partId;
  v.orientation = ViewOrientation::Front;
  v.scale = 0.0; // the fitted standard scale
  const uint64_t id = ui::AddDrawingViewPlaced(*s_App, v);
  ctx->Yield(3);
  return id;
}

// Pans the sheet so `mm` sits a third of the way across the viewport: clear
// of the tool window, which floats at its top right.
void LookAt(ImGuiTestContext* ctx, glm::dvec2 mm) {
  const double s = s_App->sheetView.scale;
  const double cx = s_App->viewportPos.x + s_App->viewportSize.x * 0.5;
  const double want = s_App->viewportPos.x + s_App->viewportSize.x * 0.33;
  const double cy = s_App->viewportPos.y + s_App->viewportSize.y * 0.5;
  const double wantY = s_App->viewportPos.y + s_App->viewportSize.y * 0.55;
  s_App->sheetView.offset.x = mm.x - (want - cx) / s;
  s_App->sheetView.offset.y = mm.y + (wantY - cy) / s;
  ctx->Yield(2);
}

const drawing::SheetView& RenderOf(uint64_t viewId) {
  return s_App->drawingRenders.at(viewId);
}

// The view's straight edge along sheet X nearest its bottom (or its top):
// a line a datum or a frame can stand on.
const drawing::SheetCurve* EdgeAt(uint64_t viewId, bool bottom) {
  const drawing::SheetCurve* best = nullptr;
  double bestY = 0.0;
  for (const drawing::SheetCurve& c : RenderOf(viewId).curves) {
    if (!c.visible || !c.edge.IsSet() || c.points.size() != 2)
      continue;
    if (std::abs(c.points[0].y - c.points[1].y) > 1e-6)
      continue;
    const double y = c.points[0].y;
    if (!best || (bottom ? y < bestY : y > bestY)) {
      best = &c;
      bestY = y;
    }
  }
  return best;
}

glm::dvec2 Mid(const drawing::SheetCurve& c) {
  return 0.5 * (c.points.front() + c.points.back());
}

void ClickSheet(ImGuiTestContext* ctx, glm::dvec2 mm) {
  ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, mm));
  ctx->Yield();
  ctx->MouseClick(ImGuiMouseButton_Left);
  ctx->Yield(2);
}

bool AcceptEnabled(ImGuiTestContext* ctx) {
  const ImGuiTestItemInfo accept =
      ctx->ItemInfo("//###ToolWindow/###tw_accept", ImGuiTestOpFlags_NoError);
  return accept.ID != 0 && (accept.ItemFlags & ImGuiItemFlags_Disabled) == 0;
}

size_t NotesOfStyle(NoteStyle style) {
  size_t n = 0;
  for (const DrawingNote& note : s_App->drawing->GetNotes())
    n += note.style == style ? 1 : 0;
  return n;
}

// A part carrying a four-turn helical spring and a box -- seconds of exact
// hidden-line removal in one view -- and a drawing tab holding its front
// view, never projected and not the tab shown. Returns the sheet's tab index
// and the view's id.
std::pair<size_t, uint64_t> SpringOnAnUnvisitedSheet(ImGuiTestContext* ctx) {
  ctx->SetRef("//##MainMenuBar");
  auto helix = std::make_unique<CurveFeature>("Helix", Curve3DKind::Helix);
  helix->radius = 11;
  helix->pitch = 7;
  helix->turns = 4;
  Doc()->AddFeature(std::move(helix));
  const uint64_t pathId = Doc()->GetFeature(Doc()->FeatureCount() - 1)->GetId();
  Doc()->AddFeature(std::make_unique<PipeFeature>("Spring", pathId, 3.2));
  Doc()->AddFeature(std::make_unique<BoxFeature>(
      "Block", glm::dvec3(30.0, -5.0, 0.0), 10.0, 10.0, 10.0));
  ctx->Yield(2);
  const size_t partIndex = static_cast<size_t>(s_App->activePart);
  const uint64_t partId = s_App->parts[partIndex].id;
  const size_t sheetIndex = ui::AddDrawing(*s_App, "Sheet");
  DrawingView v;
  v.name = "front";
  v.sourceDocId = partId;
  v.orientation = ViewOrientation::Front;
  v.scale = 1.0;
  v.position = ChromeOf(s_App->drawing->GetSheet()).extent * 0.5;
  const uint64_t viewId = s_App->drawing->AddView(v); // not projected
  ui::SetActivePart(*s_App, partIndex);
  ctx->Yield(3);
  return {sheetIndex, viewId};
}
} // namespace

// Drawing GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterDrawingGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
  t = IM_REGISTER_TEST(engine, "drawing", "build_a_drawing_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ctx->MenuClick("File/New");
    ctx->Yield();
    AddFixtureBox(ctx);
    ctx->ItemDoubleClick("//###Model/Box 1##0");
    ctx->Yield();
    // A plate lying flat: 80 wide (X), 60 deep (Y), 8 thick along world up.
    ctx->ItemInputValue("//Box###ToolWindow/Width/##Width", 80.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Height/##Height", 60.0f);
    ctx->ItemInputValue("//Box###ToolWindow/Depth/##Depth", 8.0f);
    ctx->Yield(2); // editing a committed feature commits as you type

    // A drawing tab, from the tab bar's + menu.
    ctx->ItemClick("//##PartTabs/+##addmenu");
    ctx->ItemClick("//$FOCUSED/New Drawing##adddwg");
    ctx->Yield(3);
    IM_CHECK(s_App->drawing != nullptr);

    // Two views, by clicking: the orientation combo then Add view.
    // ComboClick wants a ref RELATIVE to the window
    // (docs/ui/testing.md#making-a-widget-addressable). Add view is a tool
    // window, opened from the toolbar
    // (docs/ui/panels.md#drawing-tab-the-drawing-panel).
    ToolbarClick(ctx, "###tb_dwg_add_view");
    ctx->Yield(2);
    ctx->SetRef("//###ToolWindow");
    ctx->ItemInputValue("###dwg_scale", 0.0f); // 0 = choose a scale for me
    for (const char* orient : {"front", "top"}) {
      ctx->ComboClick((std::string("###dwg_orientation/") + orient).c_str());
      ctx->Yield();
      ctx->ItemClick("###dwg_add_view");
      ctx->Yield(3);
    }
    ctx->ItemClick(
        "###dwg_add_view_close"); // a floating window a test opens, it closes
    ctx->Yield();
    const auto& views = s_App->drawing->GetViews();
    IM_CHECK_EQ(views.size(), (size_t)2);

    // What each named view actually SHOWS: the front of a plate 80 wide and
    // 8 thick is 80 x 8, and looking down on it gives the 80 x 60 face —
    // the axis-swap that a mangled orientation table produces is exactly
    // what this catches (docs/conventions.md#world-axes).
    // No IM_CHECK in here: its failure path returns void, which makes the
    // lambda's return type ambiguous. An unprojected view comes back as
    // zero and fails the size checks below just as loudly.
    const auto extent = [&](size_t i) -> glm::dvec2 {
      const auto it = s_App->drawingRenders.find(views[i].id);
      if (it == s_App->drawingRenders.end() || !it->second.hasGeometry)
        return glm::dvec2(0.0);
      return it->second.bboxMax - it->second.bboxMin;
    };
    const glm::dvec2 front = extent(0), top = extent(1);
    ctx->LogInfo("front %.1f x %.1f, top %.1f x %.1f", front.x, front.y, top.x,
                 top.y);
    IM_CHECK(std::abs(front.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(front.y - 8.0) < 1e-6);
    IM_CHECK(std::abs(top.x - 80.0) < 1e-6);
    IM_CHECK(std::abs(top.y - 60.0) < 1e-6);

    // Two views of one part share a scale — a top view at 2:1 beside a front
    // view at 1:1 is a drafting error, and it is what fitting each view to
    // the sheet on its own produced.
    IM_CHECK_EQ(views[0].scale, views[1].scale);

    // ...and they do not land on top of each other. The sheet validator is
    // the same one the panel shows, so this is the user-visible verdict.
    const std::vector<drawing::DrawingIssue> issues = drawing::ValidateDrawing(
        *s_App->drawing, s_App->drawingRenders, {}, {});
    for (const drawing::DrawingIssue& issue : issues)
      ctx->LogWarning("%s", issue.detail.c_str());
    IM_CHECK(issues.empty());

    // A view is a row (docs/ui/panels.md#drawing-tab-the-drawing-panel): its
    // menu sets the scale, which only the add form could before, and deletes it
    // -- where a bare "x" button was.
    const uint64_t firstId = views[0].id;
    const std::string row =
        "//###Model/**/###dwg_view_" + std::to_string(firstId);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    ctx->MenuClick(
        ("###dwg_view_scale/###dwg_scale_" + std::to_string(0.5)).c_str());
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->drawing->FindView(firstId)->scale, 0.5);
    // ... and turns it on the sheet: at 90 the drawn view's width and height
    // trade places.
    const glm::dvec2 before = extent(0);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->SetRef("//$FOCUSED");
    ctx->MenuClick("###dwg_view_rotate/###dwg_rotate_90");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->drawing->FindView(firstId)->rotationDeg, 90.0);
    IM_CHECK(std::abs(extent(0).x - before.y) < 1e-3);
    IM_CHECK(std::abs(extent(0).y - before.x) < 1e-3);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###dwg_view_delete");
    ctx->Yield(3);
    IM_CHECK_EQ(s_App->drawing->GetViews().size(), (size_t)1);
    IM_CHECK(s_App->drawing->FindView(firstId) == nullptr);
  };

  // A datum feature and the frames that name it, by clicking
  // (docs/drawings/guide.md#in-the-gui): the pick is the feature, the symbol
  // stands clear of the view where a drag moves it, the defaults come from
  // the feature, and what add_drawing_note refuses the window refuses.
  t = IM_REGISTER_TEST(engine, "drawing", "datum_and_frame_by_clicking");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    using Kind = ui::AppState::DrawingNoteTool::Kind;
    const uint64_t front = PlateOnASheet(ctx);
    IM_CHECK(s_App->drawingRenders.contains(front));
    const drawing::SheetCurve* bottom = EdgeAt(front, true);
    const drawing::SheetCurve* top = EdgeAt(front, false);
    IM_CHECK(bottom != nullptr && top != nullptr && bottom != top);
    const glm::dvec2 bottomMid = Mid(*bottom), topMid = Mid(*top);
    const ProjRef bottomRef = ui::RefOfCurve(*bottom, front);
    const ProjRef topRef = ui::RefOfCurve(*top, front);
    const glm::dvec2 boxMin = RenderOf(front).bboxMin;
    const glm::dvec2 boxMax = RenderOf(front).bboxMax;
    LookAt(ctx, 0.5 * (boxMin + boxMax));

    // The datum: with nothing picked it waits, its letter the sheet's next.
    ToolbarClick(ctx, "###tb_dwg_datum");
    ctx->Yield(2);
    IM_CHECK(s_App->drawingNoteTool.kind == Kind::Datum);
    IM_CHECK_STR_EQ(s_App->drawingNoteTool.letter.c_str(), "A");
    IM_CHECK(!AcceptEnabled(ctx));
    ClickSheet(ctx, bottomMid);
    IM_CHECK(s_App->drawingNoteTool.targeted);
    IM_CHECK(AcceptEnabled(ctx));
    // It stands off the view, below the edge it is on.
    const glm::dvec2 placed = s_App->drawingNoteTool.pos;
    ctx->LogInfo("datum at %.1f, %.1f; view %.1f..%.1f x %.1f..%.1f", placed.x,
                 placed.y, boxMin.x, boxMax.x, boxMin.y, boxMax.y);
    IM_CHECK(placed.y < boxMin.y);
    // A drag moves it.
    ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, placed));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->MouseMoveToPos(
        ui::SheetToScreen(*s_App, placed + glm::dvec2(-10.0, 0.0)));
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK(std::abs(s_App->drawingNoteTool.pos.x - (placed.x - 10.0)) < 0.5);
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK(s_App->drawingNoteTool.kind == Kind::None);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Datum), (size_t)1);
    const DrawingNote datum = s_App->drawing->GetNotes().back();
    IM_CHECK_STR_EQ(datum.text.c_str(), "A");
    IM_CHECK(datum.leader.edge.name == bottomRef.edge.name);

    // A second datum takes the next letter; typing A again is refused, as
    // the RPC refuses a letter used twice.
    ToolbarClick(ctx, "###tb_dwg_datum");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(s_App->drawingNoteTool.letter.c_str(), "B");
    ClickSheet(ctx, topMid);
    IM_CHECK(AcceptEnabled(ctx));
    ctx->ItemInputValue("//###ToolWindow/###datum_letter", "A");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(s_App->drawingNoteTool.letter.c_str(), "A");
    IM_CHECK(!AcceptEnabled(ctx));
    ctx->ItemClick("//###ToolWindow/###tw_cancel");
    ctx->Yield(2);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Datum), (size_t)1);

    // A frame on the top edge: flatness, its zone ISO 2768-K for 80 mm.
    ToolbarClick(ctx, "###tb_dwg_fcf");
    ctx->Yield(2);
    IM_CHECK(!AcceptEnabled(ctx));
    ClickSheet(ctx, topMid);
    {
      const auto& tool = s_App->drawingNoteTool;
      IM_CHECK(tool.targeted && tool.dimId == 0);
      IM_CHECK(tool.fcf.symbol == GeoSymbol::Flatness);
      IM_CHECK(std::abs(tool.featureMM - 80.0) < 1e-6);
      IM_CHECK(std::abs(tool.fcf.tolerance - 0.2) < 1e-9);
      IM_CHECK(tool.pos.y > boxMax.y); // above the edge it is on
    }
    IM_CHECK(AcceptEnabled(ctx));
    // A perpendicularity is measured from a datum: refused until it names one.
    ctx->SetRef("//###ToolWindow");
    ctx->ComboClick("###fcf_symbol/perpendicularity");
    ctx->Yield(2);
    IM_CHECK(s_App->drawingNoteTool.fcf.symbol == GeoSymbol::Perpendicularity);
    IM_CHECK(std::abs(s_App->drawingNoteTool.fcf.tolerance - 0.4) < 1e-9);
    IM_CHECK(!AcceptEnabled(ctx));
    ctx->ComboClick("###fcf_datum0/A");
    ctx->Yield(2);
    IM_CHECK(AcceptEnabled(ctx));
    // A typed zone is the user's: it holds.
    ctx->ItemInputValue("###fcf_tolerance", 0.05f);
    ctx->Yield(2);
    IM_CHECK(s_App->drawingNoteTool.toleranceTyped);
    IM_CHECK(std::abs(s_App->drawingNoteTool.fcf.tolerance - 0.05) < 1e-6);
    ctx->ItemClick("###tw_accept");
    ctx->Yield(2);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Fcf), (size_t)1);
    {
      const DrawingNote& frame = s_App->drawing->GetNotes().back();
      IM_CHECK(frame.fcf.symbol == GeoSymbol::Perpendicularity);
      IM_CHECK_EQ(frame.fcf.datums.size(), (size_t)1);
      IM_CHECK(std::abs(frame.fcf.tolerance - 0.05) < 1e-6);
      IM_CHECK(frame.leader.edge.name == topRef.edge.name);
      IM_CHECK(ui::ResolveDrawingNote(*s_App, frame).resolved);
    }

    // Under a dimension, chosen from its row (select-first): no leader, it
    // stands with the value.
    DrawingDim height;
    height.kind = DrawingDimKind::DistanceY;
    height.viewId = front;
    height.refs = {bottomRef, topRef};
    height.labelOffset = ui::AutoDimOffset(*s_App, height);
    ui::SnapshotDrawing(*s_App);
    const uint64_t dimId = s_App->drawing->AddDim(height);
    ctx->Yield(2);
    const std::string row = "//###Model/**/###dwg_dim_" + std::to_string(dimId);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###dwg_dim_fcf");
    ctx->Yield(2);
    IM_CHECK(s_App->drawingNoteTool.kind == Kind::Fcf);
    IM_CHECK_EQ(s_App->drawingNoteTool.dimId, dimId);
    IM_CHECK(s_App->drawingNoteTool.fcf.symbol == GeoSymbol::Position);
    IM_CHECK(AcceptEnabled(ctx));
    ctx->ItemClick("//###ToolWindow/###tw_cancel");
    ctx->Yield(2);
    // ...and by clicking the value itself.
    ToolbarClick(ctx, "###tb_dwg_fcf");
    ctx->Yield(2);
    const drawing::ResolvedDim rd =
        ui::ResolveDrawingDim(*s_App, *s_App->drawing->FindDim(dimId));
    IM_CHECK(rd.resolved);
    LookAt(ctx, rd.labelPos);
    ClickSheet(ctx, rd.labelPos);
    IM_CHECK_EQ(s_App->drawingNoteTool.dimId, dimId);
    const size_t undoDepth = s_App->drawingHistory.undo.size();
    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(2);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Fcf), (size_t)2);
    IM_CHECK_EQ(s_App->drawing->GetNotes().back().attachDim, dimId);
    IM_CHECK(!s_App->drawing->GetNotes().back().leader.edge.IsSet());
    // One undo step.
    IM_CHECK_EQ(s_App->drawingHistory.undo.size(), undoDepth + 1);
    IM_CHECK(ui::UndoDrawing(*s_App));
    ctx->Yield(2);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Fcf), (size_t)1);
  };

  // A detail view by direct manipulation: press on the view at the centre,
  // drag out the radius; it comes up at an enlarging scale, clear of the
  // other views, and a drag moves it.
  t = IM_REGISTER_TEST(engine, "drawing", "detail_view_by_press_and_drag");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const uint64_t front = PlateOnASheet(ctx);
    IM_CHECK(s_App->drawingRenders.contains(front));
    const glm::dvec2 boxMin = RenderOf(front).bboxMin;
    const glm::dvec2 boxMax = RenderOf(front).bboxMax;
    const DrawingView parent = *s_App->drawing->FindView(front);
    LookAt(ctx, 0.5 * (boxMin + boxMax));

    ToolbarClick(ctx, "###tb_dwg_detail");
    ctx->Yield(2);
    IM_CHECK(s_App->detailTool.active);
    IM_CHECK(!AcceptEnabled(ctx));

    // Press near the plate's lower-left corner, drag out 6 sheet mm: the
    // circle takes the corner in.
    const glm::dvec2 at = boxMin + glm::dvec2(4.0, 4.0);
    ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, at));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, at + glm::dvec2(6.0, 0.0)));
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    const auto& tool = s_App->detailTool;
    IM_CHECK_EQ(tool.parentViewId, front);
    const double px = 1.5 / s_App->sheetView.scale; // a pixel and a half
    ctx->LogInfo("radius %.3f mm at %g:1, detail at %g", tool.radius,
                 parent.scale, tool.scale);
    IM_CHECK(std::abs(tool.radius * parent.scale - 6.0) < px);
    // The circle is where the press was.
    glm::dvec3 vd, up;
    ViewAxes(parent, vd, up);
    const glm::dvec2 centre =
        ui::DrawingFoldedPosition(*s_App, parent).pos +
        parent.scale * drawing::ProjectPoint(tool.center, vd, up);
    IM_CHECK(glm::length(centre - at) < px);
    // An enlarging standard scale, the next letter, a place off the views.
    IM_CHECK(tool.scale >= 2.0 * parent.scale - 1e-9);
    IM_CHECK_STR_EQ(tool.label.c_str(), "A");
    IM_CHECK(AcceptEnabled(ctx));

    // A drag moves it.
    const glm::dvec2 was = tool.position;
    ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, was));
    ctx->Yield();
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->MouseMoveToPos(ui::SheetToScreen(*s_App, was + glm::dvec2(0.0, -5.0)));
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);
    IM_CHECK(std::abs(s_App->detailTool.position.y - (was.y - 5.0)) < px);
    IM_CHECK(s_App->detailTool.moved);
    // ...back where it was derived, so the sheet stays clear.
    s_App->detailTool.position = was;

    ctx->ItemClick("//###ToolWindow/###tw_accept");
    ctx->Yield(3);
    IM_CHECK(!s_App->detailTool.active);
    IM_CHECK_EQ(s_App->drawing->GetViews().size(), (size_t)2);
    const DrawingView& detail = s_App->drawing->GetViews().back();
    IM_CHECK(detail.detail.on);
    IM_CHECK_EQ(detail.detail.parentViewId, front);
    IM_CHECK(s_App->drawingRenders.contains(detail.id));
    IM_CHECK(RenderOf(detail.id).hasGeometry); // the corner, enlarged
    const std::vector<drawing::DrawingIssue> issues = drawing::ValidateDrawing(
        *s_App->drawing, s_App->drawingRenders, {}, {});
    for (const drawing::DrawingIssue& issue : issues)
      ctx->LogWarning("%s", issue.detail.c_str());
    IM_CHECK(issues.empty());
  };

  // "Lay out balloons" on an assembly view: its row's menu and the toolbar,
  // one balloon per parts-list row, laid out again rather than added twice.
  t = IM_REGISTER_TEST(engine, "drawing", "balloons_from_the_view_row");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 20, 20, 20));
    ctx->Yield(2);
    const uint64_t block = s_App->parts[s_App->activePart].id;
    ui::SetActivePart(*s_App, ui::AddPart(*s_App, "Pin"));
    Doc()->AddFeature(
        std::make_unique<BoxFeature>("Pin", glm::dvec3(0.0), 8, 8, 40));
    ctx->Yield(2);
    const uint64_t pin = s_App->parts[s_App->activePart].id;
    const size_t asmIdx = ui::AddAssembly(*s_App, "Rig");
    ui::SetActivePart(*s_App, asmIdx);
    const uint64_t rig = s_App->parts[asmIdx].id;
    for (int i = 0; i < 3; ++i) {
      Occurrence o;
      o.name = "Block " + std::to_string(i + 1);
      o.partId = block;
      o.position = glm::dvec3(40.0 * i, 0.0, 0.0);
      s_App->assembly->AddOccurrence(o);
    }
    Occurrence p;
    p.name = "Pin";
    p.partId = pin;
    p.position = glm::dvec3(6.0, 6.0, 20.0);
    s_App->assembly->AddOccurrence(p);
    ctx->Yield(3);
    ui::SetActivePart(*s_App, ui::AddDrawing(*s_App, "Sheet"));
    ctx->Yield(3);
    DrawingView v;
    v.name = "front";
    v.sourceDocId = rig;
    v.orientation = ViewOrientation::Front;
    v.scale = 0.0;
    const uint64_t view = ui::AddDrawingViewPlaced(*s_App, v);
    ctx->Yield(3);
    IM_CHECK_EQ(ui::BalloonViews(*s_App).size(), (size_t)1);

    const std::string row = "//###Model/**/###dwg_view_" + std::to_string(view);
    ctx->ItemClick(row.c_str(), ImGuiMouseButton_Right);
    ctx->Yield();
    ctx->ItemClick("//$FOCUSED/###dwg_view_balloons");
    ctx->Yield(3);
    ctx->LogInfo("%s", s_App->drawingMessage.c_str());
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Balloon), (size_t)2); // two parts
    IM_CHECK(!s_App->drawingMessageIsError);
    IM_CHECK(s_App->drawingMessage.find("2 balloons") != std::string::npos);
    for (const DrawingNote& n : s_App->drawing->GetNotes())
      if (n.style == NoteStyle::Balloon)
        IM_CHECK(ui::ResolveDrawingNote(*s_App, n).resolved);
    // The toolbar lays them out again: replaced, not doubled.
    ToolbarClick(ctx, "###tb_dwg_balloons");
    ctx->Yield(3);
    IM_CHECK_EQ(NotesOfStyle(NoteStyle::Balloon), (size_t)2);
  };

  // The drawing sessions' half of the defaults gate
  // (docs/ui/tool-sessions.md#defaults-and-refusals): opened with nothing
  // picked, each says what it needs and offers no Accept, and a verb with
  // nothing to act on is not offered at all.
  t = IM_REGISTER_TEST(engine, "defaults", "drawing_sessions_wait_for_a_pick");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    PlateOnASheet(ctx);
    for (const char* tb :
         {"###tb_dwg_fcf", "###tb_dwg_datum", "###tb_dwg_detail"}) {
      ToolbarClick(ctx, tb);
      ctx->Yield(2);
      ctx->LogInfo("%s", tb);
      IM_CHECK(ctx->ItemExists("//###ToolWindow/###tw_accept"));
      IM_CHECK(!AcceptEnabled(ctx));
      const size_t notes = s_App->drawing->GetNotes().size();
      const size_t views = s_App->drawing->GetViews().size();
      ctx->KeyPress(ImGuiKey_Enter); // Enter does not take a refused Accept
      ctx->Yield(2);
      IM_CHECK_EQ(s_App->drawing->GetNotes().size(), notes);
      IM_CHECK_EQ(s_App->drawing->GetViews().size(), views);
      ctx->ItemClick("//###ToolWindow/###tw_cancel");
      ctx->Yield(2);
    }
    IM_CHECK(s_App->drawingNoteTool.kind ==
             ui::AppState::DrawingNoteTool::Kind::None);
    IM_CHECK(!s_App->detailTool.active);
    // A sheet of a part has no parts list to balloon.
    const ImGuiTestItemInfo balloons =
        ctx->ItemInfo("//##DockHost/**/###tb_dwg_balloons");
    IM_CHECK(balloons.ID != 0);
    IM_CHECK((balloons.ItemFlags & ImGuiItemFlags_Disabled) != 0);
  };

  // docs/drawings/README.md#projection-off-the-frame: switching to a drawing
  // tab whose views were never projected this session projected every one of
  // them inside the click's frame -- the pan/tilt head's sheet froze the app
  // for 43.7 s. A part carrying a helical spring and a box (seconds of exact
  // HLR in one front view), its sheet never shown, the tab clicked: the
  // projection runs on a `drawing` job, a placeholder stands in its place,
  // and no frame is held while it runs.
  t = IM_REGISTER_TEST(engine, "drawing",
                       "a_slow_drawing_tab_does_not_stall_the_frame");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const auto [sheetIndex, viewId] = SpringOnAnUnvisitedSheet(ctx);
    s_App->drawingDeferProjection = true; // the real default, off for the suite
    const uint64_t placeholderBefore =
        s_App->parts[sheetIndex].drawingProjections.placeholderFrames;

    // The click itself -- the press and the release, the mouse already on
    // the tab -- and what its frames COST (MainThreadCpuMs), not how long
    // they waited for a core. The projection inside one of them was the
    // stall.
    ctx->MouseMove(
        ("//##PartTabs/**/###part" + std::to_string(sheetIndex)).c_str());
    const double c0 = MainThreadCpuMs();
    ctx->MouseClick(ImGuiMouseButton_Left);
    const double clickMs = MainThreadCpuMs() - c0;
    IM_CHECK(s_App->drawing != nullptr);
    ctx->LogInfo("the tab switch cost %.1f ms of main-thread time", clickMs);
    IM_CHECK(clickMs < 250.0);

    // Frames keep coming while the view projects, and none of them is held.
    int framesWhileRunning = 0;
    double worstMs = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    while (ui::BackgroundJobRunning(*s_App, "drawing")) {
      const double f0 = MainThreadCpuMs();
      ctx->Yield();
      worstMs = std::max(worstMs, MainThreadCpuMs() - f0);
      framesWhileRunning++;
      if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120))
        break;
    }
    ctx->LogInfo("%d frames while projecting, worst %.1f ms",
                 framesWhileRunning, worstMs);
    IM_CHECK(!ui::BackgroundJobRunning(*s_App, "drawing"));
    IM_CHECK(framesWhileRunning >= 2);
    IM_CHECK(worstMs < 250.0);
    // The placeholder was drawn in the view's place while it projected.
    IM_CHECK(s_App->drawingProjections.placeholderFrames > placeholderBefore);

    // ...and the view landed, with curves, having cost what makes the
    // question worth asking: a second or more of hidden-line removal, so the
    // case cannot pass by being fast.
    ctx->Yield(2);
    IM_CHECK(s_App->drawingRenders.contains(viewId));
    const drawing::SheetView& sv = RenderOf(viewId);
    IM_CHECK(sv.error.empty());
    IM_CHECK(sv.hasGeometry);
    IM_CHECK(!sv.curves.empty());
    const double projectedMs = sv.updateMs + sv.hideMs;
    ctx->LogInfo("the view's own projection cost %.0f ms", projectedMs);
    IM_CHECK(projectedMs >= 1000.0);
    IM_CHECK(!ui::DrawingViewProjecting(*s_App, viewId));
  };

  // Cancel, from the drawing panel's bar: the view that had not landed keeps
  // its placeholder and says it was cancelled, and a later Update views
  // projects it.
  t = IM_REGISTER_TEST(engine, "drawing", "a_cancelled_projection_resumes");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const auto [sheetIndex, viewId] = SpringOnAnUnvisitedSheet(ctx);
    s_App->drawingDeferProjection = true; // the real default, off for the suite
    ctx->ItemClick(
        ("//##PartTabs/**/###part" + std::to_string(sheetIndex)).c_str());
    ctx->Yield(2);
    IM_CHECK(ui::BackgroundJobRunning(*s_App, "drawing"));
    IM_CHECK(ui::DrawingViewProjecting(*s_App, viewId));
    ctx->ItemClick("//###Model/**/###dwg_cancel_projection");
    IM_CHECK(WaitForJobs(ctx, "drawing"));
    ctx->Yield(2);
    IM_CHECK(!ui::DrawingViewProjecting(*s_App, viewId));
    IM_CHECK(s_App->drawingRenders.contains(viewId));
    IM_CHECK(!RenderOf(viewId).hasGeometry);
    IM_CHECK(RenderOf(viewId).error.find("projection cancelled") !=
             std::string::npos);
    const DrawingView* view = s_App->drawing->FindView(viewId);
    IM_CHECK(view != nullptr);
    glm::dvec2 lo, hi;
    IM_CHECK(ui::DrawingPlaceholderBox(*s_App, *view, lo, hi));

    ToolbarClick(ctx, "###tb_dwg_update");
    ctx->Yield(2);
    IM_CHECK(WaitForJobs(ctx, "drawing"));
    ctx->Yield(2);
    IM_CHECK(RenderOf(viewId).error.empty());
    IM_CHECK(RenderOf(viewId).hasGeometry);
    IM_CHECK(!ui::DrawingPlaceholderBox(*s_App, *view, lo, hi));
  };

  // F11 layer 3: what the GEOMETRY did between a version and now. The
  // numbers have to be real — a box grown from 20 to 30 mm on one axis adds
  // exactly the slab that appeared, and nothing is removed.
}
