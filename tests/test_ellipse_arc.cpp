#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Document.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "TestIO.h"
#include "core/sketch/SketchProfile.h"

#include <cmath>
#include <filesystem>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;
using sketch::EllipseArc;

static const SketchPlane kXY(PlaneType::XY);

// --- Model ------------------------------------------------------------------

TEST_CASE("EllipseArc endpoints evaluate at its params", "[ellipse_arc]") {
  EllipseArc e{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI}; // upper half, +X to -X
  CHECK_THAT(sketch::EllipseArcPointUV(e, 1).x, WithinAbs(2.0f, 1e-5)); // start
  CHECK_THAT(sketch::EllipseArcPointUV(e, 1).y, WithinAbs(0.0f, 1e-5));
  CHECK_THAT(sketch::EllipseArcPointUV(e, 2).x, WithinAbs(-2.0f, 1e-5)); // end
  CHECK_THAT(sketch::EllipseArcPointUV(e, 2).y, WithinAbs(0.0f, 1e-5));
  // Mid-sweep point (param pi/2) sits on the minor axis at (0, +1).
  const glm::vec2 mid = sketch::EllipseArcEvalUV(e, M_PI / 2);
  CHECK_THAT(mid.x, WithinAbs(0.0f, 1e-5));
  CHECK_THAT(mid.y, WithinAbs(1.0f, 1e-5));
}

TEST_CASE("EllipseParamOfPoint inverts EllipseArcEvalUV", "[ellipse_arc]") {
  EllipseArc e{{1, 2}, 3.0, 1.5, 0.7, 0.0, M_PI};
  for (double t : {0.1, 1.0, 2.5}) {
    const glm::vec2 p = sketch::EllipseArcEvalUV(e, t);
    const double got =
        sketch::EllipseParamOfPoint(e.center, e.rMajor, e.rMinor, e.rotation, p);
    CHECK_THAT(got, WithinAbs(t, 1e-5));
  }
}

TEST_CASE("Translate/rotate keep the sweep; reflect flips winding",
          "[ellipse_arc]") {
  sketch::Entity e = EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI};
  sketch::Entity te = sketch::TranslateEntity(e, {5, 5});
  const auto& t = std::get<EllipseArc>(te);
  CHECK(t.center == glm::vec2(5, 5));
  CHECK_THAT(t.startParam, WithinAbs(0.0, 1e-9)); // params unchanged
  // Reflect across the X axis: the upper half becomes the lower half; the
  // reflected start/end points swap (winding flips).
  sketch::Entity r = sketch::ReflectEntity(e, {0, 0}, {1, 0});
  auto& ra = std::get<EllipseArc>(r);
  const glm::vec2 mid = sketch::EllipseArcEvalUV(
      ra, ra.startParam + 0.5 * (ra.endParam - ra.startParam));
  CHECK_THAT(mid.y, WithinAbs(-1.0f, 1e-4)); // now bulges downward
}

// --- Serialization ----------------------------------------------------------

TEST_CASE("An elliptical arc round-trips through JSON", "[ellipse_arc]") {
  const fs::path path = fs::temp_directory_path() / "designer_ellarc_test.json";
  {
    Document doc;
    auto sk = std::make_unique<SketchFeature>("Sk", kXY);
    sk->AddEntity(EllipseArc{{1, 2}, 3.0, 1.5, 0.4, 0.2, 2.0});
    doc.AddFeature(std::move(sk));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* sk = dynamic_cast<SketchFeature*>(loaded->GetFeature(0));
  REQUIRE(sk != nullptr);
  auto* ea = std::get_if<EllipseArc>(&sk->entities.at(0));
  REQUIRE(ea != nullptr);
  CHECK_THAT(ea->rMajor, WithinRel(3.0, 1e-9));
  CHECK_THAT(ea->rMinor, WithinRel(1.5, 1e-9));
  CHECK_THAT(ea->rotation, WithinRel(0.4, 1e-9));
  CHECK_THAT(ea->startParam, WithinRel(0.2, 1e-9));
  CHECK_THAT(ea->endParam, WithinRel(2.0, 1e-9));
  fs::remove(path);
}

// --- Profile / extrude ------------------------------------------------------

TEST_CASE("A half-ellipse arc closed by a chord forms one loop and extrudes",
          "[ellipse_arc][extrude]") {
  // Upper half of a 2x1 ellipse, closed by the chord along the major axis.
  EllipseArc arc{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI};
  std::vector<sketch::Entity> e = {arc, sketch::Line{{-2, 0}, {2, 0}}};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  const double area = std::abs(loops[0].SignedArea());
  CHECK_THAT(area, WithinRel(M_PI * 2.0 * 1.0 / 2.0, 1e-2)); // pi*a*b/2

  CADShape prism = sketch::BuildPrism(kXY, loops, 3.0, false);
  REQUIRE(prism.IsValid());
  // OCC's exact elliptical face vs the tessellated area, x height.
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * 2.0 * 1.0 / 2.0 * 3.0, 1e-2));
}

TEST_CASE("A rotated elliptical-arc profile extrudes consistently",
          "[ellipse_arc][extrude]") {
  // Same half-ellipse rotated 90 deg: the chord is now vertical.
  const double rot = M_PI / 2;
  EllipseArc arc{{0, 0}, 2.0, 1.0, rot, 0.0, M_PI};
  const glm::vec2 s = sketch::EllipseArcPointUV(arc, 1);
  const glm::vec2 en = sketch::EllipseArcPointUV(arc, 2);
  std::vector<sketch::Entity> e = {arc, sketch::Line{en, s}};
  auto loops = sketch::BuildLoops(e);
  REQUIRE(loops.size() == 1);
  CADShape prism = sketch::BuildPrism(kXY, loops, 2.0, false);
  REQUIRE(prism.IsValid());
  CHECK_THAT(prism.Volume(), WithinRel(M_PI * 2.0 * 1.0 / 2.0 * 2.0, 1e-2));
}
