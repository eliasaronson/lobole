#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/RefoldFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/UnfoldFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Unfold / Refold (docs/sheetmetal/plan.md, S9). The headline is the
// involution the whole recorded-bend design exists to make testable:
// refold(unfold(x)) == x, to the micron, over a single bend, a tree and a
// chain. Then the reason the pair exists at all: a hole drawn FLAT across a
// crease, wrapped onto the bend, and laid flat again by the flat pattern as
// the same hole.
namespace {

constexpr double kT = 1.5;
constexpr double kR = 2.25;
constexpr double kK = 0.41;
constexpr double kW = 100.0;
constexpr double kD = 60.0;
const double kQuarter = std::numbers::pi / 2.0;

sheet::SheetMetalRules AluRules() {
  sheet::SheetMetalRules r;
  r.material = "aluminum";
  r.gauge = "1.5mm";
  r.thickness = kT;
  return r;
}

uint64_t Blank(Document& doc) {
  doc.SetSheetMetal(AluRules());
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {kW, 0}, 1});
  sk->AddEntity(sketch::Line{{kW, 0}, {kW, kD}, 2});
  sk->AddEntity(sketch::Line{{kW, kD}, {0, kD}, 3});
  sk->AddEntity(sketch::Line{{0, kD}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<BaseFlangeFeature>(
      "Base", doc.GetFeature(doc.FeatureCount() - 1)->GetId()));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

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
    if ((near(p0, a) && near(p1, b)) || (near(p0, b) && near(p1, a)))
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
  }
  return {};
}

uint64_t Flange(Document& doc, uint64_t base, const glm::dvec3& a,
                const glm::dvec3& b, double wall) {
  const std::string edge = EdgeNamed(doc.GetBodies()[0], a, b);
  REQUIRE_FALSE(edge.empty());
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, wall, 90.0);
  f->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(f));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// The top edge of the blank along y = `y`.
uint64_t FlangeAtY(Document& doc, uint64_t base, double y, double wall) {
  return Flange(doc, base, {0, y, kT}, {kW, y, kT}, wall);
}

uint64_t AddUnfold(Document& doc, uint64_t base,
                   std::vector<uint64_t> only = {}) {
  auto u = std::make_unique<UnfoldFeature>("Unfold", base);
  u->bendFeatures = std::move(only);
  doc.AddFeature(std::move(u));
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

void AddRefold(Document& doc, uint64_t unfold) {
  doc.AddFeature(std::make_unique<RefoldFeature>("Refold", unfold));
}

// A circle cut straight through whatever lies under it, from the XY plane.
void Bore(Document& doc, double x, double y, double r) {
  auto sk = std::make_unique<SketchFeature>(
      "Hole",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Circle{{x, y}, r, 99});
  doc.AddFeature(std::move(sk));
  auto cut = std::make_unique<ExtrudeFeature>(
      "Bore", doc.GetFeature(doc.FeatureCount() - 1)->GetId(), 10.0,
      ExtrudeMode::Cut);
  cut->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}

struct Snapshot {
  double volume = 0.0;
  glm::vec3 lo{0.0f}, hi{0.0f};
  std::vector<BendRegion> bends;
};

Snapshot Take(const Document& doc) {
  Snapshot s;
  const Body& b = doc.GetBodies()[0];
  s.volume = b.shape.Volume();
  b.shape.GetBoundingBox(s.lo, s.hi);
  s.bends = b.bends;
  return s;
}

void SameAs(const Snapshot& a, const Snapshot& b) {
  CHECK_THAT(b.volume, WithinRel(a.volume, 1e-6));
  for (int i = 0; i < 3; ++i) {
    CHECK_THAT(static_cast<double>(b.lo[i]),
               WithinAbs(static_cast<double>(a.lo[i]), 1e-4));
    CHECK_THAT(static_cast<double>(b.hi[i]),
               WithinAbs(static_cast<double>(a.hi[i]), 1e-4));
  }
  REQUIRE(a.bends.size() == b.bends.size());
  for (size_t i = 0; i < a.bends.size(); ++i) {
    INFO("bend " << i);
    CHECK_FALSE(b.bends[i].flat);
    CHECK(glm::length(a.bends[i].axisOrigin - b.bends[i].axisOrigin) < 1e-6);
    CHECK(glm::length(a.bends[i].axisDir - b.bends[i].axisDir) < 1e-9);
    CHECK(glm::length(a.bends[i].parentDir - b.bends[i].parentDir) < 1e-9);
    CHECK(glm::length(a.bends[i].upDir - b.bends[i].upDir) < 1e-9);
  }
}

double ThicknessOf(const Document& doc) {
  glm::vec3 lo, hi;
  doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
  return hi.z - lo.z;
}

bool Sound(const Document& doc) {
  return BRepCheck_Analyzer(doc.GetBodies()[0].shape.GetShape()).IsValid();
}

double Allowance() { return sheet::BendAllowance(kQuarter, kR, kK, kT); }

} // namespace

TEST_CASE("refold undoes unfold: one bend", "[sheetmetal][refold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  FlangeAtY(doc, base, 0.0, 20.0);
  const Snapshot folded = Take(doc);

  const uint64_t unfold = AddUnfold(doc, base);
  CHECK_THAT(ThicknessOf(doc), WithinAbs(kT, 1e-6));
  REQUIRE(doc.GetBodies()[0].bends[0].flat);
  CHECK(doc.GetBodies()[0].bends[0].unfoldedBy == unfold);

  AddRefold(doc, unfold);
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  SameAs(folded, Take(doc));
  CHECK(Sound(doc));
}

TEST_CASE("refold undoes unfold: a tree and a chain", "[sheetmetal][refold]") {
  SECTION("two flanges off one panel") {
    Document doc;
    const uint64_t base = Blank(doc);
    FlangeAtY(doc, base, 0.0, 20.0);
    FlangeAtY(doc, base, kD, 15.0);
    const Snapshot folded = Take(doc);
    const uint64_t unfold = AddUnfold(doc, base);
    CHECK_THAT(ThicknessOf(doc), WithinAbs(kT, 1e-6));
    AddRefold(doc, unfold);
    REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
    SameAs(folded, Take(doc));
  }
  SECTION("a flange on a flange") {
    Document doc;
    const uint64_t base = Blank(doc);
    FlangeAtY(doc, base, 0.0, 20.0);
    glm::vec3 lo, hi;
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    Flange(doc, base, {0, lo.y, hi.z}, {kW, lo.y, hi.z}, 10.0);
    const Snapshot folded = Take(doc);
    const uint64_t unfold = AddUnfold(doc, base);
    CHECK_THAT(ThicknessOf(doc), WithinAbs(kT, 1e-6));
    AddRefold(doc, unfold);
    REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
    SameAs(folded, Take(doc));
  }
}

TEST_CASE("an unfold can take only some bends", "[sheetmetal][refold]") {
  // The wall that stays up rides with its panel; its record has to ride too,
  // or the refold after it — and the flat pattern after that — work from a
  // bend that is no longer where the record says.
  Document doc;
  const uint64_t base = Blank(doc);
  const uint64_t first = FlangeAtY(doc, base, 0.0, 20.0);
  FlangeAtY(doc, base, kD, 15.0);
  const Snapshot folded = Take(doc);

  const uint64_t unfold = AddUnfold(doc, base, {first});
  const std::vector<BendRegion>& bends = doc.GetBodies()[0].bends;
  REQUIRE(bends.size() == 2);
  CHECK(bends[0].flat);
  CHECK_FALSE(bends[1].flat);
  CHECK(ThicknessOf(doc) > kT + 1.0); // the other wall still stands

  AddRefold(doc, unfold);
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  SameAs(folded, Take(doc));

  // And the flat pattern after the pair still lays both down.
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  CHECK_THAT(ThicknessOf(doc), WithinAbs(kT, 1e-6));
}

TEST_CASE("a hole drawn flat across a crease is wrapped onto the bend",
          "[sheetmetal][refold]") {
  // The whole reason the pair exists. A 3 mm hole in the middle of the band
  // the bend becomes, cut while flat, then folded up.
  Document doc;
  const uint64_t base = Blank(doc);
  FlangeAtY(doc, base, 0.0, 20.0);
  const double folded = doc.GetBodies()[0].shape.Volume();
  const uint64_t unfold = AddUnfold(doc, base);
  const double flatBefore = doc.GetBodies()[0].shape.Volume();

  const double r = 1.5;
  Bore(doc, 50.0, -Allowance() / 2.0, r);
  const double flatHole = doc.GetBodies()[0].shape.Volume();
  CHECK_THAT(flatBefore - flatHole,
             WithinRel(std::numbers::pi * r * r * kT, 1e-4));

  AddRefold(doc, unfold);
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  CHECK(Sound(doc));
  // Exactly what the neutral-axis map says a wrapped through-hole removes:
  // at radius r the band's length is scaled by r / (R + K t), so the plug is
  // A * t (2R + t) / 2 / (R + K t) — more than the flat hole's A * t, since
  // the outside of a bend is stretched further than the inside is squeezed.
  //
  // Measured at a tight integration tolerance: CADShape::Volume()'s default
  // is some 45 ppm loose on the ruled walls of a wrapped cut, which here is
  // half a cubic millimetre — larger than the thing being checked.
  const auto precise = [&] {
    GProp_GProps g;
    BRepGProp::VolumeProperties(doc.GetBodies()[0].shape.GetShape(), g, 1e-7);
    return g.Mass();
  };
  const double plug =
      std::numbers::pi * r * r * kT * (2.0 * kR + kT) / 2.0 / (kR + kK * kT);
  CHECK_THAT(folded - precise(), WithinAbs(plug, 0.01));

  // And laying it flat again gives back the SAME blank the hole was drawn
  // on — the wrap and the unwrap are one map.
  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(flatHole, 2e-3));
  CHECK_THAT(ThicknessOf(doc), WithinAbs(kT, 1e-6));
}

TEST_CASE("a hole straddling a crease comes back in one piece",
          "[sheetmetal][refold]") {
  // Half on the panel, half on the band: the panel half rides rigidly, the
  // band half is wrapped, and the two have to meet at the tangent line.
  Document doc;
  const uint64_t base = Blank(doc);
  FlangeAtY(doc, base, 0.0, 20.0);
  const uint64_t unfold = AddUnfold(doc, base);
  Bore(doc, 30.0, 0.0, 1.5);
  AddRefold(doc, unfold);
  INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  CHECK(Sound(doc));
  CHECK(doc.GetBodies()[0].bends.size() == 1);
  CHECK_FALSE(doc.GetBodies()[0].bends[0].flat);
}

TEST_CASE("a pocket that does not go through is refused across a crease",
          "[sheetmetal][refold]") {
  // It has no outline the neutral-axis map can carry, and mapping it anyway
  // would quietly turn it into a hole.
  Document doc;
  const uint64_t base = Blank(doc);
  FlangeAtY(doc, base, 0.0, 20.0);
  const uint64_t unfold = AddUnfold(doc, base);

  auto sk = std::make_unique<SketchFeature>(
      "Pocket",
      SketchPlane(glm::vec3(0, 0, kT), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Circle{{50.0, -Allowance() / 2.0}, 1.5, 7});
  doc.AddFeature(std::move(sk));
  auto cut = std::make_unique<ExtrudeFeature>(
      "Pocket cut", doc.GetFeature(doc.FeatureCount() - 1)->GetId(), 0.5,
      ExtrudeMode::Cut, /*reversed=*/true);
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());

  AddRefold(doc, unfold);
  const std::string why = doc.GetFeature(doc.FeatureCount() - 1)->GetError();
  CHECK_FALSE(why.empty());
  CHECK(why.find("through") != std::string::npos);
}

TEST_CASE("a refold with nothing to fold says so", "[sheetmetal][refold]") {
  Document doc;
  const uint64_t base = Blank(doc);
  FlangeAtY(doc, base, 0.0, 20.0);
  const uint64_t unfold = AddUnfold(doc, base);
  AddRefold(doc, unfold);
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  AddRefold(doc, unfold); // twice
  CHECK(doc.GetFeature(doc.FeatureCount() - 1)
            ->GetError()
            .find("already refolded") != std::string::npos);
}

TEST_CASE("the panel beside an inset flange stays in the blank",
          "[sheetmetal][refold]") {
  // The bend's slab is bounded along its axis; before it was, the panel
  // beside an inset bend line sat on the far side of the tangent plane and
  // was silently left out of the blank.
  Document doc;
  const uint64_t base = Blank(doc);
  const std::string edge =
      EdgeNamed(doc.GetBodies()[0], {0, 0, kT}, {kW, 0, kT});
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flange", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
  f->position = FlangePosition::MaterialInside; // sets the bend line back
  f->offsetStart = 10.0;
  f->offsetEnd = 10.0;
  f->reliefStyle = sheet::ReliefStyle::None;
  doc.AddFeature(std::move(f));
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
  INFO(doc.GetFeature(3)->GetError());
  REQUIRE(doc.GetFeature(3)->GetError().empty());
  // The panel is set back (R + t) over the 80 mm span only; the 10 mm either
  // side keeps its full depth.
  const double span = kW - 20.0;
  const double setback = kR + kT;
  const double area =
      kW * kD - span * setback + span * Allowance() + span * 20.0;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(area * kT, 1e-4));
}
