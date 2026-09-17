#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FlatPatternFeature.h"
#include "core/features/NormalCutFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/UnfoldFeature.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinRel;

// S10 (docs/sheetmetal/plan.md): a cut square to the metal wherever it lands.
namespace {

constexpr double kT = 1.5;
constexpr double kR = 2.25;
const double kPi = std::numbers::pi;

uint64_t Blank(Document& doc) {
  sheet::SheetMetalRules r;
  r.material = "aluminum";
  r.gauge = "1.5mm";
  r.thickness = kT;
  doc.SetSheetMetal(r);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {100, 0}, 1});
  sk->AddEntity(sketch::Line{{100, 0}, {100, 60}, 2});
  sk->AddEntity(sketch::Line{{100, 60}, {0, 60}, 3});
  sk->AddEntity(sketch::Line{{0, 60}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<BaseFlangeFeature>(
      "Base", doc.GetFeature(doc.FeatureCount() - 1)->GetId()));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// A 20 mm wall bent up from the top edge along y = 0.
void Wall(Document& doc, uint64_t base) {
  TopTools_IndexedMapOfShape edges;
  const Body& body = doc.GetBodies()[0];
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  std::string name;
  for (int i = 1; i <= edges.Extent() && name.empty(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    const gp_Pnt a = c.Value(c.FirstParameter());
    const gp_Pnt b = c.Value(c.LastParameter());
    if (c.GetType() == GeomAbs_Line && std::abs(a.Y()) < 1e-6 &&
        std::abs(b.Y()) < 1e-6 && std::abs(a.Z() - kT) < 1e-6 &&
        std::abs(b.Z() - kT) < 1e-6)
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        name = *n;
  }
  REQUIRE_FALSE(name.empty());
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Wall", std::vector<ElementRef>{ElementRef{base, name}}, 20.0, 90.0);
  f->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(f));
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
}

// A circle sketched on a plane through `origin` facing `normal`.
uint64_t Circle(Document& doc, const glm::vec3& origin, const glm::vec3& normal,
                const glm::vec3& u, glm::vec2 at, double r) {
  auto sk =
      std::make_unique<SketchFeature>("Hole", SketchPlane(origin, normal, u));
  sk->AddEntity(sketch::Circle{at, r, 7});
  doc.AddFeature(std::move(sk));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

const Feature& Last(Document& doc) {
  return *doc.GetFeature(doc.FeatureCount() - 1);
}

bool Inside(const Document& doc, const gp_Pnt& p) {
  BRepClass3d_SolidClassifier c(doc.GetBodies()[0].shape.GetShape(), p, 1e-6);
  return c.State() == TopAbs_IN;
}

bool Sound(const Document& doc) {
  return BRepCheck_Analyzer(doc.GetBodies()[0].shape.GetShape()).IsValid() &&
         doc.GetBodies()[0].shape.SolidCount() == 1;
}

} // namespace

TEST_CASE("a normal cut through a panel is a plain hole",
          "[sheetmetal][normalcut]") {
  Document doc;
  Blank(doc);
  const double before = doc.GetBodies()[0].shape.Volume();
  const uint64_t sk =
      Circle(doc, {0, 0, 5}, {0, 0, 1}, {1, 0, 0}, {50, 30}, 3.0);
  doc.AddFeature(std::make_unique<NormalCutFeature>("Cut", sk));
  INFO(Last(doc).GetError());
  REQUIRE(Last(doc).GetError().empty());
  CHECK(Sound(doc));
  // EXACTLY a hole: the profile goes onto the panel as drawn, so the circle
  // stays a circle. It used to be laid down as a sampled polygon — a 380-
  // sided pit that was within a tenth of a percent by volume and wrong in
  // every other way: 383 faces instead of 7, a DXF full of chords, and a
  // cylinder no downstream feature could recognise.
  CHECK_THAT(before - doc.GetBodies()[0].shape.Volume(),
             WithinRel(kPi * 9.0 * kT, 1e-9));
  const TopoDS_Shape& shape = doc.GetBodies()[0].shape.GetShape();
  int faces = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    ++faces;
  CHECK(faces == 7); // the blank's six, and the bore
  int circles = 0;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() == GeomAbs_Circle)
      ++circles;
  }
  CHECK(circles == 2); // the bore's rims, top and bottom
}

TEST_CASE("a normal cut makes every hole its sketch holds",
          "[sheetmetal][normalcut]") {
  // Three circles for three burner ports. Only the first was cut — silently,
  // so the part looked finished and two burners passed through solid steel,
  // which nothing but a clash sweep would ever have noticed.
  Document doc;
  Blank(doc);
  const double before = doc.GetBodies()[0].shape.Volume();
  auto sk = std::make_unique<SketchFeature>(
      "Ports",
      SketchPlane(glm::vec3(0, 0, 5), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Circle{{25, 30}, 4.0, 1});
  sk->AddEntity(sketch::Circle{{50, 30}, 4.0, 2});
  sk->AddEntity(sketch::Circle{{75, 30}, 4.0, 3});
  doc.AddFeature(std::move(sk));
  const uint64_t ports = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<NormalCutFeature>("Ports", ports));
  INFO(Last(doc).GetError());
  REQUIRE(Last(doc).GetError().empty());
  CHECK(Sound(doc));
  CHECK_THAT(before - doc.GetBodies()[0].shape.Volume(),
             WithinRel(3.0 * kPi * 16.0 * kT, 1e-9));
}

TEST_CASE("a normal cut through a bend is square to the metal",
          "[sheetmetal][normalcut]") {
  // Looking straight down on the bend, a 0.6 mm hole whose middle sits over
  // the inside of the bend at y = -1.5 — clear of the wall's footprint, which
  // starts at y = -2.25 and would otherwise be what the outline lands on.
  // Square to the metal, the hole is as wide along the bend at the OUTSIDE of
  // it as at the inside, on the same radial line; cut straight down, it is not.
  const double phi = std::asin(1.5 / kR);
  const gp_Dir radial(0.0, -std::sin(phi), -std::cos(phi));
  const gp_Pnt axis(50.45, 0.0, kT + kR);
  const gp_Pnt outside = axis.Translated(gp_Vec(radial) * (kR + kT - 0.05));
  const gp_Pnt insideFace = axis.Translated(gp_Vec(radial) * (kR + 0.05));

  SECTION("normal") {
    Document doc;
    const uint64_t base = Blank(doc);
    Wall(doc, base);
    REQUIRE(Inside(doc, outside));
    const uint64_t sk =
        Circle(doc, {0, 0, 30}, {0, 0, -1}, {1, 0, 0}, {50, 1.5}, 0.6);
    doc.AddFeature(std::make_unique<NormalCutFeature>("Cut", sk));
    INFO(Last(doc).GetError());
    REQUIRE(Last(doc).GetError().empty());
    CHECK(Sound(doc));
    CHECK_FALSE(Inside(doc, insideFace));
    CHECK_FALSE(Inside(doc, outside));
    // And the part still knows its bend, and still lays flat.
    REQUIRE(doc.GetBodies()[0].bends.size() == 1);
    CHECK_FALSE(doc.GetBodies()[0].bends[0].flat);
    doc.AddFeature(std::make_unique<FlatPatternFeature>("Flat", base));
    INFO(Last(doc).GetError());
    CHECK(Last(doc).GetError().empty());
  }
  SECTION("straight down, for contrast") {
    Document doc;
    const uint64_t base = Blank(doc);
    Wall(doc, base);
    const uint64_t sk =
        Circle(doc, {0, 0, 30}, {0, 0, -1}, {1, 0, 0}, {50, 1.5}, 0.6);
    auto cut =
        std::make_unique<ExtrudeFeature>("Cut", sk, 10.0, ExtrudeMode::Cut);
    cut->endMode = ExtrudeEnd::ThroughAll;
    doc.AddFeature(std::move(cut));
    REQUIRE(Last(doc).GetError().empty());
    CHECK_FALSE(Inside(doc, insideFace));
    CHECK(Inside(doc, outside)); // slanted: the outside of the bend is left
  }
}

TEST_CASE("a normal cut refuses what it cannot make",
          "[sheetmetal][normalcut]") {
  SECTION("an outline that lands on nothing") {
    Document doc;
    Blank(doc);
    const uint64_t sk =
        Circle(doc, {0, 0, 5}, {0, 0, 1}, {1, 0, 0}, {500, 500}, 3.0);
    doc.AddFeature(std::make_unique<NormalCutFeature>("Cut", sk));
    CHECK(Last(doc).GetError().find("does not land") != std::string::npos);
  }
  SECTION("an outline that lands on two parts of the sheet") {
    // Wide enough that, from above, part of it sees the top of the wall.
    Document doc;
    const uint64_t base = Blank(doc);
    Wall(doc, base);
    const uint64_t sk =
        Circle(doc, {0, 0, 30}, {0, 0, -1}, {1, 0, 0}, {50, 1.5}, 1.0);
    doc.AddFeature(std::make_unique<NormalCutFeature>("Cut", sk));
    CHECK(Last(doc).GetError().find("more than one part") != std::string::npos);
  }
  SECTION("a body lying flat") {
    Document doc;
    const uint64_t base = Blank(doc);
    Wall(doc, base);
    doc.AddFeature(std::make_unique<UnfoldFeature>("Unfold", base));
    REQUIRE(Last(doc).GetError().empty());
    const uint64_t sk =
        Circle(doc, {0, 0, 5}, {0, 0, 1}, {1, 0, 0}, {50, 30}, 3.0);
    doc.AddFeature(std::make_unique<NormalCutFeature>("Cut", sk));
    CHECK(Last(doc).GetError().find("refold them first") != std::string::npos);
  }
}
