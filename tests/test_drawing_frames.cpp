// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The pan/tilt round's drawings (E): feature control frames on datum
// features, values written along their lines, a placed Ø across two walls,
// balloons laid out so no two leaders cross, and the v151 golden
// (docs/drawings/README.md#feature-control-frames).
#include "TestPaths.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/drawing/DrawingBalloons.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingRender.h"
#include "core/drawing/DrawingValidate.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr double kHalfPi = 1.5707963267948966;

// Two horizontal lines 30 sheet mm apart and two vertical ones 40 apart:
// a rectangle's four sides, each its own edge.
drawing::SheetView Rectangle() {
  drawing::SheetView v;
  v.viewId = 1;
  v.hasGeometry = true;
  const auto edge = [&](const char* name, glm::dvec2 a, glm::dvec2 b) {
    drawing::SheetCurve c;
    c.edge = ElementRef{1, name};
    c.sourceId = 1;
    c.curveType = 0;
    c.points = {a, b};
    v.curves.push_back(c);
  };
  edge("bottom", {100, 100}, {140, 100});
  edge("top", {100, 130}, {140, 130});
  edge("left", {100, 100}, {100, 130});
  edge("right", {140, 100}, {140, 130});
  v.bboxMin = {100, 100};
  v.bboxMax = {140, 130};
  return v;
}

ProjRef Edge(const char* name) {
  ProjRef r;
  r.viewId = 1;
  r.edge = ElementRef{1, name};
  return r;
}

} // namespace

TEST_CASE("A frame takes only what its symbol can carry", "[drawing][fcf]") {
  FeatureControlFrame f;
  f.symbol = GeoSymbol::Perpendicularity;
  f.tolerance = 0.02;
  f.diameterZone = true;
  f.datums = {"A"};
  CHECK(ValidateFcf(f).empty());
  // Orientation is measured FROM something.
  f.datums.clear();
  CHECK_THAT(ValidateFcf(f), ContainsSubstring("measured from a datum"));
  // Form is not.
  FeatureControlFrame flat;
  flat.symbol = GeoSymbol::Flatness;
  flat.tolerance = 0.05;
  CHECK(ValidateFcf(flat).empty());
  flat.datums = {"A"};
  CHECK_THAT(ValidateFcf(flat), ContainsSubstring("takes no datum"));
  flat.datums.clear();
  flat.diameterZone = true;
  CHECK_THAT(ValidateFcf(flat), ContainsSubstring("no Ø"));
  // Runout has no material condition.
  FeatureControlFrame runout;
  runout.symbol = GeoSymbol::CircularRunout;
  runout.tolerance = 0.03;
  runout.datums = {"B"};
  CHECK(ValidateFcf(runout).empty());
  runout.modifier = MaterialModifier::Mmc;
  CHECK_THAT(ValidateFcf(runout), ContainsSubstring("material modifier"));
  // Datum letters: one capital, not I, O or Q, each once, at most three.
  FeatureControlFrame pos;
  pos.symbol = GeoSymbol::Position;
  pos.tolerance = 0.2;
  pos.datums = {"A", "O"};
  CHECK_THAT(ValidateFcf(pos), ContainsSubstring("I, O and Q"));
  pos.datums = {"A", "A"};
  CHECK_THAT(ValidateFcf(pos), ContainsSubstring("named twice"));
  pos.datums = {"A", "B", "C", "D"};
  CHECK_THAT(ValidateFcf(pos), ContainsSubstring("at most three"));
  pos.datums = {};
  CHECK(ValidateFcf(pos).empty()); // position may stand alone
  pos.tolerance = 0.0;
  CHECK_THAT(ValidateFcf(pos), ContainsSubstring("wider than zero"));
  // An unknown symbol is refused naming every one there is.
  GeoSymbol s;
  CHECK_FALSE(GeoSymbolFromString("perpendicular", s));
  CHECK_THAT(UnknownGeoSymbol("perpendicular"),
             ContainsSubstring("perpendicularity"));
  CHECK_THAT(UnknownGeoSymbol("perpendicular"),
             ContainsSubstring("total_runout"));
  MaterialModifier m;
  CHECK_FALSE(MaterialModifierFromString("max", m));
}

TEST_CASE("A frame is drawn as its compartments, with its leader",
          "[drawing][fcf]") {
  drawing::ResolvedNote n;
  n.style = NoteStyle::Fcf;
  n.fcf.symbol = GeoSymbol::Position;
  n.fcf.tolerance = 0.2;
  n.fcf.diameterZone = true;
  n.fcf.modifier = MaterialModifier::Mmc;
  n.fcf.datums = {"A", "B"};
  n.fcfTolerance = "0.2";
  n.pos = {200, 100};
  n.hasLeader = true;
  n.anchor = {150, 80};
  const drawing::SheetGraphics g = drawing::NoteGraphics(n);
  std::vector<std::string> texts;
  for (const auto& t : g.texts)
    texts.push_back(t.text);
  CHECK(texts == std::vector<std::string>{"\xC3\x98"
                                          "0.2",
                                          "M", "A", "B"});
  REQUIRE(g.knockouts.size() == 1);
  // The box is the frame: as tall as two lines, and centred on `pos`.
  glm::dvec2 mn, mx;
  drawing::NoteBox(n, mn, mx);
  CHECK(mx.y - mn.y == Approx(7.0));
  CHECK(0.5 * (mn.x + mx.x) == Approx(200.0));
  const glm::dvec2 size = drawing::FcfSize(n.fcf, n.fcfTolerance);
  CHECK(mx.x - mn.x == Approx(size.x));
  // The leader leaves the end nearer the feature and ends in an arrow on
  // it.
  bool arrowOnFeature = false;
  for (const auto& f : g.fills)
    for (const glm::dvec2& p : f)
      arrowOnFeature = arrowOnFeature || glm::length(p - n.anchor) < 1e-9;
  CHECK(arrowOnFeature);
  bool fromLeftEnd = false;
  for (const auto& l : g.lines)
    fromLeftEnd =
        fromLeftEnd || (std::abs(l.a.x - mn.x) < 1e-9 &&
                        std::abs(l.a.y - 100.0) < 1e-9 && l.b.x < mn.x);
  CHECK(fromLeftEnd);
  // Each symbol draws something: the PDF's font has none of them.
  for (GeoSymbol sym :
       {GeoSymbol::Straightness, GeoSymbol::Flatness, GeoSymbol::Circularity,
        GeoSymbol::Cylindricity, GeoSymbol::ProfileLine,
        GeoSymbol::ProfileSurface, GeoSymbol::Parallelism,
        GeoSymbol::Perpendicularity, GeoSymbol::Angularity, GeoSymbol::Position,
        GeoSymbol::Coaxiality, GeoSymbol::Symmetry, GeoSymbol::CircularRunout,
        GeoSymbol::TotalRunout}) {
    drawing::ResolvedNote one = n;
    one.fcf.symbol = sym;
    one.hasLeader = false;
    one.fcf.modifier = MaterialModifier::None;
    // The frame alone is 4 outline lines, 3 separators: more is the glyph.
    CHECK(drawing::NoteGraphics(one).lines.size() > 7);
  }
}

TEST_CASE("A linear value is written along its line, readable from the "
          "bottom or the right",
          "[drawing][dimension]") {
  const drawing::SheetView view = Rectangle();
  // Across the rectangle's height: a vertical dimension line.
  DrawingDim h;
  h.kind = DrawingDimKind::DistanceY;
  h.viewId = 1;
  h.refs = {Edge("bottom"), Edge("top")};
  h.labelOffset = {-10, 0};
  const auto rh = drawing::ResolveDim(h, view);
  REQUIRE(rh.resolved);
  CHECK(rh.aligned);
  CHECK(rh.textAngle == Approx(kHalfPi)); // reads upward: from the right
  CHECK(rh.dim1.x == Approx(110.0));      // the edges' middles, 10 to the left
  // Above its line is to its LEFT when it reads upward; clear of it.
  glm::dvec2 mn, mx;
  drawing::DimLabelBox(rh, mn, mx);
  CHECK(mx.x < 110.0);
  CHECK(mx.y - mn.y > mx.x - mn.x); // a tall box: the text turned
  CHECK(0.5 * (mn.y + mx.y) == Approx(115.0));

  // Across its width: horizontal, the value above the line.
  DrawingDim w;
  w.kind = DrawingDimKind::DistanceX;
  w.viewId = 1;
  w.refs = {Edge("left"), Edge("right")};
  w.labelOffset = {0, -10};
  const auto rw = drawing::ResolveDim(w, view);
  REQUIRE(rw.resolved);
  CHECK(rw.textAngle == Approx(0.0));
  drawing::DimLabelBox(rw, mn, mx);
  CHECK(mn.y > rw.dim1.y);
  CHECK(0.5 * (mn.x + mx.x) == Approx(120.0));

  // A line drawn right to left, or downward, still reads from the bottom
  // or the right: the turn lies in (-90, 90].
  glm::dvec2 c;
  double a = 0.0;
  drawing::AlignedLabel({50, 50}, {0, 50}, c, a);
  CHECK(a == Approx(0.0).margin(1e-12));
  CHECK(c.y > 50.0);
  drawing::AlignedLabel({0, 50}, {0, 0}, c, a);
  CHECK(a == Approx(kHalfPi));
  drawing::AlignedLabel({0, 0}, {-10, -10}, c, a);
  CHECK(a == Approx(kHalfPi / 2));

  // The exporters turn it the same way.
  DrawingDocument doc;
  doc.GetSheet().size = SheetSize::A3;
  const fs::path svg = fs::temp_directory_path() / "aligned.svg";
  const fs::path pdf = fs::temp_directory_path() / "aligned.pdf";
  REQUIRE(DrawingExport::SaveSVG(doc, {view}, {rh, rw}, svg.string(), nullptr));
  REQUIRE(DrawingExport::SavePDF(doc, {view}, {rh, rw}, pdf.string(), nullptr));
  std::stringstream s1, s2;
  s1 << std::ifstream(svg).rdbuf();
  s2 << std::ifstream(pdf).rdbuf();
  CHECK_THAT(s1.str(), ContainsSubstring("rotate(-90 "));
  CHECK_THAT(s2.str(), ContainsSubstring(" Tm ("));
  fs::remove(svg);
  fs::remove(pdf);
}

TEST_CASE("A Ø across two walls is placed beyond the view along them",
          "[drawing][dimension]") {
  drawing::SheetView view;
  view.viewId = 1;
  view.hasGeometry = true;
  for (double x : {84.0, 116.0}) {
    drawing::SheetCurve wall;
    wall.silFace = ElementRef{1, "bore"};
    wall.sourceId = 1;
    wall.curveType = 0;
    wall.points = {{x, 50}, {x, 70}};
    view.curves.push_back(wall);
  }
  view.bboxMin = {60, 40};
  view.bboxMax = {140, 70};
  DrawingDim dim;
  dim.kind = DrawingDimKind::Diameter;
  dim.viewId = 1;
  ProjRef r;
  r.viewId = 1;
  r.silFace = ElementRef{1, "bore"};
  dim.refs = {r};
  drawing::DimPlaceContext ctx;
  ctx.borderMin = {10, 10};
  ctx.borderMax = {410, 287};
  ctx.avoid.push_back({view.bboxMin, view.bboxMax});
  const glm::dvec2 off = drawing::PlaceDimOffset(dim, view, ctx);
  // Straight along the walls, not a radial leader off a midline.
  CHECK(off.x == Approx(0.0).margin(1e-9));
  dim.labelOffset = off;
  const auto rd = drawing::ResolveDim(dim, view);
  REQUIRE(rd.resolved);
  // The walls reach the view's top, so the line goes above it.
  CHECK(rd.dim1.y > view.bboxMax.y);
  glm::dvec2 mn, mx;
  drawing::DimLabelBox(rd, mn, mx);
  CHECK(mn.y > view.bboxMax.y);
}

TEST_CASE("Balloons go round the view with no two leaders crossing",
          "[drawing][balloons]") {
  CHECK(drawing::SegmentsCross({0, 0}, {10, 10}, {0, 10}, {10, 0}));
  CHECK_FALSE(drawing::SegmentsCross({0, 0}, {10, 0}, {0, 1}, {10, 1}));
  CHECK_FALSE(drawing::SegmentsCross({0, 0}, {10, 0}, {10, 0}, {10, 10}));

  // Twelve parts in a 100 mm view, their anchors in an order that a column
  // per side would tangle: alternating near the left and right edges.
  drawing::BalloonLayoutInput in;
  in.viewMin = {150, 100};
  in.viewMax = {250, 200};
  in.borderMin = {10, 10};
  in.borderMax = {410, 287};
  in.avoid.push_back({{250, 10}, {410, 58}}); // a title block
  in.avoid.push_back({in.viewMin, in.viewMax});
  for (int i = 0; i < 12; ++i) {
    drawing::BalloonItem item;
    const double y = 105.0 + 8.0 * i;
    const double x = (i % 2 == 0) ? 245.0 - 3.0 * i : 155.0 + 3.0 * i;
    item.anchors.push_back({{x, y}, 0.0});
    item.anchors.push_back({{200.0, y + 1.0}, 2.0});
    in.items.push_back(item);
  }
  const drawing::BalloonLayout out = drawing::LayoutBalloons(in);
  INFO(out.error);
  REQUIRE(out.error.empty());
  CHECK(out.crossings == 0);
  std::vector<std::pair<glm::dvec2, glm::dvec2>> leaders;
  for (size_t i = 0; i < in.items.size(); ++i) {
    const auto& p = out.placements[i];
    REQUIRE(p.placed);
    const double r = in.items[i].radius;
    // Off the view, off the title block, inside the border.
    CHECK((p.centre.x + r < in.viewMin.x || p.centre.x - r > in.viewMax.x ||
           p.centre.y + r < in.viewMin.y || p.centre.y - r > in.viewMax.y));
    CHECK_FALSE((p.centre.x + r > 250 && p.centre.y - r < 58));
    leaders.push_back(
        drawing::BalloonLeader(p.centre, r, in.items[i].anchors[p.anchor].at));
  }
  for (size_t i = 0; i < leaders.size(); ++i)
    for (size_t j = i + 1; j < leaders.size(); ++j)
      CHECK_FALSE(drawing::SegmentsCross(leaders[i].first, leaders[i].second,
                                         leaders[j].first, leaders[j].second));
  // Balloons do not sit on each other.
  for (size_t i = 0; i < leaders.size(); ++i)
    for (size_t j = i + 1; j < leaders.size(); ++j)
      CHECK(glm::length(out.placements[i].centre - out.placements[j].centre) >=
            2.0 * in.items[i].radius);

  // A sheet with no room says how many it could take.
  drawing::BalloonLayoutInput cramped = in;
  cramped.borderMin = {140, 90};
  cramped.borderMax = {260, 210};
  CHECK_THAT(drawing::LayoutBalloons(cramped).error,
             ContainsSubstring("room round this view"));
}

TEST_CASE("check_drawing names crossing balloon leaders",
          "[drawing][balloons]") {
  DrawingDocument doc;
  doc.GetSheet().size = SheetSize::A3;
  std::vector<drawing::ResolvedNote> notes(2);
  for (int i = 0; i < 2; ++i) {
    notes[i].id = 10 + i;
    notes[i].style = NoteStyle::Balloon;
    notes[i].text = std::to_string(i + 1);
    notes[i].hasLeader = true;
  }
  notes[0].pos = {100, 200};
  notes[0].anchor = {200, 100};
  notes[1].pos = {100, 100};
  notes[1].anchor = {200, 200};
  const auto issues = drawing::ValidateDrawing(doc, {}, {}, {}, notes);
  CHECK(std::any_of(issues.begin(), issues.end(), [](const auto& i) {
    return i.kind == "balloon_leaders_cross";
  }));
  notes[1].anchor = {200, 90};
  notes[1].pos = {100, 90};
  const auto clean = drawing::ValidateDrawing(doc, {}, {}, {}, notes);
  CHECK(std::none_of(clean.begin(), clean.end(), [](const auto& i) {
    return i.kind == "balloon_leaders_cross";
  }));
}

TEST_CASE("Drawing golden loads and round-trips (v151 frames and details)",
          "[drawing][serialization][golden]") {
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden" /
                          "drawing" / "v151-frames-details.json";
  REQUIRE(fs::exists(golden));
  auto dwg = testio::LoadDrawing(golden.string());
  REQUIRE(dwg != nullptr);
  const auto check = [](const DrawingDocument& d) {
    REQUIRE(d.ViewCount() == 3);
    const DrawingView& detail = d.GetViews()[2];
    REQUIRE(detail.detail.on);
    CHECK(detail.detail.parentViewId == 2);
    CHECK(detail.detail.radius == Approx(6.0));
    CHECK(detail.detail.label == "B");
    CHECK(detail.detail.center.z == Approx(24.7));
    REQUIRE(d.NoteCount() == 4);
    const DrawingNote& perp = d.GetNotes()[2];
    CHECK(perp.style == NoteStyle::Fcf);
    CHECK(perp.fcf.symbol == GeoSymbol::Perpendicularity);
    CHECK(perp.fcf.tolerance == Approx(0.02));
    CHECK(perp.fcf.diameterZone);
    CHECK(perp.fcf.datums == std::vector<std::string>{"A"});
    const DrawingNote& pos = d.GetNotes()[3];
    CHECK(pos.fcf.symbol == GeoSymbol::Position);
    CHECK(pos.fcf.modifier == MaterialModifier::Mmc);
    CHECK(pos.fcf.datums == std::vector<std::string>{"A", "B"});
    CHECK(pos.attachDim == 7);
  };
  check(*dwg);
  const fs::path tmp = fs::temp_directory_path() / "dwg_v151_rt.json";
  REQUIRE(testio::SaveDrawing(*dwg, tmp.string()));
  auto reloaded = testio::LoadDrawing(tmp.string());
  REQUIRE(reloaded != nullptr);
  check(*reloaded);
  fs::remove(tmp);

  // A symbol, a modifier or a frame this build would not write is refused,
  // not drawn as something else.
  std::ifstream in(golden);
  std::stringstream ss;
  ss << in.rdbuf();
  for (const auto& [from, to] :
       std::vector<std::pair<std::string, std::string>>{
           {"\"perpendicularity\"", "\"perpendicular\""},
           {"\"mmc\"", "\"max\""},
           {"\"perpendicularity\"", "\"flatness\""}, // flatness takes no datum
           {"\"radius_mm\": 6.0", "\"radius_mm\": 0.0"}}) {
    std::string text = ss.str();
    REQUIRE(text.find(from) != std::string::npos);
    text.replace(text.find(from), from.size(), to);
    const fs::path bad = fs::temp_directory_path() / "dwg_v151_bad.json";
    std::ofstream(bad) << text;
    CHECK(testio::LoadDrawing(bad.string()) == nullptr);
    fs::remove(bad);
  }
}
