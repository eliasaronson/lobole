// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "TestPaths.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSection.h"
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
  const glm::dvec2 first = sketch::BSplineEvalUV(s, 0.0);
  const glm::dvec2 last = sketch::BSplineEvalUV(s, 1e9); // clamps to the end
  CHECK_THAT(first.x, WithinAbs(0.0, 1e-5));
  CHECK_THAT(first.y, WithinAbs(0.0, 1e-5));
  CHECK_THAT(last.x, WithinAbs(4.0, 1e-5));
  CHECK_THAT(last.y, WithinAbs(0.0, 1e-5));
}

TEST_CASE("A B-spline stays within the convex hull of its poles", "[spline]") {
  BSpline s;
  s.poles = {{0, 0}, {1, 3}, {3, 3}, {4, 0}};
  for (auto& p : sketch::BSplineSampleUV(s, 6)) {
    CHECK(p.x >= -1e-4);
    CHECK(p.x <= 4.0 + 1e-4);
    CHECK(p.y >= -1e-4);
    CHECK(p.y <= 3.0 + 1e-4);
  }
}

TEST_CASE("A two-pole B-spline degrades to the segment between them",
          "[spline]") {
  BSpline s;
  s.poles = {{0, 0}, {2, 4}};
  CHECK(sketch::BSplineDegree(s) == 1);
  const glm::dvec2 mid =
      sketch::BSplineEvalUV(s, 0.5 * (2 - 1)); // umax = n-p = 1
  CHECK_THAT(mid.x, WithinAbs(1.0, 1e-5));
  CHECK_THAT(mid.y, WithinAbs(2.0, 1e-5));
}

TEST_CASE("EntityPointUV addresses pole k of a spline", "[spline]") {
  sketch::Entity e = BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}};
  CHECK(sketch::EntityPointUV(e, 0) == glm::dvec2(0, 0));
  CHECK(sketch::EntityPointUV(e, 2) == glm::dvec2(3, 2));
}

TEST_CASE("Translate/rotate/reflect move every pole", "[spline]") {
  sketch::Entity e = BSpline{{{0, 0}, {1, 2}, {4, 0}}};
  sketch::Entity t = sketch::TranslateEntity(e, {10, 5});
  auto& tp = std::get<BSpline>(t).poles;
  CHECK(tp[0] == glm::dvec2(10, 5));
  CHECK(tp[2] == glm::dvec2(14, 5));
  // Reflect across the X axis (y -> -y).
  sketch::Entity r = sketch::ReflectEntity(e, {0, 0}, {1, 0});
  auto& rp = std::get<BSpline>(r).poles;
  CHECK_THAT(rp[1].y, WithinAbs(-2.0, 1e-5));
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
  CHECK(bs->poles[3] == glm::dvec2(4, 0));
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

TEST_CASE("A lone closed spline extrudes to its own area",
          "[spline][extrude]") {
  // One spline fitted round a 30 x 12 ellipse, its ends meeting: built as
  // one clamped edge closed on its own vertex, the face weighed 1113 mm3 a
  // millimetre tall where the ellipse is 1131 (and 1426 with an adaptive
  // tolerance), the curve itself right. Built as two edges split at its
  // middle parameter it weighs its area.
  std::vector<glm::dvec2> pts;
  for (int k = 0; k <= 6000; k++) {
    const double t = 2.0 * M_PI * k / 6000.0;
    pts.push_back({30.0 * std::cos(t), 12.0 * std::sin(t)});
  }
  const double tol = 0.002;
  const std::vector<BSpline> fit = sketch::FitSplines(pts, false, tol);
  REQUIRE(fit.size() == 1);
  REQUIRE(glm::length(fit[0].poles.front() - fit[0].poles.back()) < tol);
  const auto loops = sketch::BuildLoops({fit[0]});
  REQUIRE(loops.size() == 1);
  const CADShape prism = sketch::BuildPrism(kXY, loops, 1.0, false);
  REQUIRE(prism.IsValid());
  // To the fit's tolerance round its perimeter (about 134 mm).
  CHECK_THAT(prism.Volume(), WithinAbs(M_PI * 30.0 * 12.0, 134.0 * tol));
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
  CHECK_THAT(bs.poles[2].x, WithinAbs(0.0, 1e-4));
  CHECK_THAT(bs.poles[2].y, WithinAbs(0.0, 1e-4));
  CHECK(bs.poles[0] == glm::dvec2(0, 0)); // untouched
  CHECK(bs.poles[3] == glm::dvec2(4, 0)); // untouched
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
  CHECK(bs.poles[1] == glm::dvec2(1, 2));
}

#endif // HAVE_PLANEGCS

TEST_CASE("The v217 golden's closed spline is two edges and weighs its area",
          "[spline][golden]") {
  // tests/golden/v217-closed-spline-two-edges.json: one spline fitted
  // round a 30 x 12 ellipse, padded 4 mm. Its top outline is filleted here,
  // by the two U names the cap gives its two halves, and not in the golden:
  // every golden is the reader fuzz's input, and a fillet after a damaged
  // spline walks for minutes (docs/known-bugs.md).
  auto doc = testio::Load(std::string(TestSourceDir()) +
                          "/tests/golden/v217-closed-spline-two-edges.json");
  REQUIRE(doc != nullptr);
  INFO(doc->GetFeature(1)->GetError());
  CHECK(doc->GetFeature(1)->GetError().empty());
  const std::vector<Body>* pad = doc->CachedBodiesAfter(1);
  REQUIRE(pad != nullptr);
  CHECK_THAT(pad->at(0).shape.Volume(),
             WithinAbs(4.0 * M_PI * 30.0 * 12.0, 4.0 * 134.0 * 0.002));
  const uint64_t padId = doc->GetFeature(1)->GetId();
  const std::string cap = std::to_string(padId) + "/cap1";
  doc->AddFeature(std::make_unique<FilletFeature>(
      "Round",
      std::vector<ElementRef>{{padId, cap + ";U0"}, {padId, cap + ";U1"}},
      0.5));
  doc->Regenerate();
  INFO(doc->GetFeature(2)->GetError());
  CHECK(doc->GetFeature(2)->GetError().empty());
  CHECK_THAT(doc->GetBodies().at(0).shape.Volume(),
             WithinAbs(4516.701355, 1e-3));
}

TEST_CASE("A lone closed spline's pad shells", "[spline][extrude]") {
  // Split in two halves trimming ONE shared curve, the pad's two side faces
  // lay on one surface and every Shell of it came back unchanged: each half
  // is its own curve.
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Base", kXY);
  BSpline s;
  s.poles = {{2, 0}, {2, 2}, {-2, 2}, {-2, -2}, {2, -2}, {2, 0}};
  sk->AddEntity(s);
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  auto pad = std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                              ExtrudeMode::NewBody);
  doc.AddFeature(std::move(pad));
  const uint64_t padId = doc.GetFeature(1)->GetId();
  REQUIRE(doc.Regenerate());
  const double solid = doc.GetBodies().at(0).shape.Volume();
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Shell", padId,
      std::vector<ElementRef>{{padId, std::to_string(padId) + "/cap1"}}, 0.2));
  doc.Regenerate();
  INFO(doc.GetFeature(2)->GetError());
  CHECK(doc.GetFeature(2)->GetError().empty());
  CHECK(doc.GetBodies().at(0).shape.Volume() < 0.5 * solid);
}
