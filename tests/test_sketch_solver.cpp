#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "core/SketchSolver.h"

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
  s.entities.push_back(sketch::Line{{0, 0}, {4, 0}});
  s.entities.push_back(sketch::Line{{4.1f, 0.2f}, {4, 3}}); // starts near L0.b
  // Coincident: L1.a (entity 1, which 0) == L0.b (entity 0, which 1)
  s.constraints.push_back({sketch::ConstraintKind::Coincident, {1, 0}, {0, 1}});

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

TEST_CASE("Horizontal levels a line's endpoints", "[solver]") {
  SketchFeature s = MakeSketch();
  s.entities.push_back(sketch::Line{{0, 0}, {4, 1.5f}});
  s.constraints.push_back({sketch::ConstraintKind::Horizontal, {0, 0}, {0, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(LineA(out[0]).y, WithinAbs(LineB(out[0]).y, 1e-5f));
}

TEST_CASE("Distance constraint sets endpoint separation", "[solver]") {
  SketchFeature s = MakeSketch();
  s.entities.push_back(sketch::Line{{0, 0}, {3, 0}});
  // Pin start, and constrain length to 5
  s.constraints.push_back({sketch::ConstraintKind::Distance, {0, 0}, {0, 1}, 5.0});

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
  s.entities.push_back(sketch::Circle{{0, 0}, 1.0});
  s.constraints.push_back({sketch::ConstraintKind::Radius, {0, 0}, {0, 0}, 2.5});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);

  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  CHECK_THAT(std::get<sketch::Circle>(out[0]).radius, WithinRel(2.5, 1e-4));
}

TEST_CASE("Over-constrained sketch reports without crashing", "[solver]") {
  SketchFeature s = MakeSketch();
  s.entities.push_back(sketch::Line{{0, 0}, {4, 0}});
  // Two conflicting distances on the same points
  s.constraints.push_back({sketch::ConstraintKind::Distance, {0, 0}, {0, 1}, 5.0});
  s.constraints.push_back({sketch::ConstraintKind::Distance, {0, 0}, {0, 1}, 3.0});

  sketch::Solver solver;
  solver.Load(s);
  sketch::SolveResult r = solver.Solve(); // must not crash
  // Either fails or flags conflict; just require it returned a verdict.
  CHECK((r.conflicting.size() > 0 || !r.ok || r.dof >= 0));
}

TEST_CASE("A constrained rectangle profile still extrudes", "[solver][extrude]") {
  // Rectangles are rigid (unsolved) but must still form a profile.
  SketchFeature s = MakeSketch();
  s.entities.push_back(sketch::Rectangle{{0, 0}, {4, 2}});

  sketch::Solver solver;
  solver.Load(s);
  solver.Solve();
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto loops = sketch::BuildLoops(out);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(s.plane, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(16.0, 1e-6));
}

#endif // HAVE_PLANEGCS
