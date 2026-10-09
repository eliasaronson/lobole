// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/SketchFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kThickness = 1.5;  // aluminum "1.5mm"
constexpr double kMinRadius = 2.25; // 1.5 * t for aluminum
constexpr double kWidth = 100.0;    // along X
constexpr double kDepth = 60.0;     // along Y

sheet::SheetMetalRules AluRules() {
  sheet::SheetMetalRules r;
  r.material = "aluminum";
  r.gauge = "1.5mm";
  r.thickness = kThickness;
  return r;
}

// A 100 x 60 blank on the XY plane, thickened upward into a sheet body.
uint64_t Blank(Document& doc) {
  doc.SetSheetMetal(AluRules());
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {kWidth, 0}, 1});
  sk->AddEntity(sketch::Line{{kWidth, 0}, {kWidth, kDepth}, 2});
  sk->AddEntity(sketch::Line{{kWidth, kDepth}, {0, kDepth}, 3});
  sk->AddEntity(sketch::Line{{0, kDepth}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<BaseFlangeFeature>("Base", sketchId));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The stable name of the edge running between `a` and `b` (either way round).
std::string EdgeNamed(const Body& body, const glm::dvec3& a,
                      const glm::dvec3& b) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  const auto near = [](const gp_Pnt& p, const glm::dvec3& q) {
    return std::abs(p.X() - q.x) < 1e-6 && std::abs(p.Y() - q.y) < 1e-6 &&
           std::abs(p.Z() - q.z) < 1e-6;
  };
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt p0 = c.Value(c.FirstParameter());
    const gp_Pnt p1 = c.Value(c.LastParameter());
    if ((near(p0, a) && near(p1, b)) || (near(p0, b) && near(p1, a))) {
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
    }
  }
  return {};
}

// The bend's own volume: the annular sector between R and R+t, swept `span`.
double BendVolume(double angle, double r, double t, double span) {
  return 0.5 * angle * ((r + t) * (r + t) - r * r) * span;
}

} // namespace

TEST_CASE("a base flange is the profile one gauge thick", "[sheetmetal]") {
  Document doc;
  Blank(doc);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetFeature(1)->GetError().empty());
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(kWidth * kDepth * kThickness, 1e-9));
  // It wears the sheet's material without being told, because the wall is
  // made OF the sheet.
  CHECK(doc.GetBodies()[0].material == "aluminum");
  CHECK(doc.GetBodies()[0].bends.empty());
}

TEST_CASE("a base flange refuses a part with no sheet-metal rules",
          "[sheetmetal]") {
  Document doc;
  Blank(doc);
  doc.SetSheetMetal(std::nullopt);
  // Refuse, don't guess: there is no thickness to fall back on.
  CHECK_FALSE(doc.GetFeature(1)->GetError().empty());
  CHECK(doc.GetFeature(1)->GetError().find("set_sheet_metal_rules") !=
        std::string::npos);
}

TEST_CASE("an edge flange bends a wall up and records the bend",
          "[sheetmetal]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
  REQUIRE_FALSE(edge.empty());

  const double wall = 20.0;
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, wall, 90.0);
  doc.AddFeature(std::move(flange));
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);

  // material_inside sets the bend line back by (R + t) at 90 degrees, so the
  // panel loses that strip and gains the bend plus the wall.
  const double quarter = std::numbers::pi / 2.0;
  const double setback = kMinRadius + kThickness;
  const double expected = kWidth * kDepth * kThickness    // the blank
                          - kWidth * setback * kThickness // set back
                          +
                          BendVolume(quarter, kMinRadius, kThickness, kWidth) +
                          wall * kThickness * kWidth; // the wall
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-4));

  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  const BendRegion& bend = doc.GetBodies()[0].bends[0];
  CHECK_THAT(bend.radius, WithinAbs(kMinRadius, 1e-9));
  CHECK_THAT(bend.angle, WithinAbs(quarter, 1e-9));
  // K is the table's, and says so — a bend whose K nobody can trace is a
  // bend nobody can argue with when the part comes back the wrong length.
  CHECK_THAT(bend.kFactor, WithinAbs(0.41, 1e-9)); // aluminum, R/t = 1.5
  CHECK(bend.kSource == "table:aluminum R/t<2");
  // The axis runs along the picked edge, one radius clear of the panel face
  // that was picked — and this edge is on the UNDERSIDE (z = 0), so the wall
  // folds down. Which of a rim's two edges you pick is the whole of how the
  // direction is chosen.
  CHECK_THAT(std::abs(bend.axisDir.x), WithinAbs(1.0, 1e-9));
  CHECK_THAT(bend.axisOrigin.z, WithinAbs(-kMinRadius, 1e-6));
  glm::dvec3 lo, hi;
  {
    glm::dvec3 l, h;
    doc.GetBodies()[0].shape.GetBoundingBox(l, h);
    lo = l;
    hi = h;
  }
  CHECK(lo.z < -kMinRadius); // the wall hangs below the blank
  // And both cylindrical faces of the bend are nameable afterwards.
  CHECK(bend.innerFace.IsSet());
  CHECK(bend.outerFace.IsSet());
}

TEST_CASE("the picked edge chooses which way the wall folds", "[sheetmetal]") {
  // Same rim, the other edge of it: the flange goes up instead of down. This
  // is the only control there is over bend direction, so it has to be exact.
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string top = EdgeNamed(doc.GetBodies()[0], {0, 0, kThickness},
                                    {kWidth, 0, kThickness});
  REQUIRE_FALSE(top.empty());
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, top}}, 20.0, 90.0));
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  CHECK_THAT(doc.GetBodies()[0].bends[0].axisOrigin.z,
             WithinAbs(kThickness + kMinRadius, 1e-6));
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  CHECK(hi.z > kThickness + kMinRadius);
  CHECK_THAT(static_cast<double>(lo.z), WithinAbs(0.0, 1e-6));
}

TEST_CASE("bend_outside leaves the panel its full size", "[sheetmetal]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
  flange->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(flange));
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  const double quarter = std::numbers::pi / 2.0;
  const double expected = kWidth * kDepth * kThickness // nothing set back
                          +
                          BendVolume(quarter, kMinRadius, kThickness, kWidth) +
                          20.0 * kThickness * kWidth;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("length_mode says which dimension length states", "[sheetmetal]") {
  // The same drawn number means three different walls. Getting this wrong is
  // a flange right by construction and wrong by a radius plus a thickness.
  const auto build = [](FlangeLengthMode mode) {
    Document doc;
    const uint64_t base = Blank(doc);
    const std::string edge =
        EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
    auto flange = std::make_unique<EdgeFlangeFeature>(
        "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
    flange->lengthMode = mode;
    flange->position = FlangePosition::BendOutside;
    doc.AddFeature(std::move(flange));
    REQUIRE(doc.GetFeature(2)->GetError().empty());
    return doc.GetBodies()[0].shape.Volume();
  };
  const double flat = build(FlangeLengthMode::Flat);
  const double outside = build(FlangeLengthMode::Outside);
  const double inside = build(FlangeLengthMode::Inside);
  // outside subtracts R + t of setback, inside subtracts R, so at 90 degrees
  // the three walls differ by exactly those amounts of material.
  CHECK_THAT(flat - outside,
             WithinRel((kMinRadius + kThickness) * kThickness * kWidth, 1e-4));
  CHECK_THAT(flat - inside, WithinRel(kMinRadius * kThickness * kWidth, 1e-4));
}

TEST_CASE("a flange refuses a radius the material cannot take",
          "[sheetmetal]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
  flange->radius = 0.5; // well under aluminium's 1.5t
  doc.AddFeature(std::move(flange));
  const std::string why = doc.GetFeature(2)->GetError();
  CHECK_FALSE(why.empty());
  CHECK(why.find("tighter") != std::string::npos);
}

TEST_CASE("a flange refuses a length that leaves no wall", "[sheetmetal]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 2.0, 90.0);
  flange->lengthMode = FlangeLengthMode::Outside; // 2 mm < R + t of setback
  doc.AddFeature(std::move(flange));
  CHECK_FALSE(doc.GetFeature(2)->GetError().empty());
}

TEST_CASE("an inset bend line gets its relief", "[sheetmetal]") {
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0});
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
  flange->position = FlangePosition::BendOutside;
  flange->offsetStart = 10.0;
  flange->offsetEnd = 10.0;
  doc.AddFeature(std::move(flange));
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  const double span = kWidth - 20.0;
  const double quarter = std::numbers::pi / 2.0;
  const sheet::SheetMetalRules rules = AluRules();
  const double w = rules.EffectiveReliefWidth();
  const double d = rules.EffectiveReliefDepth(kMinRadius);
  // Two relief slots are cut out of the panel, one at each end of the bend
  // line — without them the metal tears the panel beside it.
  const double expected = kWidth * kDepth * kThickness +
                          BendVolume(quarter, kMinRadius, kThickness, span) +
                          20.0 * kThickness * span - 2.0 * w * d * kThickness;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(expected, 1e-3));

  // `none` cuts nothing, and says so by leaving the material there.
  Document plain;
  const uint64_t base2 = Blank(plain);
  auto bare = std::make_unique<EdgeFlangeFeature>(
      "Flange",
      std::vector<ElementRef>{ElementRef{
          base2, EdgeNamed(plain.GetBodies()[0], {0, 0, 0}, {kWidth, 0, 0})}},
      20.0, 90.0);
  bare->position = FlangePosition::BendOutside;
  bare->offsetStart = 10.0;
  bare->offsetEnd = 10.0;
  bare->reliefStyle = sheet::ReliefStyle::None;
  plain.AddFeature(std::move(bare));
  REQUIRE(plain.GetFeature(2)->GetError().empty());
  CHECK_THAT(plain.GetBodies()[0].shape.Volume(),
             WithinRel(expected + 2.0 * w * d * kThickness, 1e-3));
}

TEST_CASE("a flange refuses a curved edge", "[sheetmetal]") {
  Document doc;
  doc.SetSheetMetal(AluRules());
  auto sk = std::make_unique<SketchFeature>(
      "Disc",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Circle{{0, 0}, 30.0, 1});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<BaseFlangeFeature>("Base", sketchId));
  const uint64_t base = doc.GetFeature(1)->GetId();
  REQUIRE(doc.GetBodies().size() == 1);

  // The disc's rim carries a straight seam edge as well as its two circles,
  // so pick the circle deliberately rather than whatever comes first.
  const Body& body = doc.GetBodies()[0];
  std::string edge;
  {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent() && edge.empty(); ++i) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      if (c.GetType() != GeomAbs_Circle)
        continue;
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        edge = *n;
    }
  }
  REQUIRE_FALSE(edge.empty());
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 10.0, 90.0));
  const std::string why = doc.GetFeature(2)->GetError();
  CHECK_FALSE(why.empty());
  CHECK(why.find("not straight") != std::string::npos);
}

TEST_CASE("a corner edge is not a panel edge", "[sheetmetal]") {
  // Where two rims meet, both faces are a thickness wide, both are flat and
  // perpendicular, and the edge lies in both: every other check passes.
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string corner =
      EdgeNamed(doc.GetBodies()[0], {0, 0, 0}, {0, 0, kThickness});
  REQUIRE_FALSE(corner.empty());
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, corner}}, 10.0, 90.0));
  const std::string why = doc.GetFeature(2)->GetError();
  CHECK(why.find("not the edge of a panel") != std::string::npos);
}

TEST_CASE("a set-back flange is one solid with its panel", "[sheetmetal]") {
  // The setback cut used to start a micron behind the tangent line, leaving a
  // gap no volume could see: 0.2 mm3 in 12 700, and two solids.
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge = EdgeNamed(doc.GetBodies()[0], {0, 0, kThickness},
                                     {kWidth, 0, kThickness});
  doc.AddFeature(std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0));
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  CHECK(doc.GetBodies()[0].shape.SolidCount() == 1);
}
