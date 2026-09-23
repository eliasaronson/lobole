#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/BooleanFeature.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

TEST_CASE("An element's position is not quantised to float", "[measure]") {
  // FaceInfo/EdgeInfo carried positions as glm::vec3 while the kernel
  // computes them in double, so an answer to "where is this face" was
  // quantised on the way out for no reason. 164.8 is not representable in
  // binary, which is what makes it tell the two apart: as float it reads
  // 164.800003.
  //
  // A body built on a SKETCH is still placed in float32 (SketchPlane holds
  // its origin as glm::vec3), so this test uses a primitive deliberately —
  // it pins the measurement path, which is the part that had no excuse.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::dvec3(0.0), 164.8, 164.8, 164.8));
  const uint64_t id = doc.GetFeature(0)->GetId();
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];

  bool sawFace = false, sawEdge = false;
  for (const auto& [idx, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, idx);
    if (!f || f->kind != "plane")
      continue;
    for (int axis = 0; axis < 3; axis++) {
      // Each face sits at 0 or 164.8 along its own normal.
      if (std::abs(f->normal[axis]) < 0.5)
        continue;
      const double at = f->centroid[axis];
      if (std::abs(at) < 1e-12)
        continue;
      CHECK_THAT(at, WithinAbs(164.8, 1e-12));
      sawFace = true;
    }
  }
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    const auto e = Measure::DescribeEdgeAt(body, idx);
    if (!e || e->kind != "line")
      continue;
    for (int axis = 0; axis < 3; axis++) {
      const double at = e->midpoint[axis];
      // An edge midpoint is at 0, 82.4 (mid-span) or 164.8.
      if (std::abs(at) < 1e-12 || std::abs(at - 82.4) < 1e-12)
        continue;
      CHECK_THAT(at, WithinAbs(164.8, 1e-12));
      sawEdge = true;
    }
  }
  CHECK(sawFace); // a check that examined nothing is not a check
  CHECK(sawEdge);
}

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

TEST_CASE("Describe reports a cylindrical face's radius", "[measure]") {
  // The gap this closes: Describe (resolve by stable NAME — what the GUI tool
  // and the `measure` RPC both use) classified circular EDGES only, so
  // clicking a bore reported 4712 mm² of surface area and no diameter, while
  // DescribeFaceAt — resolve by INDEX, used by query_geometry — had the
  // radius all along. One classifier now serves both.
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "C", glm::vec3(0), glm::vec3(0, 0, 1), /*radius=*/2.0, /*height=*/5.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];

  bool sawWall = false, sawCap = false;
  for (const std::string& name : FaceNames(body)) {
    const auto e = Measure::Describe(doc.GetBodies(), ElementRef{id, name});
    REQUIRE(e.has_value());
    CHECK(e->kind == "face");
    if (e->shape == "cylinder") {
      sawWall = true;
      REQUIRE(e->hasRadius);
      CHECK_THAT(e->radius, WithinRel(2.0, 1e-9));
      REQUIRE(e->hasAxis);
      // The axis is the cylinder's, up to an arbitrary sign.
      CHECK_THAT(std::abs(glm::dot(e->axis, glm::vec3(0, 0, 1))),
                 WithinRel(1.0f, 1e-4f));
    } else if (e->shape == "plane") {
      sawCap = true;
      CHECK_FALSE(e->hasRadius); // a flat cap is not round in the CAD sense
    }
  }
  CHECK(sawWall);
  CHECK(sawCap);

  // Edges keep what they always reported, now through the shared classifier.
  bool sawCircle = false;
  for (const std::string& name : EdgeNames(body)) {
    const auto e = Measure::Describe(doc.GetBodies(), ElementRef{id, name});
    REQUIRE(e.has_value());
    if (e->shape == "circle") {
      sawCircle = true;
      REQUIRE(e->hasRadius);
      CHECK_THAT(e->radius, WithinRel(2.0, 1e-9));
    }
  }
  CHECK(sawCircle);
}

namespace {
// The named face whose outward normal points along `axis`, for the angle
// tests below — a box's faces are only identifiable by where they point.
std::string FaceFacing(const Document& doc, const glm::vec3& axis) {
  const Body& body = doc.GetBodies()[0];
  for (const auto& [idx, name] : body.elementMap.names)
    if (const auto f = Measure::DescribeFaceAt(body, idx))
      if (glm::dot(f->normal, glm::dvec3(axis)) > 0.99)
        return name;
  return {};
}
// The named straight edge running along `axis` (either sign).
std::string EdgeAlong(const Document& doc, uint64_t id, const glm::vec3& axis) {
  const Body& body = doc.GetBodies()[0];
  for (const auto& [idx, name] : body.elementMap.edgeNames) {
    const auto e = Measure::DescribeEdgeAt(body, idx);
    if (!e || e->kind != "line")
      continue;
    // Direction from the ref itself, so this helper cannot disagree with
    // what DirectionOf will see.
    const auto d = Measure::DirectionOf(doc.GetBodies(), ElementRef{id, name});
    if (d &&
        std::abs(glm::dot(glm::normalize(d->dir), glm::dvec3(axis))) > 0.99)
      return name;
  }
  return {};
}
} // namespace

TEST_CASE("Angle between a box's faces and edges", "[measure]") {
  // The measurement the tool could not take. The screenshot that started
  // this: two edges meeting at a corner report distance 0.000 mm, which is
  // true and useless — the question was the 90 degrees between them.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const auto ref = [&](const std::string& n) { return ElementRef{id, n}; };

  const std::string px = FaceFacing(doc, {1, 0, 0});
  const std::string nx = FaceFacing(doc, {-1, 0, 0});
  const std::string py = FaceFacing(doc, {0, 1, 0});
  REQUIRE_FALSE(px.empty());
  REQUIRE_FALSE(nx.empty());
  REQUIRE_FALSE(py.empty());

  // Two ADJACENT faces: 90 degrees, and flagged perpendicular.
  {
    const auto a = Measure::Angle(doc.GetBodies(), ref(px), ref(py));
    REQUIRE(a.has_value());
    CHECK_THAT(a->degrees, WithinAbs(90.0, 1e-6));
    CHECK(a->perpendicular);
    CHECK_FALSE(a->parallel);
    CHECK(a->basis == "between the face normals");
  }
  // Two OPPOSITE faces: 180 degrees, because a planar face's outward normal
  // is oriented and this is what a draftsman means by the angle between two
  // faces. Parallel is still true — the planes are parallel; it is the
  // ANGLE that is degenerate, which is what that flag is for.
  {
    const auto a = Measure::Angle(doc.GetBodies(), ref(px), ref(nx));
    REQUIRE(a.has_value());
    CHECK_THAT(a->degrees, WithinAbs(180.0, 1e-6));
    CHECK_THAT(a->acute, WithinAbs(0.0, 1e-6));
    CHECK(a->parallel);
  }
  // A face against itself: 0 degrees, parallel.
  {
    const auto a = Measure::Angle(doc.GetBodies(), ref(px), ref(px));
    REQUIRE(a.has_value());
    CHECK_THAT(a->degrees, WithinAbs(0.0, 1e-6));
    CHECK(a->parallel);
  }
  // Two perpendicular EDGES: 90 degrees. A line's direction sign is
  // whichever way OCC parameterised it, so the result is unsigned and the
  // basis says so — otherwise the same pair could read 90 or 270 depending
  // on which corner of the box the edge was built from.
  {
    const std::string ex = EdgeAlong(doc, id, {1, 0, 0});
    const std::string ez = EdgeAlong(doc, id, {0, 0, 1});
    REQUIRE_FALSE(ex.empty());
    REQUIRE_FALSE(ez.empty());
    const auto a = Measure::Angle(doc.GetBodies(), ref(ex), ref(ez));
    REQUIRE(a.has_value());
    CHECK_THAT(a->degrees, WithinAbs(90.0, 1e-6));
    CHECK(a->perpendicular);
    CHECK(a->basis.find("unsigned") != std::string::npos);

    // Two PARALLEL edges never report 180: unsigned folds to [0, 90].
    const auto par = Measure::Angle(doc.GetBodies(), ref(ex), ref(ex));
    REQUIRE(par.has_value());
    CHECK_THAT(par->degrees, WithinAbs(0.0, 1e-6));
    CHECK(par->parallel);
  }
}

TEST_CASE("DirectionOf refuses elements that define no direction",
          "[measure]") {
  // A sphere has no axis. Handing back a plausible one is exactly how a
  // measurement comes back confidently wrong, so this is a refusal.
  Document doc;
  doc.AddFeature(std::make_unique<SphereFeature>("S", glm::vec3(0), 3.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];
  int refused = 0, total = 0;
  for (const auto& [idx, name] : body.elementMap.names) {
    (void)idx;
    total++;
    if (!Measure::DirectionOf(doc.GetBodies(), ElementRef{id, name}))
      refused++;
  }
  CHECK(total > 0);
  CHECK(refused == total);
  // ...and an angle against one is nullopt, not zero.
  const std::string first = body.elementMap.names.begin()->second;
  CHECK_FALSE(Measure::Angle(doc.GetBodies(), ElementRef{id, first},
                             ElementRef{id, first})
                  .has_value());
  // A ref that no longer resolves is refused too.
  CHECK_FALSE(Measure::DirectionOf(doc.GetBodies(), ElementRef{id, "gone"})
                  .has_value());
}

TEST_CASE("Angle across placed occurrences", "[measure]") {
  // The assembly overload: a face's direction is rotated by the occurrence
  // transform before the angle, and translation cannot change one.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 2, 2));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string px = FaceFacing(doc, {1, 0, 0});
  REQUIRE_FALSE(px.empty());
  const ElementRef r{id, px};

  const glm::mat4 far = glm::translate(glm::mat4(1.0f), glm::vec3(500, 0, 0));
  const auto moved = Measure::Angle(doc.GetBodies(), r, glm::mat4(1.0f),
                                    doc.GetBodies(), r, far);
  REQUIRE(moved.has_value());
  CHECK_THAT(moved->degrees, WithinAbs(0.0, 1e-6)); // still facing the same way

  const glm::mat4 spun =
      glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0, 1, 0));
  const auto turned = Measure::Angle(doc.GetBodies(), r, glm::mat4(1.0f),
                                     doc.GetBodies(), r, spun);
  REQUIRE(turned.has_value());
  CHECK_THAT(turned->degrees, WithinAbs(30.0, 1e-4));
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

TEST_CASE("Distance kinds: minimum, maximum, centres", "[measure]") {
  // Minimum is one of three questions. Maximum is the clearance question —
  // "will it still fit at the WORST point" — which a minimum-only tool
  // answers plausibly and wrongly; centres is hole spacing, and the only one
  // of the three that is stable as the shapes around it change.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2));
  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();

  // The two facing faces: A's +X at x=2, B's -X at x=10.
  std::string aPlusX, bMinusX;
  for (const auto& [idx, name] : doc.GetBodies()[0].elementMap.names)
    if (const auto f = Measure::DescribeFaceAt(doc.GetBodies()[0], idx))
      if (f->normal.x > 0.99f)
        aPlusX = name;
  for (const auto& [idx, name] : doc.GetBodies()[1].elementMap.names)
    if (const auto f = Measure::DescribeFaceAt(doc.GetBodies()[1], idx))
      if (f->normal.x < -0.99f)
        bMinusX = name;
  REQUIRE_FALSE(aPlusX.empty());
  REQUIRE_FALSE(bMinusX.empty());
  const ElementRef ra{idA, aPlusX}, rb{idB, bMinusX};
  const glm::mat4 I(1.0f);

  const auto mn =
      Measure::DistanceOfKind(doc.GetBodies(), ra, I, doc.GetBodies(), rb, I,
                              Measure::DistanceKind::Minimum);
  REQUIRE(mn.has_value());
  CHECK_THAT(mn->distance, WithinAbs(8.0, 1e-6)); // 10 - 2, face to face

  // Centres: both face centroids are at y=z=1, so the gap is the same 8 —
  // but now anchored at the centres rather than wherever the extrema landed.
  const auto ce =
      Measure::DistanceOfKind(doc.GetBodies(), ra, I, doc.GetBodies(), rb, I,
                              Measure::DistanceKind::Centres);
  REQUIRE(ce.has_value());
  CHECK_THAT(ce->distance, WithinAbs(8.0, 1e-6));
  CHECK_THAT(ce->pointA.x, WithinAbs(2.0f, 1e-4f));
  CHECK_THAT(ce->pointA.y, WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(ce->pointB.x, WithinAbs(10.0f, 1e-4f));

  // Maximum: corner to opposite corner of the two 2x2 faces — 8 across plus
  // 2 and 2 along the face diagonals => sqrt(64 + 4 + 4).
  const auto mx =
      Measure::DistanceOfKind(doc.GetBodies(), ra, I, doc.GetBodies(), rb, I,
                              Measure::DistanceKind::Maximum);
  REQUIRE(mx.has_value());
  CHECK_THAT(mx->distance, WithinAbs(std::sqrt(72.0), 1e-6));
  CHECK(mx->distance > mn->distance);
}

TEST_CASE("CornerOf resolves a corner from its two named edges", "[measure]") {
  // The point that has no name of its own. Two named edges have one, and so
  // therefore does the vertex they share — which is what makes a corner
  // measurement survive a regen like every other measurement here.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];

  // Any face's corners, through the kernel, then back through Measure.
  std::vector<CADShape::FaceCorner> corners;
  body.shape.FaceCorners(0, corners);
  REQUIRE(corners.size() == 4);
  int checked = 0;
  for (const CADShape::FaceCorner& c : corners) {
    const std::string* a = body.elementMap.EdgeNameOf(c.edgeA);
    const std::string* b = body.elementMap.EdgeNameOf(c.edgeB);
    REQUIRE((a && b));
    const auto p = Measure::CornerOf(doc.GetBodies(), ElementRef{id, *a},
                                     ElementRef{id, *b});
    REQUIRE(p.has_value());
    CHECK_THAT(p->x, WithinAbs(c.point.x, 1e-6f));
    CHECK_THAT(p->y, WithinAbs(c.point.y, 1e-6f));
    CHECK_THAT(p->z, WithinAbs(c.point.z, 1e-6f));
    checked++;
  }
  CHECK(checked == 4);

  // Two edges that never meet name no corner — refused, not approximated.
  const auto names = EdgeNames(body);
  bool sawRefusal = false;
  for (const std::string& n1 : names)
    for (const std::string& n2 : names)
      if (n1 != n2 && !Measure::CornerOf(doc.GetBodies(), ElementRef{id, n1},
                                         ElementRef{id, n2}))
        sawRefusal = true;
  CHECK(sawRefusal); // a box has plenty of non-adjacent edge pairs
  // A dangling name is refused too.
  CHECK_FALSE(Measure::CornerOf(doc.GetBodies(), ElementRef{id, names[0]},
                                ElementRef{id, "gone"})
                  .has_value());
}

TEST_CASE("Corner to corner is the diagonal", "[measure]") {
  // The measurement point picking was wanted FOR: this corner to that one.
  // Max distance between two faces gives the furthest pair, not the pair you
  // pointed at; a corner names exactly one point.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 3, 4, 12));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];

  // Gather every distinct corner of the solid, through the face corners.
  std::vector<glm::vec3> pts;
  for (int f = 0; f < 6; f++) {
    std::vector<CADShape::FaceCorner> corners;
    body.shape.FaceCorners(f, corners);
    for (const CADShape::FaceCorner& c : corners) {
      const std::string* a = body.elementMap.EdgeNameOf(c.edgeA);
      const std::string* b = body.elementMap.EdgeNameOf(c.edgeB);
      if (!a || !b)
        continue;
      const auto p = Measure::CornerOf(doc.GetBodies(), ElementRef{id, *a},
                                       ElementRef{id, *b});
      REQUIRE(p.has_value());
      bool seen = false;
      for (const glm::vec3& q : pts)
        if (glm::distance(q, *p) < 1e-6f)
          seen = true;
      if (!seen)
        pts.push_back(*p);
    }
  }
  CHECK(pts.size() == 8); // a box has eight, each found from three faces

  // The furthest pair is the body diagonal, exactly.
  double far = 0.0;
  for (const glm::vec3& a : pts)
    for (const glm::vec3& b : pts)
      far = std::max(far, static_cast<double>(glm::distance(a, b)));
  CHECK_THAT(far, WithinAbs(std::sqrt(9.0 + 16.0 + 144.0), 1e-4));
}

TEST_CASE("Centre to centre uses the CENTRE of a round element", "[measure]") {
  // Hole spacing, and the reason it needs its own function. AnchorOf gives a
  // representative point ON the element, which for a circle is on the RIM.
  // Two holes of the same radius then cancel their offsets and answer
  // correctly by luck; two holes of DIFFERENT radii report a plausible wrong
  // number — measured on a 200x100 plate, 102.6 mm where the spacing is
  // 104.4.
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "A", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), /*radius=*/5.0,
      /*height=*/10.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "B", glm::vec3(100, 30, 0), glm::vec3(0, 0, 1), /*radius=*/12.0,
      /*height=*/10.0));
  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();

  // The circular rim edge of each.
  const auto rimOf = [&](size_t bodyIndex, uint64_t id) {
    const Body& body = doc.GetBodies()[bodyIndex];
    for (const auto& [idx, name] : body.elementMap.edgeNames) {
      const auto e = Measure::DescribeEdgeAt(body, idx);
      if (e && e->kind == "circle")
        return ElementRef{id, name};
    }
    return ElementRef{id, ""};
  };
  const ElementRef ra = rimOf(0, idA), rb = rimOf(1, idB);
  REQUIRE_FALSE(ra.name.empty());
  REQUIRE_FALSE(rb.name.empty());

  // The rim points are NOT the centres — that is the whole trap.
  const auto anchorA = Measure::AnchorOf(doc.GetBodies(), ra);
  const auto centreA = Measure::CentreOf(doc.GetBodies(), ra);
  REQUIRE(anchorA.has_value());
  REQUIRE(centreA.has_value());
  CHECK(glm::distance(*anchorA, *centreA) > 4.9f); // a radius apart
  CHECK_THAT(centreA->x, WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(centreA->y, WithinAbs(0.0f, 1e-4f));

  const glm::mat4 I(1.0f);
  const auto d =
      Measure::DistanceOfKind(doc.GetBodies(), ra, I, doc.GetBodies(), rb, I,
                              Measure::DistanceKind::Centres);
  REQUIRE(d.has_value());
  CHECK_THAT(d->distance, WithinAbs(std::hypot(100.0, 30.0), 1e-4));

  // A cylindrical FACE centres on its axis, level with the face — where a
  // drawing would put the centre mark.
  std::string wallA;
  for (const auto& [idx, name] : doc.GetBodies()[0].elementMap.names)
    if (const auto f = Measure::DescribeFaceAt(doc.GetBodies()[0], idx))
      if (f->kind == "cylinder")
        wallA = name;
  REQUIRE_FALSE(wallA.empty());
  const auto wc = Measure::CentreOf(doc.GetBodies(), ElementRef{idA, wallA});
  REQUIRE(wc.has_value());
  CHECK_THAT(wc->x, WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(wc->y, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Distance kinds refuse an unknown name", "[measure]") {
  // Never defaulted to minimum: a substituted minimum is plausible and wrong
  // exactly where the caller asked for something else.
  Measure::DistanceKind k = Measure::DistanceKind::Maximum;
  CHECK(Measure::DistanceKindFromString("minimum", k));
  CHECK(k == Measure::DistanceKind::Minimum);
  CHECK(Measure::DistanceKindFromString("centers", k)); // both spellings
  CHECK(k == Measure::DistanceKind::Centres);
  CHECK(Measure::DistanceKindFromString("centres", k));
  CHECK(k == Measure::DistanceKind::Centres);
  const Measure::DistanceKind before = k;
  CHECK_FALSE(Measure::DistanceKindFromString("nearest", k));
  CHECK(k == before); // a refusal leaves the output alone
  CHECK(Measure::DistanceKindNames().size() == 3);
}

TEST_CASE("Whole bodies are measurable, and a clash reports as one",
          "[measure]") {
  // "How far apart are these two parts" is the most natural question in an
  // assembly. ContactBetween has answered it since I4 for the interference
  // report; the measure tool's armed kinds were Face|Edge and nothing else,
  // so the pick was simply refused.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2));
  const uint64_t idA = doc.GetFeature(0)->GetId();
  const uint64_t idB = doc.GetFeature(1)->GetId();
  const glm::mat4 I(1.0f);

  Measure::Target a{&doc.GetBodies(), I, ElementRef{idA, ""}};
  Measure::Target b{&doc.GetBodies(), I, ElementRef{idB, ""}};
  CHECK(a.Whole());

  const auto gap =
      Measure::DistanceBetween(a, b, Measure::DistanceKind::Minimum);
  REQUIRE(gap.has_value());
  CHECK_THAT(gap->distance, WithinAbs(8.0, 1e-6)); // 10 - 2

  // Centres of two 2-cubes 10 apart: 10 between the centres of mass.
  const auto ctr =
      Measure::DistanceBetween(a, b, Measure::DistanceKind::Centres);
  REQUIRE(ctr.has_value());
  CHECK_THAT(ctr->distance, WithinAbs(10.0, 1e-6));

  // A whole body reports what a part actually has.
  const auto d = Measure::DescribeTarget(a);
  REQUIRE(d.has_value());
  CHECK_THAT(d->volume, WithinAbs(8.0, 1e-6));
  CHECK_THAT(d->surfaceArea, WithinAbs(24.0, 1e-6));
  CHECK_THAT(d->size.x, WithinAbs(2.0f, 1e-4f));
  CHECK(d->solids == 1);

  // Clear, not overlapping.
  const auto clear = Measure::OverlapBetween(a, b);
  REQUIRE(clear.has_value());
  CHECK_THAT(clear->volume, WithinAbs(0.0, 1e-9));

  // A MIXED pair — this face to that whole part — is one question, not a
  // case anyone has to remember to write.
  std::string aPlusX;
  for (const auto& [idx, name] : doc.GetBodies()[0].elementMap.names)
    if (const auto f = Measure::DescribeFaceAt(doc.GetBodies()[0], idx))
      if (f->normal.x > 0.99f)
        aPlusX = name;
  REQUIRE_FALSE(aPlusX.empty());
  Measure::Target face{&doc.GetBodies(), I, ElementRef{idA, aPlusX}};
  const auto mixed =
      Measure::DistanceBetween(face, b, Measure::DistanceKind::Minimum);
  REQUIRE(mixed.has_value());
  CHECK_THAT(mixed->distance, WithinAbs(8.0, 1e-6));
  // ...but a face has no volume to overlap with, so that is refused.
  CHECK_FALSE(Measure::OverlapBetween(face, b).has_value());
}

TEST_CASE("Two overlapping bodies report the overlap, not distance 0",
          "[measure]") {
  // The extrema solver says "0" about any two interpenetrating solids, which
  // is true and the least useful available fact. A clash should report as a
  // clash.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 4, 4, 4));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(2, 0, 0), 4, 4, 4));
  const glm::mat4 I(1.0f);
  Measure::Target a{&doc.GetBodies(), I,
                    ElementRef{doc.GetFeature(0)->GetId(), ""}};
  Measure::Target b{&doc.GetBodies(), I,
                    ElementRef{doc.GetFeature(1)->GetId(), ""}};

  const auto gap =
      Measure::DistanceBetween(a, b, Measure::DistanceKind::Minimum);
  REQUIRE(gap.has_value());
  CHECK_THAT(gap->distance, WithinAbs(0.0, 1e-6)); // true, and useless alone

  const auto over = Measure::OverlapBetween(a, b);
  REQUIRE(over.has_value());
  CHECK_THAT(over->volume, WithinAbs(2.0 * 4.0 * 4.0, 1e-3)); // the 2mm slab
}

TEST_CASE("TotalOf sums a set, and refuses a mixed one", "[measure]") {
  // "3 elements selected - keep two for a distance" was a refusal with no
  // reason and no alternative. Three edges have a total length; three faces
  // have a total area; an area plus a length is not a quantity at all.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 3, 4));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const Body& body = doc.GetBodies()[0];

  std::vector<ElementRef> edges;
  for (const std::string& n : EdgeNames(body))
    edges.push_back(ElementRef{id, n});
  const auto le = Measure::TotalOf(doc.GetBodies(), edges);
  REQUIRE(le.has_value());
  CHECK(le->kind == "edge");
  CHECK(le->count == 12);
  // A 2x3x4 box: four edges of each length.
  CHECK_THAT(le->total, WithinAbs(4.0 * (2.0 + 3.0 + 4.0), 1e-6));

  std::vector<ElementRef> faces;
  for (const std::string& n : FaceNames(body))
    faces.push_back(ElementRef{id, n});
  const auto ar = Measure::TotalOf(doc.GetBodies(), faces);
  REQUIRE(ar.has_value());
  CHECK(ar->kind == "face");
  CHECK(ar->count == 6);
  CHECK_THAT(ar->total, WithinAbs(2.0 * (6.0 + 8.0 + 12.0), 1e-6));

  std::vector<ElementRef> mixed{faces[0], edges[0]};
  CHECK_FALSE(Measure::TotalOf(doc.GetBodies(), mixed).has_value());
  CHECK_FALSE(Measure::TotalOf(doc.GetBodies(), {}).has_value());
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
    CHECK_THAT(glm::length(f->normal), WithinRel(1.0, 1e-4));
    if (glm::dot(f->normal, f->centroid - glm::dvec3(center)) > 1e-4)
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

// The dock's bathing ladder rested on its end joist and read as a CLASH of
// 0.1337740952176187 mm3 — a sliver, not an overlap: backing the part out by
// 0.0001 mm left the volume BIT-IDENTICAL (a real overlap shrinks
// continuously) and 0.001 mm took it to exactly zero. Fixed by giving the
// contact booleans a micron of fuzz (Measure::kContactFuzz), verified
// causally: scratch/build_dock.py fails its own "must touch" gate with the
// fuzz at 0 and passes every gate with it on.
//
// HONEST LIMIT: this case does NOT reproduce that sliver. Neither a planar
// pair, nor a cylinder tangent to a plane, nor either of those under a
// rotation written to 8 decimals, produces one — they all come out clean
// from an exact boolean. Whatever combination the ladder hits was not
// reduced. What this case pins is the INVARIANT the fix must keep: tangency
// reads as touching, a real bite still reads as a clash, and clear stays
// clear. The standing regression net for the sliver itself is the dock
// script, which is run by hand.
TEST_CASE("ContactBetween: a tangent cylinder is touching, not clashing",
          "[measure][assembly]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Joist", glm::vec3(0), 100.0, 10.0, 100.0));
  Document rod;
  rod.AddFeature(std::make_unique<CylinderFeature>(
      "Rail", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0, 0, 1), 16.0, 80.0));

  // ...and the placement is ROTATED by a quaternion written to 8 decimals,
  // as a hand-authored build script writes 90 degrees. That is the missing
  // ingredient: an exactly-coincident pair comes out of the boolean clean,
  // but a rotation carrying 1e-9 of error makes the faces microscopically
  // non-coincident, and THAT is what leaves a sliver.
  const glm::mat4 spin =
      glm::mat4_cast(glm::quat(0.70710678f, 0.0f, 0.70710678f, 0.0f));

  // Sitting exactly ON the joist's top face: tangent along a line.
  const float top = 10.0f;
  auto c = Measure::ContactBetween(
      doc.GetBodies(), glm::mat4(1.0f), rod.GetBodies(),
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, top + 16.0f, 0.0f)) *
          spin);
  REQUIRE(c.has_value());
  CHECK_THAT(c->volume, WithinAbs(0.0, 1e-9)); // touching, NOT a clash
  CHECK_THAT(c->distance, WithinAbs(0.0, 1e-3));

  // A real interference is still reported: sink it 0.5 mm into the joist.
  c = Measure::ContactBetween(
      doc.GetBodies(), glm::mat4(1.0f), rod.GetBodies(),
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, top + 15.5f, 0.0f)) *
          spin);
  REQUIRE(c.has_value());
  CHECK(c->volume > 1.0); // a 0.5 mm bite out of an 80 mm bar is cubic mm

  // And clear stays clear.
  c = Measure::ContactBetween(
      doc.GetBodies(), glm::mat4(1.0f), rod.GetBodies(),
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, top + 18.0f, 0.0f)) *
          spin);
  REQUIRE(c.has_value());
  CHECK_THAT(c->volume, WithinAbs(0.0, 1e-9));
  CHECK_THAT(c->distance, WithinRel(2.0, 1e-3));
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
  CHECK_FALSE(caps[0].GenerateMesh().vertices.empty());
  // A plane missing the body yields an empty/invalid cap, not a crash.
  auto missCaps = Measure::SectionCaps(doc.GetBodies(), glm::vec3(0, 0, 99),
                                       glm::vec3(0, 0, 1));
  REQUIRE(missCaps.size() == 1);
  CHECK_FALSE(missCaps[0].IsValid());
}

// Two sections at once INTERSECT, so each one's cap must be cut back to the
// other's kept side. Without that the fill juts out past the neighbouring cut
// and hangs in space — the "floating lid".
TEST_CASE("A cap is trimmed by the other section", "[measure][section]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 10, 10, 10));

  // Cut A: z = 5, keeping z <= 5. Its cap is the full 10x10 square...
  auto full = Measure::SectionCaps(doc.GetBodies(), glm::vec3(0, 0, 5),
                                   glm::vec3(0, 0, 1));
  REQUIRE(full.size() == 1);
  REQUIRE(full[0].IsValid());
  CADShape::MassProps mp;
  REQUIRE(full[0].ComputeMassProps(mp));
  CHECK_THAT(mp.surfaceArea, WithinRel(100.0, 1e-6));

  // ...until cut B (x = 5, keeping x <= 5) takes half of it away.
  const std::vector<glm::vec4> trim{glm::vec4(1, 0, 0, 5)};
  auto trimmed = Measure::SectionCaps(doc.GetBodies(), glm::vec3(0, 0, 5),
                                      glm::vec3(0, 0, 1), trim);
  REQUIRE(trimmed.size() == 1);
  REQUIRE(trimmed[0].IsValid());
  REQUIRE(trimmed[0].ComputeMassProps(mp));
  CHECK_THAT(mp.surfaceArea, WithinRel(50.0, 1e-6));
  glm::vec3 lo, hi;
  trimmed[0].GetBoundingBox(lo, hi);
  CHECK_THAT(hi.x, WithinAbs(5.0, 1e-6));  // stops at the other cut
  CHECK_THAT(hi.y, WithinAbs(10.0, 1e-6)); // untouched across

  // A trim plane that removes the cap entirely leaves no cap, not a sliver.
  auto gone =
      Measure::SectionCaps(doc.GetBodies(), glm::vec3(0, 0, 5),
                           glm::vec3(0, 0, 1), {glm::vec4(1, 0, 0, -1)});
  REQUIRE(gone.size() == 1);
  CHECK_FALSE(gone[0].IsValid());
}

// Every section test above cuts a body sitting on the origin, and that is
// exactly why this went unnoticed: the cutter face was SIZED from the bodies'
// bounding box but CENTERED on the plane's origin, so a model built away from
// the origin fell outside it. The area came back 0 and the section view drew
// no caps — a hollow-looking cut, on real projects (which are rarely modeled
// at 0,0,0) and on every assembly occurrence placed away from it.
TEST_CASE("Sections work on a body far from the origin", "[measure][section]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(1000, -500, 250), 2, 3, 4));

  // Cut along Z, halfway up: a 2x3 rectangle, wherever the box is.
  const glm::vec3 n(0, 0, 1);
  const glm::vec3 origin(0, 0, 252); // ONLY the normal component matters
  auto area = Measure::CrossSectionArea(doc.GetBodies(), origin, n);
  REQUIRE(area);
  CHECK_THAT(*area, WithinRel(6.0, 1e-6));

  auto caps = Measure::SectionCaps(doc.GetBodies(), origin, n);
  REQUIRE(caps.size() == 1);
  REQUIRE(caps[0].IsValid());
  CADShape::MassProps mp;
  REQUIRE(caps[0].ComputeMassProps(mp));
  CHECK_THAT(mp.surfaceArea, WithinRel(6.0, 1e-6));

  // ...and the cap is where the body is, not out at the origin.
  glm::vec3 lo, hi;
  caps[0].GetBoundingBox(lo, hi);
  CHECK_THAT(lo.x, WithinAbs(1000.0, 1e-6));
  CHECK_THAT(hi.y, WithinAbs(-497.0, 1e-6));
}

TEST_CASE("A section says how many ISLANDS the cut crosses",
          "[measure][section]") {
  using Catch::Matchers::WithinRel;
  // generators/README.md#single-cylinder-and-air-cooled-engines. The total area
  // cannot say whether the material is where you think it is: two boxes side by
  // side and one box of the same total area section identically. The regions
  // can.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2, 3, 4));
  auto one = Measure::CrossSectionRegions(doc.GetBodies(), glm::vec3(0, 0, 2),
                                          glm::vec3(0, 0, 1));
  REQUIRE(one.has_value());
  REQUIRE(one->size() == 1);
  CHECK_THAT((*one)[0].area, WithinRel(6.0, 1e-6));

  // A second box, clear of the first: same plane, two islands now.
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 1, 3, 4));
  auto two = Measure::CrossSectionRegions(doc.GetBodies(), glm::vec3(0, 0, 2),
                                          glm::vec3(0, 0, 1));
  REQUIRE(two.has_value());
  REQUIRE(two->size() == 2);
  // Biggest first, so the part comes before the bit that may not belong.
  CHECK_THAT((*two)[0].area, WithinRel(6.0, 1e-6));
  CHECK_THAT((*two)[1].area, WithinRel(3.0, 1e-6));
  // And each says WHERE it is, which is how you tell which island is which.
  CHECK_THAT((*two)[1].centroid.x, WithinRel(10.5, 1e-5));
  // The total is unchanged and still agrees with the sum.
  auto area = Measure::CrossSectionArea(doc.GetBodies(), glm::vec3(0, 0, 2),
                                        glm::vec3(0, 0, 1));
  REQUIRE(area.has_value());
  CHECK_THAT(*area, WithinRel((*two)[0].area + (*two)[1].area, 1e-9));

  // Regions are PER BODY, and two overlapping bodies are two of them. That
  // is the honest answer rather than a shortcoming: the document holds them
  // as two solids, and joining solids is what a Boolean (an extrude's `add`)
  // is for. A caller asking "is this feature joined to the rest" is asking
  // about a part that has been through that already.
  Document touching;
  touching.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2, 3, 4));
  touching.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(1.5, 0, 0), 2, 3, 4));
  auto overlap = Measure::CrossSectionRegions(
      touching.GetBodies(), glm::vec3(0, 0, 2), glm::vec3(0, 0, 1));
  REQUIRE(overlap.has_value());
  CHECK(overlap->size() == 2);

  // Once they ARE one body, the same geometry is one island — which is the
  // case that makes the check mean anything: a boss merged into a wall must
  // not read as separate.
  Document fused;
  fused.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 2, 3, 4));
  fused.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(1.5, 0, 0), 2, 3, 4));
  fused.AddFeature(std::make_unique<BooleanFeature>(
      "Join", fused.GetFeature(0)->GetId(), fused.GetFeature(1)->GetId(),
      BooleanOp::Union));
  REQUIRE(fused.GetBodies().size() == 1);
  auto joined = Measure::CrossSectionRegions(
      fused.GetBodies(), glm::vec3(0, 0, 2), glm::vec3(0, 0, 1));
  REQUIRE(joined.has_value());
  CHECK(joined->size() == 1);
}

TEST_CASE("A section can report the OUTLINE of what it cuts",
          "[measure][section]") {
  // An area and a centroid say a brace is 34,466 mm2 somewhere near the
  // middle. Recovering the SHAPE of a part you did not draw — which is most
  // of what porting a design in consists of — needs the boundary, and
  // without it one was measured off screenshots and then abandoned.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("plate", glm::vec3(0, 0, 0), 20, 10, 4));

  // Off by default: it walks every edge of the cut.
  auto plain = Measure::CrossSectionRegions(doc.GetBodies(), glm::vec3(0, 0, 2),
                                            glm::vec3(0, 0, 1));
  REQUIRE(plain.has_value());
  REQUIRE(plain->size() == 1);
  CHECK((*plain)[0].loops.empty());

  auto cut = Measure::CrossSectionRegions(doc.GetBodies(), glm::vec3(0, 0, 2),
                                          glm::vec3(0, 0, 1), true);
  REQUIRE(cut.has_value());
  REQUIRE(cut->size() == 1);
  const Measure::SectionRegion& r = (*cut)[0];
  REQUIRE(r.loops.size() == 1);
  CHECK(r.loops[0].outer); // the one loop a solid rectangle has IS the outline
  REQUIRE(r.loops[0].segments.size() == 4);

  double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
  for (const Measure::SectionSegment& seg : r.loops[0].segments) {
    CHECK(seg.kind == "line"); // a box's cut has no arcs in it
    for (const glm::vec3& pt : {seg.from, seg.to}) {
      minX = std::min(minX, double(pt.x));
      maxX = std::max(maxX, double(pt.x));
      minY = std::min(minY, double(pt.y));
      maxY = std::max(maxY, double(pt.y));
    }
    CHECK_THAT(seg.from.z, WithinAbs(2.0f, 1e-4f)); // on the cutting plane
  }
  CHECK_THAT(maxX - minX, WithinRel(20.0, 1e-6));
  CHECK_THAT(maxY - minY, WithinRel(10.0, 1e-6));

  // A hole makes a second loop, and only the outline is the outer one — the
  // distinction a caller needs to tell an outline from what is missing out
  // of it. `BRepTools::OuterWire` does NOT survive the boolean that makes a
  // cut face (nothing it returns is IsSame with any wire the explorer
  // walks), so this is the largest bounding box instead.
  Document holed;
  holed.AddFeature(
      std::make_unique<BoxFeature>("plate", glm::vec3(0, 0, 0), 20, 10, 4));
  holed.AddFeature(std::make_unique<CylinderFeature>(
      "bore", glm::vec3(10, 5, -1), glm::vec3(0, 0, 1), /*radius=*/2.0,
      /*height=*/6.0));
  holed.AddFeature(std::make_unique<BooleanFeature>(
      "drill", holed.GetFeature(0)->GetId(), holed.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  REQUIRE(holed.GetBodies().size() == 1);
  auto ring = Measure::CrossSectionRegions(
      holed.GetBodies(), glm::vec3(0, 0, 2), glm::vec3(0, 0, 1), true);
  REQUIRE(ring.has_value());
  REQUIRE(ring->size() == 1);
  REQUIRE((*ring)[0].loops.size() == 2);
  int outer = 0, arcs = 0;
  for (const Measure::SectionLoop& loop : (*ring)[0].loops) {
    if (loop.outer)
      outer++;
    for (const Measure::SectionSegment& seg : loop.segments)
      if (seg.kind == "arc") {
        arcs++;
        CHECK_THAT(seg.radius, WithinRel(2.0, 1e-6)); // the bore, by radius
      }
  }
  CHECK(outer == 1);
  CHECK(arcs >= 1);
}

TEST_CASE("Comparing two sets without the overlap measures everything else",
          "[measure][compare]") {
  // The overlap metrics cost three OCC booleans, and on a part carrying
  // toroidal fillets that is SECONDS — 13.2 s for one brace of the smart
  // speaker against 0.03 s for a tube with none. A caller reading
  // volume_ratio was paying for four metrics it never read
  // (docs/inspect/README.md#comparing-against-a-reference).
  CADShape a = CADKernel::Instance().CreateBox(40.0, 30.0, 20.0);
  CADShape b = CADKernel::Instance().CreateBox(40.0, 30.0, 22.0);
  REQUIRE(a.IsValid());
  REQUIRE(b.IsValid());
  std::vector<Body> setA(1), setB(1);
  setA[0].shape = a;
  setB[0].shape = b;

  const auto full = Measure::CompareBodySets(setA, glm::mat4(1.0f), setB,
                                             glm::mat4(1.0f), nullptr, nullptr,
                                             /*overlap=*/true);
  const auto cheap =
      Measure::CompareBodySets(setA, glm::mat4(1.0f), setB, glm::mat4(1.0f),
                               nullptr, nullptr, /*overlap=*/false);
  REQUIRE(full.has_value());
  REQUIRE(cheap.has_value());

  // Everything measured off the two shapes agrees between the modes.
  CHECK(cheap->hasOverlap == false);
  CHECK(full->hasOverlap == true);
  CHECK_THAT(cheap->volumeA, WithinRel(full->volumeA, 1e-9));
  CHECK_THAT(cheap->volumeB, WithinRel(full->volumeB, 1e-9));
  CHECK_THAT(cheap->surfaceA, WithinRel(full->surfaceA, 1e-9));
  CHECK(cheap->facesA == full->facesA);
  CHECK(cheap->edgesB == full->edgesB);
  CHECK_THAT(cheap->comB.z, WithinAbs(full->comB.z, 1e-9));

  // The overlap really was computed in one and not the other — and the cheap
  // one says so rather than leaving a zero that reads as "no overlap at all".
  CHECK(full->common > 0.0);
  CHECK(cheap->common == 0.0);
  CHECK(cheap->hasOverlap == false);
}
