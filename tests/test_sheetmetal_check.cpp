#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sheet/Check.h"

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <vector>

namespace {

constexpr double kT = 1.5;
constexpr double kW = 100.0;
constexpr double kD = 60.0;

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

std::string TopEdge(const Body& body) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.GetType() != GeomAbs_Line)
      continue;
    const gp_Pnt a = c.Value(c.FirstParameter());
    const gp_Pnt b = c.Value(c.LastParameter());
    if (std::abs(a.Y()) < 1e-6 && std::abs(b.Y()) < 1e-6 &&
        std::abs(a.Z() - kT) < 1e-6 && std::abs(b.Z() - kT) < 1e-6)
      if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
        return *n;
  }
  return {};
}

EdgeFlangeFeature* AddFlange(Document& doc, uint64_t base, double wall) {
  auto f = std::make_unique<EdgeFlangeFeature>(
      "Flange",
      std::vector<ElementRef>{ElementRef{base, TopEdge(doc.GetBodies()[0])}},
      wall, 90.0);
  f->position = FlangePosition::BendOutside;
  EdgeFlangeFeature* raw = f.get();
  doc.AddFeature(std::move(f));
  return raw;
}

bool Fired(const sheet::CheckResult& r, const std::string& kind) {
  return std::any_of(r.violations.begin(), r.violations.end(),
                     [&](const sheet::Violation& v) { return v.kind == kind; });
}

} // namespace

TEST_CASE("a part a brake can make fires nothing", "[sheetmetal][dfm]") {
  // A check nothing fails is not a check — and neither is one that fires on
  // work that is fine.
  Document doc;
  const uint64_t base = Blank(doc);
  AddFlange(doc, base, 20.0);
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  const sheet::CheckResult r =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  for (const sheet::Violation& v : r.violations)
    UNSCOPED_INFO(v.message);
  CHECK(r.violations.empty());
  CHECK(r.bendsChecked == 1);
  // And it says what it could NOT answer, so clean is never read as all-clear.
  CHECK_FALSE(r.notChecked.empty());
}

TEST_CASE("a wall too short for the die is caught", "[sheetmetal][dfm]") {
  Document doc;
  const uint64_t base = Blank(doc);
  AddFlange(doc, base, 4.0); // the die wants 4t + t = 7.5 mm
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  const sheet::CheckResult r =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  REQUIRE(Fired(r, "flange_below_minimum_height"));
  const sheet::Violation& v = *std::find_if(
      r.violations.begin(), r.violations.end(), [](const sheet::Violation& x) {
        return x.kind == "flange_below_minimum_height";
      });
  // The number, not just the noun: required-versus-actual is what makes a
  // finding something an agent can act on.
  CHECK_THAT(v.required, Catch::Matchers::WithinAbs(4.0 * kT + kT, 1e-9));
  CHECK(v.actual < v.required);
}

TEST_CASE("a bend line stopping inside a panel is caught",
          "[sheetmetal][dfm]") {
  Document doc;
  const uint64_t base = Blank(doc);
  EdgeFlangeFeature* f = AddFlange(doc, base, 20.0);
  f->offsetStart = 10.0;
  f->offsetEnd = 10.0;
  f->reliefStyle = sheet::ReliefStyle::None; // the user says there is nothing
  doc.Regenerate();
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  const sheet::CheckResult r =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  CHECK(Fired(r, "missing_bend_relief"));

  // Cut the relief and it goes quiet — the check is about the METAL, not
  // about what the feature was asked for.
  f->reliefStyle = sheet::ReliefStyle::Rectangular;
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  const sheet::CheckResult after =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  CHECK_FALSE(Fired(after, "missing_bend_relief"));
}

TEST_CASE("a hole too close to a bend is caught", "[sheetmetal][dfm]") {
  Document doc;
  const uint64_t base = Blank(doc);
  AddFlange(doc, base, 20.0);
  REQUIRE(doc.GetFeature(2)->GetError().empty());

  // A 6 mm hole whose edge sits 1 mm from the bend line (which runs along
  // y = 0). 1 mm is under the 1.5t = 2.25 mm a hole needs to survive the
  // metal moving.
  auto sk = std::make_unique<SketchFeature>(
      "Hole",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Circle{{50.0, 4.0}, 3.0, 9});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(3)->GetId();
  auto cut = std::make_unique<ExtrudeFeature>("Bore", sketchId, 10.0,
                                              ExtrudeMode::Cut);
  cut->endMode = ExtrudeEnd::ThroughAll;
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetFeature(4)->GetError().empty());

  const sheet::CheckResult r =
      sheet::Check(doc.GetBodies()[0], *doc.SheetMetal());
  CHECK(r.holesChecked > 0);
  REQUIRE(Fired(r, "hole_too_close_to_bend"));

  // Move it clear and it goes quiet.
  auto* holeSketch = dynamic_cast<SketchFeature*>(doc.GetFeature(3));
  REQUIRE(holeSketch != nullptr);
  holeSketch->entities[0] = sketch::Circle{{50.0, 20.0}, 3.0, 9};
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_FALSE(Fired(sheet::Check(doc.GetBodies()[0], *doc.SheetMetal()),
                    "hole_too_close_to_bend"));
}

TEST_CASE("a radius the material stopped taking is caught",
          "[sheetmetal][dfm]") {
  // A flange REFUSES a sub-minimum radius when it is made, so this can only
  // arrive by the material changing under a bend that was already there —
  // which is exactly why the check re-asks rather than trusting the feature.
  Document doc;
  const uint64_t base = Blank(doc);
  AddFlange(doc, base, 20.0);
  REQUIRE(doc.GetFeature(2)->GetError().empty());
  Body body = doc.GetBodies()[0];
  REQUIRE(body.bends.size() == 1);

  sheet::SheetMetalRules harder = AluRules();
  body.bends[0].radius = 1.0; // as if it had been bent in a softer alloy
  const sheet::CheckResult r = sheet::Check(body, harder);
  REQUIRE(Fired(r, "bend_below_minimum_radius"));
  const sheet::Violation& v = *std::find_if(
      r.violations.begin(), r.violations.end(), [](const sheet::Violation& x) {
        return x.kind == "bend_below_minimum_radius";
      });
  CHECK_THAT(v.required, Catch::Matchers::WithinAbs(1.5 * kT, 1e-9));
  CHECK(v.message.find("aluminum") != std::string::npos);
}
