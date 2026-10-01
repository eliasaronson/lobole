#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <memory>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
double PolylineLength(const std::vector<glm::dvec3>& p) {
  double len = 0.0;
  for (size_t i = 1; i < p.size(); i++)
    len += glm::length(p[i] - p[i - 1]);
  return len;
}
CurveFeature* AddCurve(Document& doc, std::unique_ptr<CurveFeature> cv) {
  CurveFeature* raw = cv.get();
  doc.AddFeature(std::move(cv));
  return raw;
}
} // namespace

TEST_CASE("3D fit-spline interpolates through its points", "[curve3d]") {
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Fit", Curve3DKind::FitSpline);
  cv->points = {{0, 0, 0}, {1, 2, 0}, {2, 0, 1}, {3, 3, 3}};
  CurveFeature* c = AddCurve(doc, std::move(cv));
  REQUIRE(doc.Regenerate());
  REQUIRE(c->GetError().empty());
  const auto& poly = c->Polyline();
  REQUIRE(poly.size() >= 2);
  // A fit spline passes through its first and last fit points exactly.
  CHECK(glm::length(poly.front() - glm::dvec3(0, 0, 0)) < 1e-4);
  CHECK(glm::length(poly.back() - glm::dvec3(3, 3, 3)) < 1e-4);
  CHECK_FALSE(c->EdgeShape().IsValid() == false); // built a real edge
}

TEST_CASE("Helix has the expected height and length", "[curve3d]") {
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Coil", Curve3DKind::Helix);
  cv->axisOrigin = {0, 0, 0};
  cv->axisDir = {0, 0, 1};
  cv->radius = 2.0;
  cv->pitch = 1.0;
  cv->turns = 3.0;
  CurveFeature* c = AddCurve(doc, std::move(cv));
  REQUIRE(doc.Regenerate());
  REQUIRE(c->GetError().empty());
  const auto& poly = c->Polyline();
  REQUIRE(poly.size() >= 2);
  // Starts on the reference X direction, one radius out.
  CHECK(glm::length(poly.front() - glm::dvec3(2, 0, 0)) < 1e-3);
  // Total rise = turns * pitch.
  CHECK_THAT(poly.back().z, WithinRel(3.0, 1e-2));
  // Length = turns * sqrt((2*pi*r)^2 + pitch^2).
  const double perTurn = std::sqrt(std::pow(2.0 * M_PI * 2.0, 2) + 1.0);
  CHECK_THAT(PolylineLength(poly), WithinRel(3.0 * perTurn, 2e-2));
}

TEST_CASE("Left-handed helix winds the opposite way", "[curve3d]") {
  auto build = [](bool rh) {
    Document doc;
    auto cv = std::make_unique<CurveFeature>("H", Curve3DKind::Helix);
    cv->radius = 1.0;
    cv->pitch = 4.0;
    cv->turns = 0.25; // quarter turn: y-sign reveals handedness
    cv->rightHanded = rh;
    CurveFeature* c = AddCurve(doc, std::move(cv));
    doc.Regenerate();
    return c->Polyline().back();
  };
  // After a quarter turn: RH goes to +Y, LH to -Y (both rise in +Z).
  CHECK(build(true).y > 0.5);
  CHECK(build(false).y < -0.5);
}

TEST_CASE("Control-poly spline is clamped to its end poles", "[curve3d]") {
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Ctrl", Curve3DKind::ControlPoly);
  cv->points = {{0, 0, 0}, {1, 3, 0}, {4, 3, 2}, {5, 0, 2}};
  cv->degree = 3;
  CurveFeature* c = AddCurve(doc, std::move(cv));
  REQUIRE(doc.Regenerate());
  REQUIRE(c->GetError().empty());
  const auto& poly = c->Polyline();
  REQUIRE(poly.size() >= 2);
  CHECK(glm::length(poly.front() - glm::dvec3(0, 0, 0)) < 1e-4);
  CHECK(glm::length(poly.back() - glm::dvec3(5, 0, 2)) < 1e-4);
}

TEST_CASE("Degenerate curves fail cleanly", "[curve3d]") {
  {
    Document doc;
    auto cv = std::make_unique<CurveFeature>("One", Curve3DKind::FitSpline);
    cv->points = {{0, 0, 0}};
    AddCurve(doc, std::move(cv));
    CHECK_FALSE(doc.Regenerate());
    CHECK_FALSE(doc.GetFeature(0)->GetError().empty());
  }
  {
    Document doc;
    auto cv = std::make_unique<CurveFeature>("Flat", Curve3DKind::Helix);
    cv->radius = 0.0; // invalid
    AddCurve(doc, std::move(cv));
    CHECK_FALSE(doc.Regenerate());
    CHECK_FALSE(doc.GetFeature(0)->GetError().empty());
  }
}

TEST_CASE("Curve3D round-trips through JSON", "[curve3d]") {
  auto helix = std::make_unique<CurveFeature>("Coil", Curve3DKind::Helix);
  helix->axisOrigin = {1, 2, 3};
  helix->axisDir = {0, 1, 0};
  helix->radius = 2.5;
  helix->pitch = 1.5;
  helix->turns = 4.0;
  helix->rightHanded = false;
  helix->pitchExpr = "gap * 2";

  const nlohmann::json j = DocumentIO::FeatureToJson(*helix);
  auto back = DocumentIO::FeatureFromJson(j);
  auto* cv = dynamic_cast<CurveFeature*>(back.get());
  REQUIRE(cv);
  CHECK(cv->kind == Curve3DKind::Helix);
  CHECK(cv->axisOrigin == glm::dvec3(1, 2, 3));
  CHECK(cv->axisDir == glm::dvec3(0, 1, 0));
  CHECK_THAT(cv->radius, WithinAbs(2.5, 1e-9));
  CHECK_THAT(cv->pitch, WithinAbs(1.5, 1e-9));
  CHECK_THAT(cv->turns, WithinAbs(4.0, 1e-9));
  CHECK_FALSE(cv->rightHanded);
  CHECK(cv->pitchExpr == "gap * 2");

  auto fit = std::make_unique<CurveFeature>("Fit", Curve3DKind::FitSpline);
  fit->points = {{0, 0, 0}, {1, 1, 1}, {2, 0, 2}};
  fit->closed = true;
  auto* cv2 = dynamic_cast<CurveFeature*>(
      DocumentIO::FeatureFromJson(DocumentIO::FeatureToJson(*fit)).release());
  REQUIRE(cv2);
  CHECK(cv2->points.size() == 3);
  CHECK(cv2->closed);
  CHECK(cv2->points[1] == glm::dvec3(1, 1, 1));
  delete cv2;
}

TEST_CASE("A bent path is straights and arcs, exactly", "[curve3d][bent]") {
  // What a tube bender is programmed with: the corners, and the former the
  // tube is bent round. A fitted spline through sampled arc points is none
  // of that — it bows between the points on the straights, its radius is an
  // approximation nothing can read back, and it took twenty-nine generated
  // points to say what four and a radius say here
  // (docs/tube/README.md#members-and-their-paths).
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Handle", Curve3DKind::BentPath);
  cv->points = {{-200, 0, 0}, {-200, 0, 90}, {200, 0, 90}, {200, 0, 0}};
  cv->radius = 45.0;
  CurveFeature* c = AddCurve(doc, std::move(cv));
  REQUIRE(doc.Regenerate());
  INFO(c->GetError());
  REQUIRE(c->GetError().empty());

  // Two legs less what each corner eats, two quarter turns, and the span:
  // the length is arithmetic, not a sampling.
  const double want = 2 * (90.0 - 45.0) + 2 * (std::numbers::pi / 2 * 45.0) +
                      (400.0 - 2 * 45.0);
  CHECK_THAT(c->Length(), WithinRel(want, 1e-9));
  CHECK_THAT(PolylineLength(c->Polyline()), WithinRel(want, 1e-3));

  // And the bends are a SCHEDULE: each corner's turn, its former, and the
  // free straight before it.
  REQUIRE(c->Bends().size() == 2);
  for (const CurveFeature::Bend& b : c->Bends()) {
    CHECK_THAT(b.angleDeg, WithinAbs(90.0, 1e-9));
    CHECK_THAT(b.radius, WithinAbs(45.0, 1e-12));
    CHECK_THAT(b.rollDeg, WithinAbs(0.0, 1e-9)); // one plane: a flat handle
  }
  CHECK_THAT(c->Bends()[0].straightBefore, WithinAbs(45.0, 1e-9));
  CHECK_THAT(c->Bends()[1].straightBefore, WithinAbs(400.0 - 2 * 45.0, 1e-9));
}

TEST_CASE("A former too big for the straight is refused by name",
          "[curve3d][bent]") {
  Document doc;
  auto cv = std::make_unique<CurveFeature>("Tight", Curve3DKind::BentPath);
  cv->points = {{0, 0, 0}, {0, 0, 20}, {100, 0, 20}, {100, 0, 0}};
  cv->radius = 45.0; // each corner eats 45 of a 20 mm leg
  CurveFeature* c = AddCurve(doc, std::move(cv));
  doc.Regenerate();
  INFO(c->GetError());
  CHECK_FALSE(c->GetError().empty());
  CHECK(c->GetError().find("20 mm") != std::string::npos);
  CHECK(c->GetError().find("tighter former") != std::string::npos);

  // A path that doubles back on itself is not a bend either — and just
  // short of doubling back is refused by the straight it would need, which
  // is the same answer said in the units the caller can act on.
  auto back = std::make_unique<CurveFeature>("Back", Curve3DKind::BentPath);
  back->points = {{0, 0, 0}, {100, 0, 0}, {0, 0, 0}};
  back->radius = 5.0;
  CurveFeature* b = AddCurve(doc, std::move(back));
  doc.Regenerate();
  CHECK(b->GetError().find("doubles back") != std::string::npos);
}

TEST_CASE("A bent path that cannot be bent is the ONLY error",
          "[curve3d][bent][tube]") {
  // From the PES-on-OM615 dogfood: a leak-off pipe whose straight was 20 mm
  // with 27.3 mm of bends on it. The refusal said so — and was followed by
  // "pipe path must be an open sketch chain or a 3D curve", which it was:
  // the pipe had been added on the path the first message refused, and a
  // reader has to work out which of the two is true.
  auto tight =
      std::make_unique<CurveFeature>("Leak-off path", Curve3DKind::BentPath);
  tight->points = {{0, 0, 0}, {0, 0, 20}, {100, 0, 20}, {100, 0, 0}};
  tight->radius = 45.0;

  // It is refused where it is ADDED (add_feature asks WhyIncomplete before
  // committing), so the caller's next step — the pipe — never runs on it.
  const std::string why = tight->WhyIncomplete();
  INFO(why);
  CHECK(why.find("20 mm") != std::string::npos);
  CHECK(why.find("tighter former") != std::string::npos);
  CHECK(why.find("open sketch chain") == std::string::npos);

  // A path that CAN be bent is not refused, and nor is any other kind: a
  // fit spline's geometry is its own business at regeneration.
  auto fine = std::make_unique<CurveFeature>("Fine", Curve3DKind::BentPath);
  fine->points = {{0, 0, 0}, {0, 0, 90}, {100, 0, 90}};
  fine->radius = 45.0;
  CHECK(fine->WhyIncomplete().empty());

  // A path that breaks LATER (an edit, a file) still regenerates broken, and
  // the pipe on it defers to it by name instead of misdescribing it.
  Document doc;
  doc.AddFeature(std::move(tight));
  const uint64_t pathId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<PipeFeature>("Leak-off", pathId, 6.0, 1.0));
  doc.Regenerate();
  const std::string pathErr = doc.GetFeature(0)->GetError();
  const std::string pipeErr = doc.GetFeature(1)->GetError();
  INFO(pathErr << " / " << pipeErr);
  CHECK(pathErr.find("20 mm") != std::string::npos);
  CHECK(pipeErr.find("open sketch chain") == std::string::npos);
  CHECK(pipeErr.find("'Leak-off path' failed to build") != std::string::npos);
  CHECK(doc.GetFeature(1)->ErrorAfterEarlierFailure());
}

TEST_CASE("A bent path round-trips through JSON", "[curve3d][bent]") {
  auto cv = std::make_unique<CurveFeature>("Bent", Curve3DKind::BentPath);
  cv->points = {{0, 0, 0}, {0, 0, 50}, {80, 0, 50}};
  cv->radius = 12.0;
  cv->radiusExpr = "former";

  const nlohmann::json j = DocumentIO::FeatureToJson(*cv);
  auto back = DocumentIO::FeatureFromJson(j);
  auto* read = dynamic_cast<CurveFeature*>(back.get());
  REQUIRE(read != nullptr);
  CHECK(read->kind == Curve3DKind::BentPath);
  CHECK(read->points.size() == 3);
  CHECK_THAT(read->radius, WithinAbs(12.0, 1e-12));
  CHECK(read->radiusExpr == "former");
}

TEST_CASE("Helix start_deg turns its start round the axis", "[curve3d]") {
  const auto start = [](glm::dvec3 axis, double deg, bool rh = true) {
    Document doc;
    auto cv = std::make_unique<CurveFeature>("H", Curve3DKind::Helix);
    cv->axisOrigin = {0, 0, 0};
    cv->axisDir = axis;
    cv->radius = 2.0;
    cv->pitch = 1.0;
    cv->turns = 0.5;
    cv->rightHanded = rh;
    cv->startDeg = deg;
    CurveFeature* c = AddCurve(doc, std::move(cv));
    REQUIRE(doc.Regenerate());
    return c->Polyline().front();
  };
  // Unchanged default: a +Z axis starts on +X.
  CHECK(glm::length(start({0, 0, 1}, 0.0) - glm::dvec3(2, 0, 0)) < 1e-3);
  // Right-handed about +Z: 90 degrees is +Y, 190 is just past -X.
  CHECK(glm::length(start({0, 0, 1}, 90.0) - glm::dvec3(0, 2, 0)) < 1e-3);
  const glm::dvec3 p = start({0, 0, 1}, 190.0);
  CHECK_THAT(std::atan2(p.y, p.x) * 180.0 / M_PI, WithinAbs(-170.0, 1e-2));
  // The start does not depend on the winding.
  CHECK(glm::length(start({0, 0, 1}, 90.0, false) - glm::dvec3(0, 2, 0)) <
        1e-3);
  // The documented references for the world axes.
  CHECK(glm::distance(glm::dvec3(HelixReference({0, 0, -1})),
                      glm::dvec3(-1, 0, 0)) < 1e-9);
  CHECK(glm::distance(glm::dvec3(HelixReference({1, 0, 0})),
                      glm::dvec3(0, 0, 1)) < 1e-9);
  CHECK(glm::distance(glm::dvec3(HelixReference({0, 1, 0})),
                      glm::dvec3(0, 0, 1)) < 1e-9);
  CHECK(glm::distance(glm::dvec3(HelixReference({0, -1, 0})),
                      glm::dvec3(0, 0, -1)) < 1e-9);
  CHECK(glm::length(start({0, 1, 0}, 0.0) - glm::dvec3(0, 0, 2)) < 1e-3);
}

TEST_CASE("Helix start_deg is an expression and round-trips", "[curve3d]") {
  auto helix = std::make_unique<CurveFeature>("Coil", Curve3DKind::Helix);
  helix->startDeg = 45.0;
  helix->startDegExpr = "phase + 15";
  const nlohmann::json j = DocumentIO::FeatureToJson(*helix);
  CHECK_THAT(j.at("start_deg").get<double>(), WithinAbs(45.0, 1e-12));
  auto back = DocumentIO::FeatureFromJson(j);
  auto* cv = dynamic_cast<CurveFeature*>(back.get());
  REQUIRE(cv);
  CHECK_THAT(cv->startDeg, WithinAbs(45.0, 1e-12));
  CHECK(cv->startDegExpr == "phase + 15");
  // A helix that never set it writes nothing: files from before v128 stay
  // byte-identical when re-saved.
  CurveFeature plain("P", Curve3DKind::Helix);
  CHECK_FALSE(DocumentIO::FeatureToJson(plain).contains("start_deg"));

  Document doc;
  doc.SetParameter("phase", "75");
  auto cv2 = std::make_unique<CurveFeature>("H", Curve3DKind::Helix);
  cv2->radius = 2.0;
  cv2->turns = 0.25;
  cv2->startDegExpr = "phase + 15"; // 90
  CurveFeature* c = AddCurve(doc, std::move(cv2));
  REQUIRE(doc.Regenerate());
  CHECK(glm::length(c->Polyline().front() - glm::dvec3(0, 2, 0)) < 1e-3);
}
