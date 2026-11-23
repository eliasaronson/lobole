// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The size that fits (docs/modeling/README.md#the-size-that-fits): a
// Fillet or Chamfer that does not build says the largest size that does,
// and each case checks that answer by what it is for -- the size is built,
// to the closed-form volume, and 2 % above it is not.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CostCheck.h"
#include "SameCore.h"
#include "TestPaths.h"
#include "core/BlendFit.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/SketchFeature.h"

#include <cmath>
#include <cstdio>
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
  // The two rounds meet across the end face: a bound that refuses
  // (docs/modeling/README.md#bounds-that-refuse), so 10 is not built.
  CHECK(h.basis == "bound");
  CHECK(*h.fitsMm > 7.4);
  CHECK(*h.fitsMm <= 7.5);
  CHECK(f->GetError().find("fillet radius 10 mm does not fit: face '") == 0);
  CHECK(f->GetError().find("' is 15 mm wide between edge '") !=
        std::string::npos);
  CHECK(f->GetError().find(", so the rounds meet at 7.5 mm; the largest "
                           "radius that fits is ") != std::string::npos);
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
  CHECK(f->GetError().find("chamfer distance 10 mm does not fit: face '") == 0);
  CHECK(f->GetError().find(", so the chamfers meet at 7.5 mm; the largest "
                           "distance that fits is ") != std::string::npos);
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
  // Past a disc's rim nothing builds, so the size asked is not built
  // (docs/modeling/README.md#bounds-that-refuse).
  CHECK(h.basis == "bound");
  CHECK(f->GetError().find("fillet radius 12 mm does not fit: edge '") == 0);
  CHECK(f->GetError().find("' is a circle of radius 10 mm; the largest "
                           "radius that fits is " +
                           fit::Mm(*h.fitsMm) + " mm") != std::string::npos);
  BuildsAtAndNotAbove(doc, f, *h.fitsMm);
}

TEST_CASE("Far past a rim, the size asked is not built", "[fillet][fit]") {
  // A round of the end of a 10 mm cylinder at 30: the builder's own failure
  // there is not tried, only the size just under the rim, which builds.
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::dvec3(0), glm::dvec3(0, 0, 1), 10.0, 30.0));
  REQUIRE(doc.Regenerate());
  const auto edges = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) { return Near(q.z, 30.0f); });
  });
  REQUIRE(edges.size() == 1);
  // The look before every build finds the rim BlendBounds measures.
  {
    const Body& body = doc.GetBodies().at(0);
    const std::vector<int> idx{*body.elementMap.ResolveEdge(edges[0].name)};
    for (const EdgeBlendOp op : {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
      const auto rims = fit::RimBounds(body, idx, op);
      REQUIRE(rims.size() == 1);
      int measured = 0;
      for (const fit::Bound& b : fit::BlendBounds(body, idx, op))
        if (b.kind == fit::BoundKind::Rim) {
          measured++;
          CHECK_THAT(b.size, WithinRel(rims[0].size, 1e-6));
          CHECK(b.why == rims[0].why);
        }
      CHECK(measured >= 1);
      CHECK_THAT(rims[0].size, WithinRel(10.0, 1e-6));
    }
  }
  FilletFeature* f = AddBlend(doc, edges, 5.0);
  REQUIRE(f->GetError().empty());
  for (const EdgeBlendOp op : {EdgeBlendOp::Fillet, EdgeBlendOp::Chamfer}) {
    f->op = op;
    const std::uint64_t before = fit::CandidatesBuilt();
    const FitHint h = Refused(doc, f, 30.0);
    CHECK(h.basis == "bound");
    CHECK(fit::CandidatesBuilt() - before == 1);
    CHECK(*h.fitsMm == 9.99);
    // Just past the rim it is refused the same way, and at the rim it is
    // the builder's to answer.
    f->radius = 10.02;
    CHECK_FALSE(doc.Regenerate());
    CHECK(f->GetFitHint()->basis == "bound");
    f->radius = 10.0;
    doc.Regenerate();
    if (f->GetFitHint())
      CHECK(f->GetFitHint()->basis == "built");
    BuildsAtAndNotAbove(doc, f, *h.fitsMm);
  }
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
  f->radius = 7.505;
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
  // The picked corner arcs bring the whole outline in by tangency, so the
  // outer bores' rims meet the outline's round across the 2 mm between
  // them: a round stops just under 1 (the bores are R3, and a bore's rim
  // has no limit of its own at its radius:
  // docs/modeling/README.md#bounds-that-refuse).
  // The two times are compared, so both are taken on one core: moved from
  // a performance core to a low-power one between them, the refusal read
  // 31 builds with nothing changed (0.26 s against 8.0 s).
  const SameCore held;
  const auto cpu = [] { return double(std::clock()) / CLOCKS_PER_SEC; };
  double t0 = cpu();
  FilletFeature* f = AddBlend(doc, rims, thick * 0.05); // adding regenerates
  const double one = cpu() - t0;
  REQUIRE(f->GetError().empty());
  // Far past it: refused before a build, the search's one candidate under
  // the bound built.
  const std::uint64_t built = fit::CandidatesBuilt();
  f->radius = 30.0;
  t0 = cpu();
  CHECK_FALSE(doc.Regenerate());
  const double far = cpu() - t0;
  INFO(f->GetError());
  REQUIRE(f->GetFitHint().has_value());
  CHECK(f->GetFitHint()->basis == "bound");
  CHECK(f->GetFitHint()->fitsMm == 0.999);
  CHECK(fit::CandidatesBuilt() - built == 1);
  CHECK(f->GetError().find(" is 2 mm wide between edge '") !=
        std::string::npos);
  CHECK(f->GetError().find("(tangent to a picked edge, so blended with it)") !=
        std::string::npos);
  CHECK(f->GetError().find("so the rounds meet at 1 mm; the largest radius "
                           "that fits is 0.999 mm") != std::string::npos);
  // One past it: the same, searched afresh under the size that failed.
  f->radius = 1.5;
  t0 = cpu();
  CHECK_FALSE(doc.Regenerate());
  const double failed = cpu() - t0;
  REQUIRE(f->GetFitHint().has_value());
  INFO("one build " << one << " s of CPU; the refusal at 30 " << far
                    << " s, at 1.5 " << failed << " s");
  CHECK_COST(failed < 10.0 * one);
  // A cost check (docs/ui/testing.md#tests-that-time-things): the refusal
  // at 30 is the look before the build and the one candidate under the
  // bound, 1.3 times the round that fits, where building 30 first cost
  // twenty times it (3.4 s against 0.17 s). A planted 1 s spin fails it.
  CHECK_COST(far < 3.0 * one);
}

namespace {

// A block over [lo, hi], a rectangle on the plane z = lo.z extruded to hi.z.
void Block(Document& doc, glm::dvec3 lo, glm::dvec3 hi, ExtrudeMode mode) {
  auto s = std::make_unique<SketchFeature>(
      "S", SketchPlane(PlaneType::XY, {0.0, 0.0, lo.z}));
  s->AddRectangle({lo.x, lo.y}, {hi.x, hi.y});
  doc.AddFeature(std::move(s));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "E", doc.GetFeature(doc.FeatureCount() - 1)->GetId(), hi.z - lo.z, mode));
}

// The faces of the first body on the plane `axis` = `at` (0 x, 1 y, 2 z),
// by name.
std::vector<std::string> FacesOnPlane(const Document& doc, int axis,
                                      double at) {
  const Body& body = doc.GetBodies().at(0);
  std::vector<std::string> out;
  for (int i = 0; i < body.shape.FaceCount(); i++) {
    glm::dvec3 c, n, u;
    if (!body.shape.FacePlane(i, c, n, u) || std::abs(c[axis] - at) > 1e-6 ||
        std::abs(std::abs(n[axis]) - 1.0) > 1e-9)
      continue;
    const std::string* name = body.elementMap.NameOf(i);
    REQUIRE(name != nullptr);
    out.push_back(*name);
  }
  return out;
}

// The kernel case corpus's L (solidcore/fillets/12342): a 4 mm wall
// (x 16..20, y -20..20, z -40..2) on a 7 mm base (x 16..23, z -50..-40),
// joined from two blocks (`add`, so its back face y = 20 and its side x = 16
// are each two coplanar faces) or extruded from one L profile.
void TwoBlockL(Document& doc) {
  Block(doc, {16, -20, -50}, {23, 20, -40}, ExtrudeMode::NewBody);
  Block(doc, {16, -20, -40}, {20, 20, 2}, ExtrudeMode::Add);
  REQUIRE(doc.Regenerate());
}

std::vector<ElementRef> WallEdge(const Document& doc) {
  return EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return Near(q.x, 20.0f) && Near(q.y, 20.0f) && q.z > -40.001f;
    });
  });
}

} // namespace

TEST_CASE("A round beside a plane an add left in two pieces builds as on one "
          "face",
          "[fillet][fit]") {
  // docs/modeling/README.md#a-blend-beside-a-split: the wall's edge ends on
  // the edge between the back face's two pieces, where the builder failed
  // at every size ("no radius builds ... down to 0.00257 mm").
  Document doc;
  TwoBlockL(doc);
  REQUIRE(FacesOnPlane(doc, 1, 20.0).size() == 2);
  const std::vector<std::string> back = FacesOnPlane(doc, 1, 20.0);
  const std::vector<std::string> bottom = FacesOnPlane(doc, 2, -50.0);
  const auto edge = WallEdge(doc);
  REQUIRE(edge.size() == 1);
  FilletFeature* f = AddBlend(doc, edge, 2.5);
  REQUIRE(doc.Regenerate());
  INFO(f->GetError());
  CHECK(Measure::InvalidReasons(doc.GetBodies().at(0).shape).empty());
  // The closed form, as the L extruded from one profile builds it: the
  // spandrel (1 - pi/4) r^2 along the wall's 42 mm.
  const double spandrel = (1.0 - std::numbers::pi / 4.0) * 2.5 * 2.5;
  CHECK_THAT(doc.GetBodies().at(0).shape.Volume(),
             WithinRel(9520.0 - spandrel * 42.0, 1e-9));

  // The names: the back face is one face now, named after its first piece
  // as the blend names a face it modifies; the other piece's name is gone;
  // a face the blend does not touch keeps its name.
  const std::vector<std::string> merged = FacesOnPlane(doc, 1, 20.0);
  REQUIRE(merged.size() == 1);
  const std::string mark = ";M" + std::to_string(f->GetId());
  CHECK((merged[0] == back[0] + mark || merged[0] == back[1] + mark));
  const std::string& gone = merged[0] == back[0] + mark ? back[1] : back[0];
  CHECK_FALSE(doc.GetBodies().at(0).elementMap.Resolve(gone).has_value());
  CHECK(FacesOnPlane(doc, 2, -50.0) == bottom);
}

TEST_CASE("Past the wall, the round beside the split says its wall's width",
          "[fillet][fit]") {
  // The width is measured to the face's own boundary, not to the edge
  // between its pieces: R5 past the 4 mm wall fits 3.99.
  Document doc;
  TwoBlockL(doc);
  FilletFeature* f = AddBlend(doc, WallEdge(doc), 5.0);
  const FitHint hint = Refused(doc, f, 5.0);
  CHECK(*hint.fitsMm == 3.99);
  CHECK(f->GetError().find("runs 4 mm from edge") != std::string::npos);
  const double v = BuildsAtAndNotAbove(doc, f, 3.99);
  const double spandrel = (1.0 - std::numbers::pi / 4.0) * 3.99 * 3.99;
  CHECK_THAT(v, WithinRel(9520.0 - spandrel * 42.0, 1e-9));
}

TEST_CASE("A rim chamfer beside a bar flush with its bottom builds",
          "[fillet][fit]") {
  // A cylinder r 22.5 joined with a bar (x 0..110, |y| <= 9) flush with its
  // bottom: the bottom is two faces, and the rim's chamfer ran into the
  // edge between them ("the largest distance that fits is 0.121 mm").
  // Built against the same part refined, its bottom one face.
  const auto build = [](Document& doc, bool refine) {
    auto s = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
    s->AddEntity(sketch::Circle{{0.0, 0.0}, 22.5});
    doc.AddFeature(std::move(s));
    doc.AddFeature(std::make_unique<ExtrudeFeature>(
        "E", doc.GetFeature(0)->GetId(), 30.0, ExtrudeMode::NewBody));
    Block(doc, {0, -9, 0}, {110, 9, 7.5}, ExtrudeMode::Add);
    if (refine)
      doc.AddFeature(
          std::make_unique<RefineFeature>("R", doc.GetFeature(1)->GetId()));
    REQUIRE(doc.Regenerate());
  };
  const auto rim = [](const Document& doc) {
    return EdgesWhere(doc, [](const auto& p) {
      return All(p, [](const glm::vec3& q) {
        // The rim outside the bar: inside it the circle is the edge
        // between the bottom's pieces.
        return Near(q.z, 0.0f) &&
               std::abs(std::hypot(q.x, q.y) - 22.5f) < 1e-3f &&
               (q.x < 1e-3f || std::abs(q.y) > 9.0f - 1e-3f);
      });
    });
  };
  Document split;
  build(split, false);
  REQUIRE(FacesOnPlane(split, 2, 0.0).size() > 1);
  FilletFeature* f = AddBlend(split, rim(split), 5.0, EdgeBlendOp::Chamfer);
  const bool built = split.Regenerate();
  INFO(f->GetError());
  REQUIRE(built);
  CHECK(Measure::InvalidReasons(split.GetBodies().at(0).shape).empty());

  Document whole;
  build(whole, true);
  REQUIRE(FacesOnPlane(whole, 2, 0.0).size() == 1);
  AddBlend(whole, rim(whole), 5.0, EdgeBlendOp::Chamfer);
  REQUIRE(whole.Regenerate());
  CHECK_THAT(split.GetBodies().at(0).shape.Volume(),
             WithinRel(whole.GetBodies().at(0).shape.Volume(), 1e-9));

  // And the size that fits is found on one bottom face: at 30 it is near
  // the bar's 7.5 mm height, where the chamfer's top reaches the bar's top
  // (the split bottom named 0.121).
  const FitHint hint = Refused(split, f, 30.0);
  CHECK(*hint.fitsMm > 7.0);
  BuildsAtAndNotAbove(split, f, *hint.fitsMm);
}

TEST_CASE("A blend that builds a valid solid of the wrong volume is refused",
          "[fillet][fit]") {
  // docs/modeling/README.md#a-blend-moves-the-volume-it-can. Planted: a
  // round of 1 mm on a box's edge, convex, removes (1 - pi/4) per mm of it;
  // it is held to the ball's disc, pi r^2 a mm, which is more.
  Document doc;
  Box(doc, 10, 10, 10);
  const auto edge = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return Near(q.x, 0.0f) && Near(q.y, 0.0f);
    });
  });
  REQUIRE(edge.size() == 1);
  const Body& body = doc.GetBodies().at(0);
  const int idx = *body.elementMap.ResolveEdge(edge[0].name);
  const auto reach = fit::ReachOf(body, {idx});
  REQUIRE(reach.has_value());
  const fit::VolumeReach at1 = fit::ReachAt(*reach, EdgeBlendOp::Fillet, 1.0);
  CHECK_THAT(at1.remove, WithinRel(std::numbers::pi * 10.0, 1e-12));
  CHECK(at1.add == 0.0);
  // Past each end, a ball of its setback (1 mm): 2 x 4/3 pi.
  CHECK_THAT(at1.ends, WithinRel(8.0 / 3.0 * std::numbers::pi, 1e-12));
  const double v = 1000.0, round = (1.0 - std::numbers::pi / 4.0) * 10.0;
  // The right body passes; one that grew, or lost far more than a round
  // can, does not.
  CHECK(fit::VolumeObjection(EdgeBlendOp::Fillet, 1.0, v, v - round, at1)
            .empty());
  const std::string grew =
      fit::VolumeObjection(EdgeBlendOp::Fillet, 1.0, v, v + 50.0, at1);
  CHECK(grew.find("50 mm3 larger than the part") != std::string::npos);
  CHECK(grew.find("(past their ends: they are convex)") != std::string::npos);
  CHECK_FALSE(fit::VolumeObjection(EdgeBlendOp::Fillet, 1.0, v, v - 100.0, at1)
                  .empty());
  // The slack the caller gives (the volumes' uncertainty) is room on top.
  CHECK(fit::VolumeObjection(EdgeBlendOp::Fillet, 1.0, v, v + 50.0, at1, 40.0)
            .empty());
}

TEST_CASE("The size that fits a tee's joint builds a round's volume",
          "[fillet][fit]") {
  // Two R15 tubes in a tee (kernel case solidcore/fillets/16654), the whole
  // joint at R2: refused, and the size the refusal named, R0.143, built a
  // valid solid 773 mm3 over the union where a round of it adds under 1.
  Document doc;
  auto along = std::make_unique<SketchFeature>(
      "S", SketchPlane({0, 0, 0}, {0, -1, 0}, {1, 0, 0}));
  along->AddEntity(sketch::Circle{{0.0, 0.0}, 15.0});
  doc.AddFeature(std::move(along));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Tube", doc.GetFeature(0)->GetId(), 80.0, ExtrudeMode::NewBody));
  auto up = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  up->AddEntity(sketch::Circle{{0.0, -40.0}, 15.0});
  doc.AddFeature(std::move(up));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Branch", doc.GetFeature(2)->GetId(), 40.0, ExtrudeMode::Add));
  REQUIRE(doc.Regenerate());
  const double v0 = doc.GetBodies().at(0).shape.Volume();
  REQUIRE_THAT(v0, WithinRel(75823.0, 1e-4));
  const auto joint = EdgesWhere(doc, [](const auto& p) {
    return All(p, [](const glm::vec3& q) {
      return q.z > -1e-3f && std::abs(std::hypot(q.x, q.z) - 15.0f) < 1e-2f &&
             std::abs(std::hypot(q.x, q.y + 40.0f) - 15.0f) < 1e-2f;
    });
  });
  REQUIRE_FALSE(joint.empty());
  FilletFeature* f = AddBlend(doc, joint, 0.143);
  // The size the search named before: valid, and refused for its volume.
  CHECK_FALSE(doc.Regenerate());
  INFO(f->GetError());
  CHECK(f->GetError().find("OpenCASCADE's blend builder built a wrong "
                           "solid") != std::string::npos);
  // At R2 the search keeps no wrong solid either: whatever it names builds
  // a round's volume, more than the union (the joint is concave) and under
  // the case's bound, 2 L 0.858 r^2.
  f->radius = 2.0;
  const auto t0 = std::clock();
  CHECK_FALSE(doc.Regenerate());
  INFO("the refusal at R2: " << double(std::clock() - t0) / CLOCKS_PER_SEC
                             << " s of CPU");
  NOT_A_COST_CHECK("the refusal's CPU is reported, not judged");
  if (const auto hint = f->GetFitHint(); hint && hint->fitsMm) {
    f->radius = *hint->fitsMm;
    REQUIRE(doc.Regenerate());
    const double v = doc.GetBodies().at(0).shape.Volume();
    const double r = *hint->fitsMm;
    CHECK(v > v0);
    CHECK(v - v0 < 2.0 * 114.6 * (1.0 - std::numbers::pi / 4.0) * r * r);
  } else {
    CHECK(f->GetError().find("no radius builds on these edges") !=
          std::string::npos);
  }
}
