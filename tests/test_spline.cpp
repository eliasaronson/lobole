#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"

#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;
using sketch::BSpline;

// --- Model + de Boor evaluator ---------------------------------------------

TEST_CASE("A clamped B-spline interpolates its first and last poles",
          "[spline]") {
  BSpline s;
  s.poles = {{0, 0}, {1, 2}, {3, 2}, {4, 0}};
  const glm::vec2 first = sketch::BSplineEvalUV(s, 0.0);
  const glm::vec2 last = sketch::BSplineEvalUV(s, 1e9); // clamps to the end
  CHECK_THAT(first.x, WithinAbs(0.0f, 1e-5));
  CHECK_THAT(first.y, WithinAbs(0.0f, 1e-5));
  CHECK_THAT(last.x, WithinAbs(4.0f, 1e-5));
  CHECK_THAT(last.y, WithinAbs(0.0f, 1e-5));
}

TEST_CASE("A B-spline stays within the convex hull of its poles", "[spline]") {
  BSpline s;
  s.poles = {{0, 0}, {1, 3}, {3, 3}, {4, 0}};
  for (auto& p : sketch::BSplineSampleUV(s, 6)) {
    CHECK(p.x >= -1e-4f);
    CHECK(p.x <= 4.0f + 1e-4f);
    CHECK(p.y >= -1e-4f);
    CHECK(p.y <= 3.0f + 1e-4f);
  }
}

TEST_CASE("A two-pole B-spline degrades to the segment between them",
          "[spline]") {
  BSpline s;
  s.poles = {{0, 0}, {2, 4}};
  CHECK(sketch::BSplineDegree(s) == 1);
  const glm::vec2 mid =
      sketch::BSplineEvalUV(s, 0.5 * (2 - 1)); // umax = n-p = 1
  CHECK_THAT(mid.x, WithinAbs(1.0f, 1e-5));
  CHECK_THAT(mid.y, WithinAbs(2.0f, 1e-5));
}

TEST_CASE("EntityPointUV addresses pole k of a spline", "[spline]") {
  sketch::Entity e = BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}};
  CHECK(sketch::EntityPointUV(e, 0) == glm::vec2(0, 0));
  CHECK(sketch::EntityPointUV(e, 2) == glm::vec2(3, 2));
}

TEST_CASE("Translate/rotate/reflect move every pole", "[spline]") {
  sketch::Entity e = BSpline{{{0, 0}, {1, 2}, {4, 0}}};
  sketch::Entity t = sketch::TranslateEntity(e, {10, 5});
  auto& tp = std::get<BSpline>(t).poles;
  CHECK(tp[0] == glm::vec2(10, 5));
  CHECK(tp[2] == glm::vec2(14, 5));
  // Reflect across the X axis (y -> -y).
  sketch::Entity r = sketch::ReflectEntity(e, {0, 0}, {1, 0});
  auto& rp = std::get<BSpline>(r).poles;
  CHECK_THAT(rp[1].y, WithinAbs(-2.0f, 1e-5));
}

// --- Serialization ----------------------------------------------------------

TEST_CASE("A sketch B-spline round-trips through JSON", "[spline]") {
  const fs::path path = fs::temp_directory_path() / "designer_spline_test.json";
  {
    Document doc;
    auto sk = std::make_unique<SketchFeature>("Sk", SketchPlane(PlaneType::XY));
    sk->AddEntity(BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}, 3});
    doc.AddFeature(std::move(sk));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* sk = dynamic_cast<SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(sk != nullptr);
  REQUIRE(sk->entities.size() == 1);
  auto* bs = std::get_if<BSpline>(&sk->entities[0]);
  REQUIRE(bs != nullptr);
  REQUIRE(bs->poles.size() == 4);
  CHECK(bs->degree == 3);
  CHECK(bs->poles[3] == glm::vec2(4, 0));
  CHECK(sketch::EntityId(sk->entities[0]) != 0); // id assigned on load
  fs::remove(path);
}

// --- Profile / extrude ------------------------------------------------------

static const SketchPlane kXY(PlaneType::XY);

TEST_CASE("A self-closed spline forms one profile loop", "[spline][profile]") {
  // A closed cubic loop: first pole == last pole.
  BSpline s;
  s.poles = {{2, 0}, {2, 2}, {-2, 2}, {-2, -2}, {2, -2}, {2, 0}};
  auto loops = sketch::BuildLoops({s});
  REQUIRE(loops.size() == 1);
  CHECK(std::abs(loops[0].SignedArea()) > 1.0); // encloses real area
}

TEST_CASE("A closed spline extrudes to a solid whose volume matches its area",
          "[spline][extrude]") {
  BSpline s;
  s.poles = {{2, 0}, {2, 2}, {-2, 2}, {-2, -2}, {2, -2}, {2, 0}};
  auto loops = sketch::BuildLoops({s});
  REQUIRE(loops.size() == 1);
  const double area = std::abs(loops[0].SignedArea());
  const double height = 3.0;
  CADShape prism = sketch::BuildPrism(kXY, loops, height, false);
  REQUIRE(prism.IsValid());
  // The OCC B-spline solid and the de Boor tessellation agree (watertight):
  // volume == area * height to tessellation tolerance.
  CHECK_THAT(prism.Volume(), WithinRel(area * height, 1e-2));
}

TEST_CASE("A spline closed by a line forms one loop and extrudes",
          "[spline][extrude]") {
  // A "D": an open spline bulge from (0,0) to (4,0), closed by a straight line.
  BSpline s;
  s.poles = {{0, 0}, {1, 3}, {3, 3}, {4, 0}};
  std::vector<sketch::Entity> e = {s, sketch::Line{{4, 0}, {0, 0}}};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  const double area = std::abs(loops[0].SignedArea());
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(area * 2.0, 1e-2));
}

// --- Solver (rigid net: poles are movable solver points) --------------------

#ifdef HAVE_PLANEGCS

TEST_CASE("A B-spline pole is a solver point (Coincident moves it)",
          "[spline][solver]") {
  SketchFeature s("S", SketchPlane(PlaneType::XY));
  const uint32_t id = s.AddEntity(BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
  // Pin pole 2 to the (fixed) sketch origin: it must move, others must not.
  s.constraints.push_back(
      {sketch::ConstraintKind::Coincident, {id, 2}, {sketch::kOrigin, 0}});

  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);

  auto& bs = std::get<BSpline>(out[0]);
  CHECK_THAT(bs.poles[2].x, WithinAbs(0.0f, 1e-4));
  CHECK_THAT(bs.poles[2].y, WithinAbs(0.0f, 1e-4));
  CHECK(bs.poles[0] == glm::vec2(0, 0)); // untouched
  CHECK(bs.poles[3] == glm::vec2(4, 0)); // untouched
}

TEST_CASE("An unconstrained B-spline solves without moving",
          "[spline][solver]") {
  SketchFeature s("S", SketchPlane(PlaneType::XY));
  s.AddEntity(BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  auto& bs = std::get<BSpline>(out[0]);
  CHECK(bs.poles[1] == glm::vec2(1, 2));
}

#endif // HAVE_PLANEGCS
