// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// cad::SampleCurve: a curve's chords stay within the deflection asked for,
// on the shape OCC 7.6's quasi-uniform sampler collapses to one chord.

#include <catch2/catch_test_macros.hpp>

#include "cad/CurveSample.h"

#include <GeomAPI_Interpolate.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <numbers>
#include <string>

namespace {

// A spline through points on a 10 mm circle that stops just short of a full
// turn, so it ends almost where it began, pointing the same way. On OCC 7.6.3
// (Ubuntu 24.04's) GCPnts_QuasiUniformDeflection returns its two ends at both
// deflections tested below: a 0.02 mm chord standing in for the circle, 20 mm
// off it at the middle. OCC 7.9 samples it properly; the outcome is asserted
// either way.
Handle(Geom_BSplineCurve) NearlyClosedSpline() {
  const int n = 13;
  Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n);
  for (int i = 0; i < n; i++) {
    const double t = 0.999 * 2.0 * std::numbers::pi * i / (n - 1);
    pts->SetValue(i + 1, gp_Pnt(10.0 * std::cos(t), 10.0 * std::sin(t), 0.0));
  }
  GeomAPI_Interpolate interp(pts, false, 1e-7);
  interp.Perform();
  REQUIRE(interp.IsDone());
  return interp.Curve();
}

} // namespace

TEST_CASE("a nearly closed spline is sampled within its deflection",
          "[kernel]") {
  const Handle(Geom_BSplineCurve) g = NearlyClosedSpline();
  const GeomAdaptor_Curve c(g);
  for (const double deflection : {0.01, 0.1}) {
    INFO("deflection " << deflection << " mm");
    const std::vector<cad::CurveSample> s = cad::SampleCurve(c, deflection);
    REQUIRE(s.size() > 2);
    CHECK(s.front().u == c.FirstParameter());
    CHECK(s.back().u == c.LastParameter());
    for (size_t i = 0; i + 1 < s.size(); i++) {
      INFO("chord " << i << " of " << s.size() - 1);
      REQUIRE(s[i].u < s[i + 1].u);
      CHECK(s[i].p.Distance(c.Value(s[i].u)) < 1e-9);
      // The curve's point halfway along the chord's span lies within the
      // deflection of the chord.
      const gp_Pnt m = c.Value(0.5 * (s[i].u + s[i + 1].u));
      const gp_Vec ab(s[i].p, s[i + 1].p), am(s[i].p, m);
      const double off = ab.Crossed(am).Magnitude() / ab.Magnitude();
      CHECK(off <= deflection * (1.0 + 1e-9));
    }
  }
}
