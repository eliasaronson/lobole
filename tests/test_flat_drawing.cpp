// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
#include <cstdio>
#include <numbers>

using Catch::Matchers::WithinAbs;

// A flat pattern on a drawing (docs/sheetmetal/README.md#dxf-and-drawings): a
// view names the configuration it draws, a flat blank's creases draw as bend
// lines, and a bend table lists them.

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
// Flat configuration's id (and its flat pattern feature's, through
// `flatFeatureOut`); the document stays in its base configuration.
uint64_t FlangedPart(Document& doc, uint64_t* flatFeatureOut = nullptr) {
  sheet::SheetMetalRules rules;
  rules.material = "aluminum_6061_t6";
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
  if (flatFeatureOut)
    *flatFeatureOut = flatId;
  return flat.id;
}

// The bend allowance this part's one bend eats, from the same functions the
// feature used — a literal here would agree with the code by luck.
double Allowance() {
  const double r = sheet::MinBendRadius("aluminum_6061_t6", kT);
  return sheet::BendAllowance(std::numbers::pi / 2.0, r,
                              sheet::KFactor("aluminum_6061_t6", r, kT), kT);
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
  uint64_t flatFeature = 0;
  const uint64_t flatCfg = FlangedPart(*state.document, &flatFeature);
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
  ui::UpdateDrawingRendersNow(state);

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
  // The blank on the sheet is the panel, the bend allowance and the wall —
  // the whole of the flat-pattern arithmetic, measured off the DRAWING at
  // 1:1 rather than off the body.
  CHECK_THAT(blankY, WithinAbs(kD + Allowance() + kWall, 1e-6));
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
  // And it lands where the allowance says: the crease is the centreline of
  // the flattened bend, so it sits half an allowance in from the panel and
  // the wall plus half an allowance in from the blank's other end. A bend
  // line drawn at the tangent instead would be half an allowance out, which
  // is the error that bends a part to the wrong length.
  const double fromTop = bv.bboxMax.y - a.y;
  const double fromBottom = a.y - bv.bboxMin.y;
  CHECK_THAT(std::min(fromTop, fromBottom),
             WithinAbs(kWall + Allowance() / 2.0, 1e-6));
  CHECK_THAT(std::max(fromTop, fromBottom),
             WithinAbs(kD + Allowance() / 2.0, 1e-6));

  // The model tab never moved: still base configuration, still bent.
  CHECK(doc->ActiveConfiguration() == 0);
  CHECK(doc->Version() == liveVersion);

  // A side view of the blank sees the crease edge-on and draws no line.
  DrawingView side = blank;
  side.orientation = ViewOrientation::Front;
  side.position = {200.0, 200.0};
  const uint64_t sideId = state.drawing->AddView(side);
  ui::UpdateDrawingRendersNow(state);
  CHECK(state.drawingRenders.at(sideId).hasGeometry);
  CHECK(BendLines(state.drawingRenders.at(sideId)) == 0);

  // An edit to the part reaches the configured view.
  const double before = bv.bboxMax.y - bv.bboxMin.y;
  auto* flange = dynamic_cast<EdgeFlangeFeature*>(doc->GetFeature(2));
  REQUIRE(flange);
  flange->length = kWall + 10.0;
  doc->Regenerate();
  ui::UpdateDrawingRendersNow(state);
  const drawing::SheetView& after = state.drawingRenders.at(blankId);
  CHECK_THAT(after.bboxMax.y - after.bboxMin.y, WithinAbs(before + 10.0, 1e-6));

  // An edit to the CONFIGURATION reaches it too. Editing one the document
  // does not have active changes no geometry, so it bumps no version — the
  // sheet still has to notice, or it keeps drawing the variant as it was and
  // reports itself up to date while doing it.
  Configuration edited = *doc->FindConfiguration(flatCfg);
  edited.unsuppressedFeatures.clear(); // no longer unfolds anything
  REQUIRE(doc->ReplaceConfiguration(edited));
  ui::UpdateDrawingRendersNow(state);
  CHECK(BendLines(state.drawingRenders.at(blankId)) == 0);
  edited.unsuppressedFeatures = {flatFeature};
  REQUIRE(doc->ReplaceConfiguration(edited));
  ui::UpdateDrawingRendersNow(state);
  CHECK(BendLines(state.drawingRenders.at(blankId)) == 1);

  // A configuration the part no longer has is an error on the view, not the
  // active configuration drawn in its place.
  REQUIRE(doc->RemoveConfiguration(flatCfg));
  ui::UpdateDrawingRendersNow(state);
  CHECK(state.drawingRenders.at(blankId).error.find("configuration") !=
        std::string::npos);
}

TEST_CASE("a dimension across the blank reads the developed length",
          "[sheetmetal][drawing]") {
  // The number a shop reads off the flat pattern. It goes the whole way
  // through — bend record, unfold, projection, dimension — and it is the one
  // that is expensive to get wrong: cut to the folded depth and every part
  // in the batch is short by the bend allowance.
  ui::AppState state = MakeState();
  const uint64_t flatCfg = FlangedPart(*state.document);
  const uint64_t part = state.parts[state.activePart].id;
  ui::AddDrawing(state, "Sheet");
  DrawingView blank;
  blank.sourceDocId = part;
  blank.orientation = ViewOrientation::Top;
  blank.configuration = flatCfg;
  blank.scale = 0.5; // not 1:1, so a sheet-mm answer cannot pass as model mm
  blank.position = {150.0, 150.0};
  const uint64_t viewId = state.drawing->AddView(blank);
  ui::UpdateDrawingRendersNow(state);
  const drawing::SheetView sv = state.drawingRenders.at(viewId);
  REQUIRE(sv.hasGeometry);

  // The blank's two ends: the outermost curves running across the view,
  // which are the edges a draughtsman would pick.
  const drawing::SheetCurve* low = nullptr;
  const drawing::SheetCurve* high = nullptr;
  for (const drawing::SheetCurve& c : sv.curves) {
    if (c.kind == drawing::CurveKind::Bend || !c.visible || !c.edge.IsSet())
      continue;
    if (c.points.size() != 2 || std::abs(c.points[0].y - c.points[1].y) > 1e-9)
      continue;
    if (!low || c.points[0].y < low->points[0].y)
      low = &c;
    if (!high || c.points[0].y > high->points[0].y)
      high = &c;
  }
  REQUIRE((low != nullptr && high != nullptr));

  DrawingDim dim;
  dim.kind = DrawingDimKind::DistanceY;
  dim.viewId = viewId;
  dim.refs.push_back(ProjRef{viewId, 0, low->edge, {}, 0});
  dim.refs.push_back(ProjRef{viewId, 0, high->edge, {}, 0});
  const drawing::ResolvedDim rd = ui::ResolveDrawingDim(state, dim);
  INFO(rd.text);
  REQUIRE(rd.resolved);
  // Model millimetres: the panel, the bend allowance, and the wall.
  CHECK_THAT(rd.value, WithinAbs(kD + Allowance() + kWall, 1e-6));
  // The folded part measures the panel and a wall's thickness instead, so
  // the two views do NOT read the same number — which is the whole point of
  // putting the blank on the sheet.
  CHECK(rd.value > kD + kWall);
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
  ui::UpdateDrawingRendersNow(state);

  DrawingNote table;
  table.table = NoteTable::Bends;
  table.viewId = viewId;
  table.pos = {150, 150};
  drawing::ResolvedNote rn = ui::ResolveDrawingNote(state, table);
  INFO(rn.error);
  REQUIRE(rn.resolved);
  CHECK(rn.text.rfind("BEND TABLE\nB1  UP 90", 0) == 0);
  // The numbers are the PART's, not a rounding of something near them: the
  // radius the flange used and the allowance the unfolder ate, formatted the
  // way the rest of the sheet formats lengths.
  const double radius = sheet::MinBendRadius("aluminum_6061_t6", kT);
  CHECK(rn.text.find("R " + units::FormatLength(radius, {})) !=
        std::string::npos);
  CHECK(rn.text.find("BA " + units::FormatLength(Allowance(), {})) !=
        std::string::npos);
  char k[32];
  std::snprintf(k, sizeof k, "K %.3f",
                sheet::KFactor("aluminum_6061_t6", radius, kT));
  CHECK(rn.text.find(k) != std::string::npos);
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
  CHECK(drawing::BendTableText(boxDoc, "", {0, 0, -1}).empty());
  DrawingNote free;
  free.table = NoteTable::Bends;
  rn = ui::ResolveDrawingNote(state, free);
  CHECK_FALSE(rn.resolved);
  CHECK(rn.error.find("view of a part") != std::string::npos);
}
