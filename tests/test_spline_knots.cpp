// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A sketch spline that carries its own knots
// (docs/sketching/README.md#representation): evaluated, sampled, built into
// OpenCASCADE edges and reversed on those knots, checked against
// OpenCASCADE's own evaluation of the same knots, never against itself.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSpline.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <Geom2dAPI_ProjectPointOnCurve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using sketch::BSpline;

namespace {

// OpenCASCADE's curve on the same poles and knots: the independent
// evaluation every check here is made against.
Handle(Geom2d_BSplineCurve) Occ(const BSpline& s) {
  const std::vector<double> U = sketch::SplineKnots(s);
  std::vector<double> distinct;
  std::vector<int> mult;
  for (double k : U) {
    if (distinct.empty() || distinct.back() != k) {
      distinct.push_back(k);
      mult.push_back(0);
    }
    mult.back()++;
  }
  TColgp_Array1OfPnt2d poles(1, static_cast<int>(s.poles.size()));
  for (size_t i = 0; i < s.poles.size(); i++)
    poles.SetValue(static_cast<int>(i) + 1,
                   gp_Pnt2d(s.poles[i].x, s.poles[i].y));
  TColStd_Array1OfReal knots(1, static_cast<int>(distinct.size()));
  TColStd_Array1OfInteger mults(1, static_cast<int>(distinct.size()));
  for (size_t i = 0; i < distinct.size(); i++) {
    knots.SetValue(static_cast<int>(i) + 1, distinct[i]);
    mults.SetValue(static_cast<int>(i) + 1, mult[i]);
  }
  return new Geom2d_BSplineCurve(poles, knots, mults, sketch::BSplineDegree(s));
}

// How far `p` is from the curve: projected span by span, since a
// projection across a C0 knot (a polyline's corner) can miss its foot.
double OffOcc(const Handle(Geom2d_BSplineCurve) & c, glm::dvec2 p) {
  const gp_Pnt2d q(p.x, p.y);
  double best = std::min(q.Distance(c->Value(c->FirstParameter())),
                         q.Distance(c->Value(c->LastParameter())));
  for (int i = 1; i < c->NbKnots(); i++) {
    const double a = c->Knot(i), b = c->Knot(i + 1);
    best = std::min(best, q.Distance(c->Value(b)));
    Geom2dAPI_ProjectPointOnCurve proj(q, c, a, b);
    if (proj.NbPoints() > 0)
      best = std::min(best, proj.LowerDistance());
  }
  return best;
}

// The test splines: degree 1-3, uniform or not, open or closed.
struct Case {
  std::string name;
  BSpline s;
};

std::vector<Case> Cases() {
  std::vector<Case> out;
  const std::vector<glm::dvec2> open = {{0, 0},  {3, 5},  {7, 6}, {10, 1},
                                        {14, 4}, {17, 9}, {21, 2}};
  const std::vector<glm::dvec2> closed = {{0, 0},  {8, -3}, {14, 2}, {12, 9},
                                          {5, 11}, {-2, 6}, {0, 0}};
  for (int p = 1; p <= 3; p++)
    for (const bool isClosed : {false, true}) {
      const auto& poles = isClosed ? closed : open;
      const int n = static_cast<int>(poles.size());
      BSpline uniform{poles, p};
      out.push_back({"degree " + std::to_string(p) + " uniform" +
                         (isClosed ? " closed" : " open"),
                     uniform});
      // Non-uniform and not starting at 0: p+1 equal ends, uneven interior
      // knots, one of them repeated where the degree allows.
      BSpline uneven{poles, p};
      uneven.knots.assign(static_cast<size_t>(p + 1), 1.5);
      const std::vector<double> inner =
          p == 1   ? std::vector<double>{1.9, 2.0, 3.7, 4.1, 6.0}
          : p == 2 ? std::vector<double>{1.9, 3.7, 3.7, 4.1}
                   : std::vector<double>{1.9, 3.7, 3.7};
      REQUIRE(inner.size() == static_cast<size_t>(n - p - 1));
      uneven.knots.insert(uneven.knots.end(), inner.begin(), inner.end());
      uneven.knots.insert(uneven.knots.end(), static_cast<size_t>(p + 1), 7.25);
      REQUIRE(sketch::SplineKnotsRefusal(uneven).empty());
      out.push_back({"degree " + std::to_string(p) + " non-uniform" +
                         (isClosed ? " closed" : " open"),
                     uneven});
    }
  return out;
}

// The edges of `shape` that are B-splines, sampled along their parameter.
std::vector<glm::dvec2> SplineEdgePoints(const TopoDS_Shape& shape) {
  std::vector<glm::dvec2> out;
  for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    BRepAdaptor_Curve c(e);
    if (c.GetType() != GeomAbs_BSplineCurve)
      continue;
    for (int i = 0; i <= 200; i++) {
      const gp_Pnt p =
          c.Value(c.FirstParameter() +
                  (c.LastParameter() - c.FirstParameter()) * i / 200.0);
      if (std::abs(p.Z()) < 1e-9) // the base cap's edges
        out.push_back({p.X(), p.Y()});
    }
  }
  return out;
}

} // namespace

TEST_CASE("A spline with stored knots evaluates and samples on the curve "
          "OpenCASCADE builds from the same knots",
          "[sketch][spline][knots]") {
  for (const Case& c : Cases()) {
    INFO(c.name);
    const Handle(Geom2d_BSplineCurve) occ = Occ(c.s);
    const auto [lo, hi] = sketch::SplineDomain(c.s);
    CHECK(lo == occ->FirstParameter());
    CHECK(hi == occ->LastParameter());
    for (int i = 0; i <= 400; i++) {
      const double u = lo + (hi - lo) * i / 400.0;
      const gp_Pnt2d want = occ->Value(u);
      const glm::dvec2 got = sketch::BSplineEvalUV(c.s, u);
      CHECK(glm::length(got - glm::dvec2(want.X(), want.Y())) < 1e-9);
      // The hodographs too: the fillet's Newton and the nearest point
      // read them.
      if (sketch::BSplineDegree(c.s) >= 1 && i > 0 && i < 400) {
        gp_Pnt2d p;
        gp_Vec2d d1, d2;
        occ->D2(u, p, d1, d2);
        const glm::dvec2 g1 = sketch::BSplineDerivUV(c.s, u);
        CHECK(glm::length(g1 - glm::dvec2(d1.X(), d1.Y())) <
              1e-9 * (1.0 + d1.Magnitude()));
        if (sketch::BSplineDegree(c.s) >= 2) {
          const glm::dvec2 g2 = sketch::BSplineDeriv2UV(c.s, u);
          CHECK(glm::length(g2 - glm::dvec2(d2.X(), d2.Y())) <
                1e-9 * (1.0 + d2.Magnitude()));
        }
      }
    }
    for (const glm::dvec2& q : sketch::BSplineSampleUV(c.s, 8))
      CHECK(OffOcc(occ, q) < 1e-9);
    // The nearest parameter finds a point's own parameter.
    for (const double t : {0.13, 0.5, 0.77}) {
      const double u = lo + (hi - lo) * t;
      const glm::dvec2 q = sketch::BSplineEvalUV(c.s, u);
      const double back = sketch::SplineParamNearest(c.s, q);
      CHECK(glm::length(sketch::BSplineEvalUV(c.s, back) - q) < 1e-9);
    }
  }
}

TEST_CASE("A spline with stored knots builds its profile edge on its own "
          "curve, reversed too",
          "[sketch][spline][knots]") {
  const SketchPlane plane(PlaneType::XY);
  for (const Case& c : Cases()) {
    INFO(c.name);
    const Handle(Geom2d_BSplineCurve) occ = Occ(c.s);
    const bool closed = sketch::SplineClosed(c.s);
    for (const bool backwards : {false, true}) {
      INFO((backwards ? "traversed backwards" : "traversed forwards"));
      // An open spline closed by a line; listing the line first and
      // running it from the spline's START makes the walk take the spline
      // from its end, reversed.
      std::vector<sketch::Entity> entities;
      BSpline s = c.s;
      s.id = 1;
      if (!closed) {
        sketch::Line close{backwards ? s.poles.back() : s.poles.front(),
                           backwards ? s.poles.front() : s.poles.back()};
        close.id = 2;
        if (backwards) {
          close = sketch::Line{s.poles.front(), s.poles.back()};
          close.id = 2;
          entities.push_back(close);
          entities.push_back(s);
        } else {
          entities.push_back(s);
          entities.push_back(close);
        }
      } else {
        if (backwards)
          sketch::ReverseSpline(s); // the same curve, read the other way
        entities.push_back(s);
      }
      const std::vector<sketch::Loop> loops = sketch::BuildLoops(entities);
      REQUIRE(loops.size() == 1);
      for (const sketch::LoopEdge& le : loops[0].edges)
        if (le.spline)
          for (const glm::dvec2& q : sketch::BSplineSampleUV(*le.spline, 8))
            CHECK(OffOcc(occ, q) < 1e-9);
      const CADShape prism = sketch::BuildPrism(plane, loops, 2.0, false);
      REQUIRE(prism.IsValid());
      const std::vector<glm::dvec2> pts = SplineEdgePoints(prism.GetShape());
      REQUIRE(pts.size() > 100);
      for (const glm::dvec2& q : pts)
        CHECK(OffOcc(occ, q) < 1e-9);
      if (closed)
        continue;
      // The same spline as an open path, the line leading into its end.
      std::vector<sketch::Entity> path;
      sketch::Line lead{s.poles.back() + glm::dvec2(0, -5), s.poles.back()};
      lead.id = 3;
      path.push_back(lead);
      path.push_back(s);
      const TopoDS_Wire wire = sketch::BuildOpenPathWire(plane, path);
      REQUIRE(!wire.IsNull());
      const std::vector<glm::dvec2> wpts = SplineEdgePoints(wire);
      REQUIRE(wpts.size() > 100);
      for (const glm::dvec2& q : wpts)
        CHECK(OffOcc(occ, q) < 1e-9);
    }
  }
}

TEST_CASE("ReverseSpline is the same curve read the other way",
          "[sketch][spline][knots]") {
  for (const Case& c : Cases()) {
    INFO(c.name);
    BSpline r = c.s;
    sketch::ReverseSpline(r);
    const auto [lo, hi] = sketch::SplineDomain(c.s);
    const auto [rlo, rhi] = sketch::SplineDomain(r);
    CHECK(rlo == lo);
    CHECK(rhi == hi);
    for (int i = 0; i <= 50; i++) {
      const double u = lo + (hi - lo) * i / 50.0;
      CHECK(glm::length(sketch::BSplineEvalUV(r, lo + hi - u) -
                        sketch::BSplineEvalUV(c.s, u)) < 1e-9);
    }
  }
}

TEST_CASE("Stored knots round-trip; clamped-uniform ones are not written",
          "[sketch][spline][knots][io]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  BSpline uneven{{{0, 0}, {2, 3}, {5, 4}, {8, 0}, {11, 2}}, 3};
  uneven.knots = {0, 0, 0, 0, 0.25, 2, 2, 2, 2};
  const uint32_t a = sk.AddEntity(uneven);
  BSpline plain{{{0, 5}, {2, 8}, {5, 9}, {8, 5}}, 3};
  plain.knots = {0, 0, 0, 0, 1, 1, 1, 1}; // the default, spelled out
  const uint32_t b = sk.AddEntity(plain);
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  const nlohmann::json* ja = nullptr;
  const nlohmann::json* jb = nullptr;
  for (const auto& e : j.at("entities")) {
    if (e.at("id") == a)
      ja = &e;
    if (e.at("id") == b)
      jb = &e;
  }
  REQUIRE(ja);
  REQUIRE(jb);
  CHECK(ja->at("knots") == nlohmann::json(uneven.knots));
  CHECK_FALSE(jb->contains("knots"));
  const auto back = DocumentIO::FeatureFromJson(j);
  const auto* sk2 = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(sk2);
  const auto& got = std::get<BSpline>(sk2->entities[sk2->IndexOfEntity(a)]);
  CHECK(got.knots == uneven.knots);
  CHECK(got.poles == uneven.poles);
  CHECK(std::get<BSpline>(sk2->entities[sk2->IndexOfEntity(b)]).knots.empty());
  CHECK(DocumentIO::FeatureToJson(*sk2) == j);
}

TEST_CASE("Malformed spline knots are refused by name",
          "[sketch][spline][knots][io]") {
  const auto load = [](nlohmann::json knots, int degree = 3,
                       int poles = 5) -> std::string {
    nlohmann::json ps = nlohmann::json::array();
    for (int i = 0; i < poles; i++)
      ps.push_back({2.0 * i, i % 2});
    const nlohmann::json f = {
        {"type", "Sketch"},
        {"name", "S"},
        {"id", 1},
        {"plane",
         {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}, {"uAxis", {1, 0, 0}}}},
        {"entities",
         {{{"type", "spline"},
           {"id", 1},
           {"degree", degree},
           {"poles", ps},
           {"knots", knots}}}}};
    try {
      (void)DocumentIO::FeatureFromJson(f);
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  };
  CHECK(load({0, 0, 0, 0, 0.5, 2, 2, 2, 2}).empty());
  CHECK_THAT(load({0, 0, 0, 0, 2, 2, 2, 2}),
             ContainsSubstring("5 poles and degree 3 takes 9 knots, not 8"));
  CHECK_THAT(load({0, 0, 0, 0, 1.5, 1, 2, 2, 2}),
             ContainsSubstring("must not decrease"));
  CHECK_THAT(load({0, 0, 0, 0.1, 0.5, 2, 2, 2, 2}),
             ContainsSubstring("must be clamped"));
  CHECK_THAT(load({0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, 3, 8),
             ContainsSubstring("repeated 4 times, more than its degree (3)"));
  CHECK_THAT(load({0, 0, 0, 0, 0, 0, 0, 0, 0}),
             ContainsSubstring("span nothing"));
  CHECK_THAT(load({0, 0, 0, 0, "x", 2, 2, 2, 2}),
             ContainsSubstring("array of numbers"));
  // The knot count follows the EFFECTIVE degree: a cubic of three poles is
  // a quadratic, which takes six knots.
  CHECK_THAT(load({0, 0, 0, 0, 1, 1, 1}, 3, 3),
             ContainsSubstring("3 poles and degree 2 takes 6 knots, not 7"));
  CHECK(load({0, 0, 0, 1, 1, 1}, 3, 3).empty());
}

TEST_CASE("A polyline's corners are full-multiplicity interior knots, and "
          "OpenCASCADE extrudes and measures them as the sampler draws them",
          "[sketch][spline][knots]") {
  // Degree 1: every interior knot has multiplicity 1, the degree -- the
  // curve is C0 at each. And a cubic with a knot of multiplicity 3: a
  // corner in a smooth curve. Each closed on itself and extruded; the
  // volume is the area the sampled polyline encloses times the height.
  const SketchPlane plane(PlaneType::XY);
  BSpline poly{{{0, 0}, {12, 0}, {12, 7}, {5, 10}, {0, 7}, {0, 0}}, 1};
  BSpline corner{{{0, 0},
                  {6, -3},
                  {12, 0},
                  {14, 6},
                  {12, 12},
                  {6, 12},
                  {1, 9},
                  {-2, 4},
                  {0, 0}},
                 3};
  corner.knots = {0, 0, 0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4};
  REQUIRE(sketch::SplineKnotsRefusal(corner).empty());
  for (const BSpline& s : {poly, corner}) {
    INFO("degree " << s.degree);
    BSpline e = s;
    e.id = 1;
    const auto loops = sketch::BuildLoops({e});
    REQUIRE(loops.size() == 1);
    const CADShape prism = sketch::BuildPrism(plane, loops, 3.0, false);
    REQUIRE(prism.IsValid());
    // The enclosed area by Green's theorem over a fine sample.
    const std::vector<glm::dvec2> pts = sketch::BSplineSampleUV(s, 4000);
    double area = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); i++)
      area += pts[i].x * pts[i + 1].y - pts[i + 1].x * pts[i].y;
    area = 0.5 * std::abs(area);
    CADShape::MassProps mp;
    REQUIRE(prism.ComputeMassProps(mp));
    CHECK_THAT(mp.volume, WithinRel(3.0 * area, 1e-7));
    CHECK_THAT(prism.Volume(), WithinRel(3.0 * area, 1e-7));
    CHECK_THAT(mp.centerOfMass.z, WithinAbs(1.5, 1e-7));
    const Handle(Geom2d_BSplineCurve) occ = Occ(s);
    for (const glm::dvec2& q : SplineEdgePoints(prism.GetShape()))
      CHECK(OffOcc(occ, q) < 1e-9);
  }
}
