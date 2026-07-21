#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <memory>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
double PolylineLength(const std::vector<glm::vec3>& p) {
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
  CHECK(glm::length(poly.front() - glm::vec3(0, 0, 0)) < 1e-4f);
  CHECK(glm::length(poly.back() - glm::vec3(3, 3, 3)) < 1e-4f);
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
  CHECK(glm::length(poly.front() - glm::vec3(2, 0, 0)) < 1e-3f);
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
  CHECK(build(true).y > 0.5f);
  CHECK(build(false).y < -0.5f);
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
  CHECK(glm::length(poly.front() - glm::vec3(0, 0, 0)) < 1e-4f);
  CHECK(glm::length(poly.back() - glm::vec3(5, 0, 2)) < 1e-4f);
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
  CHECK(cv->axisOrigin == glm::vec3(1, 2, 3));
  CHECK(cv->axisDir == glm::vec3(0, 1, 0));
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
  CHECK(cv2->points[1] == glm::vec3(1, 1, 1));
  delete cv2;
}
