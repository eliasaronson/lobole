#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/drawing/DrawingDimension.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/SketchFeature.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>

using Catch::Matchers::WithinAbs;

// A flat pattern on a drawing (docs/sheetmetal/plan.md, S11): a view names
// the configuration it draws, a flat blank's creases draw as bend lines, and
// a bend table lists them.

namespace {

constexpr double kT = 1.5;
constexpr double kW = 100.0;
constexpr double kD = 60.0;
constexpr double kWall = 20.0;

ui::AppState MakeState() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  return state;
}

std::string EdgeAlongX(const Body& body, double y, double z) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt a = c.Value(c.FirstParameter());
    const gp_Pnt b = c.Value(c.LastParameter());
    if (std::abs(a.Y() - y) < 1e-6 && std::abs(b.Y() - y) < 1e-6 &&
        std::abs(a.Z() - z) < 1e-6 && std::abs(b.Z() - z) < 1e-6 &&
        std::abs(std::abs(a.X() - b.X()) - kW) < 1e-6)
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
  }
  return {};
}

// A 100 x 60 aluminium blank with one 20 mm flange, and a Flat
// configuration that turns its (suppressed) flat pattern on. Returns the
// Flat configuration's id; the document stays in its base configuration.
uint64_t FlangedPart(Document& doc) {
  sheet::SheetMetalRules rules;
  rules.material = "aluminum";
  rules.gauge = "1.5mm";
  rules.thickness = kT;
  doc.SetSheetMetal(rules);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {kW, 0}, 1});
  sk->AddEntity(sketch::Line{{kW, 0}, {kW, kD}, 2});
  sk->AddEntity(sketch::Line{{kW, kD}, {0, kD}, 3});
  sk->AddEntity(sketch::Line{{0, kD}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(
      std::make_unique<BaseFlangeFeature>("Base", doc.GetFeature(0)->GetId()));
  const uint64_t base = doc.GetFeature(1)->GetId();
  const std::string edge = EdgeAlongX(doc.GetBodies()[0], 0.0, kT);
  REQUIRE_FALSE(edge.empty());
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, kWall, 90.0);
  f->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(f));
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat pattern", base));
  const uint64_t flatId = doc.GetFeature(3)->GetId();
  doc.GetFeature(3)->SetSuppressed(true);
  doc.Regenerate();
  Configuration& flat = doc.AddConfiguration("Flat");
  flat.unsuppressedFeatures = {flatId};
  return flat.id;
}

size_t BendLines(const drawing::SheetView& sv) {
  return std::count_if(sv.curves.begin(), sv.curves.end(), [](const auto& c) {
    return c.kind == drawing::CurveKind::Bend;
  });
}

} // namespace

TEST_CASE("a drawing view draws the configuration it names",
          "[sheetmetal][drawing]") {
  ui::AppState state = MakeState();
  const uint64_t flatCfg = FlangedPart(*state.document);
  REQUIRE(state.document->GetFeature(2)->GetError().empty());
  const uint64_t part = state.parts[state.activePart].id;
  const uint64_t liveVersion = state.document->Version();
  // The drawing tab takes over state.document; the part lives on in its slot.
  Document* doc = state.document.get();

  ui::AddDrawing(state, "Sheet");
  DrawingView folded;
  folded.sourceDocId = part;
  folded.orientation = ViewOrientation::Top;
  DrawingView blank = folded;
  blank.configuration = flatCfg;
  blank.position = {0.0, 200.0};
  const uint64_t foldedId = state.drawing->AddView(folded);
  const uint64_t blankId = state.drawing->AddView(blank);
  ui::UpdateDrawingRenders(state);

  const drawing::SheetView fv = state.drawingRenders.at(foldedId);
  const drawing::SheetView bv = state.drawingRenders.at(blankId);
  REQUIRE(fv.error.empty());
  REQUIRE(bv.error.empty());
  REQUIRE(fv.hasGeometry);
  REQUIRE(bv.hasGeometry);

  // The folded part from above is the panel plus a wall's thickness and
  // bend; the blank is the panel plus the whole flange laid out.
  const double foldedY = fv.bboxMax.y - fv.bboxMin.y;
  const double blankY = bv.bboxMax.y - bv.bboxMin.y;
  CHECK(foldedY < kD + 2.0 * kT + 3.0);
  CHECK(blankY > kD + kWall);
  CHECK_THAT(bv.bboxMax.x - bv.bboxMin.x, WithinAbs(kW, 1e-6));

  // One crease on the blank, none on the folded part (its bend is a real
  // cylinder that projects itself), and the crease runs the width.
  CHECK(BendLines(fv) == 0);
  REQUIRE(BendLines(bv) == 1);
  const auto crease =
      std::find_if(bv.curves.begin(), bv.curves.end(), [](const auto& c) {
        return c.kind == drawing::CurveKind::Bend;
      });
  REQUIRE(crease->points.size() == 2);
  const glm::dvec2 a = crease->points[0], b = crease->points[1];
  CHECK_THAT(glm::length(b - a), WithinAbs(kW, 1e-6));
  CHECK_THAT(a.y, WithinAbs(b.y, 1e-9));
  // Inside the blank, between the panel and the flange: the panel's 60 mm
  // are on one side of it and the wall's 20 on the other.
  const double fromTop = bv.bboxMax.y - a.y;
  const double fromBottom = a.y - bv.bboxMin.y;
  CHECK((std::min(fromTop, fromBottom) > kWall - 1e-6 &&
         std::max(fromTop, fromBottom) > kD));
  CHECK(std::min(fromTop, fromBottom) < kWall + kT * 4);

  // The model tab never moved: still base configuration, still bent.
  CHECK(doc->ActiveConfiguration() == 0);
  CHECK(doc->Version() == liveVersion);

  // A side view of the blank sees the crease edge-on and draws no line.
  DrawingView side = blank;
  side.orientation = ViewOrientation::Front;
  side.position = {200.0, 200.0};
  const uint64_t sideId = state.drawing->AddView(side);
  ui::UpdateDrawingRenders(state);
  CHECK(state.drawingRenders.at(sideId).hasGeometry);
  CHECK(BendLines(state.drawingRenders.at(sideId)) == 0);

  // An edit to the part reaches the configured view.
  const double before = bv.bboxMax.y - bv.bboxMin.y;
  auto* flange = dynamic_cast<EdgeFlangeFeature*>(doc->GetFeature(2));
  REQUIRE(flange);
  flange->length = kWall + 10.0;
  doc->Regenerate();
  ui::UpdateDrawingRenders(state);
  const drawing::SheetView& after = state.drawingRenders.at(blankId);
  CHECK_THAT(after.bboxMax.y - after.bboxMin.y, WithinAbs(before + 10.0, 1e-6));

  // A configuration the part no longer has is an error on the view, not the
  // active configuration drawn in its place.
  REQUIRE(doc->RemoveConfiguration(flatCfg));
  ui::UpdateDrawingRenders(state);
  CHECK(state.drawingRenders.at(blankId).error.find("configuration") !=
        std::string::npos);
}

TEST_CASE("a bend table lists the bends of its view's part",
          "[sheetmetal][drawing]") {
  ui::AppState state = MakeState();
  const uint64_t flatCfg = FlangedPart(*state.document);
  const uint64_t part = state.parts[state.activePart].id;
  ui::AddDrawing(state, "Sheet");
  DrawingView view;
  view.sourceDocId = part;
  view.orientation = ViewOrientation::Top;
  view.configuration = flatCfg;
  const uint64_t viewId = state.drawing->AddView(view);
  ui::UpdateDrawingRenders(state);

  DrawingNote table;
  table.table = NoteTable::Bends;
  table.viewId = viewId;
  table.pos = {150, 150};
  drawing::ResolvedNote rn = ui::ResolveDrawingNote(state, table);
  INFO(rn.error);
  REQUIRE(rn.resolved);
  CHECK(rn.text.rfind("BEND TABLE\nB1  UP 90", 0) == 0);
  CHECK(rn.text.find("K 0.4") != std::string::npos);
  // A flat crease says how much it eats.
  CHECK(rn.text.find("BA ") != std::string::npos);
  CHECK(rn.text.find("B2") == std::string::npos);

  // UP and DOWN are the READER's: the same crease seen from underneath
  // folds away from them. The flange here goes up +Z whichever way its
  // picked edge ran, which is what a stored rotation sense got wrong.
  state.drawing->FindView(viewId)->orientation = ViewOrientation::Bottom;
  rn = ui::ResolveDrawingNote(state, table);
  CHECK(rn.text.rfind("BEND TABLE\nB1  DOWN 90", 0) == 0);
  // Edge-on, a crease has no up or down to state.
  state.drawing->FindView(viewId)->orientation = ViewOrientation::Front;
  rn = ui::ResolveDrawingNote(state, table);
  CHECK(rn.text.rfind("BEND TABLE\nB1  90", 0) == 0);
  state.drawing->FindView(viewId)->orientation = ViewOrientation::Top;

  table.text = "BENDS — {{2 + 1}} OFF";
  rn = ui::ResolveDrawingNote(state, table);
  CHECK(rn.text.rfind("BENDS — 3 OFF\nB1", 0) == 0);

  // A table of a part with no bends says so rather than drawing an empty box.
  auto box = std::make_unique<Document>();
  box->AddFeature(
      std::make_unique<BoxFeature>("Box", glm::vec3(0), 10, 10, 10));
  const Document boxDoc(*box);
  CHECK(drawing::BendTableText(boxDoc, "", {}, {0, 0, -1}).empty());
  DrawingNote free;
  free.table = NoteTable::Bends;
  rn = ui::ResolveDrawingNote(state, free);
  CHECK_FALSE(rn.resolved);
  CHECK(rn.error.find("view of a part") != std::string::npos);
}
