// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The size that fits (docs/modeling/README.md#the-size-that-fits): a
// Fillet or Chamfer that does not build says the largest size that does,
// and each case checks that answer by what it is for -- the size is built,
// to the closed-form volume, and 2 % above it is not.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestPaths.h"
#include "core/BlendFit.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/SketchFeature.h"

#include <cmath>
#include <ctime>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {

using EdgePred = std::function<bool(const std::vector<glm::vec3>&)>;

// Every edge of the first body whose polyline `pred` accepts, as refs.
std::vector<ElementRef> EdgesWhere(const Document& doc, const EdgePred& pred) {
  const Body& body = doc.GetBodies().at(0);
  const auto polylines = body.shape.GenerateEdgePolylines();
  std::vector<ElementRef> out;
  for (size_t e = 0; e < polylines.size(); e++) {
    if (polylines[e].size() < 2 || !pred(polylines[e]))
      continue;
    const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
    REQUIRE(name != nullptr);
    out.push_back({body.createdBy, *name});
  }
  return out;
}

bool All(const std::vector<glm::vec3>& poly,
         const std::function<bool(const glm::vec3&)>& p) {
  for (const auto& q : poly)
    if (!p(q))
      return false;
  return true;
}

bool Near(float a, float b) { return std::abs(a - b) < 1e-3f; }

FilletFeature* AddBlend(Document& doc, std::vector<ElementRef> edges, double r,
                        EdgeBlendOp op = EdgeBlendOp::Fillet) {
  doc.AddFeature(
      std::make_unique<FilletFeature>("Blend", std::move(edges), r, op));
  return dynamic_cast<FilletFeature*>(doc.GetFeature(doc.FeatureCount() - 1));
}

// The answer the feature gives at `r`, which must not build.
FitHint Refused(Document& doc, FilletFeature* f, double r) {
  f->radius = r;
  CHECK_FALSE(doc.Regenerate());
  INFO(f->GetError());
  REQUIRE(f->GetFitHint().has_value());
  REQUIRE(f->GetFitHint()->fitsMm.has_value());
  return *f->GetFitHint();
}

// What the answer is for: it builds a valid solid, and 2 % above it does
// not. Returns the volume at it.
double BuildsAtAndNotAbove(Document& doc, FilletFeature* f, double fits) {
  f->radius = fits * 1.02;
  CHECK_FALSE(doc.Regenerate());
  CHECK_FALSE(f->GetError().empty());
  f->radius = fits;
  const bool ok = doc.Regenerate();
  INFO(f->GetError());
  REQUIRE(ok);
  CHECK(f->GetError().empty());
  CHECK_FALSE(f->GetFitHint().has_value());
  CHECK(Measure::InvalidReasons(doc.GetBodies().at(0).shape).empty());
  return doc.GetBodies().at(0).shape.Volume();
}

void Box(Document& doc, double x, double y, double z) {
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::dvec3(0), x, y, z));
  REQUIRE(doc.Regenerate());
  glm::dvec3 lo, hi;
  doc.GetBodies().at(0).shape.GetBoundingBox(lo, hi, true);
  REQUIRE(std::abs(hi.x - lo.x - x) < 1e-6);
  REQUIRE(std::abs(hi.y - lo.y - y) < 1e-6);
}

} // namespace

TEST_CASE("RoundDown3 keeps three significant digits, downward",
          "[fillet][fit]") {
  CHECK(fit::RoundDown3(7.4925) == 7.49);
  CHECK(fit::RoundDown3(10.0) == 10.0);
  CHECK(fit::RoundDown3(1234.5) == 1230.0);
  CHECK(fit::RoundDown3(0.0123456) == 0.0123);
  CHECK(fit::Mm(7.49) == "7.49");
  CHECK(fit::Mm(10.0) == "10");
}

TEST_CASE("Two rounds on a 15 mm end face: the size that fits is under 7.5",
          "[fillet][fit]") {
  Document doc;
  Box(doc, 80, 15, 20);
  // Both vertical edges of the x = 0 end.
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) { return Near(q.x, 0.0f); }) &&
           std::abs(p.front().z - p.back().z) > 19.0f;
  });
  REQUIRE(edges.size() == 2);
  FilletFeature* f = AddBlend(doc, edges, 10.0);
  const FitHint h = Refused(doc, f, 10.0);
  CHECK(h.param == "radius");
  CHECK(h.askedMm == 10.0);
  CHECK(h.basis == "built");
  CHECK(*h.fitsMm > 7.4);
  CHECK(*h.fitsMm <= 7.5);
  CHECK(f->GetError().find("fillet radius 10 mm does not fit: the largest "
                           "radius that fits is ") == 0);
  CHECK(f->GetError().find("too large for the geometry") == std::string::npos);
  const double r = *h.fitsMm;
  const double v = BuildsAtAndNotAbove(doc, f, r);
  CHECK_THAT(v, WithinRel(24000.0 - 2.0 * (1.0 - std::numbers::pi / 4.0) * r *
                                        r * 20.0,
                          1e-6));
}

TEST_CASE("A chamfer says distance", "[fillet][fit]") {
  Document doc;
  Box(doc, 80, 15, 20);
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) { return Near(q.x, 0.0f); }) &&
           std::abs(p.front().z - p.back().z) > 19.0f;
  });
  REQUIRE(edges.size() == 2);
  FilletFeature* f = AddBlend(doc, edges, 10.0, EdgeBlendOp::Chamfer);
  const FitHint h = Refused(doc, f, 10.0);
  CHECK(h.param == "radius");
  CHECK(f->GetError().find("chamfer distance 10 mm does not fit: the largest "
                           "distance that fits is ") == 0);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("A cylinder's end rounded past its radius names at most it",
          "[fillet][fit]") {
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::dvec3(0), glm::dvec3(0, 0, 1), 10.0, 30.0));
  REQUIRE(doc.Regenerate());
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) { return Near(q.z, 30.0f); });
  });
  REQUIRE(edges.size() == 1);
  FilletFeature* f = AddBlend(doc, edges, 12.0);
  const FitHint h = Refused(doc, f, 12.0);
  CHECK(*h.fitsMm <= 10.0);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("A hole's rim chamfered past the plate names at most its thickness",
          "[fillet][fit]") {
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({-30, -30}, {30, 30});
  sk->AddEntity(sketch::Circle{{0, 0}, 8.0});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Plate", doc.GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return Near(q.z, 10.0f) &&
             std::abs(glm::length(glm::vec2(q)) - 8.0f) < 1e-2f;
    });
  });
  REQUIRE(edges.size() == 1);
  FilletFeature* f = AddBlend(doc, edges, 10.5, EdgeBlendOp::Chamfer);
  const FitHint h = Refused(doc, f, 10.5);
  CHECK(*h.fitsMm <= 10.0);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("Three rounds at a cube's corner name a radius under its side",
          "[fillet][fit]") {
  Document doc;
  Box(doc, 20, 20, 20);
  // The three edges at the origin corner.
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    const glm::vec3 a = p.front(), b = p.back();
    return (glm::length(a) < 1e-3f || glm::length(b) < 1e-3f);
  });
  REQUIRE(edges.size() == 3);
  FilletFeature* f = AddBlend(doc, edges, 20.0);
  const FitHint h = Refused(doc, f, 20.0);
  CHECK(*h.fitsMm < 20.0);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("An L-bracket's inside bend past the shorter leg names the face",
          "[fillet][fit]") {
  // Legs 5 thick: along x to 40, up z... drawn in XY and extruded 20, the
  // inside corner at (5, 5), the upright leg's inside face 10 long.
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const glm::dvec2 pts[] = {{0, 0}, {40, 0}, {40, 5}, {5, 5}, {5, 15}, {0, 15}};
  for (int i = 0; i < 6; i++)
    sk->AddEntity(sketch::Line{pts[i], pts[(i + 1) % 6]});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "L", doc.GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return Near(q.x, 5.0f) && Near(q.y, 5.0f);
    });
  });
  REQUIRE(edges.size() == 1);
  FilletFeature* f = AddBlend(doc, edges, 12.0);
  const FitHint h = Refused(doc, f, 12.0);
  INFO(f->GetError());
  CHECK(*h.fitsMm <= 10.0);
  CHECK_FALSE(h.element.empty());
  CHECK(f->GetError().find("(face '" + h.element + "' runs 10 mm") !=
        std::string::npos);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("A limit below every bound: the search's size, not the bound's",
          "[fillet][fit]") {
  // A bore drilled down beside the rounded edge, between two of the
  // stations the face's width is measured at: the round runs into its rim
  // near 2 mm, where every bound says 20.
  Document doc;
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  sk->AddRectangle({0, 0}, {80, 20});
  sk->AddEntity(sketch::Circle{{16, 4}, 2.0});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Drilled", doc.GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return Near(q.z, 20.0f) && Near(q.y, 0.0f);
    });
  });
  REQUIRE(edges.size() == 1);
  FilletFeature* f = AddBlend(doc, edges, 25.0);
  const FitHint h = Refused(doc, f, 25.0);
  INFO(f->GetError());
  CHECK(*h.fitsMm < 4.0);
  CHECK(h.element.empty());
  CHECK(f->GetError().find("(face '") == std::string::npos);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("A fillet that stays broken builds its candidates once",
          "[fillet][fit]") {
  Document doc;
  Box(doc, 80, 15, 20);
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) { return Near(q.x, 0.0f); }) &&
           std::abs(p.front().z - p.back().z) > 19.0f;
  });
  REQUIRE(edges.size() == 2);
  // Adding it regenerates, and searches.
  const std::uint64_t before = fit::CandidatesBuilt();
  FilletFeature* f = AddBlend(doc, edges, 10.0);
  CHECK_FALSE(f->GetError().empty());
  const std::uint64_t once = fit::CandidatesBuilt() - before;
  CHECK(once >= 1);
  CHECK(once <= 1 + fit::kSearchSteps);
  const std::string error = f->GetError();
  for (int i = 0; i < 5; i++) {
    CHECK_FALSE(doc.Regenerate());
    CHECK(f->GetError() == error);
  }
  f->radius = 12.0; // further past the limit
  CHECK_FALSE(doc.Regenerate());
  CHECK(f->GetError().find("fillet radius 12 mm does not fit") == 0);
  CHECK(fit::CandidatesBuilt() - before == once);
  // Below the size known to fail it searches again.
  f->radius = 7.6;
  CHECK_FALSE(doc.Regenerate());
  CHECK(fit::CandidatesBuilt() - before > once);
}

TEST_CASE("A failed fillet on a plate of 100 holes answers within ten builds",
          "[fillet][fit][slow]") {
  Document doc;
  auto imp = std::make_unique<ImportFeature>(
      "Part", std::string(TestSourceDir()) +
                  "/tests/fixtures/step/plate_100_holes.step");
  imp->keepOrigin = true;
  doc.AddFeature(std::move(imp));
  REQUIRE(doc.Regenerate());
  const Body& b = doc.GetBodies().at(0);
  glm::dvec3 lo, hi;
  b.shape.GetBoundingBox(lo, hi, true);
  const double thick = std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
  // Every hole's rim on one side: the plate's outline is a rectangle, so
  // every edge on the top that is not straight is a rim.
  const auto zTop = static_cast<float>(hi.z);
  REQUIRE(thick == hi.z - lo.z);
  const auto rims = EdgesWhere(doc, [&](const auto& p) {
    if (!All(p, [&](const glm::vec3& q) { return Near(q.z, zTop); }))
      return false;
    const glm::vec3 a = p.front(), m = p[p.size() / 2], c = p.back();
    return glm::length(glm::cross(m - a, c - a)) > 1e-3f ||
           glm::length(c - a) < 1e-3f;
  });
  REQUIRE(rims.size() >= 100);
  // A round that fits: one build, its validity check included. The rims
  // are 2 mm bores, so a round stops just under 1.
  const auto cpu = [] { return double(std::clock()) / CLOCKS_PER_SEC; };
  double t0 = cpu();
  FilletFeature* f = AddBlend(doc, rims, thick * 0.05); // adding regenerates
  const double one = cpu() - t0;
  REQUIRE(f->GetError().empty());
  // One past it: the build that fails, the bounds and the search after it.
  // (Far past it the kernel's own failing build is the cost: at 30 mm it
  // takes 17 builds' time before anything here runs.)
  f->radius = 1.5;
  t0 = cpu();
  CHECK_FALSE(doc.Regenerate());
  const double failed = cpu() - t0;
  INFO(f->GetError());
  REQUIRE(f->GetFitHint().has_value());
  INFO("one build " << one << " s of CPU; the refusal " << failed << " s");
  CHECK(failed < 10.0 * one);
}
