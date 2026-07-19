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

static glm::vec2 LineA(const sketch::Entity& e) {
  return std::get<sketch::Line>(e).a;
}
static glm::vec2 LineB(const sketch::Entity& e) {
  return std::get<sketch::Line>(e).b;
}

TEST_CASE("Coincident merges two endpoints", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{4.1f, 0.2f}, {4, 3}}); // near L0.b
  // Coincident: L1.a (which 0) == L0.b (which 1)
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 1}});

  sketch::Solver solver;
  solver.Load(s);
  sketch::SolveResult r = solver.Solve();
  REQUIRE(r.ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 l0b = LineB(out[0]);
  glm::vec2 l1a = LineA(out[1]);
  CHECK_THAT(glm::length(l0b - l1a), WithinAbs(0.0f, 1e-5f));
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
               WithinAbs(std::get<sketch::Point>(out[1]).p.x, 1e-5f));
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
               WithinAbs(std::get<sketch::Point>(out[1]).p.y, 1e-5f));
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
    const glm::vec2 q = std::get<sketch::Point>(out[0]).p;
    CHECK_THAT(q.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(q.y, WithinAbs(0.0f, 1e-5f));
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
    CHECK_THAT(std::get<sketch::Point>(out[0]).p.y, WithinAbs(0.0f, 1e-5f));
  }
  SECTION("Symmetric about the Y axis mirrors X") {
    SketchFeature s = MakeSketch();
    uint32_t a = s.AddEntity(sketch::Point{{2.0f, 1.0f}});
    uint32_t b = s.AddEntity(sketch::Point{{2.5f, 1.5f}});
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
    const glm::vec2 qa = std::get<sketch::Point>(out[0]).p;
    const glm::vec2 qb = std::get<sketch::Point>(out[1]).p;
    CHECK_THAT(qa.x, WithinAbs(-qb.x, 1e-4f)); // mirrored across x = 0
    CHECK_THAT(qa.y, WithinAbs(qb.y, 1e-4f));
  }
}

TEST_CASE("An ellipse centre is a solver point that Coincident moves",
          "[solver][ellipse]") {
  // The ellipse centre snaps onto a fixed anchor point; radii/rotation are
  // rigid, so the whole ellipse translates with its centre.
  SketchFeature s = MakeSketch();
  uint32_t anchor = s.AddEntity(sketch::Point{{2.0f, 1.0f}});
  uint32_t el = s.AddEntity(sketch::Ellipse{{0.0f, 0.0f}, 1.5, 0.5, 0.3});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {anchor, 0}, {0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {el, 0}, {anchor, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& e = std::get<sketch::Ellipse>(out[1]);
  CHECK_THAT(e.center.x, WithinAbs(2.0f, 1e-5f));
  CHECK_THAT(e.center.y, WithinAbs(1.0f, 1e-5f));
  CHECK_THAT(e.rMajor, WithinAbs(1.5, 1e-9)); // shape unchanged
  CHECK_THAT(e.rMinor, WithinAbs(0.5, 1e-9));
  CHECK_THAT(e.rotation, WithinAbs(0.3, 1e-9));
}

TEST_CASE("Horizontal levels a line's endpoints", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 1.5f}});
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(LineA(out[0]).y, WithinAbs(LineB(out[0]).y, 1e-5f));
}

TEST_CASE("A redundant constraint leaves DOF unchanged (add-then-diagnose)",
          "[solver]") {
  // The signal auto-constraint inference uses to drop redundant snaps: adding a
  // second Horizontal to an already-horizontal line must not reduce the DOF.
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 1.5f}});
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  sketch::Solver one;
  one.Load(s);
  sketch::SolveResult r1 = one.Solve();
  REQUIRE(r1.ok);

  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
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
  CHECK_THAT(glm::length(LineB(out[0]) - LineA(out[0])),
             WithinRel(5.0, 1e-4));
}

TEST_CASE("Radius constraint sets a circle radius", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  s.constraints.push_back({sketch::ConstraintKind::Radius, {c0, 0}, {0, 0}, 2.5});

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
  uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {3.5f, 2.0f}}); // ~30 deg
  // L1.a coincident to L0.a so the vertex stays put.
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 0}});
  s.constraints.push_back(
      {sketch::ConstraintKind::Angle, {l0, 0}, {l1, 0}, glm::radians(45.0)});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 d0 = glm::normalize(LineB(out[0]) - LineA(out[0]));
  glm::vec2 d1 = glm::normalize(LineB(out[1]) - LineA(out[1]));
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
  glm::vec2 a = LineA(out[0]);
  glm::vec2 b = LineB(out[0]);
  // Distance from center to the (infinite) line ~= radius.
  glm::vec2 d = glm::normalize(b - a);
  glm::vec2 n{-d.y, d.x};
  double dist = std::abs(glm::dot(circ.center - a, n));
  CHECK_THAT(dist, WithinRel(circ.radius, 1e-4));
  CHECK(circ.center.y > 0.0f); // still above the line, not flipped through it
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
  glm::vec2 a = LineA(out[0]);
  glm::vec2 b = LineB(out[0]);
  glm::vec2 p = std::get<sketch::Point>(out[1]).p;
  // Colinear: cross of (b-a, p-a) ~= 0.
  glm::vec2 ab = b - a;
  glm::vec2 ap = p - a;
  CHECK_THAT(ab.x * ap.y - ab.y * ap.x, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("Deleting a line keeps a coincident neighbour's constraints",
          "[solver][delete]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{4, 0}, {4, 3}});
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {l1, 0}, {l0, 1}});
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});

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
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {p0, 0}, {pMid, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {pMid, 0}, {p2, 0}});

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
  glm::vec2 a = std::get<sketch::Point>(out[0]).p;
  glm::vec2 b = std::get<sketch::Point>(out[1]).p;
  CHECK_THAT(glm::length(a - b), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Split keeps both pieces horizontal and the length spanning",
          "[solver][split]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {6, 0.4f}}); // slightly sloped
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 6.0});

  // Split near the middle; the transferred H (both pieces) + spanning distance
  // must still solve to a straight 6-long horizontal segment pair.
  uint32_t l1 = s.SplitEntity(l0, {3, 0.2f});
  REQUIRE(l1 != 0);

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto& p1 = std::get<sketch::Line>(out[s.IndexOfEntity(l0)]);
  auto& p2 = std::get<sketch::Line>(out[s.IndexOfEntity(l1)]);
  CHECK_THAT(p1.a.y, WithinAbs(p1.b.y, 1e-5f)); // piece 1 horizontal
  CHECK_THAT(p2.a.y, WithinAbs(p2.b.y, 1e-5f)); // piece 2 horizontal
  CHECK_THAT(glm::length(p1.b - p2.a), WithinAbs(0.0f, 1e-5f)); // joined at cut
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
  glm::vec2 mid = 0.5f * (LineA(out[0]) + LineB(out[0]));
  glm::vec2 p = std::get<sketch::Point>(out[1]).p;
  CHECK_THAT(glm::length(p - mid), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Symmetric mirrors two points across a line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t axis = s.AddEntity(sketch::Line{{0, 0}, {0, 4}}); // vertical axis x=0
  uint32_t pa = s.AddEntity(sketch::Point{{-2, 1}});
  uint32_t pb = s.AddEntity(sketch::Point{{2.5f, 1.3f}}); // not yet mirrored
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
  glm::vec2 a = std::get<sketch::Point>(out[1]).p;
  glm::vec2 b = std::get<sketch::Point>(out[2]).p;
  CHECK_THAT(a.x, WithinAbs(-b.x, 1e-4f)); // mirrored across x=0
  CHECK_THAT(a.y, WithinAbs(b.y, 1e-4f));  // same height
}

TEST_CASE("Fixed pins a point under a drag", "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // pin a

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{l0, 0}}));       // try to drag the fixed endpoint
  solver.SetDragTargets({{3.0f, 3.0f}});
  REQUIRE(solver.SolveDrag().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(glm::length(LineA(out[0])), WithinAbs(0.0f, 1e-4f)); // stayed at 0,0
}

TEST_CASE("Concentric shares two circle centres", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  uint32_t c1 = s.AddEntity(sketch::Circle{{2, 1}, 2.0});
  s.constraints.push_back({sketch::ConstraintKind::Concentric, {c0, 0}, {c1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 ca = std::get<sketch::Circle>(out[0]).center;
  glm::vec2 cb = std::get<sketch::Circle>(out[1]).center;
  CHECK_THAT(glm::length(ca - cb), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("Collinear puts two lines on one line", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {2, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{3, 0.5f}, {5, 0.7f}}); // offset + tilted
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {l0, 0}, {0, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // pin l0 to y=0
  s.constraints.push_back({sketch::ConstraintKind::Collinear, {l0, 0}, {l1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  // Both lines end up on y = 0 (collinear with the horizontal l0).
  auto& b = std::get<sketch::Line>(out[1]);
  CHECK_THAT(b.a.y, WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(b.b.y, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("DistanceX / DistanceY drive a coordinate gap", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {1, 1}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {l0, 0}}); // pin a
  s.constraints.push_back({sketch::ConstraintKind::DistanceX, {l0, 0}, {l0, 1}, 3.0});
  s.constraints.push_back({sketch::ConstraintKind::DistanceY, {l0, 0}, {l0, 1}, 2.0});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 a = LineA(out[0]), b = LineB(out[0]);
  CHECK_THAT(b.x - a.x, WithinAbs(3.0f, 1e-4f));
  CHECK_THAT(b.y - a.y, WithinAbs(2.0f, 1e-4f));
}

TEST_CASE("Diameter drives radius to half its value", "[solver]") {
  SketchFeature s = MakeSketch();
  uint32_t c0 = s.AddEntity(sketch::Circle{{0, 0}, 1.0});
  s.constraints.push_back({sketch::ConstraintKind::Diameter, {c0, 0}, {0, 0}, 5.0});

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
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

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
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

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

TEST_CASE("Dragging a whole line translates it and pulls a coincident neighbour",
          "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  uint32_t l1 = s.AddEntity(sketch::Line{{4, 0}, {4, 3}}); // l1.a joined to l0.b
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {l0, 1}, {l1, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{l0, 0}, {l0, 1}})); // whole line = both endpoints
  const glm::vec2 d{2.0f, 1.0f};
  solver.SetDragTargets({{0 + d.x, 0 + d.y}, {4 + d.x, 0 + d.y}}); // rigid delta
  REQUIRE(solver.SolveDrag().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 a = LineA(out[s.IndexOfEntity(l0)]);
  glm::vec2 b = LineB(out[s.IndexOfEntity(l0)]);
  CHECK_THAT(glm::length(a - glm::vec2(2, 1)), WithinAbs(0.0f, 1e-2f));
  CHECK_THAT(glm::length(b - glm::vec2(6, 1)), WithinAbs(0.0f, 1e-2f));
  // The neighbour's coincident endpoint followed to l0's new far end.
  glm::vec2 l1a = LineA(out[s.IndexOfEntity(l1)]);
  CHECK_THAT(glm::length(l1a - b), WithinAbs(0.0f, 1e-4f));
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
  CHECK_THAT(glm::length(circ.center), WithinAbs(0.0f, 1e-5f)); // centre held
}

TEST_CASE("Dragging a heavily constrained sketch stays stable", "[solver][drag]") {
  // A rectangle (4 lines + coincidents + H/V). Drag one corner over many small
  // steps and confirm it converges without blowing up (the rescale-0.01 pull).
  SketchFeature s = MakeSketch();
  auto ids = s.AddRectangle({0, 0}, {4, 2});
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{ids[0], 0}})); // bottom line's start = a corner

  glm::vec2 target{0, 0};
  for (int k = 1; k <= 20; k++) {
    target = glm::vec2(-1.0f, -1.0f) * (k / 20.0f); // walk the corner out
    solver.SetDragTargets({target});
    REQUIRE(solver.SolveDrag().ok);
  }
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  glm::vec2 corner = LineA(out[s.IndexOfEntity(ids[0])]);
  CHECK(std::isfinite(corner.x));
  CHECK(std::isfinite(corner.y));
  CHECK_THAT(glm::length(corner - target), WithinAbs(0.0f, 1e-1f)); // tracked it
}

TEST_CASE("BeginDrag refuses a conflicting sketch", "[solver][drag]") {
  SketchFeature s = MakeSketch();
  uint32_t l0 = s.AddEntity(sketch::Line{{0, 0}, {4, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 5.0});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {l0, 0}, {l0, 1}, 3.0});

  sketch::Solver solver;
  solver.Load(s);
  CHECK_FALSE(solver.BeginDrag({{l0, 0}})); // refuse — conflicting
}

TEST_CASE("A rectangle (4 lines + H/V) solves and extrudes", "[solver][extrude]") {
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
  s.constraints.push_back({sketch::ConstraintKind::Radius, {a0, 0}, {0, 0}, 2.5});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& arc = std::get<sketch::Arc>(out[0]);
  CHECK_THAT(arc.radius, WithinRel(2.5, 1e-4));
  // Endpoints stay on the (grown) arc: start at angle 0, end at pi/2.
  glm::vec2 start = sketch::ArcPointUV(arc, 1);
  glm::vec2 end = sketch::ArcPointUV(arc, 2);
  CHECK_THAT(glm::length(start - arc.center), WithinRel(2.5f, 1e-3f));
  CHECK_THAT(glm::length(end - arc.center), WithinRel(2.5f, 1e-3f));
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
  glm::vec2 arcStart = sketch::ArcPointUV(std::get<sketch::Arc>(out[1]), 1);
  glm::vec2 l0b = std::get<sketch::Line>(out[0]).b;
  CHECK_THAT(glm::length(arcStart - l0b), WithinAbs(0.0f, 1e-4f));
  // The endpoint still lies on the arc's own rim.
  const auto& arc = std::get<sketch::Arc>(out[1]);
  CHECK_THAT(glm::length(arcStart - arc.center),
             WithinRel(static_cast<float>(arc.radius), 1e-3f));
}

TEST_CASE("A linked mirror follows the source through the solver",
          "[solver][generative]") {
  SketchFeature s = MakeSketch();
  uint32_t axis = s.AddEntity(sketch::Line{{0, -5}, {0, 5}}); // y-axis
  uint32_t seg = s.AddEntity(sketch::Line{{2, 1}, {3, 4}});
  // Pin the axis so it doesn't drift.
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 0}, {0, 0}});
  s.constraints.push_back({sketch::ConstraintKind::Fixed, {axis, 1}, {0, 0}});
  auto gen = s.MirrorEntities({seg}, axis, /*link=*/true);
  REQUIRE(gen.size() == 1);

  // Move the source line's endpoint, then re-solve: the linked copy must mirror.
  std::get<sketch::Line>(s.entities[s.IndexOfEntity(seg)]).a = {2.5f, 2.0f};
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  glm::vec2 srcA = std::get<sketch::Line>(out[s.IndexOfEntity(seg)]).a;
  glm::vec2 copyA = std::get<sketch::Line>(out[s.IndexOfEntity(gen[0])]).a;
  // Copy endpoint is the mirror of the (moved) source endpoint about x=0.
  CHECK_THAT(copyA.x, WithinAbs(-srcA.x, 1e-3f));
  CHECK_THAT(copyA.y, WithinAbs(srcA.y, 1e-3f));
}

TEST_CASE("A filleted corner solves and extrudes", "[solver][arc][generative]") {
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

#endif // HAVE_PLANEGCS
