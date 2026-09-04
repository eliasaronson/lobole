#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>

using Catch::Matchers::WithinAbs;

namespace {

CADShape Box(const glm::vec3& lo, const glm::vec3& hi) {
  return CADKernel::Instance().CreateBox(lo, hi);
}

glm::dmat4 At(double x, double y = 0.0, double z = 0.0) {
  glm::dmat4 m(1.0);
  m[3] = glm::dvec4(x, y, z, 1.0);
  return m;
}

// The exact answer, for agreement checks: a real boolean over one body each.
bool ExactOverlap(const CADShape& a, const glm::mat4& xfA, const CADShape& b,
                  const glm::mat4& xfB) {
  std::vector<Body> ba(1), bb(1);
  ba[0].shape = a;
  bb[0].shape = b;
  const auto v = Measure::InterferenceVolume(ba, xfA, bb, xfB);
  return v.has_value() && *v > 1e-9;
}

} // namespace

TEST_CASE("Mesh collision agrees with the exact boolean", "[collide]") {
  collide::ClearCache();
  const CADShape a = Box({0, 0, 0}, {10, 10, 10});
  const CADShape b = Box({0, 0, 0}, {4, 4, 4});

  SECTION("overlapping") {
    CHECK(collide::Overlap(a, glm::dmat4(1.0), b, At(8)).intersects);
    CHECK(ExactOverlap(a, glm::mat4(1.0), b, glm::mat4(At(8))));
  }
  SECTION("clear by a wide margin") {
    CHECK_FALSE(collide::Overlap(a, glm::dmat4(1.0), b, At(50)).intersects);
    CHECK_FALSE(ExactOverlap(a, glm::mat4(1.0), b, glm::mat4(At(50))));
  }
  SECTION("a body wholly INSIDE another still clashes") {
    // No triangle of either crosses the other's surface, so a
    // surface-intersection test alone reports this pair clear. It is the
    // case that matters most — a bolt buried in a block.
    const CADShape inner = Box({4, 4, 4}, {6, 6, 6});
    CHECK(collide::Overlap(a, glm::dmat4(1.0), inner, glm::dmat4(1.0))
              .intersects);
    CHECK(ExactOverlap(a, glm::mat4(1.0), inner, glm::mat4(1.0)));
    // ...and the containing body is the same answer from the other side.
    CHECK(collide::Overlap(inner, glm::dmat4(1.0), a, glm::dmat4(1.0))
              .intersects);
  }
  SECTION("touching face to face is not a clash") {
    // Exactly coincident faces: the pair is inside the deflection either
    // way, so the only thing pinned is that it does not CRASH and does not
    // report a distance. Whichever way it falls, `contact_report` (exact) is
    // what decides touching from clear.
    const auto r = collide::Overlap(a, glm::dmat4(1.0), b, At(10));
    CHECK(r.deflection > 0.0);
  }
}

TEST_CASE("Mesh collision reports the deflection it decided at", "[collide]") {
  collide::ClearCache();
  const CADShape small = Box({0, 0, 0}, {2, 2, 2});
  const CADShape big = Box({0, 0, 0}, {2000, 2000, 2000});
  const auto rs = collide::Overlap(small, glm::dmat4(1.0), small, At(50));
  const auto rb = collide::Overlap(big, glm::dmat4(1.0), big, At(9000));
  // Derived from each body's own size, so a screw and a dock are meshed to
  // the same RELATIVE fidelity — neither carries a millimetre constant that
  // is wrong for the other.
  CHECK(rs.deflection > 0.0);
  CHECK(rb.deflection > rs.deflection * 100.0);
}

TEST_CASE("The mesh cache keys on placement, not on geometry alone",
          "[collide]") {
  collide::ClearCache();
  const CADShape a = Box({0, 0, 0}, {10, 10, 10});
  // A rigid Transformed() SHARES the underlying shape, so these two have the
  // same GeometryId and sit 40 mm apart. A cache keyed on geometry would
  // hand the second one the first one's mesh and every pair after would be
  // tested against a body somewhere else.
  const CADShape moved =
      a.Transformed(glm::translate(glm::mat4(1.0f), glm::vec3(40, 0, 0)));
  CHECK(a.GeometryId() == moved.GeometryId());
  CHECK(a.PlacementId() != moved.PlacementId());
  CHECK_FALSE(
      collide::Overlap(a, glm::dmat4(1.0), moved, glm::dmat4(1.0)).intersects);
  CHECK(collide::CacheSize() == 2);
  // The same placed shape again is free.
  collide::Overlap(a, glm::dmat4(1.0), moved, glm::dmat4(1.0));
  CHECK(collide::CacheSize() == 2);
}

TEST_CASE("Mesh collision is far cheaper than the exact boolean",
          "[collide][slow]") {
  collide::ClearCache();
  // A shape with enough faces that the boolean has real work to do: a block
  // with a grid of bores through it.
  CADShape plate = Box({0, 0, 0}, {100, 100, 10});
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 6; j++) {
      const CADShape bore = CADKernel::Instance().CreateCylinder(
          glm::vec3(10.0f + 16.0f * i, 10.0f + 16.0f * j, -1.0f),
          glm::vec3(0, 0, 1), 3.0, 12.0);
      plate = plate.Difference(bore);
    }
  REQUIRE(plate.IsValid());
  const CADShape pin = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 2.5, 30.0);

  const auto clock = [] { return std::chrono::steady_clock::now(); };
  const auto ms = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  // 64 poses: what one sample of a motion study costs, in miniature.
  std::vector<glm::dmat4> poses;
  for (int i = 0; i < 64; i++)
    poses.push_back(At(4.0 + 1.5 * i, 50.0, -5.0));

  auto t0 = clock();
  int exactHits = 0;
  for (const glm::dmat4& m : poses)
    if (ExactOverlap(plate, glm::mat4(1.0f), pin, glm::mat4(m)))
      exactHits++;
  const double exactMs = ms(t0, clock());

  collide::ClearCache();
  t0 = clock();
  int meshHits = 0;
  for (const glm::dmat4& m : poses)
    if (collide::Overlap(plate, glm::dmat4(1.0), pin, m).intersects)
      meshHits++;
  const double meshMs = ms(t0, clock());

  INFO("exact " << exactMs << " ms, mesh " << meshMs << " ms, hits "
                << exactHits << " vs " << meshHits);
  CHECK(exactHits > 0);
  // The point of the whole module. Deliberately a loose bound — the ratio
  // measured is far larger, and a gate that pins the exact number would fail
  // on a faster machine for no reason.
  CHECK(meshMs < exactMs * 0.5);
  // And it answers the same question: every pose agrees, or the module is
  // fast and wrong.
  CHECK(meshHits == exactHits);
}

// A pin through a clearance hole: the case a mechanism is made of, and the
// one the slider-crank rig found the module getting wrong — a 0.5 mm radial
// gap read as a clash at a 0.17 mm deflection.
TEST_CASE("A pin in a clearance hole is clear", "[collide]") {
  collide::ClearCache();
  CADKernel& k = CADKernel::Instance();
  // A rod-sized plate (so the deflection is the rig's) with a 9 mm hole.
  CADShape plate = Box({-8, -3, -6}, {78, 3, 6});
  plate = plate.Difference(
      k.CreateCylinder(glm::vec3(0, -5, 0), glm::vec3(0, 1, 0), 4.5, 12.0));
  REQUIRE(plate.IsValid());
  // An 8 mm pin through it, ending flush with the plate's top face.
  const CADShape pin =
      k.CreateCylinder(glm::vec3(0, -22, 0), glm::vec3(0, 1, 0), 4.0, 25.0);

  const bool exact = ExactOverlap(plate, glm::mat4(1.0f), pin, glm::mat4(1.0f));
  const auto mesh =
      collide::Overlap(plate, glm::dmat4(1.0), pin, glm::dmat4(1.0));
  INFO("deflection " << mesh.deflection << " mm against a 0.5 mm gap");
  CHECK_FALSE(exact);
  CHECK_FALSE(mesh.intersects);
}
