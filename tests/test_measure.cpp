#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinAbs;

namespace {
// A box owns faces "<id>/faceN"; return them sorted so tests are stable.
std::vector<std::string> FaceNames(const Body& body) {
  std::vector<std::string> out;
  for (const auto& [idx, name] : body.elementMap.names)
    out.push_back(name);
  std::sort(out.begin(), out.end());
  return out;
}
std::vector<std::string> EdgeNames(const Body& body) {
  std::vector<std::string> out;
  for (const auto& [idx, name] : body.elementMap.edgeNames)
    out.push_back(name);
  std::sort(out.begin(), out.end());
  return out;
}
} // namespace

TEST_CASE("Measure a box's face areas and edge lengths", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2.0, 3.0,
                                              4.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];

  // Every face resolves and reports one of the three distinct areas.
  std::vector<double> areas;
  for (const std::string& name : FaceNames(body)) {
    auto e = Measure::Describe(doc.GetBodies(), ElementRef{id, name});
    REQUIRE(e.has_value());
    CHECK(e->kind == "face");
    areas.push_back(e->area);
  }
  std::sort(areas.begin(), areas.end());
  REQUIRE(areas.size() == 6);
  CHECK_THAT(areas[0], WithinRel(6.0, 1e-9));  // 2x3
  CHECK_THAT(areas[2], WithinRel(8.0, 1e-9));  // 2x4
  CHECK_THAT(areas[5], WithinRel(12.0, 1e-9)); // 3x4

  // Edge lengths are the box's side lengths {2,3,4}.
  std::vector<double> lengths;
  for (const std::string& name : EdgeNames(body)) {
    auto e = Measure::Describe(doc.GetBodies(), ElementRef{id, name});
    REQUIRE(e.has_value());
    CHECK(e->kind == "edge");
    lengths.push_back(e->length);
  }
  REQUIRE(lengths.size() == 12);
  double lo = *std::min_element(lengths.begin(), lengths.end());
  double hi = *std::max_element(lengths.begin(), lengths.end());
  CHECK_THAT(lo, WithinRel(2.0, 1e-9));
  CHECK_THAT(hi, WithinRel(4.0, 1e-9));
}

TEST_CASE("Measure distance between two boxes' faces", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0,
                                              1.0));
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 1.0,
                                              1.0, 1.0));
  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();

  // A spans x[0,1], B spans x[5,6] -> nearest faces are 4 apart. Any
  // face-face pair is at least that far; find the minimum over all pairs.
  double best = 1e30;
  for (const std::string& na : FaceNames(doc.GetBodies()[0]))
    for (const std::string& nb : FaceNames(doc.GetBodies()[1])) {
      auto d = Measure::Distance(doc.GetBodies(), ElementRef{idA, na},
                                 ElementRef{idB, nb});
      REQUIRE(d.has_value());
      best = std::min(best, *d);
    }
  CHECK_THAT(best, WithinAbs(4.0, 1e-6));
}

TEST_CASE("Transformed distance measures occurrences in world space",
          "[measure][assembly]") {
  // One part, a unit box at the origin — measured as two "occurrences", the
  // second translated (10,0,0): the facing named faces are 9 apart in world.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const auto& bodies = doc.GetBodies();

  const glm::mat4 xfA(1.0f);
  const glm::mat4 xfB = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
  double best = 1e30;
  glm::vec3 bestA{0}, bestB{0};
  for (const std::string& na : FaceNames(bodies[0]))
    for (const std::string& nb : FaceNames(bodies[0])) {
      auto d = Measure::DistanceDetailed(bodies, ElementRef{id, na}, xfA,
                                         bodies, ElementRef{id, nb}, xfB);
      REQUIRE(d.has_value());
      if (d->distance < best) {
        best = d->distance;
        bestA = d->pointA;
        bestB = d->pointB;
      }
    }
  CHECK_THAT(best, WithinAbs(9.0, 1e-6));
  // Closest points land on the facing sides, in WORLD coordinates.
  CHECK_THAT(bestA.x, WithinAbs(1.0, 1e-6));
  CHECK_THAT(bestB.x, WithinAbs(10.0, 1e-6));

  // A rotated occurrence: spin the second box 90° about Z, then place at
  // x=10. Local (x,y) -> world (10-y, x), so it occupies x[9,10] and the
  // nearest gap to box A (x[0,1]) is 8 — catches transform-order mistakes.
  const glm::mat4 xfR =
      glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
  best = 1e30;
  for (const std::string& na : FaceNames(bodies[0]))
    for (const std::string& nb : FaceNames(bodies[0])) {
      auto d = Measure::DistanceDetailed(bodies, ElementRef{id, na}, xfA,
                                         bodies, ElementRef{id, nb}, xfR);
      REQUIRE(d.has_value());
      best = std::min(best, d->distance);
    }
  CHECK_THAT(best, WithinAbs(8.0, 1e-6));
}

TEST_CASE("Element bounds of a box's faces", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2.0, 3.0,
                                              4.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];
  glm::vec3 bmin, bmax;
  body.shape.GetBoundingBox(bmin, bmax);

  glm::vec3 unionMin(std::numeric_limits<float>::max());
  glm::vec3 unionMax(-std::numeric_limits<float>::max());
  for (const std::string& name : FaceNames(body)) {
    auto b = Measure::ElementBounds(doc.GetBodies(), ElementRef{id, name});
    REQUIRE(b.has_value());
    // A box face is axis-aligned: exactly one degenerate axis.
    int flat = 0;
    for (int a = 0; a < 3; a++)
      if (std::abs(b->max[a] - b->min[a]) < 1e-6)
        flat++;
    CHECK(flat == 1);
    unionMin = glm::min(unionMin, b->min);
    unionMax = glm::max(unionMax, b->max);
  }
  // The six faces together span exactly the body's own bounding box.
  for (int a = 0; a < 3; a++) {
    CHECK_THAT(unionMin[a], WithinAbs(bmin[a], 1e-3));
    CHECK_THAT(unionMax[a], WithinAbs(bmax[a], 1e-3));
  }
  CHECK_FALSE(Measure::ElementBounds(doc.GetBodies(), ElementRef{id, "nope"})
                  .has_value());
}

TEST_CASE("Measure of a dangling ref returns nullopt", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  const uint64_t id = doc.GetFeature(0)->GetId();
  CHECK_FALSE(
      Measure::Describe(doc.GetBodies(), ElementRef{id, "nope"}).has_value());
  CHECK_FALSE(
      Measure::Describe(doc.GetBodies(), ElementRef{999, "x"}).has_value());
}

TEST_CASE("Cross-section area of a box", "[measure][section]") {
  using Catch::Matchers::WithinRel;
  Document doc;
  // Box 2x3x4 spanning [0,2]x[0,3]x[0,4].
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  // A horizontal (XY-normal) cut at z=2 slices a 2x3 rectangle = area 6.
  auto a = Measure::CrossSectionArea(doc.GetBodies(), glm::vec3(0, 0, 2),
                                     glm::vec3(0, 0, 1));
  REQUIRE(a.has_value());
  CHECK_THAT(*a, WithinRel(6.0, 1e-6));
  // A vertical (YZ-normal) cut at x=1 slices a 3x4 rectangle = area 12.
  auto b = Measure::CrossSectionArea(doc.GetBodies(), glm::vec3(1, 0, 0),
                                     glm::vec3(1, 0, 0));
  REQUIRE(b.has_value());
  CHECK_THAT(*b, WithinRel(12.0, 1e-6));
  // A plane that misses the body -> zero area.
  auto miss = Measure::CrossSectionArea(doc.GetBodies(), glm::vec3(0, 0, 99),
                                        glm::vec3(0, 0, 1));
  REQUIRE(miss.has_value());
  CHECK_THAT(*miss, Catch::Matchers::WithinAbs(0.0, 1e-6));
}

TEST_CASE("Section caps fill the cut face (C4)", "[measure][section]") {
  using Catch::Matchers::WithinRel;
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  // The cap shape for a YZ cut at x=1 is the 3x4 section rectangle: a valid
  // planar face whose surface area matches CrossSectionArea (12).
  auto caps = Measure::SectionCaps(doc.GetBodies(), glm::vec3(1, 0, 0),
                                   glm::vec3(1, 0, 0));
  REQUIRE(caps.size() == 1);
  REQUIRE(caps[0].IsValid());
  CADShape::MassProps mp;
  REQUIRE(caps[0].ComputeMassProps(mp));
  CHECK_THAT(mp.surfaceArea, WithinRel(12.0, 1e-6));
  // The cap tessellates to a non-empty mesh (what the renderer fills with).
  CHECK_FALSE(caps[0].GenerateMesh(0.05).vertices.empty());
  // A plane missing the body yields an empty/invalid cap, not a crash.
  auto missCaps = Measure::SectionCaps(doc.GetBodies(), glm::vec3(0, 0, 99),
                                       glm::vec3(0, 0, 1));
  REQUIRE(missCaps.size() == 1);
  CHECK_FALSE(missCaps[0].IsValid());
}
