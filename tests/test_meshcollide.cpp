// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/CreateActions.h"
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

TEST_CASE("A freed shape's address never brings back its mesh", "[collide]") {
  // The cache is keyed on PlacementId, which mixes the TShape's ADDRESS, and
  // a freed TShape's address is handed to the next one allocated. An entry
  // that kept only the mesh then served it for a different shape: the print
  // check's overlapping pair read the meshes of the pair 0.1 mm apart made
  // just before it, called the surfaces apart and skipped its boolean.
  collide::ClearCache();
  for (int i = 0; i < 200; ++i) {
    const bool apart = i % 2 == 0;
    const float x = apart ? 10.5f : 9.0f;
    const CADShape a = Box({0, 0, 0}, {10, 10, 10});
    const CADShape b = Box({x, 0, 0}, {x + 10, 10, 10});
    const collide::Gap gap =
        collide::Nearest(a, glm::dmat4(1.0), b, glm::dmat4(1.0), 2.0);
    INFO("round " << i << ", the second box at x = " << x);
    REQUIRE(gap.known);
    CHECK((gap.distance > 0.0) == apart);
  }
  // Entries the cache alone keeps alive are dropped as it grows, so holding
  // the shapes does not keep every one ever meshed.
  CHECK(collide::CacheSize() < 400);
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

TEST_CASE("A cap that STRADDLES a shaft is clear, not buried in it",
          "[collide]") {
  collide::ClearCache();
  CADKernel& k = CADKernel::Instance();
  // A camshaft, and a bearing cap sitting over it with a saddle notched out.
  // The cap's bounding box NESTS inside the shaft's — it is short, narrow and
  // well within the shaft's length — and its box CENTRE lands in the notch,
  // which is inside the shaft. Testing that centre said the cap was buried in
  // a shaft it merely sits on: `camshaft x cam cap` in a clean engine, where
  // an exact boolean called the same pose clear
  // (generators/README.md#single-cylinder-and-air-cooled-engines).
  // The shaft carries a LOBE, which is what makes its bounding box wide
  // enough to swallow the cap's — without that the boxes do not nest and the
  // containment path is never reached. That is why this needs a camshaft and
  // not a plain bar.
  CADShape shaft =
      k.CreateCylinder(glm::vec3(0, -90, 0), glm::vec3(0, 1, 0), 13.0, 180.0);
  shaft = shaft.Union(
      k.CreateCylinder(glm::vec3(0, -30, 0), glm::vec3(0, 1, 0), 24.0, 16.0));
  CADShape cap = Box({-20, 45, 0}, {20, 59, 14});
  cap = cap.Difference(
      k.CreateCylinder(glm::vec3(0, 40, 0), glm::vec3(0, 1, 0), 17.0, 25.0));
  REQUIRE(shaft.IsValid());
  REQUIRE(cap.IsValid());

  const bool exact = ExactOverlap(shaft, glm::mat4(1.0f), cap, glm::mat4(1.0f));
  const auto mesh =
      collide::Overlap(shaft, glm::dmat4(1.0), cap, glm::dmat4(1.0));
  INFO("deflection " << mesh.deflection << " mm against a 4 mm saddle gap");
  CHECK_FALSE(exact);
  CHECK_FALSE(mesh.intersects);

  // And the containment path it exercises must still WORK: a pin wholly
  // inside a block is a clash, whatever the boxes do. That is the case the
  // box-centre test was written for, and the fix must not lose it.
  CADShape block = Box({-30, -30, -30}, {30, 30, 30});
  const CADShape buried =
      k.CreateCylinder(glm::vec3(0, -10, 0), glm::vec3(0, 1, 0), 3.0, 20.0);
  const auto swallowed =
      collide::Overlap(block, glm::dmat4(1.0), buried, glm::dmat4(1.0));
  CHECK(swallowed.intersects);
}

TEST_CASE("Mesh distance answers what the exact query answers", "[collide]") {
  // The mesh distance is to the TESSELLATIONS, so it is right to within the
  // deflection and no better — that is the whole contract. What makes it
  // worth having is the second half: which FACES could hold the answer, so
  // an exact query can be asked of two of them instead of all of them.
  collide::ClearCache();
  const CADShape a = Box({0, 0, 0}, {10, 10, 10});
  const CADShape b = Box({0, 0, 0}, {10, 10, 10});

  SECTION("a known gap, on flat faces") {
    const collide::Gap gap =
        collide::Nearest(a, glm::dmat4(1.0), b, At(30), 2.0);
    REQUIRE(gap.known);
    CHECK_THAT(gap.distance, WithinAbs(20.0, 1e-9)); // 30 - 10
    // Planar faces are meshed exactly, so this one is not approximate at
    // all; the tolerance is the deflection only where a surface curves.
    CHECK(gap.deflection > 0.0);
    // Several pairs qualify, not one: the +X face holds the answer and the
    // four faces around it share its edges, whose points are the same 20 mm
    // away. That is the selection doing its job — it must not MISS the pair
    // that holds the answer, and a tie is not a miss. They come back
    // NEAREST FIRST, which is what lets a caller stop early.
    REQUIRE_FALSE(gap.pairs.empty());
    CHECK(gap.pairs.size() < 36); // not the whole 6x6 cross product
    CHECK_THAT(gap.pairs.front().distance, WithinAbs(20.0, 1e-9));
    for (size_t i = 1; i < gap.pairs.size(); i++)
      CHECK(gap.pairs[i].distance >= gap.pairs[i - 1].distance);
  }

  SECTION("diagonal, so the answer is a corner") {
    const collide::Gap gap =
        collide::Nearest(a, glm::dmat4(1.0), b, At(20, 20, 20), 2.0);
    REQUIRE(gap.known);
    CHECK_THAT(gap.distance, WithinAbs(std::sqrt(3.0) * 10.0, 1e-9));
  }

  SECTION("touching reads zero, overlapping reads zero") {
    CHECK_THAT(collide::Nearest(a, glm::dmat4(1.0), b, At(10), 2.0).distance,
               WithinAbs(0.0, 1e-9));
    CHECK_THAT(collide::Nearest(a, glm::dmat4(1.0), b, At(5), 2.0).distance,
               WithinAbs(0.0, 1e-9));
  }

  SECTION("curved: within the deflection of the exact answer, and far faster") {
    // Two cylinders side by side. The closest points are in the middle of
    // curved faces, where a tessellation is inscribed — so the mesh reads
    // slightly FAR, by less than the deflection.
    const CADShape c1 =
        CADKernel::Instance().CreateCylinder({0, 0, 0}, {0, 0, 1}, 5, 20);
    const CADShape c2 =
        CADKernel::Instance().CreateCylinder({0, 0, 0}, {0, 0, 1}, 5, 20);
    const collide::Gap gap =
        collide::Nearest(c1, glm::dmat4(1.0), c2, At(60), 2.0);
    REQUIRE(gap.known);
    const double exact = 50.0; // 60 between axes, 5 of radius each
    INFO("mesh " << gap.distance << " exact " << exact << " deflection "
                 << gap.deflection);
    CHECK(gap.distance >= exact - 1e-9);
    CHECK(gap.distance <= exact + 2.0 * gap.deflection);
    CHECK_FALSE(gap.pairs.empty());
  }

  SECTION("a shape with no triangles says so rather than guessing") {
    const collide::Gap gap =
        collide::Nearest(CADShape(), glm::dmat4(1.0), b, At(30), 2.0);
    CHECK_FALSE(gap.known);
  }
}

// A running gap finer than the derived deflection: a 100 mm ring meshes at
// about 0.2 mm, and print-in-place parts run at 0.1 mm a side. The check has
// to be ASKED for finer triangles, and must then actually cut them -- Build
// once took a fraction that changed only the number it reported.
TEST_CASE("A stated deflection is the one the mesh is cut to", "[collide]") {
  collide::ClearCache();
  CADKernel& k = CADKernel::Instance();
  // A ring with a 60 mm bore, and pins 0.1 mm clear of it and 0.05 mm over.
  CADShape ring =
      k.CreateCylinder(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 50.0, 10.0);
  ring = ring.Difference(
      k.CreateCylinder(glm::vec3(0, 0, -1), glm::vec3(0, 0, 1), 30.0, 12.0));
  REQUIRE(ring.IsValid());
  const CADShape clear =
      k.CreateCylinder(glm::vec3(0, 0, -5), glm::vec3(0, 0, 1), 29.9, 20.0);
  const CADShape over =
      k.CreateCylinder(glm::vec3(0, 0, -5), glm::vec3(0, 0, 1), 30.05, 20.0);
  REQUIRE_FALSE(ExactOverlap(ring, glm::mat4(1.0f), clear, glm::mat4(1.0f)));
  REQUIRE(ExactOverlap(ring, glm::mat4(1.0f), over, glm::mat4(1.0f)));

  const auto derived = collide::Mesh::Build(ring);
  const double fine = 0.02; // a fifth of the gap
  const auto ringFine = collide::Mesh::Build(ring, fine);
  REQUIRE_FALSE(ringFine->Empty());
  INFO("derived " << derived->Deflection() << " mm, "
                  << derived->TriangleCount() << " triangles; stated "
                  << ringFine->Deflection() << " mm, "
                  << ringFine->TriangleCount() << " triangles");
  CHECK(derived->Deflection() > 0.1); // coarser than the gap: the problem
  CHECK_THAT(ringFine->Deflection(), WithinAbs(fine, 1e-12));
  CHECK(ringFine->TriangleCount() > derived->TriangleCount());

  const auto clearFine = collide::Mesh::Build(clear, fine);
  const auto overFine = collide::Mesh::Build(over, fine);
  CHECK_FALSE(ringFine->Overlaps(glm::dmat4(1.0), *clearFine, glm::dmat4(1.0)));
  CHECK(ringFine->Overlaps(glm::dmat4(1.0), *overFine, glm::dmat4(1.0)));

  // ...and the fine mesh was cut on a copy: the document's own shape still
  // carries the triangulation the renderer drew, not the fine one.
  const auto again = collide::Mesh::Build(ring);
  CHECK(again->TriangleCount() == derived->TriangleCount());
}

// docs/inspect/README.md#touching-is-not-a-clash. Two faces seated on each
// other have coplanar triangles, and the separating-axis test reports any
// two coplanar triangles that overlap in their plane as meeting: a mesh
// check called every seat a clash, where exact interference (volume 0) calls
// it touching. The clock spaced its wheels an end shake apart to get past
// it. Touching is now its own answer, and an overlap deeper than the
// deflection is still a clash -- flush sides included, where no triangle
// pierces another and only the volume tells the two apart.
TEST_CASE("Two blocks face to face touch; overlapped by two deflections "
          "they clash",
          "[collide]") {
  collide::ClearCache();
  const CADShape a = Box({0, 0, 0}, {10, 10, 10});
  const CADShape b = Box({0, 0, 0}, {10, 10, 10});
  const collide::Result seated =
      collide::Overlap(a, glm::dmat4(1.0), b, At(10));
  const double d = seated.deflection;
  REQUIRE(d > 0.0);
  CHECK(seated.touching);
  CHECK_FALSE(seated.intersects);
  CHECK_FALSE(ExactOverlap(a, glm::mat4(1.0), b, glm::mat4(At(10))));

  // Flush sides, sunk end to end by twice the deflection.
  const collide::Result sunk =
      collide::Overlap(a, glm::dmat4(1.0), b, At(10 - 2 * d));
  CHECK(sunk.intersects);
  CHECK_FALSE(sunk.touching);
  CHECK(ExactOverlap(a, glm::mat4(1.0), b, glm::mat4(At(10 - 2 * d))));

  // Apart by a little more than nothing: clear.
  const collide::Result apart =
      collide::Overlap(a, glm::dmat4(1.0), b, At(10 + 0.5));
  CHECK_FALSE(apart.intersects);
  CHECK_FALSE(apart.touching);
}

TEST_CASE("A small block seated on a big one touches; sunk into it, it "
          "clashes",
          "[collide]") {
  collide::ClearCache();
  const CADShape big = Box({0, 0, 0}, {40, 40, 10});
  const CADShape small = Box({0, 0, 0}, {6, 6, 6});
  const collide::Result seated =
      collide::Overlap(big, glm::dmat4(1.0), small, At(5, 7, 10));
  const double d = seated.deflection;
  CHECK(seated.touching);
  CHECK_FALSE(seated.intersects);
  const collide::Result sunk =
      collide::Overlap(big, glm::dmat4(1.0), small, At(5, 7, 10 - 2 * d));
  CHECK(sunk.intersects);
  CHECK(ExactOverlap(big, glm::mat4(1.0), small,
                     glm::mat4(At(5, 7, 10 - 2 * d))));
  // Seated with a side flush with the big block's side.
  CHECK(collide::Overlap(big, glm::dmat4(1.0), small, At(0, 7, 10)).touching);
  CHECK(collide::Overlap(big, glm::dmat4(1.0), small, At(0, 7, 10 - 2 * d))
            .intersects);
}
