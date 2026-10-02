// The literals lint and the extract_variables planner
// (docs/variables/README.md#literals-that-should-be-expressions).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/Literals.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"

#include <algorithm>

using Catch::Matchers::WithinAbs;

namespace {

const SketchPlane kXY(PlaneType::XY);

void AddRect(SketchFeature& sk, glm::dvec2 c1, glm::dvec2 c2) {
  glm::dvec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  sk.AddEntity(sketch::Line{a, b});
  sk.AddEntity(sketch::Line{b, c});
  sk.AddEntity(sketch::Line{c, d});
  sk.AddEntity(sketch::Line{d, a});
}

// A 20 x 20 plate on Top, extruded `thickness` up; returns the extrude's id.
uint64_t Plate(Document& doc, double thickness) {
  auto sk = std::make_unique<SketchFeature>("Plate sketch", kXY);
  AddRect(*sk, {0, 0}, {20, 20});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Plate", sketchId, thickness,
                                                  ExtrudeMode::NewBody));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

std::vector<literals::Variable> Vars(const Document& doc) {
  return literals::VariablesInScope(doc, nullptr, {&doc});
}

const literals::LiteralFinding* Finding(const literals::Report& r,
                                        const std::string& feature,
                                        const std::string& path) {
  for (const literals::LiteralFinding& f : r.literals)
    if (f.feature == feature && f.field.path == path)
      return &f;
  return nullptr;
}

} // namespace

TEST_CASE("a literal equal to a variable in scope is reported", "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("wall_r", "52"));
  Plate(doc, 52.0);
  const literals::Report r = literals::Lint(doc, Vars(doc));
  const literals::LiteralFinding* f = Finding(r, "Plate", "distance");
  REQUIRE(f != nullptr);
  CHECK(f->variables == std::vector<std::string>{"wall_r"});
  CHECK(f->field.quantity == literals::Quantity::Length);
}

TEST_CASE("the same field driven by the variable is not reported",
          "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("wall_r", "52"));
  Plate(doc, 52.0);
  auto* ext = dynamic_cast<ExtrudeFeature*>(doc.GetFeature(1));
  ext->distanceExpr = "wall_r";
  doc.Regenerate();
  CHECK(Finding(literals::Lint(doc, Vars(doc)), "Plate", "distance") ==
        nullptr);
}

TEST_CASE("a literal no variable equals is not reported", "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("wall_r", "50"));
  Plate(doc, 52.0);
  CHECK(literals::Lint(doc, Vars(doc)).literals.empty());
}

TEST_CASE("a count is not compared with a length variable", "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("wall_r", "52"));
  const uint64_t plate = Plate(doc, 10.0);
  auto pattern =
      std::make_unique<PatternFeature>("Teeth", PatternKind::Circular, plate);
  pattern->count = 52;
  doc.AddFeature(std::move(pattern));
  const literals::Report r = literals::Lint(doc, Vars(doc));
  CHECK(Finding(r, "Teeth", "count") == nullptr);

  // A variable USED as a count is a count, and a count literal equal to it
  // is reported.
  REQUIRE(doc.SetParameter("teeth", "52"));
  CHECK(Finding(literals::Lint(doc, Vars(doc)), "Teeth", "count") != nullptr);
}

TEST_CASE("a sketch placed on a world plane where a face lies is reported",
          "[literals]") {
  Document doc;
  const uint64_t plate = Plate(doc, 10.0);
  auto datum = std::make_unique<DatumPlaneFeature>("Deck");
  datum->offset = 10.0;
  doc.AddFeature(std::move(datum));
  const uint64_t datumId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto boss = std::make_unique<SketchFeature>("Boss sketch", kXY);
  boss->planeRef = datumId;
  AddRect(*boss, {5, 5}, {10, 10});
  doc.AddFeature(std::move(boss));
  const uint64_t bossSketch = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Boss", bossSketch, 5.0,
                                                  ExtrudeMode::Add));
  REQUIRE(doc.GetBodies().size() == 1);

  const literals::Report r = literals::Lint(doc, Vars(doc));
  REQUIRE(r.placements.size() == 1);
  const literals::PlacementFinding& p = r.placements[0];
  CHECK(p.feature == "Boss sketch");
  CHECK(p.on == "datum plane 'Deck'");
  CHECK(p.face.featureId == plate);
  CHECK(p.face.name == std::to_string(plate) + "/cap1");
  CHECK(p.faceMadeBy == plate);
  // The plate's own sketch is on Top, where the plate's bottom lies — but
  // that is the plane it was drawn on: there by construction, not reported.

  // Attached to the face instead, it is not reported.
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(3));
  sk->planeRef = 0;
  sk->faceRef = p.face;
  doc.Regenerate();
  REQUIRE(sk->GetError().empty());
  CHECK(literals::Lint(doc, Vars(doc)).placements.empty());
}

TEST_CASE("a placement whose offset follows a variable is not reported",
          "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("deck", "10"));
  Plate(doc, 10.0);
  auto datum = std::make_unique<DatumPlaneFeature>("Deck");
  datum->offsetExpr = "deck";
  doc.AddFeature(std::move(datum));
  const uint64_t datumId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto boss = std::make_unique<SketchFeature>("Boss sketch", kXY);
  boss->planeRef = datumId;
  AddRect(*boss, {5, 5}, {10, 10});
  doc.AddFeature(std::move(boss));
  CHECK(literals::Lint(doc, Vars(doc)).placements.empty());
}

TEST_CASE("the field walk names every expression-capable number",
          "[literals]") {
  auto sk = std::make_unique<SketchFeature>("S", kXY);
  const uint32_t circle = sk->AddEntity(sketch::Circle{{0, 0}, 5.0});
  const uint32_t point = sk->AddEntity(sketch::Point{{3, 4}});
  sketch::Constraint dia{};
  dia.kind = sketch::ConstraintKind::Diameter;
  dia.a = {circle, 0};
  dia.value = 10.0;
  sk->constraints.push_back(dia);
  const std::vector<literals::Field> fields = literals::Fields(*sk);
  const auto find = [&](const std::string& path) {
    const auto it =
        std::find_if(fields.begin(), fields.end(),
                     [&](const literals::Field& f) { return f.path == path; });
    return it == fields.end() ? nullptr : &*it;
  };
  const literals::Field* d = find("constraints/0");
  REQUIRE(d != nullptr);
  CHECK(d->label == "diameter dimension");
  CHECK(d->value == 10.0);
  // The circle's radius is fixed by that dimension: not drivable.
  const literals::Field* r = find("entities/0/r");
  REQUIRE(r != nullptr);
  CHECK_FALSE(r->notDrivable.empty());
  // The free point's coordinates are.
  const literals::Field* x = find("entities/1/x");
  REQUIRE(x != nullptr);
  CHECK(x->value == 3.0);
  CHECK(x->notDrivable.empty());
  (void)point;

  // A through-all extrude's distance means nothing.
  ExtrudeFeature through("Cut", 1, 7.0, ExtrudeMode::Cut);
  through.endMode = ExtrudeEnd::ThroughAll;
  for (const literals::Field& f : literals::Fields(through))
    if (f.path == "distance")
      CHECK_FALSE(f.notDrivable.empty());
}

TEST_CASE("an extraction plan drives the same geometry", "[literals]") {
  Document doc;
  Plate(doc, 12.5);
  const double volume = doc.GetBodies().at(0).shape.Volume();

  literals::ExtractOptions o;
  const literals::ExtractPlan plan = literals::PlanExtract(doc, Vars(doc), o);
  // The plate's distance; the rectangle's corners are coordinates too (the
  // nonzero ones), and the zeros are left alone with a reason.
  const auto distance = std::find_if(
      plan.proposals.begin(), plan.proposals.end(),
      [](const literals::Proposal& p) { return p.name == "plate_distance"; });
  REQUIRE(distance != plan.proposals.end());
  CHECK(distance->expression == "12.5");
  CHECK(std::any_of(plan.leftAlone.begin(), plan.leftAlone.end(),
                    [](const literals::LeftAlone& l) {
                      return l.reason.find("zero") != std::string::npos;
                    }));
  // Within one feature equal values share a variable: the rectangle's four
  // 20s are one.
  size_t twenties = 0;
  for (const literals::Proposal& p : plan.proposals)
    if (p.value == 20.0)
      twenties++;
  CHECK(twenties == 1);

  // Applied by hand the way ApplyExtractVariables does: geometry identical.
  for (const literals::Proposal& p : plan.proposals) {
    REQUIRE(doc.SetParameter(p.name, p.expression));
    for (const literals::Use& u : p.uses)
      REQUIRE(literals::SetFieldExpr(*doc.FindFeatureById(u.featureId),
                                     u.field.path, u.expression));
  }
  REQUIRE(doc.Regenerate());
  CHECK_THAT(doc.GetBodies().at(0).shape.Volume(), WithinAbs(volume, 1e-6));
  // And the variable drives it.
  REQUIRE(doc.SetParameter("plate_distance", "25"));
  CHECK_THAT(doc.GetBodies().at(0).shape.Volume(),
             WithinAbs(volume * 2.0, 1e-6));
}

TEST_CASE("an extraction refuses what it cannot name", "[literals]") {
  Document doc;
  Plate(doc, 10.0);
  literals::ExtractOptions o;
  o.featureIds = {999};
  CHECK_THROWS_WITH(literals::PlanExtract(doc, Vars(doc), o),
                    Catch::Matchers::ContainsSubstring("no feature with id"));
  o.featureIds = {doc.GetFeature(1)->GetId()};
  o.paths = {"radius"};
  CHECK_THROWS_WITH(literals::PlanExtract(doc, Vars(doc), o),
                    Catch::Matchers::ContainsSubstring("distance"));
}

TEST_CASE("merge_equal and reuse", "[literals]") {
  Document doc;
  REQUIRE(doc.SetParameter("thick", "10"));
  Plate(doc, 10.0);
  Plate(doc, 10.0);
  literals::ExtractOptions o;
  o.paths = {"distance"};
  CHECK(literals::PlanExtract(doc, Vars(doc), o).proposals.size() == 2);
  o.mergeEqual = true;
  CHECK(literals::PlanExtract(doc, Vars(doc), o).proposals.size() == 1);
  o.reuse = true;
  const literals::ExtractPlan plan = literals::PlanExtract(doc, Vars(doc), o);
  REQUIRE(plan.proposals.size() == 1);
  CHECK(plan.proposals[0].existing);
  CHECK(plan.proposals[0].name == "thick");
  CHECK(plan.proposals[0].uses.size() == 2);
}

TEST_CASE("a field spelled in degrees is named by its own key", "[literals]") {
  // A revolve writes `angle_deg` beside `angle_expr`: the path is the key
  // the number is under, and it is not "a number without a twin".
  RevolveFeature rev("Turn", 1, 90.0, ExtrudeMode::NewBody);
  const std::vector<literals::Field> fields = literals::Fields(rev);
  REQUIRE(fields.size() == 1);
  CHECK(fields[0].path == "angle_deg");
  CHECK(fields[0].quantity == literals::Quantity::Angle);
  CHECK(fields[0].value == 90.0);
  const std::vector<std::string> bare = literals::FieldsWithoutTwin(rev);
  CHECK(std::find(bare.begin(), bare.end(), "angle_deg") == bare.end());
  REQUIRE(literals::SetFieldExpr(rev, "angle_deg", "turn"));
  CHECK(rev.angleExpr == "turn");
}

TEST_CASE("a variable's comment says what it measures", "[literals]") {
  // The pan/tilt generator's `finger_n = 8` is a force ("N, a fingertip
  // pull"), and its `face_k` a width in modules: neither is the 8 mm hole
  // depth or the 6.7 mm frame height it happened to equal.
  Document doc;
  REQUIRE(doc.SetParameter("finger_n", "8", "N, a fingertip pull"));
  REQUIRE(doc.SetParameter("face_k", "10", "face width in modules"));
  Plate(doc, 8.0);
  Plate(doc, 10.0);
  CHECK(literals::Lint(doc, Vars(doc)).literals.empty());
  REQUIRE(doc.SetParameter("plate_t", "10", "mm, the plate"));
  CHECK(literals::Lint(doc, Vars(doc)).literals.size() == 1);
}
