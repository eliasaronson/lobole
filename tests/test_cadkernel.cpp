#include "ui/CreateActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"

#include "cad/MeshExport.h"
#include <algorithm>
#include <chrono>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unistd.h>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("CreateBox produces a valid solid with correct volume",
          "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 3.0, 4.0);
  REQUIRE(box.IsValid());
  CHECK_THAT(box.Volume(), WithinRel(24.0, 1e-6));

  glm::vec3 min, max;
  box.GetBoundingBox(min, max);
  CHECK_THAT(max.x - min.x, WithinRel(2.0f, 1e-4f));
  CHECK_THAT(max.y - min.y, WithinRel(3.0f, 1e-4f));
  CHECK_THAT(max.z - min.z, WithinRel(4.0f, 1e-4f));
}

TEST_CASE("CreateBox from two corners", "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(glm::vec3(-1, -1, -1),
                                                 glm::vec3(1, 1, 1));
  REQUIRE(box.IsValid());
  CHECK_THAT(box.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("CreateBox rejects degenerate dimensions", "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(0.0, 1.0, 1.0);
  CHECK_FALSE(box.IsValid());
}

TEST_CASE("CreateSphere volume", "[kernel][primitives]") {
  const double r = 1.5;
  CADShape sphere = CADKernel::Instance().CreateSphere(glm::vec3(0), r);
  REQUIRE(sphere.IsValid());
  const double expected = 4.0 / 3.0 * 3.14159265358979 * r * r * r;
  CHECK_THAT(sphere.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("CreateCylinder volume", "[kernel][primitives]") {
  const double r = 1.0, h = 2.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  REQUIRE(cyl.IsValid());
  const double expected = 3.14159265358979 * r * r * h;
  CHECK_THAT(cyl.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("FaceSnapPoints finds a cylinder's circle centre", "[kernel]") {
  // A cylinder about +Z at the origin: its flat cap faces each have one
  // circular edge, centered on the axis (A2.5 connector inference).
  const double r = 1.0, h = 2.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  REQUIRE(cyl.IsValid());

  // Find a planar cap face (the flat end), and confirm it reports a circle
  // center on the Z axis.
  bool foundCap = false;
  for (int f = 0; f < 8 && !foundCap; f++) {
    glm::vec3 c, n, u;
    if (!cyl.FacePlane(f, c, n, u))
      continue; // skip the curved side
    std::vector<CADShape::FaceSnap> snaps;
    cyl.FaceSnapPoints(f, snaps);
    const auto circle = std::find_if(
        snaps.begin(), snaps.end(),
        [](const CADShape::FaceSnap& s) { return s.circleCenter; });
    if (circle == snaps.end())
      continue;
    foundCap = true;
    CHECK_THAT(circle->point.x, WithinAbs(0.0, 1e-6)); // on the axis
    CHECK_THAT(circle->point.y, WithinAbs(0.0, 1e-6));
    // EdgeSnapPoint — the single-edge path the solve still uses — agrees
    // with the batched one, so the two cannot drift.
    glm::vec3 snap;
    REQUIRE(cyl.EdgeSnapPoint(circle->edgeIndex, snap));
    CHECK_THAT(snap.x, WithinAbs(circle->point.x, 1e-6));
    CHECK_THAT(snap.y, WithinAbs(circle->point.y, 1e-6));
    CHECK_THAT(snap.z, WithinAbs(circle->point.z, 1e-6));
  }
  CHECK(foundCap);
}

TEST_CASE("FaceSnapPoints exposes box-edge midpoint snaps", "[kernel]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  REQUIRE(box.IsValid());

  bool foundFace = false;
  for (int face = 0; face < 8 && !foundFace; ++face) {
    glm::vec3 center, normal, uAxis;
    if (!box.FacePlane(face, center, normal, uAxis))
      continue;
    std::vector<CADShape::FaceSnap> snaps;
    box.FaceSnapPoints(face, snaps);
    if (snaps.empty())
      continue;
    foundFace = true;
    CHECK(snaps.size() == 4);
    for (const CADShape::FaceSnap& snap : snaps) {
      CHECK_FALSE(snap.circleCenter); // a box has no round edges
      CHECK(std::isfinite(snap.point.x));
      CHECK(std::isfinite(snap.point.y));
      CHECK(std::isfinite(snap.point.z));
      glm::vec3 midpoint;
      REQUIRE(box.EdgeSnapPoint(snap.edgeIndex, midpoint));
      CHECK_THAT(midpoint.x, WithinAbs(snap.point.x, 1e-9));
    }
  }
  CHECK(foundFace);
}

TEST_CASE("FaceCorners names a corner by its two edges", "[kernel]") {
  // A corner has no stable name of its own — the ElementMap names faces and
  // edges, not vertices — so it is named by the two edges that meet there,
  // both of which do.
  CADShape box = CADKernel::Instance().CreateBox(2.0, 3.0, 4.0);
  REQUIRE(box.IsValid());

  int facesSeen = 0;
  for (int f = 0; f < 6; f++) {
    std::vector<CADShape::FaceCorner> corners;
    box.FaceCorners(f, corners);
    if (corners.empty())
      continue;
    facesSeen++;
    CHECK(corners.size() == 4); // a quad face has four corners
    for (const CADShape::FaceCorner& c : corners) {
      CHECK(c.edgeA < c.edgeB); // normalised, so the pair is one key
      // The stored pair resolves back to the same point — this is the whole
      // premise: two named edges are enough to find the corner again.
      glm::vec3 resolved;
      REQUIRE(box.CornerPoint(c.edgeA, c.edgeB, resolved));
      CHECK_THAT(resolved.x, WithinAbs(c.point.x, 1e-9));
      CHECK_THAT(resolved.y, WithinAbs(c.point.y, 1e-9));
      CHECK_THAT(resolved.z, WithinAbs(c.point.z, 1e-9));
      // A box corner is a corner of its bounding box.
      CHECK(((std::abs(c.point.x) < 1e-9 || std::abs(c.point.x - 2.0) < 1e-9)));
      CHECK(((std::abs(c.point.y) < 1e-9 || std::abs(c.point.y - 3.0) < 1e-9)));
    }
  }
  CHECK(facesSeen == 6);

  // Two edges that do not meet name no corner, and a degenerate pair is
  // refused rather than answered.
  glm::vec3 nowhere;
  CHECK_FALSE(box.CornerPoint(0, 0, nowhere));
  CHECK_FALSE(box.CornerPoint(-1, 3, nowhere));
}

TEST_CASE("FaceCorners refuses an ambiguous edge pair", "[kernel]") {
  // A cylinder's wall is bounded by two circles and a seam counted twice; one
  // of those pairs meets at TWO vertices, so it names two points and
  // therefore neither. Not hypothetical — it is the first curved face anyone
  // hovers, which is why the refusal is day-one and not later hardening.
  CADShape cyl = CADKernel::Instance().CreateCylinder(
      glm::vec3(0), glm::vec3(0, 0, 1), 5.0, 10.0);
  REQUIRE(cyl.IsValid());

  bool sawWall = false;
  for (int f = 0; f < 3; f++) {
    glm::vec3 c, n, u;
    if (cyl.FacePlane(f, c, n, u))
      continue; // a flat cap
    sawWall = true;
    std::vector<CADShape::FaceCorner> corners;
    cyl.FaceCorners(f, corners);
    // Whatever survives must be unambiguous: every reported pair resolves to
    // exactly one point.
    for (const CADShape::FaceCorner& corner : corners) {
      glm::vec3 resolved;
      CHECK(cyl.CornerPoint(corner.edgeA, corner.edgeB, resolved));
    }
  }
  CHECK(sawWall);

  // A flat cap is bounded by ONE closed circular edge: no two edges meet, so
  // no corner. Its snap point is the circle centre, which it already had.
  for (int f = 0; f < 3; f++) {
    glm::vec3 c, n, u;
    if (!cyl.FacePlane(f, c, n, u))
      continue;
    std::vector<CADShape::FaceCorner> corners;
    cyl.FaceCorners(f, corners);
    CHECK(corners.empty());
  }
}

TEST_CASE("FaceSnapPoints stays linear on a drilled plate", "[kernel][slow]") {
  // The regression this exists for: FaceSnapPoints replaced three calls that
  // each mapped every edge of the WHOLE BODY — one of them once PER EDGE,
  // from inside the caller's loop over a face's edges. The mate tool resolves
  // this every frame while hovering, so a plate covered in holes cost
  // 10 ms/frame at 144 holes and 88 ms at 400. See
  // docs/assemblies/README.md#snap-points-while-hovering.
  //
  // A box cannot catch it. The cost is the PRODUCT of the face's edge count
  // and the body's, so it needs a face that carries many edges on a body that
  // has many more — which is what a drilled plate is.
  CADShape plate = CADKernel::Instance().CreateBox(400.0, 20.0, 400.0);
  REQUIRE(plate.IsValid());
  constexpr int kPerSide = 12; // 144 holes
  for (int i = 0; i < kPerSide; i++)
    for (int j = 0; j < kPerSide; j++) {
      const CADShape drill = CADKernel::Instance().CreateCylinder(
          glm::vec3(20.0f + i * 360.0f / kPerSide, -5.0f,
                    20.0f + j * 360.0f / kPerSide),
          glm::vec3(0, 1, 0), 6.0, 30.0);
      plate = plate.Difference(drill);
    }
  REQUIRE(plate.IsValid());

  // The drilled face: the one carrying every hole's rim.
  int worst = -1;
  size_t worstEdges = 0;
  for (int f = 0; f < 500; f++) {
    std::vector<CADShape::FaceSnap> snaps;
    plate.FaceSnapPoints(f, snaps);
    if (snaps.size() > worstEdges) {
      worstEdges = snaps.size();
      worst = f;
    }
  }
  REQUIRE(worst >= 0);
  CHECK(worstEdges > 100); // 4 outline + one rim per hole

  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kFrames = 30;
  for (int f = 0; f < kFrames; f++) {
    std::vector<CADShape::FaceSnap> snaps;
    plate.FaceSnapPoints(worst, snaps);
    CHECK(snaps.size() == worstEdges);
  }
  const double msPerFrame = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count() /
                            kFrames;
  INFO("FaceSnapPoints on a " << worstEdges << "-edge face: " << msPerFrame
                              << " ms");
  // Measured at 0.16 ms once the map is built once; 10 ms when it was built
  // per edge. The bound is deliberately far from BOTH — 12x the real cost, a
  // fifth of the broken one — so a slow machine cannot fail it and a return
  // to quadratic cannot pass it.
  CHECK(msPerFrame < 2.0);
}

TEST_CASE("Boolean union of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape u = a.Union(b);
  REQUIRE(u.IsValid());
  // Two 8-unit boxes overlapping in a 1x2x2 = 4 unit region
  CHECK_THAT(u.Volume(), WithinRel(12.0, 1e-6));
}

TEST_CASE("Boolean intersection of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape i = a.Intersection(b);
  REQUIRE(i.IsValid());
  CHECK_THAT(i.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Boolean difference of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape d = a.Difference(b);
  REQUIRE(d.IsValid());
  CHECK_THAT(d.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Translate moves the bounding box", "[kernel][transform]") {
  CADShape box =
      CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1, 1, 1));
  box.Translate(glm::vec3(10, 0, 0));

  glm::vec3 min, max;
  box.GetBoundingBox(min, max);
  CHECK_THAT(min.x, WithinRel(10.0f, 1e-3f));
  CHECK_THAT(max.x, WithinRel(11.0f, 1e-3f));
  CHECK_THAT(box.Volume(), WithinRel(1.0, 1e-6));
}

TEST_CASE("an in-place transform forgets the memoized bounding box",
          "[kernel][bbox]") {
  // GetBoundingBox remembers its answer on the shape. The five transforms
  // that replace the shape IN PLACE are the only way that answer can become
  // a lie, so each is asked before and after: a memo that survived one of
  // them would report where the body USED to be, and CAM would cut it there.
  const auto span = [](const CADShape& s) {
    glm::vec3 lo(0.0f), hi(0.0f);
    s.GetBoundingBox(lo, hi);
    return std::pair<glm::vec3, glm::vec3>(lo, hi);
  };

  SECTION("Translate") {
    CADShape b = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1));
    CHECK_THAT(span(b).first.x, WithinAbs(0.0f, 1e-4f)); // warms the memo
    b.Translate(glm::vec3(10, 0, 0));
    CHECK_THAT(span(b).first.x, WithinAbs(10.0f, 1e-3f));
    CHECK_THAT(span(b).second.x, WithinAbs(11.0f, 1e-3f));
  }
  SECTION("Rotate") {
    CADShape b = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1));
    CHECK_THAT(span(b).second.x, WithinAbs(1.0f, 1e-4f));
    b.Rotate(glm::vec3(0, 0, 1), 3.14159265358979 / 2.0);
    // A quarter turn about +Z puts the x span where the y span was, negated.
    CHECK_THAT(span(b).first.x, WithinAbs(-1.0f, 1e-3f));
    CHECK_THAT(span(b).second.x, WithinAbs(0.0f, 1e-3f));
  }
  SECTION("Scale") {
    CADShape b = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1));
    CHECK_THAT(span(b).second.z, WithinAbs(1.0f, 1e-4f));
    b.Scale(3.0);
    CHECK_THAT(span(b).second.z, WithinAbs(3.0f, 1e-3f));
  }
  SECTION("RotateAbout") {
    CADShape b = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1));
    CHECK_THAT(span(b).first.y, WithinAbs(0.0f, 1e-4f));
    b.RotateAbout(glm::vec3(10, 0, 0), glm::vec3(0, 0, 1),
                  3.14159265358979 / 2.0);
    // A quarter turn about the pivot at (10,0,0) swings the unit box round
    // to x 9..10, y -10..-9.
    CHECK_THAT(span(b).first.y, WithinAbs(-10.0f, 1e-3f));
    CHECK_THAT(span(b).second.y, WithinAbs(-9.0f, 1e-3f));
    CHECK_THAT(span(b).first.x, WithinAbs(9.0f, 1e-3f));
  }
  SECTION("Mirror") {
    CADShape b = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1));
    CHECK_THAT(span(b).second.x, WithinAbs(1.0f, 1e-4f));
    b.Mirror(glm::vec3(0), glm::vec3(1, 0, 0));
    CHECK_THAT(span(b).first.x, WithinAbs(-1.0f, 1e-3f));
    CHECK_THAT(span(b).second.x, WithinAbs(0.0f, 1e-3f));
  }
}

TEST_CASE("the two bounding-box flavours keep their own memo",
          "[kernel][bbox]") {
  // Conservative and tight are separate answers with separate slots. Asking
  // one must not hand back the other's box — on a swept or B-spline surface
  // they differ by tens of percent, which is exactly where it would matter.
  CADShape s = CADKernel::Instance().CreateSphere(glm::vec3(0, 0, 0), 5.0);
  CADShape fresh = CADKernel::Instance().CreateSphere(glm::vec3(0, 0, 0), 5.0);

  glm::vec3 tLo(0.0f), tHi(0.0f), cLo(0.0f), cHi(0.0f);
  s.GetBoundingBox(cLo, cHi, /*tight=*/false); // conservative first
  s.GetBoundingBox(tLo, tHi, /*tight=*/true);
  glm::vec3 fLo(0.0f), fHi(0.0f);
  fresh.GetBoundingBox(fLo, fHi, /*tight=*/true); // tight first, cold shape
  CHECK_THAT(tLo.x, WithinAbs(fLo.x, 1e-9f));
  CHECK_THAT(tHi.z, WithinAbs(fHi.z, 1e-9f));

  // Tight is never larger than conservative, and both bound the sphere.
  CHECK(tLo.x >= cLo.x - 1e-6f);
  CHECK(tHi.x <= cHi.x + 1e-6f);
  CHECK_THAT(tHi.x, WithinAbs(5.0f, 1e-3f));

  // A second ask is the memo, and it agrees with the first to the bit.
  glm::vec3 again(0.0f), againHi(0.0f);
  s.GetBoundingBox(again, againHi, /*tight=*/true);
  CHECK(again == tLo);
  CHECK(againHi == tHi);
}

TEST_CASE("a quarter-turned copy carries the tight box, and it is exact",
          "[kernel][bbox]") {
  // CAM frames every body of a job through quarter turns (laid on a face,
  // flipped, a placement turned), and each framed copy recomputed its tight
  // box from the surfaces -- seconds on a turned blank. A copy moved by such a
  // turn now carries the box, permuted; it must equal what a cold copy
  // computes, for turns that swap axes, flip them and translate.
  const auto make = [] {
    return CADKernel::Instance().CreateCylinder(glm::vec3(3, -2, 7),
                                                glm::vec3(1, 0, 0), 4.0, 25.0);
  };
  CADShape warm = make();
  glm::vec3 lo(0.0f), hi(0.0f);
  warm.GetBoundingBox(lo, hi, /*tight=*/true);
  const glm::mat4 moves[] = {
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)),
      glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(1, 0, 0)),
      glm::translate(glm::mat4(1.0f), glm::vec3(40, -5, 2)) *
          glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f),
                      glm::vec3(0, 0, 1)),
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0)),
      // not a quarter turn: nothing carried, and still right
      glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(1, 0, 0)),
  };
  for (const glm::mat4& m : moves) {
    glm::vec3 cLo(0.0f), cHi(0.0f), fLo(0.0f), fHi(0.0f);
    warm.Transformed(m).GetBoundingBox(cLo, cHi, /*tight=*/true);
    make().Transformed(m).GetBoundingBox(fLo, fHi, /*tight=*/true);
    for (int a = 0; a < 3; a++) {
      CHECK_THAT(cLo[a], WithinAbs(fLo[a], 1e-3f));
      CHECK_THAT(cHi[a], WithinAbs(fHi[a], 1e-3f));
    }
  }
}

TEST_CASE("a shape framed again reads the exact box another framing paid for",
          "[kernel][bbox]") {
  // A job tab frames its parts afresh on every visit: a new CADShape, the
  // same TShape, the same location. At an angle no quarter turn reaches, the
  // memo on the object was cold every time; the box is now remembered by
  // identity and location too (docs/cam/gui.md#job-tab-frame-cost).
  CADShape::ForgetSharedTightBoxes();
  const CADShape source = CADKernel::Instance().CreateCylinder(
      glm::vec3(3, -2, 7), glm::vec3(1, 0, 0), 4.0, 25.0);
  const glm::mat4 at30 =
      glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(1, 0, 0));
  const CADShape first = source.Transformed(at30);
  REQUIRE_FALSE(first.HasTightBox());
  glm::vec3 lo(0.0f), hi(0.0f);
  first.GetBoundingBox(lo, hi, /*tight=*/true);
  CHECK(CADShape::SharedTightBoxCount() == 1);

  // Framed again: cold on the object, answered from the shared memo, to the
  // bit.
  const CADShape again = source.Transformed(at30);
  REQUIRE_FALSE(again.HasTightBox());
  glm::vec3 aLo(0.0f), aHi(0.0f);
  again.GetBoundingBox(aLo, aHi, /*tight=*/true);
  CHECK(aLo == lo);
  CHECK(aHi == hi);
  CHECK(CADShape::SharedTightBoxCount() == 1);

  // Another angle is another entry, and another answer.
  const CADShape at40 = source.Transformed(
      glm::rotate(glm::mat4(1.0f), glm::radians(40.0f), glm::vec3(1, 0, 0)));
  glm::vec3 bLo(0.0f), bHi(0.0f);
  at40.GetBoundingBox(bLo, bHi, /*tight=*/true);
  CHECK(CADShape::SharedTightBoxCount() == 2);
  CHECK(bHi.z != hi.z);

  // A worker's answer, from a detached copy placed where the framing is, is
  // the framing's own box -- and remembered under the framing's identity it
  // serves the next framing.
  CADShape::ForgetSharedTightBoxes();
  const CADShape copy = source.DetachedUnlocated().LocatedLike(first);
  REQUIRE(copy.IsValid());
  CHECK(copy.GeometryId() != first.GeometryId());
  glm::vec3 cLo(0.0f), cHi(0.0f);
  copy.ExactBoxUnmemoized(cLo, cHi);
  CHECK(CADShape::SharedTightBoxCount() == 0); // unmemoized: nothing kept
  for (int a = 0; a < 3; a++) {
    CHECK_THAT(cLo[a], WithinAbs(lo[a], 1e-4f));
    CHECK_THAT(cHi[a], WithinAbs(hi[a], 1e-4f));
  }
  CADShape::RememberTightBox(first, cLo, cHi);
  const CADShape third = source.Transformed(at30);
  glm::vec3 tLo(0.0f), tHi(0.0f);
  third.GetBoundingBox(tLo, tHi, /*tight=*/true);
  CHECK(tLo == cLo);
  CHECK(tHi == cHi);
  CADShape::ForgetSharedTightBoxes();
}

TEST_CASE("GenerateMesh honours a stated deflection", "[kernel][mesh]") {
  // The deflection argument used to be decoration: the size-scaling rule
  // (0.2% of the bounding diagonal) overwrote it whenever the shape had a
  // bounding box, which is always. So `GenerateMesh(0.05)` on a 200 mm part
  // meshed at 0.4 mm, and the CAM verifier compared machined stock against a
  // "designed" surface coarser than the 0.25 mm grid measuring it
  // (docs/cam/simulation.md#one-engine).
  //
  // A cylinder, because only a curved face has a chord tolerance to observe.
  const auto cylinder = [] {
    return CADKernel::Instance().CreateCylinder(
        glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 50.0, 200.0);
  };

  // A FRESH shape per measurement. Meshing writes the triangulation into the
  // shape, so asking one shape twice measures OCC's cache, not the argument.
  const size_t derived = cylinder().GenerateMesh().indices.size() / 3;
  const size_t fine = cylinder().GenerateMesh(0.01).indices.size() / 3;
  const size_t finer = cylinder().GenerateMesh(0.001).indices.size() / 3;
  REQUIRE(derived > 0);

  // Asked for finer, got finer: the scaling rule here would give
  // diag * 0.002 ~ 0.49 mm, so 0.01 mm is far tighter and must show up.
  CHECK(fine > derived);
  // ...and it keeps scaling, rather than snapping to one "fine" setting. A
  // one-sided check against `derived` alone would pass on a clamp that
  // refined once and then ignored the argument.
  CHECK(finer > fine);
  // Saying nothing still gets the size-scaled default, not zero and not a
  // literal -- the behaviour every existing caller relies on.
  CHECK(cylinder().GenerateMesh(0.0).indices.size() / 3 == derived);

  // COARSER than the derived value is deliberately NOT asserted, and the
  // reason is the other half of the tessellation rule rather than a gap in
  // this test: the angular deflection (0.2 rad) floors every circle at ~32
  // segments, and on r=50 the chord term only becomes the binding constraint
  // below 0.25 mm. Above that, asking for 0.5 mm and 5 mm gives the same mesh
  // -- which is the behaviour that stopped small cylinders rendering as
  // n-gons, and is why the two deflections either side of `fine` above are
  // both chosen inside the chord-bound regime.
  CHECK(cylinder().GenerateMesh(5.0).indices.size() / 3 == derived);
}

TEST_CASE("a detached mesh leaves the shape's own tessellation alone",
          "[kernel][mesh]") {
  // The triangulation rides on the SHARED TShape, so it is visible through
  // every copy of a shape -- including the copies the document and the
  // renderer are holding. A verifier meshing a Body at its own fine tolerance
  // therefore re-meshed the whole document behind the renderer's back: a
  // cylinder answering 248 triangles answers 888 forever after any copy of it
  // is meshed at 0.01 mm. Same hazard as the bounding box that disagreed with
  // itself once the shape had been meshed.
  CADShape shape = CADKernel::Instance().CreateCylinder(
      glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 50.0, 200.0);
  REQUIRE(shape.IsValid());

  CADShape sharer = shape; // what a Body hands around
  const size_t before = sharer.GenerateMesh().indices.size() / 3;
  REQUIRE(before > 0);

  // The fine mesh is real...
  const size_t fine = shape.GenerateMeshDetached(0.01).indices.size() / 3;
  CHECK(fine > before);

  // ...and cost the sharer nothing.
  CHECK(sharer.GenerateMesh().indices.size() / 3 == before);
  CHECK(shape.GenerateMesh().indices.size() / 3 == before);

  // The undetached call is what would have moved it, and still does -- this
  // is the behaviour GenerateMeshDetached exists to avoid, pinned so that a
  // change to it is a decision rather than a surprise.
  CHECK(shape.GenerateMesh(0.01).indices.size() / 3 == fine);
  CHECK(sharer.GenerateMesh().indices.size() / 3 == fine);
}

TEST_CASE("GenerateMesh produces triangles for a box", "[kernel][mesh]") {
  CADShape box = CADKernel::Instance().CreateBox(1.0, 1.0, 1.0);
  CADShape::Mesh mesh = box.GenerateMesh();

  REQUIRE_FALSE(mesh.vertices.empty());
  REQUIRE_FALSE(mesh.indices.empty());
  CHECK(mesh.indices.size() % 3 == 0);
  // A tessellated box is exactly 2 triangles per face
  CHECK(mesh.indices.size() / 3 == 12);
  for (uint32_t idx : mesh.indices) {
    REQUIRE(idx < mesh.vertices.size());
  }
}

TEST_CASE("STEP save/load roundtrip preserves volume", "[kernel][io]") {
  namespace fs = std::filesystem;
  CADKernel& k = CADKernel::Instance();

  std::vector<CADShape> out;
  out.push_back(k.CreateBox(2.0, 3.0, 4.0));

  fs::path path =
      std::filesystem::temp_directory_path() / "designer_test_roundtrip.step";
  REQUIRE(k.SaveSTEP(path.string(), out));

  std::vector<CADShape> in;
  REQUIRE(k.LoadSTEP(path.string(), in));
  REQUIRE(in.size() == 1);
  CHECK_THAT(in[0].Volume(), WithinRel(24.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("STL save/load roundtrip yields a SOLID with the box volume",
          "[kernel][io]") {
  namespace fs = std::filesystem;
  CADKernel& k = CADKernel::Instance();

  CADShape box = k.CreateBox(2.0, 3.0, 4.0);
  fs::path path =
      std::filesystem::temp_directory_path() / "designer_test_roundtrip.stl";
  REQUIRE(k.SaveSTL(path.string(), box, 0.1));

  // A watertight mesh must come back as a closed solid (LoadSTL promotes the
  // sewn shell), so volume/mass work on it — a planar box tessellates
  // exactly, so the volume is exact too.
  CADShape in;
  REQUIRE(k.LoadSTL(path.string(), in));
  REQUIRE(in.IsValid());
  CHECK_THAT(in.Volume(), WithinRel(24.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("Mass properties of a box match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double a = 2.0, b = 3.0, c = 4.0;
  CADShape box =
      CADKernel::Instance().CreateBox(glm::vec3(0, 0, 0), glm::vec3(a, b, c));
  CADShape::MassProps mp;
  REQUIRE(box.ComputeMassProps(mp));
  CHECK_THAT(mp.volume, WithinRel(a * b * c, 1e-9));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * (a * b + b * c + c * a), 1e-9));
  CHECK_THAT(mp.centerOfMass.x, WithinAbs(a / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.y, WithinAbs(b / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(c / 2, 1e-9));
  // Unit density: I_xx = m/12 (b^2 + c^2) about the CoM, and no products of
  // inertia for an axis-aligned box.
  const double m = a * b * c;
  CHECK_THAT(mp.inertia[0][0], WithinRel(m / 12 * (b * b + c * c), 1e-9));
  CHECK_THAT(mp.inertia[1][1], WithinRel(m / 12 * (a * a + c * c), 1e-9));
  CHECK_THAT(mp.inertia[2][2], WithinRel(m / 12 * (a * a + b * b), 1e-9));
  CHECK_THAT(mp.inertia[0][1], WithinAbs(0.0, 1e-9));
  CHECK_THAT(mp.inertia[0][2], WithinAbs(0.0, 1e-9));
}

TEST_CASE("Mass properties of a cylinder match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double r = 1.5, h = 4.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  CADShape::MassProps mp;
  REQUIRE(cyl.ComputeMassProps(mp));
  const double v = M_PI * r * r * h;
  CHECK_THAT(mp.volume, WithinRel(v, 1e-6));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * M_PI * r * (r + h), 1e-6));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(h / 2, 1e-6));
  // About the CoM: I_zz = m r^2 / 2, I_xx = I_yy = m (3r^2 + h^2) / 12.
  CHECK_THAT(mp.inertia[2][2], WithinRel(v * r * r / 2, 1e-6));
  CHECK_THAT(mp.inertia[0][0], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
  CHECK_THAT(mp.inertia[1][1], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
}

TEST_CASE("Mass properties of an invalid shape fail cleanly",
          "[kernel][massprops]") {
  CADShape::MassProps mp;
  CHECK_FALSE(CADShape().ComputeMassProps(mp));
}

// ---------------------------------------------------------------------------
// Structured STEP import (I1): names, colors, and the flattened instance tree
// read through XCAF. The fixture is authored with XCAF + STEPCAFControl so the
// test needs no binary files: an assembly "Rig" holding a red "Widget" box
// twice (second instance rotated 90° about Z then moved to x=10) and an
// unnamed-color "Pin" cylinder at y=10.
// ---------------------------------------------------------------------------

#include <BRep_Builder.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include "core/Document.h"
#include "core/Feature.h"

namespace {
std::filesystem::path WriteRigStep() {
  CADKernel& k = CADKernel::Instance();
  CADShape box = k.CreateBox(2.0, 2.0, 2.0);
  CADShape cyl =
      k.CreateCylinder(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 0.5, 3.0);

  Handle(TDocStd_Document) doc;
  XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());

  gp_Trsf rot;
  rot.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267949);
  gp_Trsf toX10;
  toX10.SetTranslation(gp_Vec(10, 0, 0));
  gp_Trsf toY10;
  toY10.SetTranslation(gp_Vec(0, 10, 0));

  TopoDS_Compound comp;
  BRep_Builder builder;
  builder.MakeCompound(comp);
  builder.Add(comp, box.GetShape());
  builder.Add(comp, box.GetShape().Moved(TopLoc_Location(toX10 * rot)));
  builder.Add(comp, cyl.GetShape().Moved(TopLoc_Location(toY10)));
  TDF_Label asmLabel = st->AddShape(comp, Standard_True);
  TDataStd_Name::Set(asmLabel, "Rig");

  TDF_Label boxLabel, cylLabel;
  REQUIRE(st->FindShape(box.GetShape(), boxLabel));
  REQUIRE(st->FindShape(cyl.GetShape(), cylLabel));
  TDataStd_Name::Set(boxLabel, "Widget");
  ct->SetColor(boxLabel, Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_RGB),
               XCAFDoc_ColorSurf);
  TDataStd_Name::Set(cylLabel, "Pin");

  std::filesystem::path path =
      // Per-process: catch_discover_tests runs each TEST_CASE in its own
      // process and ctest runs those in parallel, so a fixed filename is two
      // tests writing and reading one file at once (an intermittent import
      // failure that only appears under -j).
      std::filesystem::temp_directory_path() /
      ("designer_test_rig_" + std::to_string(::getpid()) + ".step");
  STEPCAFControl_Writer writer;
  REQUIRE(writer.Transfer(doc, STEPControl_AsIs));
  REQUIRE(writer.Write(path.string().c_str()) == IFSelect_RetDone);
  return path;
}
} // namespace

// Every import and export used to open an XCAF document and never close it.
// TDocStd_Application holds each one for the life of the process — with its
// shapes and (after a mesh export) their triangulations — so the memory grew
// with every file operation. LeakSanitizer cannot see it: the documents are
// still REACHABLE from the application singleton. NbDocuments() can.
TEST_CASE("Import and export leave no XCAF document open", "[kernel][import]") {
  const auto path = WriteRigStep();
  const Standard_Integer before =
      XCAFApp_Application::GetApplication()->NbDocuments();

  CADKernel::ImportedModel model;
  REQUIRE(CADKernel::Instance().LoadSTEPStructured(path.string(), model));

  const auto out = std::filesystem::temp_directory_path() /
                   ("designer_xcaf_" + std::to_string(::getpid()) + ".step");
  REQUIRE(CADKernel::Instance().SaveSTEPStructured(out.string(), model, "Rig"));

  std::vector<MeshExport::Item> items;
  for (const auto& product : model.products)
    items.push_back({product.shape, product.name, glm::mat4(1.0f)});
  const auto glb = std::filesystem::temp_directory_path() /
                   ("designer_xcaf_" + std::to_string(::getpid()) + ".glb");
  REQUIRE(MeshExport::SaveVisual(glb.string(), items, 0.1));

  CHECK(XCAFApp_Application::GetApplication()->NbDocuments() == before);
  std::filesystem::remove(out);
  std::filesystem::remove(glb);
  std::filesystem::remove(path);
}

TEST_CASE("Structured STEP import reads names, colors, and instances",
          "[kernel][import]") {
  const auto path = WriteRigStep();

  CADKernel::ImportedModel model;
  REQUIRE(CADKernel::Instance().LoadSTEPStructured(path.string(), model));
  REQUIRE(model.products.size() == 2);
  REQUIRE(model.instances.size() == 3);

  const auto widget =
      std::find_if(model.products.begin(), model.products.end(),
                   [](const auto& p) { return p.name == "Widget"; });
  const auto pin = std::find_if(model.products.begin(), model.products.end(),
                                [](const auto& p) { return p.name == "Pin"; });
  REQUIRE(widget != model.products.end());
  REQUIRE(pin != model.products.end());
  CHECK_THAT(widget->shape.Volume(), WithinRel(8.0, 1e-4));
  CHECK(widget->hasColor);
  CHECK_THAT(widget->color.r, WithinAbs(0.9f, 0.05f));
  CHECK_THAT(widget->color.g, WithinAbs(0.1f, 0.05f));

  // Two Widget instances: one at identity, one rotated 90° about Z at
  // x=10 (rotation maps +X to +Y — read the transform's first column).
  const size_t widgetIdx = (size_t)(widget - model.products.begin());
  std::vector<glm::mat4> widgetXf;
  glm::mat4 pinXf(0.0f);
  for (const auto& inst : model.instances) {
    if (inst.product == widgetIdx)
      widgetXf.push_back(inst.transform);
    else
      pinXf = inst.transform;
  }
  REQUIRE(widgetXf.size() == 2);
  if (glm::length(glm::vec3(widgetXf[0][3])) >
      glm::length(glm::vec3(widgetXf[1][3])))
    std::swap(widgetXf[0], widgetXf[1]);
  CHECK_THAT(glm::length(glm::vec3(widgetXf[0][3])), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(widgetXf[1][3].x, WithinAbs(10.0f, 1e-4f));
  CHECK_THAT(widgetXf[1][0].y, WithinAbs(1.0f, 1e-4f)); // +X -> +Y
  CHECK_THAT(pinXf[3].y, WithinAbs(10.0f, 1e-4f));

  std::filesystem::remove(path);
}

// The invariant that would have caught the OCC 7.6 failure without needing a
// second OpenCASCADE to find it for us.
//
// glm placements are FLOAT. `(float)cos(pi/4)` squared twice gives
// 0.99999997, so gp_Trsf::SetValues — which infers a uniform scale from the
// 3x3 block — read every rotation we produced as a transform scaled by
// 1 - 1.1e-8. TopoDS_Shape::Moved() refuses to move a shape by a scaled
// location; 7.9.3 tolerates that deviation and 7.6.3 does not, so the same
// code worked here and threw on Ubuntu ("Moving with scaling transformation
// is forbidden"). Measured on both, with identical input.
//
// A placement is rigid by construction, so assert it arrives that way rather
// than relying on any kernel's tolerance.
TEST_CASE("A rigid placement reaches the kernel unscaled",
          "[cadkernel][transform]") {
  const CADShape box = CADKernel::Instance().CreateBox(10.0, 10.0, 10.0);
  REQUIRE(box.IsValid());

  const auto scaleOf = [](const CADShape& s) {
    return s.GetShape().Location().Transformation().ScaleFactor();
  };

  SECTION("rotations at every angle, however the quaternion was built") {
    for (int deg = 0; deg <= 360; deg += 15) {
      const float rad = glm::radians(static_cast<float>(deg));
      // The app's own angle-axis path...
      const glm::mat4 viaAxis = glm::mat4_cast(
          glm::angleAxis(rad, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))));
      // ...and a quaternion a script posted by hand, which is where the
      // float error is worst.
      const glm::mat4 viaHand = glm::mat4_cast(
          glm::quat(std::cos(rad * 0.5f), 0.0f, std::sin(rad * 0.5f), 0.0f));
      for (const glm::mat4& m : {viaAxis, viaHand}) {
        const CADShape moved = box.Transformed(
            glm::translate(glm::mat4(1.0f), glm::vec3(3, -4, 5)) * m);
        INFO("angle " << deg << " deg, scale " << scaleOf(moved));
        REQUIRE(moved.IsValid());
        CHECK(scaleOf(moved) == 1.0);
        // And it really moved: a scaled location that OCC refused used to
        // leave the shape at the origin.
        CHECK_THAT(moved.Volume(), WithinRel(box.Volume(), 1e-9));
      }
    }
  }

  SECTION("a mirror still goes through the general path") {
    // Determinant -1: not a rotation, and OCC represents it fine. The rigid
    // shortcut must not swallow it (a mirrored part would come back
    // unmirrored, which no test would notice until a drawing looked wrong).
    glm::mat4 mirror(1.0f);
    mirror[0][0] = -1.0f;
    const CADShape flipped = box.Transformed(mirror);
    REQUIRE(flipped.IsValid());
    // Same size, opposite handedness. Note what is NOT asserted: the SIGN of
    // the volume. OCC 7.9.3 reports -1000 for a mirrored solid and 7.6.3
    // reports +1000, so requiring one of them would be pinning an incidental
    // difference rather than the property we care about — which is that the
    // transform arrived as a mirror instead of being flattened into a
    // rotation by the rigid shortcut above.
    CHECK_THAT(std::abs(flipped.Volume()), WithinRel(box.Volume(), 1e-9));
    // Ask WHERE THE GEOMETRY IS, not how OCC chose to record the transform.
    // A location's determinant, gp_Trsf::IsNegative() and the sign of the
    // volume all answer differently across 7.6.3/7.9.3 and across
    // implementations (a location versus baked-in vertices) — three
    // incidentals I asserted in turn before landing on the one that is the
    // actual property: the box spanned x 0..10 and must now span -10..0.
    glm::vec3 lo(0.0f), hi(0.0f);
    flipped.GetBoundingBox(lo, hi); // void: fills lo/hi
    INFO("bbox x " << lo.x << " .. " << hi.x);
    CHECK_THAT(lo.x, WithinAbs(-10.0, 1e-6));
    CHECK_THAT(hi.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(hi.y, WithinAbs(10.0, 1e-6)); // untouched axes stay put
  }
}

// An STL has no unit field, so the import is told what it was drawn in
// (docs/interchange/README.md#importing-into-a-part). Storage stays mm: the
// shape is scaled as read.
TEST_CASE("An STL import is read in the unit it was drawn in",
          "[feature][import]") {
  namespace fs = std::filesystem;
  CADKernel& k = CADKernel::Instance();
  const std::string pid = std::to_string(::getpid());
  // Upper case on purpose: ".STL" is how many tools write it, and the
  // extension check was case-sensitive.
  const fs::path stl =
      fs::temp_directory_path() / ("designer_units_" + pid + ".STL");
  REQUIRE(k.SaveSTL(stl.string(), k.CreateBox(2.0, 3.0, 4.0), 0.1));
  const auto import = [](const fs::path& path, units::Length unit) {
    auto doc = std::make_unique<Document>();
    auto imp = std::make_unique<ImportFeature>("part", path.string());
    imp->lengthUnit = unit;
    doc->AddFeature(std::move(imp));
    return doc;
  };
  const auto span = [](const Document& doc) {
    glm::vec3 lo, hi;
    doc.GetBodies()[0].shape.GetBoundingBox(lo, hi);
    return hi - lo;
  };

  SECTION("read as inches, every side is 25.4x") {
    const auto doc = import(stl, units::Length::Inch);
    REQUIRE(doc->GetBodies().size() == 1);
    const glm::vec3 s = span(*doc);
    CHECK_THAT(s.x, WithinAbs(2.0 * 25.4, 1e-3));
    CHECK_THAT(s.y, WithinAbs(3.0 * 25.4, 1e-3));
    CHECK_THAT(s.z, WithinAbs(4.0 * 25.4, 1e-3));
    CHECK_THAT(doc->GetBodies()[0].shape.Volume(),
               WithinRel(24.0 * 25.4 * 25.4 * 25.4, 1e-6));
    CHECK(doc->GetFeatures()[0]->GetWarning().empty());
  }

  SECTION("a mesh drawn in metres, read as mm, warns and names the fix") {
    // A 20 x 30 x 40 mm part, exported by a tool that thinks in metres.
    const fs::path m =
        fs::temp_directory_path() / ("designer_units_m_" + pid + ".stl");
    REQUIRE(k.SaveSTL(m.string(), k.CreateBox(0.02, 0.03, 0.04), 0.001));
    const auto wrong = import(m, units::Length::Millimetre);
    const std::string w = wrong->GetFeatures()[0]->GetWarning();
    INFO(w);
    CHECK(w.find("metres") != std::string::npos);
    CHECK(w.find("units") != std::string::npos);
    const auto right = import(m, units::Length::Metre);
    REQUIRE(right->GetBodies().size() == 1);
    CHECK_THAT(span(*right).z, WithinAbs(40.0, 1e-3));
    CHECK(right->GetFeatures()[0]->GetWarning().empty());
    fs::remove(m);
  }

  SECTION("a STEP declares its own unit, and refuses one") {
    const auto doc = import(WriteRigStep(), units::Length::Inch);
    CHECK(doc->GetBodies().empty());
    const std::string e = doc->GetFeatures()[0]->GetError();
    INFO(e);
    CHECK(e.find("declares its own unit") != std::string::npos);
  }
  fs::remove(stl);
}

TEST_CASE("ImportFeature carries names and colors onto bodies",
          "[feature][import]") {
  const auto path = WriteRigStep();

  Document doc;
  doc.AddFeature(std::make_unique<ImportFeature>("rig", path.string()));
  const auto& bodies = doc.GetBodies();
  REQUIRE(bodies.size() == 3);

  int widgets = 0, pins = 0;
  for (const auto& b : bodies) {
    if (b.name == "Widget") {
      widgets++;
      CHECK(b.hasDisplayColor);
      CHECK_THAT(b.displayColor.r, WithinAbs(0.9f, 0.05f));
    } else if (b.name == "Pin") {
      pins++;
      CHECK_FALSE(b.hasDisplayColor);
    }
  }
  CHECK(widgets == 2);
  CHECK(pins == 1);

  // Instances keep their relative placement, centered as one unit. The box
  // is corner-at-origin, so the 90°-rotation puts instance B's center at
  // (-1,1,1)+x10 = (9,1,1) vs A's (1,1,1): centers 8 apart.
  glm::vec3 minA, maxA, minB, maxB;
  const Body* w[2] = {nullptr, nullptr};
  for (const auto& b : bodies)
    if (b.name == "Widget")
      (w[0] ? w[1] : w[0]) = &b;
  REQUIRE(w[1] != nullptr);
  w[0]->shape.GetBoundingBox(minA, maxA);
  w[1]->shape.GetBoundingBox(minB, maxB);
  const glm::vec3 cA = (minA + maxA) * 0.5f, cB = (minB + maxB) * 0.5f;
  CHECK_THAT(glm::length(cB - cA), WithinAbs(8.0f, 0.01f));

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Assembly import plan (I2): SaveSTEPStructured → BuildAssemblyImportPlan
// round-trip. Parts come out centered with the offset moved into the
// occurrence, so materialized occurrences land exactly where the file put
// them.
// ---------------------------------------------------------------------------

#include "core/io/ImportAssembly.h"

TEST_CASE("Assembly import plan extracts centered parts + placed occurrences",
          "[kernel][import][assembly]") {
  CADKernel& k = CADKernel::Instance();

  // Author with the write half: a red "Widget" box (corner at origin, so
  // centering is non-trivial) twice — identity and rotated-90°-at-x10 —
  // plus a "Pin" cylinder at y=10.
  CADKernel::ImportedModel model;
  model.products.push_back({k.CreateBox(2.0, 2.0, 2.0), "Widget", true,
                            glm::vec3(0.9f, 0.1f, 0.1f)});
  model.products.push_back(
      {k.CreateCylinder(glm::vec3(0), glm::vec3(0, 0, 1), 0.5, 3.0), "Pin",
       false, glm::vec3(0.0f)});
  const glm::mat4 xfB =
      glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
  model.instances.push_back({0, glm::mat4(1.0f), "W1"});
  model.instances.push_back({0, xfB, "W2"});
  model.instances.push_back(
      {1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 10, 0)), "P1"});

  const auto stepPath =
      std::filesystem::temp_directory_path() / "designer_test_asm.step";
  REQUIRE(k.SaveSTEPStructured(stepPath.string(), model, "Rig"));

  const auto destDir =
      std::filesystem::temp_directory_path() / "designer_test_asm_out";
  std::filesystem::remove_all(destDir);
  const auto plan =
      interchange::BuildAssemblyImportPlan(stepPath.string(), destDir.string());
  REQUIRE(plan.error.empty());
  REQUIRE(plan.parts.size() == 2);
  REQUIRE(plan.occurrences.size() == 3);

  // Parts: named, colored, geometry centered at the part origin.
  const auto widget =
      std::find_if(plan.parts.begin(), plan.parts.end(),
                   [](const auto& p) { return p.name == "Widget"; });
  REQUIRE(widget != plan.parts.end());
  CHECK(widget->hasColor);
  CHECK_THAT(widget->color.r, WithinAbs(0.9f, 0.05f));
  CADShape widgetShape;
  REQUIRE(k.LoadBREP(widget->brepPath, widgetShape));
  glm::vec3 min, max;
  widgetShape.GetBoundingBox(min, max);
  CHECK_THAT(glm::length((min + max) * 0.5f), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(widgetShape.Volume(), WithinRel(8.0, 1e-4));

  // Occurrences: centered part + placement composes back to the file's
  // world positions. Box corner at origin → center (1,1,1); W2 is that
  // center pushed through xfB.
  const size_t wi = (size_t)(widget - plan.parts.begin());
  std::vector<const interchange::PlannedOccurrence*> ws;
  const interchange::PlannedOccurrence* pin = nullptr;
  for (const auto& o : plan.occurrences)
    (o.part == wi ? (void)ws.push_back(&o) : (void)(pin = &o));
  REQUIRE(ws.size() == 2);
  REQUIRE(pin != nullptr);
  if (glm::length(ws[0]->position) > glm::length(ws[1]->position))
    std::swap(ws[0], ws[1]);
  CHECK_THAT(glm::length(ws[0]->position - glm::vec3(1, 1, 1)),
             WithinAbs(0.0f, 1e-3f));
  const glm::vec3 expectB = glm::vec3(xfB * glm::vec4(1, 1, 1, 1));
  CHECK_THAT(glm::length(ws[1]->position - expectB), WithinAbs(0.0f, 1e-3f));
  // W2's orientation is the 90°-about-Z rotation.
  const glm::vec3 xAxis = ws[1]->orientation * glm::vec3(1, 0, 0);
  CHECK_THAT(xAxis.y, WithinAbs(1.0f, 1e-3f));
  // Pin: cylinder is axis-centered in XY, z spans [0,3] → center (0,0,1.5).
  CHECK_THAT(glm::length(pin->position - glm::vec3(0, 10, 1.5f)),
             WithinAbs(0.0f, 1e-3f));

  // The preloaded-model overload must plan IDENTICALLY to the path one. It
  // exists so ImportCADFile can hand on the model it already parsed to decide
  // part-vs-assembly instead of dropping it: the XCAF read is ~12 s on a
  // 48 MB / 299-product file against ~0.9 s for all of the extraction here,
  // so re-reading was most of the import. Same plan, one parse.
  CADKernel::ImportedModel reloaded;
  REQUIRE(k.LoadSTEPStructured(stepPath.string(), reloaded));
  const auto destDir2 =
      std::filesystem::temp_directory_path() / "designer_test_asm_out2";
  std::filesystem::remove_all(destDir2);
  const auto plan2 = interchange::BuildAssemblyImportPlan(
      reloaded, stepPath.string(), destDir2.string());
  REQUIRE(plan2.error.empty());
  REQUIRE(plan2.parts.size() == plan.parts.size());
  REQUIRE(plan2.occurrences.size() == plan.occurrences.size());
  for (size_t i = 0; i < plan.parts.size(); i++) {
    CHECK(plan2.parts[i].name == plan.parts[i].name);
    CHECK(plan2.parts[i].hasColor == plan.parts[i].hasColor);
    CHECK_THAT(glm::length(plan2.parts[i].color - plan.parts[i].color),
               WithinAbs(0.0f, 1e-5f));
  }
  for (size_t i = 0; i < plan.occurrences.size(); i++) {
    CHECK(plan2.occurrences[i].part == plan.occurrences[i].part);
    CHECK(plan2.occurrences[i].name == plan.occurrences[i].name);
    CHECK_THAT(glm::length(plan2.occurrences[i].position -
                           plan.occurrences[i].position),
               WithinAbs(0.0f, 1e-4f));
  }

  std::filesystem::remove(stepPath);
  std::filesystem::remove_all(destDir);
  std::filesystem::remove_all(destDir2);
}

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <gp_Ax1.hxx>

// FacePlane's normal points OUT of the solid on every planar face, including
// a revolve's end faces, whose plane is often LEFT-handed: its axis is then
// the opposite of the surface normal, and taking the axis flipped the face.
// Laying a turned chess piece's underside up clamped it underside DOWN, and
// every depth derived on it came from the wrong end.
TEST_CASE("FacePlane's normal points out of the solid on a revolve's ends",
          "[cadkernel]") {
  BRepBuilderAPI_MakePolygon poly;
  poly.Add(gp_Pnt(0, 0, 0));
  poly.Add(gp_Pnt(0, 0, 10));
  poly.Add(gp_Pnt(30, 0, 10));
  poly.Add(gp_Pnt(30, 0, 0));
  poly.Close();
  const TopoDS_Face profile = BRepBuilderAPI_MakeFace(poly.Wire()).Face();
  BRepPrimAPI_MakeRevol revol(profile,
                              gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
  REQUIRE(revol.IsDone());
  const CADShape bar(revol.Shape());
  int planar = 0;
  for (int f = 0; f < 16; f++) {
    glm::vec3 c, n, u;
    if (!bar.FacePlane(f, c, n, u))
      continue;
    planar++;
    // A step along the normal from the face's centre is outside the solid.
    const glm::vec3 out = c + n * 0.5f;
    BRepClass3d_SolidClassifier probe(bar.GetShape(),
                                      gp_Pnt(out.x, out.y, out.z), 1e-6);
    INFO("face " << f << " normal " << n.x << "," << n.y << "," << n.z);
    CHECK(probe.State() == TopAbs_OUT);
  }
  CHECK(planar == 2);
}
