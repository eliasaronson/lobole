// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Feature.h"
#include "core/MeasureSeal.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gp_Ax2.hxx>

#include <cmath>

using Catch::Approx;

namespace {

Body Make(const TopoDS_Shape& s, const char* name) {
  Body b;
  b.name = name;
  b.shape = CADShape(s);
  return b;
}

gp_Ax2 Up(double z) { return gp_Ax2(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1)); }

// A jar: a body r30 to z40, its shoulder the annulus r25-30 at z40, and a
// neck r25 to z50.
Body Jar() {
  TopoDS_Shape s = BRepPrimAPI_MakeCylinder(Up(0), 30.0, 40.0).Shape();
  s = BRepAlgoAPI_Fuse(s, BRepPrimAPI_MakeCylinder(Up(0), 25.0, 50.0).Shape())
          .Shape();
  return Make(s, "jar");
}

// A lid: a skirt r25.5-30 from the shoulder up, closed on top. With `notch`
// a slot 3 wide and 1 high is cut through the skirt's foot on +X.
Body Lid(bool notch) {
  TopoDS_Shape s = BRepPrimAPI_MakeCylinder(Up(40), 30.0, 17.0).Shape();
  s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeCylinder(Up(39), 25.5, 16.0).Shape())
          .Shape();
  if (notch)
    s = BRepAlgoAPI_Cut(
            s, BRepPrimAPI_MakeBox(gp_Pnt(20, -1.5, 39), 15, 3, 2).Shape())
            .Shape();
  return Make(s, notch ? "notched lid" : "lid");
}

} // namespace

TEST_CASE("A skirt flat on a shoulder is a ring as wide as it lands",
          "[inspect][seal]") {
  const auto r =
      Measure::Seal({Jar()}, glm::dmat4(1.0), {Lid(false)}, glm::dmat4(1.0));
  REQUIRE(r.has_value());
  CHECK(r->axisDir.z == Approx(1.0).margin(1e-9));
  CHECK(std::hypot(r->axisPoint.x, r->axisPoint.y) < 1e-6);
  CHECK(r->contactAreaMm2 == Approx(M_PI * (30.0 * 30.0 - 25.5 * 25.5)));
  CHECK(r->overlapVolumeMm3 == 0.0);
  REQUIRE(r->rings.size() == 1);
  CHECK(r->rings[0].leastWidthMm == Approx(4.5).margin(1e-6));
  CHECK(r->rings[0].leastInterferenceMm == 0.0);
  CHECK(r->rings[0].centre.z == Approx(40.0).margin(1e-6));
  CHECK(r->open.empty());
}

TEST_CASE("A notch in the skirt opens the ring where it is",
          "[inspect][seal]") {
  const auto r =
      Measure::Seal({Jar()}, glm::dmat4(1.0), {Lid(true)}, glm::dmat4(1.0));
  REQUIRE(r.has_value());
  CHECK(r->rings.empty());
  REQUIRE(r->open.size() == 1);
  // The slot is 3 wide at r25.5-30: about ±3.4° to ±2.9° about +X.
  CHECK(r->open[0].fromDeg < 0.0);
  CHECK(r->open[0].fromDeg > -4.0);
  CHECK(r->open[0].toDeg > 0.0);
  CHECK(r->open[0].toDeg < 4.0);
  CHECK_FALSE(r->roundButOpen);
}

TEST_CASE("A pressed gasket is a ring with its interference",
          "[inspect][seal]") {
  // An O-ring of section r1.5 on r27.5, its centre 1.2 over the shoulder:
  // it reaches 0.3 into the jar.
  const Body ring =
      Make(BRepPrimAPI_MakeTorus(Up(41.2), 27.5, 1.5).Shape(), "o-ring");
  const auto r =
      Measure::Seal({Jar()}, glm::dmat4(1.0), {ring}, glm::dmat4(1.0));
  REQUIRE(r.has_value());
  CHECK(r->overlapVolumeMm3 > 0.0);
  REQUIRE(r->rings.size() == 1);
  // A circle's chord 0.3 deep: 2 sqrt(1.5^2 - 1.2^2) = 1.8 wide.
  CHECK(r->rings[0].leastWidthMm == Approx(1.8).epsilon(0.01));
  // Its mean depth: the circle's segment over its chord, about 0.2.
  const double seg = 1.5 * 1.5 * std::acos(1.2 / 1.5) - 1.2 * 0.9;
  CHECK(r->rings[0].leastInterferenceMm == Approx(seg / 1.8).epsilon(0.03));
  CHECK(r->rings[0].radiusMm == Approx(27.5).margin(0.05));

  // Lifted clear, nothing touches, and the call says so.
  const Body lifted =
      Make(BRepPrimAPI_MakeTorus(Up(45.0), 27.5, 1.5).Shape(), "o-ring");
  const auto none =
      Measure::Seal({Jar()}, glm::dmat4(1.0), {lifted}, glm::dmat4(1.0));
  REQUIRE_FALSE(none.has_value());
  CHECK_THAT(none.error(), Catch::Matchers::ContainsSubstring("do not touch"));
}
