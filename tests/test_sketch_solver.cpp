// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// These tests require the real solver; skip cleanly if built without it.
#ifdef HAVE_PLANEGCS

static SketchFeature MakeSketch() {
  return SketchFeature("S", SketchPlane(PlaneType::XY));
}

static glm::dvec2 LineA(const sketch::Entity& e) {
  return std::get<sketch::Line>(e).a;
}
static glm::dvec2 LineB(const sketch::Entity& e) {
  return std::get<sketch::Line>(e).b;
}

TEST_CASE("Coincident merges two endpoints", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{4.1, 0.2}, {4, 3}}); // near L0.b
  // Coincident: L1.a (which 0) == L0.b (which 1)
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 1}});

  sketch::Solver solver;
  solver.Load(s);
  sketch::SolveResult r = solver.Solve();
  REQUIRE(r.ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 l0b = LineB(out[0]);
  glm::dvec2 l1a = LineA(out[1]);
  CHECK_THAT(glm::length(l0b - l1a), WithinAbs(0.0, 1e-5));
}

TEST_CASE("Two-point Vertical/Horizontal aligns points on an axis",
          "[solver][align]") {
  // Point-to-point alignment (Phase N5): Vertical → shared X, Horizontal →
  // shared Y. Both refs are points, so the solver takes the point-pair form.
  SECTION("Vertical shares X") {
    SketchFeature s = MakeSketch();
    uint32_t p0 = s.AddEntity(sketch::Point{{0, 0}});
    uint32_t p1 = s.AddEntity(sketch::Point{{2, 5}});
    s.constraints.push_back(
        {sketch::ConstraintKind::Vertical, {p0, 0}, {p1, 0}});
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    CHECK_THAT(std::get<sketch::Point>(out[0]).p.x,
               WithinAbs(std::get<sketch::Point>(out[1]).p.x, 1e-5));
  }
  SECTION("Horizontal shares Y") {
    SketchFeature s = MakeSketch();
    uint32_t p0 = s.AddEntity(sketch::Point{{0, 0}});
    uint32_t p1 = s.AddEntity(sketch::Point{{2, 5}});
    s.constraints.push_back(
        {sketch::ConstraintKind::Horizontal, {p0, 0}, {p1, 0}});
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    CHECK_THAT(std::get<sketch::Point>(out[0]).p.y,
               WithinAbs(std::get<sketch::Point>(out[1]).p.y, 1e-5));
  }
}

TEST_CASE("Constraints resolve against datum geometry (origin + axes)",
          "[solver][datum]") {
  SECTION("Coincident to the origin pins a point to (0,0)") {
    SketchFeature s = MakeSketch();
    uint32_t p = s.AddEntity(sketch::Point{{3, 4}});
    s.constraints.push_back(
        {sketch::ConstraintKind::Coincident, {p, 0}, {sketch::kOrigin, 0}});
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    const glm::dvec2 q = std::get<sketch::Point>(out[0]).p;
    CHECK_THAT(q.x, WithinAbs(0.0, 1e-5));
    CHECK_THAT(q.y, WithinAbs(0.0, 1e-5));
  }
  SECTION("Point-on-line onto the X axis drops Y to 0") {
    SketchFeature s = MakeSketch();
    uint32_t p = s.AddEntity(sketch::Point{{3, 4}});
    s.constraints.push_back(
        {sketch::ConstraintKind::PointOnLine, {p, 0}, {sketch::kAxisX, 0}});
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    CHECK_THAT(std::get<sketch::Point>(out[0]).p.y, WithinAbs(0.0, 1e-5));
  }
  SECTION("Symmetric about the Y axis mirrors X") {
    SketchFeature s = MakeSketch();
    uint32_t a = s.AddEntity(sketch::Point{{2.0, 1.0}});
    uint32_t b = s.AddEntity(sketch::Point{{2.5, 1.5}});
    sketch::Constraint c;
    c.kind = sketch::ConstraintKind::Symmetric;
    c.a = {a, 0};
    c.b = {b, 0};
    c.c = {sketch::kAxisY, 0}; // symmetry line = the Y axis (x = 0)
    s.constraints.push_back(c);
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    const glm::dvec2 qa = std::get<sketch::Point>(out[0]).p;
    const glm::dvec2 qb = std::get<sketch::Point>(out[1]).p;
    CHECK_THAT(qa.x, WithinAbs(-qb.x, 1e-4)); // mirrored across x = 0
    CHECK_THAT(qa.y, WithinAbs(qb.y, 1e-4));
  }
}

TEST_CASE("An ellipse centre is a solver point that Coincident moves",
          "[solver][ellipse]") {
  // The ellipse centre snaps onto a fixed anchor point; radii/rotation are
  // rigid, so the whole ellipse translates with its centre.
  SketchFeature s = MakeSketch();
  uint32_t anchor = s.AddEntity(sketch::Point{{2.0, 1.0}});
  uint32_t el = s.AddEntity(sketch::Ellipse{{0.0, 0.0}, 1.5, 0.5, 0.3});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {anchor, 0}, {0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {el, 0}, {anchor, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& e = std::get<sketch::Ellipse>(out[1]);
  CHECK_THAT(e.center.x, WithinAbs(2.0, 1e-5));
  CHECK_THAT(e.center.y, WithinAbs(1.0, 1e-5));
  CHECK_THAT(e.rMajor, WithinAbs(1.5, 1e-9)); // shape unchanged
  CHECK_THAT(e.rMinor, WithinAbs(0.5, 1e-9));
  CHECK_THAT(e.rotation, WithinAbs(0.3, 1e-9));
}

TEST_CASE("Horizontal levels a line's endpoints", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 1.5}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(LineA(out[0]).y, WithinAbs(LineB(out[0]).y, 1e-5));
}

TEST_CASE("A redundant constraint leaves DOF unchanged (add-then-diagnose)",
          "[solver]") {
  // The signal auto-constraint inference uses to drop redundant snaps: adding a
  // second Horizontal to an already-horizontal line must not reduce the DOF.
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 1.5}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  sketch::Solver one;
  one.Load(s);
  sketch::SolveResult r1 = one.Solve();
  REQUIRE(r1.ok);

  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  sketch::Solver two;
  two.Load(s);
  sketch::SolveResult r2 = two.Solve();
  REQUIRE(r2.ok);
  CHECK(r2.dof == r1.dof); // no new information → would be dropped
}

TEST_CASE("Distance constraint sets endpoint separation", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {3, 0}});
  // Pin start, and constrain length to 5
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(glm::length(LineB(out[0]) - LineA(out[0])), WithinRel(5.0, 1e-4));
}

TEST_CASE("Radius constraint sets a circle radius", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Radius, {c0, 0}, {0, 0}, 2.5});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(std::get<sketch::Circle>(out[0]).radius, WithinRel(2.5, 1e-4));
}

TEST_CASE("Angle constraint sets the angle between two lines", "[solver]") {
  SketchFeature s = MakeSketch();
  // L0 along +x (pinned-ish); L1 shares the origin at a shallow angle.
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {3.5, 2.0}}); // ~30 deg
  // L1.a coincident to L0.a so the vertex stays put.
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Angle, {l0, 0}, {l1, 0}, glm::radians(45.0)});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 d0 = glm::normalize(LineB(out[0]) - LineA(out[0]));
  glm::dvec2 d1 = glm::normalize(LineB(out[1]) - LineA(out[1]));
  // L2LAngle is measured from l0's direction to l1's; signed via cross.
  double ang = std::atan2(d0.x * d1.y - d0.y * d1.x, glm::dot(d0, d1));
  CHECK_THAT(std::abs(ang), WithinAbs(glm::radians(45.0), 1e-4));
}

TEST_CASE("Tangent keeps the circle on its side of the line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}}); // along +x, y=0
  uint32_t c0 = s.AddEntity(sketch::Circle{{2, 3}, 1.0});  // above the line
  s.constraints.push_back({sketch::ConstraintKind::Tangent, {l0, 0}, {c0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  auto& circ = std::get<sketch::Circle>(out[1]);
  glm::dvec2 a = LineA(out[0]);
  glm::dvec2 b = LineB(out[0]);
  // Distance from center to the (infinite) line ~= radius.
  glm::dvec2 d = glm::normalize(b - a);
  glm::dvec2 n{-d.y, d.x};
  double dist = std::abs(glm::dot(circ.center - a, n));
  CHECK_THAT(dist, WithinRel(circ.radius, 1e-4));
  CHECK(circ.center.y > 0.0); // still above the line, not flipped through it
}

TEST_CASE("PointOnLine pulls a point onto a line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}}); // along +x
  uint32_t p0 = s.AddEntity(sketch::Point{{2, 2}});        // off the line
  s.constraints.push_back(
      {sketch::ConstraintKind::PointOnLine, {p0, 0}, {l0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 a = LineA(out[0]);
  glm::dvec2 b = LineB(out[0]);
  glm::dvec2 p = std::get<sketch::Point>(out[1]).p;
  // Colinear: cross of (b-a, p-a) ~= 0.
  glm::dvec2 ab = b - a;
  glm::dvec2 ap = p - a;
  CHECK_THAT(ab.x * ap.y - ab.y * ap.x, WithinAbs(0.0, 1e-4));
}

TEST_CASE("Deleting a line keeps a coincident neighbour's constraints",
          "[solver][delete]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{4, 0}, {4, 3}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 1}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});

  // Deleting l1 only drops the (trivial) coincident — no warning-worthy loss.
  size_t dropped = s.DeleteEntity(l1);
  CHECK(dropped == 0);
  REQUIRE(s.entities.size() == 1);
  REQUIRE(s.constraints.size() == 1); // Horizontal on l0 survives
  CHECK(s.constraints[0].kind == sketch::ConstraintKind::Horizontal);
  CHECK(sketch::EntityId(s.entities[0]) == l0);

  sketch::Solver solver;
  solver.Load(s);
  CHECK(solver.Solve().ok);
}

TEST_CASE("Deleting a dimensioned entity counts the dropped constraint",
          "[solver][delete]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  size_t dropped = s.DeleteEntity(l0);
  CHECK(dropped == 1); // Distance is non-trivial
  CHECK(s.entities.empty());
  CHECK(s.constraints.empty());
}

TEST_CASE("Transitive coincidence survives deleting the middle point",
          "[solver][delete]") {
  SketchFeature s = MakeSketch();
  uint32_t p0 = s.AddEntity(sketch::Point{{0, 0}});
  uint32_t pMid = s.AddEntity(sketch::Point{{5, 5}});
  uint32_t p2 = s.AddEntity(sketch::Point{{10, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {p0, 0}, {pMid, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {pMid, 0}, {p2, 0}});

  // Deleting the middle point should re-link p0~p2 so they stay merged.
  s.DeleteEntity(pMid);
  REQUIRE(s.entities.size() == 2);
  REQUIRE(s.constraints.size() == 1);
  CHECK(s.constraints[0].kind == sketch::ConstraintKind::Coincident);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 a = std::get<sketch::Point>(out[0]).p;
  glm::dvec2 b = std::get<sketch::Point>(out[1]).p;
  CHECK_THAT(glm::length(a - b), WithinAbs(0.0, 1e-5));
}

TEST_CASE("Split keeps both pieces horizontal and the length spanning",
          "[solver][split]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {6, 0.4}}); // slightly sloped
  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 6.0});

  // Split near the middle; the transferred H (both pieces) + spanning distance
  // must still solve to a straight 6-long horizontal segment pair.
  uint32_t l1 = s.SplitEntity(l0, {3, 0.2});
  REQUIRE(l1 != 0);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto& p1 = std::get<sketch::Line>(out[s.IndexOfEntity(l0)]);
  auto& p2 = std::get<sketch::Line>(out[s.IndexOfEntity(l1)]);
  CHECK_THAT(p1.a.y, WithinAbs(p1.b.y, 1e-5)); // piece 1 horizontal
  CHECK_THAT(p2.a.y, WithinAbs(p2.b.y, 1e-5)); // piece 2 horizontal
  CHECK_THAT(glm::length(p1.b - p2.a), WithinAbs(0.0, 1e-5)); // joined at cut
  // Total span from piece1.a to piece2.b is the dimensioned 6.
  CHECK_THAT(glm::length(p2.b - p1.a), WithinRel(6.0, 1e-4));
}

TEST_CASE("Midpoint pins a point to a line's centre", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t p0 = s.AddEntity(sketch::Point{{1, 2}}); // off the middle
  s.constraints.push_back({sketch::ConstraintKind::Midpoint, {p0, 0}, {l0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 mid = 0.5 * (LineA(out[0]) + LineB(out[0]));
  glm::dvec2 p = std::get<sketch::Point>(out[1]).p;
  CHECK_THAT(glm::length(p - mid), WithinAbs(0.0, 1e-5));
}

TEST_CASE("Symmetric mirrors two points across a line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t axis =
      s.AddEntity(sketch::Line{{0, 0}, {0, 4}}); // vertical axis x=0
  uint32_t pa = s.AddEntity(sketch::Point{{-2, 1}});
  uint32_t pb = s.AddEntity(sketch::Point{{2.5, 1.3}}); // not yet mirrored
  // Pin the axis so symmetry is about x=0 (not a drifted axis).
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 1}});
  sketch::Constraint sym{sketch::ConstraintKind::Symmetric, {pa, 0}, {pb, 0}};
  sym.c = {axis, 0};
  s.constraints.push_back(sym);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 a = std::get<sketch::Point>(out[1]).p;
  glm::dvec2 b = std::get<sketch::Point>(out[2]).p;
  CHECK_THAT(a.x, WithinAbs(-b.x, 1e-4)); // mirrored across x=0
  CHECK_THAT(a.y, WithinAbs(b.y, 1e-4));  // same height
}

TEST_CASE("Fixed pins a point under a drag", "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // pin a

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{l0, 0}})); // try to drag the fixed endpoint
  solver.SetDragTargets({{3.0, 3.0}});
  REQUIRE(solver.SolveDrag().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(glm::length(LineA(out[0])), WithinAbs(0.0, 1e-4)); // stayed at 0,0
}

TEST_CASE("Concentric shares two circle centres", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  uint32_t c1 = s.AddEntity(sketch::Circle{{2, 1}, 2.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Concentric, {c0, 0}, {c1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 ca = std::get<sketch::Circle>(out[0]).center;
  glm::dvec2 cb = std::get<sketch::Circle>(out[1]).center;
  CHECK_THAT(glm::length(ca - cb), WithinAbs(0.0, 1e-5));
}

TEST_CASE("Collinear puts two lines on one line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {2, 0}});
  uint32_t l1 =
      s.AddEntity(sketch::Line{{3, 0.5}, {5, 0.7}}); // offset + tilted
  s.constraints.push_back(
      {sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Fixed, {l0, 0}}); // pin l0 to y=0
  s.constraints.push_back(
      {sketch::ConstraintKind::Collinear, {l0, 0}, {l1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  // Both lines end up on y = 0 (collinear with the horizontal l0).
  auto& b = std::get<sketch::Line>(out[1]);
  CHECK_THAT(b.a.y, WithinAbs(0.0, 1e-4));
  CHECK_THAT(b.b.y, WithinAbs(0.0, 1e-4));
}

TEST_CASE("DistanceX / DistanceY drive a coordinate gap", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {1, 1}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // pin a
  s.constraints.push_back(
      {sketch::ConstraintKind::DistanceX, {l0, 0}, {l0, 1}, 3.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::DistanceY, {l0, 0}, {l0, 1}, 2.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 a = LineA(out[0]), b = LineB(out[0]);
  CHECK_THAT(b.x - a.x, WithinAbs(3.0, 1e-4));
  CHECK_THAT(b.y - a.y, WithinAbs(2.0, 1e-4));
}

TEST_CASE("Diameter drives radius to half its value", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Diameter, {c0, 0}, {0, 0}, 5.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(std::get<sketch::Circle>(out[0]).radius, WithinRel(2.5, 1e-4));
}

TEST_CASE("Over-constrained sketch reports without crashing", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  // Two conflicting distances on the same points
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

  sketch::Solver solver;
  solver.Load(s);
  sketch::SolveResult r = solver.Solve(); // must not crash
  // Either fails or flags conflict; just require it returned a verdict.
  CHECK((r.conflicting.size() > 0 || !r.ok || r.dof >= 0));
}

TEST_CASE("Conflicting tags map back to constraint indices", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // idx 0
  // Two conflicting lengths on the same line (idx 1, 2).
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

  sketch::Solver solver;
  solver.Load(s);
  sketch::SolveResult r = solver.Solve();
  // The flagged constraints are the Distances (indices 1/2), never the Fixed(0)
  // which adds no GCS constraint.
  for (size_t ci : r.conflictingConstraints)
    CHECK((ci == 1 || ci == 2));
  for (size_t ci : r.redundantConstraints)
    CHECK((ci == 1 || ci == 2));
  CHECK((!r.conflictingConstraints.empty() || !r.redundantConstraints.empty() ||
         !r.ok));
}

TEST_CASE(
    "Dragging a whole line translates it and pulls a coincident neighbour",
    "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 =
      s.AddEntity(sketch::Line{{4, 0}, {4, 3}}); // l1.a joined to l0.b
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {l0, 1}, {l1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{l0, 0}, {l0, 1}})); // whole line = both endpoints
  const glm::dvec2 d{2.0, 1.0};
  solver.SetDragTargets(
      {{0 + d.x, 0 + d.y}, {4 + d.x, 0 + d.y}}); // rigid delta
  REQUIRE(solver.SolveDrag().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 a = LineA(out[s.IndexOfEntity(l0)]);
  glm::dvec2 b = LineB(out[s.IndexOfEntity(l0)]);
  CHECK_THAT(glm::length(a - glm::dvec2(2, 1)), WithinAbs(0.0, 1e-2));
  CHECK_THAT(glm::length(b - glm::dvec2(6, 1)), WithinAbs(0.0, 1e-2));
  // The neighbour's coincident endpoint followed to l0's new far end.
  glm::dvec2 l1a = LineA(out[s.IndexOfEntity(l1)]);
  CHECK_THAT(glm::length(l1a - b), WithinAbs(0.0, 1e-4));
}

TEST_CASE("Dragging a dimensioned line translates without changing its length",
          "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t line = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {line, 0}, {line, 1}, 4.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{line, 0}, {line, 1}}));
  const glm::dvec2 delta{2.0, 1.0};
  solver.SetDragTargets({delta, glm::dvec2(4, 0) + delta});
  REQUIRE(solver.SolveDrag().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& moved = std::get<sketch::Line>(out[s.IndexOfEntity(line)]);
  CHECK_THAT(glm::length(moved.a - delta), WithinAbs(0.0, 1e-3));
  CHECK_THAT(glm::length(moved.b - (glm::dvec2(4, 0) + delta)),
             WithinAbs(0.0, 1e-3));
  CHECK_THAT(glm::length(moved.b - moved.a), WithinAbs(4.0, 1e-4));
}

TEST_CASE("Dragging a circle rim resizes it with the centre fixed",
          "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginRadiusDrag(c0));
  solver.SetRadiusTarget(2.5);
  REQUIRE(solver.SolveDrag().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  auto& circ = std::get<sketch::Circle>(out[0]);
  CHECK_THAT(circ.radius, WithinAbs(2.5, 1e-2));
  CHECK_THAT(glm::length(circ.center), WithinAbs(0.0, 1e-5)); // centre held
}

TEST_CASE("Dragging a heavily constrained sketch stays stable",
          "[solver][drag]") {
  // A rectangle (4 lines + coincidents + H/V). Drag one corner over many small
  // steps and confirm it converges without blowing up (the rescale-0.01 pull).
  SketchFeature s = MakeSketch();
  auto ids = s.AddRectangle({0, 0}, {4, 2});
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{ids[0], 0}})); // bottom line's start = a corner

  glm::dvec2 target{0, 0};
  for (int k = 1; k <= 20; k++) {
    target = glm::dvec2(-1.0, -1.0) * (k / 20.0); // walk the corner out
    solver.SetDragTargets({target});
    REQUIRE(solver.SolveDrag().ok);
  }
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 corner = LineA(out[s.IndexOfEntity(ids[0])]);
  CHECK(std::isfinite(corner.x));
  CHECK(std::isfinite(corner.y));
  CHECK_THAT(glm::length(corner - target), WithinAbs(0.0, 1e-1)); // tracked it
}

TEST_CASE("BeginDrag refuses a conflicting sketch", "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back(
      {sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

  sketch::Solver solver;
  solver.Load(s);
  CHECK_FALSE(solver.BeginDrag({{l0, 0}})); // refuse — conflicting
}

TEST_CASE("A rectangle (4 lines + H/V) solves and extrudes",
          "[solver][extrude]") {
  // Rectangle = 4 lines + corner coincidents + H/V; the solver settles the
  // corners and the four lines chain into one closed profile loop.
  SketchFeature s = MakeSketch();
  s.AddRectangle({0, 0}, {4, 2});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto loops = sketch::BuildLoops(out);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(s.plane, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(16.0, 1e-6));
}

TEST_CASE("Radius constraint sets an arc radius", "[solver][arc]") {
  SketchFeature s = MakeSketch();
  // Quarter arc, CCW from 0 to pi/2, radius 1.
  uint32_t a0 = s.AddEntity(sketch::Arc{{0, 0}, 1.0, 0.0, M_PI / 2});
  s.constraints.push_back(
      {sketch::ConstraintKind::Radius, {a0, 0}, {0, 0}, 2.5});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& arc = std::get<sketch::Arc>(out[0]);
  CHECK_THAT(arc.radius, WithinRel(2.5, 1e-4));
  // Endpoints stay on the (grown) arc: start at angle 0, end at pi/2.
  glm::dvec2 start = sketch::ArcPointUV(arc, 1);
  glm::dvec2 end = sketch::ArcPointUV(arc, 2);
  CHECK_THAT(glm::length(start - arc.center), WithinRel(2.5, 1e-3));
  CHECK_THAT(glm::length(end - arc.center), WithinRel(2.5, 1e-3));
}

TEST_CASE("An arc endpoint constrained coincident to a line endpoint solves",
          "[solver][arc]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {2, 0}});
  // Arc whose start (angle 0 → point (3,1)) is near, but not on, L0.b (2,0).
  uint32_t a0 = s.AddEntity(sketch::Arc{{2, 1}, 1.0, -M_PI / 2, M_PI / 2});
  // Coincident: arc.start (which 1) == L0.b (which 1).
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {a0, 1}, {l0, 1}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::dvec2 arcStart = sketch::ArcPointUV(std::get<sketch::Arc>(out[1]), 1);
  glm::dvec2 l0b = std::get<sketch::Line>(out[0]).b;
  CHECK_THAT(glm::length(arcStart - l0b), WithinAbs(0.0, 1e-4));
  // The endpoint still lies on the arc's own rim.
  const auto& arc = std::get<sketch::Arc>(out[1]);
  CHECK_THAT(glm::length(arcStart - arc.center),
             WithinRel(static_cast<double>(arc.radius), 1e-3));
}

TEST_CASE("A linked mirror follows the source through solve + regeneration",
          "[solver][generative]") {
  // v51: the linked mirror is a derived OP — the solver doesn't couple the
  // copy; the regeneration pass after every solve re-stamps it (same
  // lifecycle as linked patterns; the UI always runs solve → WriteBack →
  // RegenerateDerived).
  SketchFeature s = MakeSketch();
  uint32_t axis = s.AddEntity(sketch::Line{{0, -5}, {0, 5}}); // y-axis
  uint32_t seg = s.AddEntity(sketch::Line{{2, 1}, {3, 4}});
  // Pin the axis so it doesn't drift.
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 0}, {0, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 1}, {0, 0}});
  auto gen = s.MirrorEntities({seg}, axis, /*link=*/true);
  REQUIRE(gen.size() == 1);

  // Move the source line's endpoint, solve, write back, regenerate: the
  // linked copy must mirror the moved source.
  std::get<sketch::Line>(s.entities[s.IndexOfEntity(seg)]).a = {2.5, 2.0};
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  solver.WriteBack(s.entities);
  s.RegenerateDerived();

  glm::dvec2 srcA = std::get<sketch::Line>(s.entities[s.IndexOfEntity(seg)]).a;
  glm::dvec2 copyA =
      std::get<sketch::Line>(s.entities[s.IndexOfEntity(gen[0])]).a;
  // Copy endpoint is the mirror of the (moved) source endpoint about x=0.
  CHECK_THAT(copyA.x, WithinAbs(-srcA.x, 1e-3));
  CHECK_THAT(copyA.y, WithinAbs(srcA.y, 1e-3));
}

TEST_CASE("A filleted corner solves and extrudes",
          "[solver][arc][generative]") {
  // A rectangle with one corner filleted still closes and extrudes.
  SketchFeature s = MakeSketch();
  auto r = s.AddRectangle({0, 0}, {4, 2});
  // Fillet the top-right corner where side r[1] (right) meets r[2] (top).
  uint32_t arcId = s.FilletCorner(r[1], r[2], 0.5);
  REQUIRE(arcId != 0);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto loops = sketch::BuildLoops(out);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(s.plane, loops, 1.0, false);
  REQUIRE(prism.IsValid());
  // Area = 8 - (corner square - quarter disk); height 1.
  const double area = 8.0 - (0.5 * 0.5 - M_PI * 0.5 * 0.5 / 4.0);
  CHECK_THAT(prism.Volume(), WithinRel(area, 1e-2));
}

TEST_CASE("Under-defined entities are identified after a solve",
          "[sketch_solver]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {2, 0}});
  sketch::Solver s;
  s.Load(sk);
  const sketch::SolveResult r = s.Solve();
  CHECK(r.dof == 4); // both endpoints free
  const std::vector<uint32_t> ud = s.UnderDefinedEntities();
  REQUIRE(ud.size() == 1);
  CHECK(ud[0] == l);

  // Fully constrain it: fixed endpoints -> no under-defined entities.
  sk.constraints.push_back({sketch::ConstraintKind::Fixed, {l, 0}, {}});
  sk.constraints.push_back({sketch::ConstraintKind::Fixed, {l, 1}, {}});
  sketch::Solver s2;
  s2.Load(sk);
  const sketch::SolveResult r2 = s2.Solve();
  CHECK(r2.dof == 0);
  CHECK(s2.UnderDefinedEntities().empty());
}

TEST_CASE("Symmetric about a POINT mirrors the pair through it",
          "[solver][symmetric]") {
  SketchFeature s = MakeSketch();
  const uint32_t p0 = s.AddEntity(sketch::Point{{1, 1}});
  const uint32_t p1 = s.AddEntity(sketch::Point{{-0.6, -0.8}});
  const uint32_t pc = s.AddEntity(sketch::Point{{0, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {pc, 0}, {}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {p0, 0}, {}});
  sketch::Constraint sym{sketch::ConstraintKind::Symmetric, {p0, 0}, {p1, 0}};
  sym.c = {pc, 0};
  s.constraints.push_back(sym);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const glm::dvec2 mirrored = std::get<sketch::Point>(out[1]).p;
  CHECK_THAT(mirrored.x, WithinAbs(-1.0, 1e-4));
  CHECK_THAT(mirrored.y, WithinAbs(-1.0, 1e-4));
}

TEST_CASE("Centered rectangle stays anchored to its centre point",
          "[solver][rectangle]") {
  SketchFeature s = MakeSketch();
  const auto ids = s.AddRectangleCentered({1.0, 0.5}, {2.0, 1.0}); // 2x1
  REQUIRE(s.entities.size() == 5); // 4 sides + the centre point
  // Pin the centre, then dimension the width: the rectangle must grow
  // symmetrically about the centre instead of dragging it along.
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {ids[4], 0}, {}});
  sketch::Constraint width{
      sketch::ConstraintKind::Distance, {ids[0], 0}, {ids[0], 1}};
  width.value = 3.0;
  s.constraints.push_back(width);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& bottom = std::get<sketch::Line>(out[0]);
  CHECK_THAT(glm::length(bottom.b - bottom.a), WithinAbs(3.0, 1e-3));
  CHECK_THAT(0.5 * (bottom.a.x + bottom.b.x), WithinAbs(1.0, 1e-3));
  const glm::dvec2 center = std::get<sketch::Point>(out[4]).p;
  CHECK_THAT(center.x, WithinAbs(1.0, 1e-4)); // the anchor never moved
  CHECK_THAT(center.y, WithinAbs(0.5, 1e-4));
}

TEST_CASE("DistancePL holds a point at a perpendicular line distance",
          "[solver][dimension]") {
  SketchFeature s = MakeSketch();
  const uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const uint32_t p0 = s.AddEntity(sketch::Point{{1, 2}});
  // Pin the line; only the point may move to satisfy the dimension.
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}, {}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 1}, {}});
  sketch::Constraint dim{sketch::ConstraintKind::DistancePL, {p0, 0}, {l0, 0}};
  dim.value = 1.5;
  s.constraints.push_back(dim);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const glm::dvec2 p = std::get<sketch::Point>(out[1]).p;
  // Distance holds AND the point stayed on its original (+Y) side.
  CHECK_THAT(p.y, WithinAbs(1.5, 1e-4));
}

TEST_CASE("DistancePL dimensions the gap between parallel lines",
          "[solver][dimension]") {
  SketchFeature s = MakeSketch();
  const uint32_t la = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  const uint32_t lb = s.AddEntity(sketch::Line{{0, 2.2}, {4, 2.2}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {la, 0}, {}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {la, 1}, {}});
  s.constraints.push_back({sketch::ConstraintKind::Parallel, {la, 0}, {lb, 0}});
  // The parallel-lines dimension: an endpoint of B held off line A.
  sketch::Constraint dim{sketch::ConstraintKind::DistancePL, {lb, 0}, {la, 0}};
  dim.value = 2.0;
  s.constraints.push_back(dim);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(LineA(out[1]).y, WithinAbs(2.0, 1e-4));
  CHECK_THAT(LineB(out[1]).y, WithinAbs(2.0, 1e-4)); // stayed parallel
}

TEST_CASE("Expression-driven entity fields are pinned during the solve",
          "[solver][exprs]") {
  // Radius: an Equal constraint against an expr-driven circle must move the
  // OTHER circle's radius — the driven one is solver-fixed (Decision 2A).
  SketchFeature s = MakeSketch();
  const uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 3.0});
  const uint32_t c1 = s.AddEntity(sketch::Circle{{10, 0}, 1.0});
  s.entityExprs[c0]["r"] = "bore / 2"; // the solver only cares it is non-empty
  s.constraints.push_back({sketch::ConstraintKind::Equal, {c0, 0}, {c1, 0}});
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(std::get<sketch::Circle>(out[0]).radius, WithinAbs(3.0, 1e-6));
  CHECK_THAT(std::get<sketch::Circle>(out[1]).radius, WithinAbs(3.0, 1e-5));

  // Line endpoint: a Distance dimension stretches only the free end; the
  // expr-driven `a` endpoint stays put.
  SketchFeature s2 = MakeSketch();
  const uint32_t l0 = s2.AddEntity(sketch::Line{{1, 2}, {4, 2}});
  s2.entityExprs[l0]["ax"] = "x0";
  s2.entityExprs[l0]["ay"] = "y0";
  sketch::Constraint dim{sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}};
  dim.value = 10.0;
  s2.constraints.push_back(dim);
  sketch::Solver solver2;
  solver2.Load(s2);
  REQUIRE(solver2.Solve().ok);
  std::vector<sketch::Entity> out2;
  solver2.WriteBack(out2);
  CHECK_THAT(LineA(out2[0]).x, WithinAbs(1.0, 1e-5));
  CHECK_THAT(LineA(out2[0]).y, WithinAbs(2.0, 1e-5));
  CHECK_THAT(glm::length(LineB(out2[0]) - LineA(out2[0])),
             WithinAbs(10.0, 1e-4));
}

TEST_CASE("An expression-driven arc keeps its angles through a solve",
          "[solver][exprs][arc]") {
  // A sketch whose arcs follow variables re-solves on every regen. The
  // arc's endpoints were seeded through float32, ~1e-6 off the arc rules;
  // the solve moved the ANGLES to meet them and the write-back kept that,
  // so the chess set's queen drifted until its arcs no longer met their
  // lines ("sketch has no closed profile") at every size.
  SketchFeature s = MakeSketch();
  const double a0 = -0.1080008521212341, a1 = 2.2845669156660735;
  const uint32_t arc = s.AddEntity(sketch::Arc{
      {60.21451950073242, 0.22611357271671295}, 2.0977032402036246, a0, a1});
  s.entityExprs[arc] = {{"cx", "x"}, {"cy", "y"}, {"r", "r"}};
  for (int pass = 0; pass < 20; pass++) {
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    solver.WriteBack(s.entities);
  }
  const auto& out = std::get<sketch::Arc>(s.entities[0]);
  CHECK_THAT(out.startAngle, WithinAbs(a0, 1e-12));
  CHECK_THAT(out.endAngle, WithinAbs(a1, 1e-12));
}

// A sketch fillet must NEVER rotate a line — it only trims to the tangent
// points and inserts the arc. These guard the reported staircase bug: a line
// whose direction is locked only IMPLICITLY (transitively through the shared
// corner) used to be dragged into a rotation when the corner was trimmed. See
// SketchFeature::FilletCorner's direction re-anchoring.
namespace {
double AngleDeg(const sketch::Entity& e) {
  const auto& l = std::get<sketch::Line>(e);
  return std::atan2(l.b.y - l.a.y, l.b.x - l.a.x) * 180.0 / M_PI;
}
} // namespace

TEST_CASE("Fillet preserves implicit line directions in a staircase",
          "[solver][arc][fillet]") {
  // The owner's live-app topology: verticality of L3 is implicit — its top
  // shares X with the (coincident) corner, via a cross-line point-pair
  // vertical {V, [L3.b], [L2.corner]}. Filleting the L2-L3 corner trims L2's
  // corner off the axis; the cross-line constraint then dragged L3 off
  // vertical (observed ~96-106 deg), including non-adjacent lines.
  SketchFeature s = MakeSketch();
  using K = sketch::ConstraintKind;
  const uint32_t L1 = s.AddEntity(sketch::Line{{-2, 0}, {-2, 2}}); // vertical
  const uint32_t L2 = s.AddEntity(sketch::Line{{-2, 2}, {0, 2}});  // horizontal
  const uint32_t L3 = s.AddEntity(sketch::Line{{0, 2}, {0, 4}});   // vertical
  const uint32_t L4 = s.AddEntity(sketch::Line{{0, 4}, {2, 4}});   // horizontal
  s.constraints.push_back({K::Coincident, {L1, 1}, {L2, 0}});
  s.constraints.push_back(
      {K::Coincident, {L2, 1}, {L3, 0}}); // the fillet corner
  s.constraints.push_back({K::Coincident, {L3, 1}, {L4, 0}});
  s.constraints.push_back({K::Vertical, {L1, 0}, {0, 0}}); // whole-line
  s.constraints.push_back({K::Fixed, {L1, 0}, {}});
  s.constraints.push_back({K::Fixed, {L1, 1}, {}});
  s.constraints.push_back({K::Horizontal, {L2, 1}, {L1, 1}}); // implicit H
  s.constraints.push_back({K::Vertical, {L2, 1}, {sketch::kOrigin, 0}});
  s.constraints.push_back({K::Vertical, {L3, 1}, {L2, 1}});   // implicit V
  s.constraints.push_back({K::Horizontal, {L4, 1}, {L3, 1}}); // implicit H

  {
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.Solve().ok);
    solver.WriteBack(s.entities);
  }
  auto A = [&](uint32_t id) {
    return AngleDeg(s.entities[s.IndexOfEntity(id)]);
  };
  const double b1 = A(L1), b2 = A(L2), b3 = A(L3), b4 = A(L4);
  REQUIRE(std::abs(std::abs(b3) - 90.0) < 1e-2); // vertical before

  REQUIRE(s.FilletCorner(L2, L3, 0.3) != 0);
  {
    sketch::Solver solver;
    solver.Load(s);
    const sketch::SolveResult r = solver.Solve();
    REQUIRE(r.ok);
    CHECK(r.conflictingConstraints.empty()); // no spurious over-constraint
    solver.WriteBack(s.entities);
  }
  // Every line keeps its exact pre-fillet direction (adjacent + non-adjacent).
  CHECK_THAT(A(L1), WithinAbs(b1, 0.1));
  CHECK_THAT(A(L2), WithinAbs(b2, 0.1));
  CHECK_THAT(A(L3), WithinAbs(b3, 0.1));
  CHECK_THAT(A(L4), WithinAbs(b4, 0.1));
}

TEST_CASE("Fillet in a fully-built corner reads clean and keeps every lock",
          "[solver][arc][fillet]") {
  // The owner's live post-fix sketch (dump): same staircase, but the corner
  // is pinned to the origin by a COINCIDENT (fully, 2 DOF) rather than a
  // single vertical. The fillet's web -- endpoint coincidents, tangency,
  // the point-on-line direction locks -- must read clean (no redundant, no
  // conflicting) with every line keeping its direction and the radius the
  // only freedom it adds. Held exactly, a centre-to-line tangency beside a
  // shared endpoint is first-order the same equation as the endpoint being
  // on the arc; the solver states it as an angle at the joint instead.
  SketchFeature s = MakeSketch();
  using K = sketch::ConstraintKind;
  const uint32_t L1 = s.AddEntity(sketch::Line{{-1.8f, -1.14f}, {-1.8f, 0}});
  const uint32_t L2 = s.AddEntity(sketch::Line{{-1.8f, 0}, {0, 0}}); // horiz
  const uint32_t L3 = s.AddEntity(sketch::Line{{0, 0}, {0, 2.19f}}); // vert
  const uint32_t L4 = s.AddEntity(sketch::Line{{0, 2.19f}, {1.44f, 2.19f}});
  s.constraints.push_back({K::Coincident, {L1, 1}, {L2, 0}});
  s.constraints.push_back(
      {K::Coincident, {L2, 1}, {L3, 0}}); // the fillet corner
  s.constraints.push_back({K::Coincident, {L3, 1}, {L4, 0}});
  s.constraints.push_back({K::Vertical, {L1, 0}, {0, 0}}); // whole-line V
  s.constraints.push_back({K::Horizontal, {L1, 1}, {sketch::kOrigin, 0}});
  s.constraints.push_back(
      {K::Coincident, {L2, 1}, {sketch::kOrigin, 0}});        // corner
  s.constraints.push_back({K::Vertical, {L3, 1}, {L2, 1}});   // L3 implicit V
  s.constraints.push_back({K::Horizontal, {L4, 1}, {L3, 1}}); // L4 implicit H

  int dofBefore = -1;
  {
    sketch::Solver solver;
    solver.Load(s);
    const sketch::SolveResult r0 = solver.Solve();
    REQUIRE(r0.ok);
    dofBefore = r0.dof;
    solver.WriteBack(s.entities);
  }
  auto A = [&](uint32_t id) {
    return AngleDeg(s.entities[s.IndexOfEntity(id)]);
  };

  const size_t constraintsBefore = s.constraints.size();
  REQUIRE(s.FilletCorner(L2, L3, 0.5) != 0);
  sketch::Solver solver;
  solver.Load(s);
  const sketch::SolveResult r = solver.Solve();
  REQUIRE(r.ok);
  solver.WriteBack(s.entities);
  // No conflicting and no redundant constraints: line-to-arc tangency at
  // the shared end is an angle there, which the coincidence does not repeat.
  CHECK(r.conflictingConstraints.empty());
  CHECK(r.redundantConstraints.empty());
  // The fillet adds exactly one freedom, its radius: no lock was lost.
  // (The corner's own coincidence became P = P and went; its six joins and
  // locks all stay.)
  CHECK(r.dof == dofBefore + 1);
  CHECK(s.constraints.size() == constraintsBefore - 1 + 6);
  // Both direction locks survive; the arc is still there and tangent.
  int arcs = 0;
  for (const auto& e : s.entities)
    if (std::holds_alternative<sketch::Arc>(e))
      arcs++;
  CHECK(arcs == 1);
  // Both verticals still 90, both horizontals still 0.
  CHECK_THAT(std::abs(A(L1)), WithinAbs(90.0, 0.1));
  CHECK_THAT(A(L2), WithinAbs(0.0, 0.1));
  CHECK_THAT(std::abs(A(L3)), WithinAbs(90.0, 0.1));
  CHECK_THAT(A(L4), WithinAbs(0.0, 0.1));
}

TEST_CASE("Fillet preserves a free corner's line directions",
          "[solver][arc][fillet]") {
  // No direction constraints at all — the re-anchoring must leave these alone.
  SketchFeature s = MakeSketch();
  using K = sketch::ConstraintKind;
  const uint32_t l0 = s.AddEntity(sketch::Line{{0, 3}, {0, 0}});
  const uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {3, 1}}); // oblique
  s.constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}});
  s.constraints.push_back({K::Fixed, {l0, 0}, {}});
  s.constraints.push_back({K::Fixed, {l1, 1}, {}});
  auto A = [&](uint32_t id) {
    return AngleDeg(s.entities[s.IndexOfEntity(id)]);
  };
  const double a0 = A(l0), a1 = A(l1);
  REQUIRE(s.FilletCorner(l0, l1, 0.3) != 0);
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  solver.WriteBack(s.entities);
  CHECK_THAT(A(l0), WithinAbs(a0, 0.5));
  CHECK_THAT(A(l1), WithinAbs(a1, 0.5));
}

TEST_CASE("Fillet on explicit H/V corner does not over-constrain",
          "[solver][arc][fillet]") {
  // Whole-line H/V already lock the directions — re-anchoring must not fire
  // (nothing to retarget) and must not introduce a spurious conflict.
  SketchFeature s = MakeSketch();
  using K = sketch::ConstraintKind;
  const uint32_t l0 = s.AddEntity(sketch::Line{{0, 3}, {0, 0}});
  const uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {3, 0}});
  s.constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}});
  s.constraints.push_back({K::Vertical, {l0, 0}, {0, 0}});
  s.constraints.push_back({K::Horizontal, {l1, 0}, {0, 0}});
  s.constraints.push_back({K::Fixed, {l0, 0}, {}});
  auto A = [&](uint32_t id) {
    return AngleDeg(s.entities[s.IndexOfEntity(id)]);
  };
  REQUIRE(s.FilletCorner(l0, l1, 0.3) != 0);
  sketch::Solver solver;
  solver.Load(s);
  const sketch::SolveResult r = solver.Solve();
  REQUIRE(r.ok);
  solver.WriteBack(s.entities);
  CHECK(r.conflictingConstraints.empty());
  CHECK_THAT(std::abs(A(l0)), WithinAbs(90.0, 0.1));
  CHECK_THAT(A(l1), WithinAbs(0.0, 0.1));
}

TEST_CASE("Fillet does not hide a real over-constraint elsewhere",
          "[solver][arc][fillet]") {
  // The add-then-prune must never mask a GENUINE conflict. Here a separate
  // line (lc) is over-constrained by two contradictory distances (3 and 5
  // between the same endpoints) — unsatisfiable — so the solver flags it.
  // Filleting an unrelated clean corner must leave that real conflict
  // flagged: the prune only removes a constraint the fillet ITSELF added,
  // and only when doing so leaves the whole sketch clean — a standing
  // conflict blocks that, so nothing is hidden.
  SketchFeature s = MakeSketch();
  using K = sketch::ConstraintKind;
  const uint32_t l0 =
      s.AddEntity(sketch::Line{{0, 3}, {0, 0}}); // fillet corner
  const uint32_t l1 =
      s.AddEntity(sketch::Line{{0, 0}, {3, 0}}); // fillet corner
  const uint32_t lc = s.AddEntity(sketch::Line{{6, 0}, {9, 1}}); // the conflict
  s.constraints.push_back({K::Coincident, {l0, 1}, {l1, 0}});
  s.constraints.push_back({K::Vertical, {l0, 0}, {0, 0}});
  s.constraints.push_back({K::Horizontal, {l1, 0}, {0, 0}});
  // lc: two contradictory endpoint distances — a real, unsatisfiable conflict.
  s.constraints.push_back({K::Distance, {lc, 0}, {lc, 1}, 3.0});
  s.constraints.push_back({K::Distance, {lc, 0}, {lc, 1}, 5.0});
  const auto refsLc = [&](const sketch::Constraint& c) {
    return c.a.entity == lc || c.b.entity == lc;
  };

  {
    sketch::Solver solver;
    solver.Load(s);
    const sketch::SolveResult r = solver.Solve();
    // The real conflict is present before the fillet.
    REQUIRE(!r.conflictingConstraints.empty());
    bool onLc = false;
    for (size_t i : r.conflictingConstraints)
      onLc |= refsLc(s.constraints[i]);
    REQUIRE(onLc);
  }

  REQUIRE(s.FilletCorner(l0, l1, 0.3) != 0);

  sketch::Solver solver;
  solver.Load(s);
  const sketch::SolveResult r = solver.Solve();
  // The real conflict SURVIVES the fillet — not pruned, not hidden.
  CHECK(!r.conflictingConstraints.empty());
  bool stillOnLc = false;
  for (size_t i : r.conflictingConstraints)
    stillOnLc |= refsLc(s.constraints[i]);
  CHECK(stillOnLc);
}

#endif // HAVE_PLANEGCS
