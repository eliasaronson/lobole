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

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

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
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 2.0, 3.0, 4.0));
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
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0), 1.0, 1.0, 1.0));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(5, 0, 0), 1.0, 1.0, 1.0));
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
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 2.0, 3.0, 4.0));
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

TEST_CASE("DescribeFaceAt/DescribeEdgeAt classify a box", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  const Body& body = doc.GetBodies()[0];

  // Box center = mean of the face centroids (robust to the corner-origin
  // convention). Used below to check each normal points OUTWARD.
  glm::vec3 center(0.0f);
  int nFaces = 0;
  for (const auto& [idx, name] : body.elementMap.names) {
    (void)name;
    if (const auto f = Measure::DescribeFaceAt(body, idx)) {
      center += f->centroid;
      nFaces++;
    }
  }
  center /= static_cast<float>(nFaces);

  // Every named face is a plane with one of the box's three areas, and reports
  // an outward unit normal aligned to one of the box's six axis directions.
  int outwardNormals = 0;
  for (const auto& [idx, name] : body.elementMap.names) {
    (void)name;
    const auto f = Measure::DescribeFaceAt(body, idx);
    REQUIRE(f.has_value());
    CHECK(f->kind == "plane");
    CHECK((std::abs(f->area - 6.0) < 1e-6 || std::abs(f->area - 8.0) < 1e-6 ||
           std::abs(f->area - 12.0) < 1e-6));
    CHECK_THAT(glm::length(f->normal), WithinRel(1.0f, 1e-4f));
    if (glm::dot(f->normal, f->centroid - center) > 1e-4f)
      outwardNormals++;
  }
  CHECK(outwardNormals == 6); // all six faces point outward
  // Every named edge is a straight line of length 2, 3, or 4 — no radius.
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    (void)name;
    const auto e = Measure::DescribeEdgeAt(body, idx);
    REQUIRE(e.has_value());
    CHECK(e->kind == "line");
    CHECK_FALSE(e->hasRadius);
    CHECK((std::abs(e->length - 2.0) < 1e-6 ||
           std::abs(e->length - 3.0) < 1e-6 ||
           std::abs(e->length - 4.0) < 1e-6));
  }
  // Out-of-range indices report nullopt, not garbage.
  CHECK_FALSE(Measure::DescribeFaceAt(body, 99).has_value());
  CHECK_FALSE(Measure::DescribeEdgeAt(body, 99).has_value());
}

TEST_CASE("DescribeFaceAt/DescribeEdgeAt classify a cylinder", "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::vec3(0), glm::vec3(0, 0, 1), /*radius=*/2.0, /*height=*/5.0));
  const Body& body = doc.GetBodies()[0];

  int planes = 0, cylinders = 0, circles = 0;
  for (const auto& [idx, name] : body.elementMap.names) {
    (void)name;
    const auto f = Measure::DescribeFaceAt(body, idx);
    REQUIRE(f.has_value());
    CHECK_THAT(glm::length(f->normal), WithinRel(1.0f, 1e-4f));
    if (f->kind == "plane") {
      planes++;
      CHECK_THAT(f->area, WithinRel(4.0 * M_PI, 1e-6)); // pi r^2, r=2
      // The caps face along the cylinder axis (+/-Z).
      CHECK_THAT(std::abs(f->normal.z), WithinRel(1.0f, 1e-4f));
    } else if (f->kind == "cylinder") {
      cylinders++;
      CHECK_THAT(f->area, WithinRel(2.0 * M_PI * 2.0 * 5.0, 1e-6));
      // The barrel normal is radial: perpendicular to the axis (its centroid
      // sits ON the axis, so only perpendicularity is well-defined here).
      CHECK(std::abs(f->normal.z) < 1e-4f);
    }
  }
  CHECK(planes == 2);    // the two caps
  CHECK(cylinders == 1); // the barrel
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    (void)name;
    const auto e = Measure::DescribeEdgeAt(body, idx);
    REQUIRE(e.has_value());
    if (e->kind == "circle") {
      circles++;
      REQUIRE(e->hasRadius);
      CHECK_THAT(e->radius, WithinRel(2.0, 1e-6));
      CHECK_THAT(e->length, WithinRel(4.0 * M_PI, 1e-6)); // 2 pi r
    }
  }
  CHECK(circles >= 2); // both rims classify as full circles
}

TEST_CASE("InterferenceVolume over transformed body sets",
          "[measure][assembly]") {
  // One unit-box part measured as two placements: overlapping by half a box
  // -> volume 0.5; fully apart -> 0.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  const auto& bodies = doc.GetBodies();

  const glm::mat4 xfA(1.0f);
  const glm::mat4 half =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));
  auto v = Measure::InterferenceVolume(bodies, xfA, bodies, half);
  REQUIRE(v.has_value());
  CHECK_THAT(*v, WithinRel(0.5, 1e-6));

  const glm::mat4 apart =
      glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
  v = Measure::InterferenceVolume(bodies, xfA, bodies, apart);
  REQUIRE(v.has_value());
  CHECK_THAT(*v, WithinAbs(0.0, 1e-9));

  // Rotation matters: a box spun 45 deg about Z at x=1.2 clips the corner of
  // the unit box; unrotated at the same spot it wouldn't.
  const glm::mat4 spun =
      glm::translate(glm::mat4(1.0f), glm::vec3(1.2f, 0.5f, 0.0f)) *
      glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0, 0, 1));
  v = Measure::InterferenceVolume(bodies, xfA, bodies, spun);
  REQUIRE(v.has_value());
  CHECK(*v > 1e-6);
  v = Measure::InterferenceVolume(
      bodies, xfA, bodies,
      glm::translate(glm::mat4(1.0f), glm::vec3(1.2f, 0.5f, 0.0f)));
  REQUIRE(v.has_value());
  CHECK_THAT(*v, WithinAbs(0.0, 1e-9));
}

TEST_CASE("ContactBetween separates clash, touching and clear",
          "[measure][assembly]") {
  // One unit-box part in three relative placements: overlapping (volume,
  // distance 0), face-tangent (no volume, distance 0), and 5 apart (clear).
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  const auto& bodies = doc.GetBodies();
  const glm::mat4 xfA(1.0f);

  auto c = Measure::ContactBetween(
      bodies, xfA, bodies,
      glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f)));
  REQUIRE(c.has_value());
  CHECK_THAT(c->volume, WithinRel(0.5, 1e-6));
  CHECK_THAT(c->distance, WithinAbs(0.0, 1e-9));

  c = Measure::ContactBetween(
      bodies, xfA, bodies,
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
  REQUIRE(c.has_value());
  CHECK_THAT(c->volume, WithinAbs(0.0, 1e-9));
  CHECK_THAT(c->distance, WithinAbs(0.0, 1e-6));

  c = Measure::ContactBetween(
      bodies, xfA, bodies,
      glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, 0.0f, 0.0f)));
  REQUIRE(c.has_value());
  CHECK_THAT(c->volume, WithinAbs(0.0, 1e-9));
  CHECK_THAT(c->distance, WithinRel(5.0, 1e-6));

  // Diagonal offset: the exact distance is corner-to-corner, shorter than
  // any single-axis reading — catches a box-distance-only shortcut.
  c = Measure::ContactBetween(
      bodies, xfA, bodies,
      glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 4.0f, 0.0f)));
  REQUIRE(c.has_value());
  CHECK_THAT(c->distance, WithinRel(3.0 * std::sqrt(2.0), 1e-6));

  // WorldBounds places the box for the report's prefilter.
  glm::vec3 mn, mx;
  REQUIRE(Measure::WorldBounds(
      bodies, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)), mn,
      mx));
  CHECK_THAT(mn.x, WithinAbs(2.0, 1e-4));
  CHECK_THAT(mx.x, WithinAbs(3.0, 1e-4));
}

TEST_CASE("AnalyzeWatertight reports closed solids and open faces",
          "[measure]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 1, 1, 1));
  const auto r = Measure::AnalyzeWatertight(doc.GetBodies()[0].shape);
  CHECK(r.watertight);
  CHECK(r.shells == 1);
  CHECK(r.freeEdges == 0);

  // A cylinder's seam edge must not count as free.
  Document doc2;
  doc2.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::vec3(0), glm::vec3(0, 0, 1), 1.0, 2.0));
  const auto rc = Measure::AnalyzeWatertight(doc2.GetBodies()[0].shape);
  CHECK(rc.watertight);
  CHECK(rc.freeEdges == 0);

  // A null shape is not watertight (and doesn't crash).
  const auto rn = Measure::AnalyzeWatertight(CADShape{});
  CHECK_FALSE(rn.watertight);
  CHECK(rn.shells == 0);
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
