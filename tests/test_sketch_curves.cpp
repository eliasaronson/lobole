// Ellipses, elliptical arcs and splines as solved curves
// (docs/sketching/README.md#ellipses-and-splines). Every case checks the
// GEOMETRY the solve leaves -- a point on the curve, directions that agree at
// a joint, the volume a profile extrudes or revolves to -- never the solver's
// own numbers back.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using sketch::ConstraintKind;
using K = sketch::ConstraintKind;

#ifdef HAVE_PLANEGCS

namespace {

SketchFeature MakeSketch() {
  return SketchFeature("S", SketchPlane(PlaneType::XY));
}

sketch::Constraint Con(K kind, sketch::PointRef a, sketch::PointRef b = {},
                       double value = 0.0) {
  sketch::Constraint c;
  c.kind = kind;
  c.a = a;
  c.b = b;
  c.value = value;
  return c;
}

// Solves the sketch in place; returns the result.
sketch::SolveResult SolveInPlace(SketchFeature& s) {
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.UnsupportedConstraints().empty());
  const sketch::SolveResult r = solver.Solve();
  CHECK(r.conflictingConstraints.empty());
  if (r.ok)
    solver.WriteBack(s.entities);
  return r;
}

template <class T> const T& Get(const SketchFeature& s, uint32_t id) {
  return std::get<T>(s.entities.at(s.IndexOfEntity(id)));
}

// (x/a)^2 + (y/b)^2 of p in the ellipse's own frame: 1 on the curve.
double EllipseLevel(glm::dvec2 c, double a, double b, double rot,
                    glm::dvec2 p) {
  const glm::dvec2 d = p - c;
  const double x = std::cos(rot) * d.x + std::sin(rot) * d.y;
  const double y = -std::sin(rot) * d.x + std::cos(rot) * d.y;
  return (x / a) * (x / a) + (y / b) * (y / b);
}

// Distance from p to the spline: a dense sample, then a bracketing search.
double DistanceToSpline(const sketch::BSpline& s, glm::dvec2 p) {
  const int n = static_cast<int>(s.poles.size());
  const double umax = n - sketch::BSplineDegree(s);
  double best = 0.0, bestD = INFINITY;
  for (int i = 0; i <= 2000; i++) {
    const double u = umax * i / 2000.0;
    const double d = glm::length(sketch::BSplineEvalUV(s, u) - p);
    if (d < bestD)
      bestD = d, best = u;
  }
  double lo = std::max(0.0, best - umax / 2000.0);
  double hi = std::min(umax, best + umax / 2000.0);
  for (int it = 0; it < 80; it++) {
    const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
    if (glm::length(sketch::BSplineEvalUV(s, m1) - p) <
        glm::length(sketch::BSplineEvalUV(s, m2) - p))
      hi = m2;
    else
      lo = m1;
  }
  return glm::length(sketch::BSplineEvalUV(s, 0.5 * (lo + hi)) - p);
}

double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

// Holds an ellipse's centre fixed and its radii and rotation dimensioned:
// what a test does when the OTHER curve is meant to move.
void PinEllipse(SketchFeature& s, uint32_t el, double a, double b) {
  s.constraints.push_back(Con(K::Fixed, {el, 0}));
  s.constraints.push_back(Con(K::Horizontal, {el, 0}));
  s.constraints.push_back(
      Con(K::Distance, {el, 0}, {el, sketch::kEllipseMajorPos}, a));
  s.constraints.push_back(
      Con(K::Distance, {el, 0}, {el, sketch::kEllipseMinorPos}, b));
}

double PrismVolume(const SketchFeature& s, double h) {
  const auto loops = sketch::BuildLoops(s.entities);
  REQUIRE(loops.size() == 1);
  const CADShape prism = sketch::BuildPrism(s.plane, loops, h, false);
  REQUIRE(prism.IsValid());
  return prism.Volume();
}

} // namespace

TEST_CASE("Degrees of freedom: ellipse 5, elliptical arc 7, spline 2 per pole",
          "[solver][ellipse][spline]") {
  {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::Ellipse{{1, 2}, 3.0, 1.0, 0.4});
    CHECK(SolveInPlace(s).dof == 5);
  }
  {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::EllipseArc{{1, 2}, 3.0, 1.0, 0.4, 0.2, 2.5});
    CHECK(SolveInPlace(s).dof == 7);
  }
  {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
    CHECK(SolveInPlace(s).dof == 8);
  }
  {
    // Closed: the first pole is the last, so they move as one.
    SketchFeature s = MakeSketch();
    s.AddEntity(
        sketch::BSpline{{{2, 0}, {2, 2}, {-2, 2}, {-2, -2}, {2, -2}, {2, 0}}});
    CHECK(SolveInPlace(s).dof == 10);
  }
  {
    // Fully dimensioned: centre fixed, both radii and the axis level.
    SketchFeature s = MakeSketch();
    const uint32_t el = s.AddEntity(sketch::Ellipse{{1, 2}, 3.0, 1.0, 0.4});
    PinEllipse(s, el, 3.0, 1.0);
    const auto r = SolveInPlace(s);
    CHECK(r.ok);
    CHECK(r.dof == 0);
    CHECK(r.redundantConstraints.empty());
  }
}

TEST_CASE("A distance to an ellipse's major end drives its major radius",
          "[solver][ellipse][extrude]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{0, 0}, 3.0, 1.0, 0.0});
  PinEllipse(s, el, 5.0, 1.0);
  REQUIRE(SolveInPlace(s).ok);
  const auto& e = Get<sketch::Ellipse>(s, el);
  CHECK_THAT(e.rMajor, WithinAbs(5.0, 1e-7));
  CHECK_THAT(e.rMinor, WithinAbs(1.0, 1e-7));
  CHECK_THAT(std::sin(e.rotation), WithinAbs(0.0, 1e-7));
  // Through the profile: an extruded ellipse of the NEW size.
  CHECK_THAT(PrismVolume(s, 2.0), WithinRel(M_PI * 5.0 * 1.0 * 2.0, 1e-6));
}

TEST_CASE("Horizontal on a rotated ellipse levels its major axis",
          "[solver][ellipse]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{2, 1}, 3.0, 1.2, 0.4});
  s.constraints.push_back(Con(K::Horizontal, {el, 0}));
  REQUIRE(SolveInPlace(s).ok);
  const auto& e = Get<sketch::Ellipse>(s, el);
  const glm::dvec2 m3 = sketch::EntityPointUV(s.entities[0], 3);
  const glm::dvec2 m4 = sketch::EntityPointUV(s.entities[0], 4);
  CHECK_THAT(m3.y - m4.y, WithinAbs(0.0, 1e-7));
  CHECK_THAT(e.rMajor, WithinAbs(3.0, 1e-3)); // turned, not resized
  CHECK_THAT(e.rMinor, WithinAbs(1.2, 1e-3));
}

TEST_CASE("A line tangent to an ellipse touches it from its own side",
          "[solver][ellipse]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{0, 0}, 3.0, 1.5, 0.0});
  const uint32_t ln = s.AddEntity(sketch::Line{{-4, 2.5}, {4, 2.9}});
  PinEllipse(s, el, 3.0, 1.5);
  s.constraints.push_back(Con(K::Tangent, {ln, 0}, {el, 0}));
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  const auto& l = Get<sketch::Line>(s, ln);
  const glm::dvec2 dir = glm::normalize(l.b - l.a);
  double minSigned = INFINITY, maxSigned = -INFINITY;
  for (int i = 0; i < 4000; i++) {
    const double t = 2.0 * M_PI * i / 4000.0;
    const glm::dvec2 p = sketch::EllipsePointUV(Get<sketch::Ellipse>(s, el), t);
    const double d = Cross(dir, p - l.a);
    minSigned = std::min(minSigned, d);
    maxSigned = std::max(maxSigned, d);
  }
  // The whole ellipse on one side (below: the side its centre was on), and
  // touching the line.
  CHECK(maxSigned < 1e-6);
  CHECK_THAT(maxSigned, WithinAbs(0.0, 1e-5));
}

TEST_CASE("A point on an ellipse or an elliptical arc lands on the curve",
          "[solver][ellipse]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{1, 1}, 3.0, 1.0, 0.5});
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{10, 0}, 2.0, 1.0, -0.3, 0.0, 2.0});
  const uint32_t p = s.AddEntity(sketch::Point{{4.5, 3.0}});
  const uint32_t q = s.AddEntity(sketch::Point{{10.5, 2.0}});
  s.constraints.push_back(Con(K::PointOnLine, {p, 0}, {el, 0}));
  s.constraints.push_back(Con(K::PointOnLine, {q, 0}, {ea, 0}));
  REQUIRE(SolveInPlace(s).ok);
  const auto& e = Get<sketch::Ellipse>(s, el);
  CHECK_THAT(EllipseLevel(e.center, e.rMajor, e.rMinor, e.rotation,
                          Get<sketch::Point>(s, p).p),
             WithinAbs(1.0, 1e-7));
  const auto& a = Get<sketch::EllipseArc>(s, ea);
  CHECK_THAT(EllipseLevel(a.center, a.rMajor, a.rMinor, a.rotation,
                          Get<sketch::Point>(s, q).p),
             WithinAbs(1.0, 1e-7));
}

TEST_CASE("A point on a spline lands on the curve", "[solver][spline]") {
  SketchFeature s = MakeSketch();
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{0, 0}, {1, 3}, {3, 3}, {5, 1}, {6, 4}}});
  const uint32_t p = s.AddEntity(sketch::Point{{2.2, 3.4}});
  for (uint8_t k = 0; k < 5; k++) // the curve stays; the point moves
    s.constraints.push_back(Con(K::Fixed, {sp, k}));
  s.constraints.push_back(Con(K::PointOnLine, {p, 0}, {sp, 0}));
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  CHECK(r.dof == 1); // the point slides along the curve
  CHECK_THAT(
      DistanceToSpline(Get<sketch::BSpline>(s, sp), Get<sketch::Point>(s, p).p),
      WithinAbs(0.0, 1e-7));
}

TEST_CASE("An elliptical arc's end moves along its ellipse to a coincident",
          "[solver][ellipse]") {
  SketchFeature s = MakeSketch();
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.2, M_PI});
  PinEllipse(s, ea, 2.0, 1.0);
  // The start joins a point that slides on a fixed vertical line, which
  // crosses the ellipse where t = 0.8.
  const glm::dvec2 target(2.0 * std::cos(0.8), std::sin(0.8));
  const uint32_t wall =
      s.AddEntity(sketch::Line{{target.x, -2.0}, {target.x, 2.0}});
  const uint32_t pt = s.AddEntity(sketch::Point{{target.x, 0.2}});
  s.constraints.push_back(Con(K::Fixed, {wall, 0}));
  s.constraints.push_back(Con(K::Fixed, {wall, 1}));
  s.constraints.push_back(Con(K::PointOnLine, {pt, 0}, {wall, 0}));
  s.constraints.push_back(Con(K::Coincident, {ea, 1}, {pt, 0}));
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  CHECK(r.redundantConstraints.empty());
  CHECK(r.dof == 1); // the free end
  const auto& a = Get<sketch::EllipseArc>(s, ea);
  const glm::dvec2 start = sketch::EllipseArcPointUV(a, 1);
  CHECK_THAT(start.x, WithinAbs(target.x, 1e-7));
  CHECK_THAT(start.y, WithinAbs(target.y, 1e-7));
  CHECK_THAT(a.startParam, WithinAbs(0.8, 1e-7));
  CHECK_THAT(a.endParam, WithinAbs(M_PI, 1e-7)); // the other end stays
  CHECK_THAT(a.rMajor, WithinAbs(2.0, 1e-9));
  CHECK_THAT(a.rMinor, WithinAbs(1.0, 1e-9));
}

TEST_CASE("A half ellipse resized by its dimensions extrudes and revolves "
          "to the new size",
          "[solver][ellipse][extrude][revolve]") {
  SketchFeature s = MakeSketch();
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI});
  const uint32_t chord = s.AddEntity(sketch::Line{{-2, 0}, {2, 0}});
  s.constraints.push_back(Con(K::Coincident, {ea, 2}, {chord, 0}));
  s.constraints.push_back(Con(K::Coincident, {ea, 1}, {chord, 1}));
  s.constraints.push_back(Con(K::PointOnLine, {ea, 0}, {chord, 0}));
  s.constraints.push_back(Con(K::Horizontal, {chord, 0}));
  PinEllipse(s, ea, 3.0, 1.5);
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  CHECK(r.dof == 0);
  const double a = 3.0, b = 1.5;
  CHECK_THAT(PrismVolume(s, 2.0), WithinRel(M_PI * a * b / 2.0 * 2.0, 1e-6));
  // About the chord (the X axis): a spheroid, 4/3·π·a·b².
  const auto loops = sketch::BuildLoops(s.entities);
  const CADShape rev = sketch::BuildRevolve(
      s.plane, loops, {0, 0, 0}, {1, 0, 0}, 2.0 * M_PI, false, false);
  REQUIRE(rev.IsValid());
  CHECK_THAT(rev.Volume(), WithinRel(4.0 / 3.0 * M_PI * a * b * b, 1e-5));
}

TEST_CASE("A line tangent to a spline at its end follows the end's leg",
          "[solver][spline]") {
  SketchFeature s = MakeSketch();
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
  // Leaving the end at about 20 degrees off the curve's own direction.
  const uint32_t ln = s.AddEntity(sketch::Line{{4, 0}, {5.4, -2.4}});
  for (uint8_t k = 0; k < 3; k++)
    s.constraints.push_back(Con(K::Fixed, {sp, k}));
  s.constraints.push_back(Con(K::Fixed, {ln, 1}));
  s.constraints.push_back(Con(K::Coincident, {ln, 0}, {sp, 3}));
  s.constraints.push_back(Con(K::Tangent, {sp, 0}, {ln, 0}));
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  CHECK(r.redundantConstraints.empty());
  const auto& bs = Get<sketch::BSpline>(s, sp);
  const auto& l = Get<sketch::Line>(s, ln);
  // The curve's own tangent at its end, numerically.
  const double umax = 4 - sketch::BSplineDegree(bs);
  const glm::dvec2 tCurve = glm::normalize(
      sketch::BSplineEvalUV(bs, umax) - sketch::BSplineEvalUV(bs, umax - 1e-6));
  const glm::dvec2 tLine = glm::normalize(l.b - l.a);
  CHECK_THAT(Cross(tCurve, tLine), WithinAbs(0.0, 1e-5));
  CHECK(glm::length(l.a - bs.poles[3]) < 1e-7); // still joined
}

TEST_CASE("An arc tangent to an elliptical arc at a shared end",
          "[solver][ellipse][arc]") {
  SketchFeature s = MakeSketch();
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI / 2});
  PinEllipse(s, ea, 2.0, 1.0);
  // An arc starting near the ellipse's top, bending away.
  const glm::dvec2 c(0.4, 2.6);
  const glm::dvec2 j = sketch::EllipseArcPointUV(
      sketch::EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.0, M_PI / 2}, 2);
  const double r0 = glm::length(j - c);
  const double a0 = std::atan2(j.y - c.y, j.x - c.x);
  const uint32_t arc = s.AddEntity(sketch::Arc{c, r0, a0 - 1.2, a0});
  s.constraints.push_back(Con(K::Coincident, {arc, 2}, {ea, 2}));
  s.constraints.push_back(Con(K::Tangent, {arc, 0}, {ea, 0}));
  const auto r = SolveInPlace(s);
  REQUIRE(r.ok);
  CHECK(r.redundantConstraints.empty());
  const auto& e = Get<sketch::EllipseArc>(s, ea);
  const auto& ar = Get<sketch::Arc>(s, arc);
  const glm::dvec2 joint = sketch::EllipseArcPointUV(e, 2);
  CHECK(glm::length(sketch::ArcPointUV(ar, 2) - joint) < 1e-7);
  // d/dt of C + a·cos t·û + b·sin t·v̂ at the end, against the arc's tangent.
  const double t = e.endParam;
  const glm::dvec2 tEll(-e.rMajor * std::sin(t), e.rMinor * std::cos(t));
  const glm::dvec2 radial = joint - ar.center;
  const glm::dvec2 tArc(-radial.y, radial.x);
  CHECK_THAT(Cross(glm::normalize(tEll), glm::normalize(tArc)),
             WithinAbs(0.0, 1e-6));
}

TEST_CASE("A closed spline stays closed when its first pole is moved",
          "[solver][spline]") {
  SketchFeature s = MakeSketch();
  const uint32_t sp = s.AddEntity(
      sketch::BSpline{{{2, 0}, {2, 2}, {-2, 2}, {-2, -2}, {2, -2}, {2, 0}}});
  const uint32_t pt = s.AddEntity(sketch::Point{{3, 0.5}});
  s.constraints.push_back(Con(K::Fixed, {pt, 0}));
  s.constraints.push_back(Con(K::Coincident, {sp, 0}, {pt, 0}));
  REQUIRE(SolveInPlace(s).ok);
  const auto& bs = Get<sketch::BSpline>(s, sp);
  CHECK(glm::length(bs.poles.front() - glm::dvec2(3, 0.5)) < 1e-7);
  CHECK(glm::length(bs.poles.back() - glm::dvec2(3, 0.5)) < 1e-7);
}

TEST_CASE("Dragging an ellipse's points: the body translates, an end resizes",
          "[solver][ellipse][drag]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{1, 1}, 3.0, 1.0, 0.3});
  const sketch::Ellipse before = Get<sketch::Ellipse>(s, el);
  {
    // The GUI's body drag, and its centre-dot drag: the centre alone. The
    // shape's soft holds carry the rest, so it moves as a whole.
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.BeginDrag({{el, 0}}));
    solver.SetDragTargets({glm::dvec2(3.0, 0.0)});
    for (int i = 0; i < 5; i++)
      REQUIRE(solver.SolveDrag().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    const auto& e = std::get<sketch::Ellipse>(out[0]);
    CHECK_THAT(e.center.x, WithinAbs(3.0, 1e-4));
    CHECK_THAT(e.center.y, WithinAbs(0.0, 1e-4));
    CHECK_THAT(e.rMajor, WithinAbs(before.rMajor, 1e-4));
    CHECK_THAT(e.rMinor, WithinAbs(before.rMinor, 1e-4));
    CHECK_THAT(e.rotation, WithinAbs(before.rotation, 1e-4));
  }
  {
    // A handle: the major end pulled outward along the axis grows rMajor --
    // it does not slide the ellipse along, though nothing holds the centre
    // but the drag's own preference.
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.BeginDrag({{el, 3}}));
    const glm::dvec2 u(std::cos(0.3), std::sin(0.3));
    solver.SetDragTargets({glm::dvec2(1, 1) + 4.5 * u});
    for (int i = 0; i < 20; i++)
      REQUIRE(solver.SolveDrag().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    const auto& e = std::get<sketch::Ellipse>(out[0]);
    CHECK_THAT(e.rMajor, WithinAbs(4.5, 1e-4));
    CHECK_THAT(e.rMinor, WithinAbs(1.0, 1e-4));
    CHECK_THAT(e.rotation, WithinAbs(0.3, 1e-4));
    CHECK_THAT(e.center.x, WithinAbs(1.0, 1e-4));
    CHECK_THAT(e.center.y, WithinAbs(1.0, 1e-4));
  }
  {
    // The minor end, pulled outward: the minor radius grows, the major axis
    // stays where it was.
    sketch::Solver solver;
    solver.Load(s);
    REQUIRE(solver.BeginDrag({{el, 5}}));
    const glm::dvec2 v(-std::sin(0.3), std::cos(0.3));
    solver.SetDragTargets({glm::dvec2(1, 1) + 1.8 * v});
    for (int i = 0; i < 20; i++)
      REQUIRE(solver.SolveDrag().ok);
    std::vector<sketch::Entity> out;
    solver.WriteBack(out);
    const auto& e = std::get<sketch::Ellipse>(out[0]);
    CHECK_THAT(e.rMinor, WithinAbs(1.8, 1e-4));
    CHECK_THAT(e.rMajor, WithinAbs(3.0, 1e-4));
    CHECK_THAT(e.rotation, WithinAbs(0.3, 1e-4));
  }
}

TEST_CASE("Dragging an elliptical arc's end slides it along its ellipse",
          "[solver][ellipse][drag]") {
  SketchFeature s = MakeSketch();
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{0, 0}, 2.0, 1.0, 0.0, 0.3, 2.0});
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.BeginDrag({{ea, 2}}));
  // Toward where t = 2.6 is (on the curve, so the end can get there).
  solver.SetDragTargets({glm::dvec2(2.0 * std::cos(2.6), std::sin(2.6))});
  for (int i = 0; i < 20; i++)
    REQUIRE(solver.SolveDrag().ok);
  std::vector<sketch::Entity> out;
  solver.WriteBack(out);
  const auto& a = std::get<sketch::EllipseArc>(out[0]);
  CHECK_THAT(a.endParam, WithinAbs(2.6, 1e-4));
  CHECK_THAT(a.startParam, WithinAbs(0.3, 1e-4)); // the other end stays
  CHECK_THAT(a.rMajor, WithinAbs(2.0, 1e-4));
  CHECK_THAT(a.rMinor, WithinAbs(1.0, 1e-4));
  CHECK(glm::length(a.center) < 1e-4);
}

TEST_CASE("Solving a solved sketch again moves nothing, bit for bit",
          "[solver][ellipse][spline]") {
  SketchFeature s = MakeSketch();
  const uint32_t ea =
      s.AddEntity(sketch::EllipseArc{{0.1, 0.2}, 2.3, 1.1, 0.7, 0.3, 2.9});
  const uint32_t el = s.AddEntity(sketch::Ellipse{{5.1, 1.3}, 1.7, 0.9, -0.4});
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{0, 3}, {1, 5}, {3, 5.5}, {4, 3.3}}});
  const uint32_t p = s.AddEntity(sketch::Point{{2, 5}});
  PinEllipse(s, el, 2.0, 0.8);
  s.constraints.push_back(Con(K::Distance, {ea, 0}, {ea, 3}, 2.5));
  s.constraints.push_back(Con(K::PointOnLine, {p, 0}, {sp, 0}));
  s.constraints.push_back(Con(K::Coincident, {ea, 2}, {sp, 0}));
  REQUIRE(SolveInPlace(s).ok);
  const std::vector<sketch::Entity> first = s.entities;
  for (int i = 0; i < 25; i++)
    REQUIRE(SolveInPlace(s).ok);
  const auto& a0 = std::get<sketch::EllipseArc>(first[0]);
  const auto& a1 = Get<sketch::EllipseArc>(s, ea);
  CHECK(a0.center == a1.center);
  CHECK(a0.rMajor == a1.rMajor);
  CHECK(a0.rMinor == a1.rMinor);
  CHECK(a0.rotation == a1.rotation);
  CHECK(a0.startParam == a1.startParam);
  CHECK(a0.endParam == a1.endParam);
  const auto& e0 = std::get<sketch::Ellipse>(first[1]);
  const auto& e1 = Get<sketch::Ellipse>(s, el);
  CHECK(e0.rMajor == e1.rMajor);
  CHECK(e0.rotation == e1.rotation);
  CHECK(std::get<sketch::BSpline>(first[2]).poles ==
        Get<sketch::BSpline>(s, sp).poles);
}

TEST_CASE("Unconstrained curves solve without moving", "[solver][ellipse]") {
  SketchFeature s = MakeSketch();
  s.AddEntity(sketch::Ellipse{{1.1, 2.3}, 3.7, 1.3, 0.77});
  s.AddEntity(sketch::EllipseArc{{-4, 1}, 2.1, 0.6, -1.1, 0.4, 5.0});
  s.AddEntity(sketch::BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
  s.AddEntity(sketch::Ellipse{{9, 9}, 2.0, 2.0, 0.3}); // circle-shaped: rigid
  const std::vector<sketch::Entity> before = s.entities;
  REQUIRE(SolveInPlace(s).ok);
  const auto& e0 = std::get<sketch::Ellipse>(before[0]);
  const auto& e1 = std::get<sketch::Ellipse>(s.entities[0]);
  CHECK(e0.rMajor == e1.rMajor);
  CHECK(e0.rotation == e1.rotation);
  CHECK(e0.center == e1.center);
  const auto& a0 = std::get<sketch::EllipseArc>(before[1]);
  const auto& a1 = std::get<sketch::EllipseArc>(s.entities[1]);
  CHECK(a0.startParam == a1.startParam);
  CHECK(a0.endParam == a1.endParam);
  CHECK(std::get<sketch::Ellipse>(s.entities[3]).rMajor == 2.0);
}

TEST_CASE("A constraint the solver cannot hold is refused, naming what its "
          "kind takes",
          "[solver][ellipse][spline][refusal]") {
  SketchFeature s = MakeSketch();
  const uint32_t el = s.AddEntity(sketch::Ellipse{{0, 0}, 3.0, 1.0, 0.0});
  const uint32_t ci = s.AddEntity(sketch::Circle{{8, 0}, 1.0});
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{0, 3}, {1, 5}, {3, 5}, {4, 3}}});
  const uint32_t ln = s.AddEntity(sketch::Line{{0, -3}, {4, -3}});
  sketch::Text label;
  label.anchor = {0, 8};
  label.text = "A";
  label.height = 2.0;
  const uint32_t tx = s.AddEntity(label);
  s.constraints.push_back(Con(K::Radius, {el, 0}, {}, 2.0));
  s.constraints.push_back(Con(K::Tangent, {ci, 0}, {sp, 0}));
  s.constraints.push_back(Con(K::PointOnLine, {tx, 0}, {tx, 0}));
  s.constraints.push_back(Con(K::Equal, {ln, 0}, {ci, 0}));
  s.constraints.push_back(Con(K::Coincident, {el, 0}, {el, 9}));
  s.constraints.push_back(Con(K::Tangent, {ln, 0}, {el, 0})); // this one holds
  sketch::Solver solver;
  solver.Load(s);
  const auto& bad = solver.UnsupportedConstraints();
  REQUIRE(bad.size() == 5);
  CHECK(bad[0].index == 0);
  CHECK_THAT(bad[0].reason,
             ContainsSubstring("radius takes a circle or an arc") &&
                 ContainsSubstring("[" + std::to_string(el) + ", 3]"));
  CHECK_THAT(bad[1].reason, ContainsSubstring("tangent takes") &&
                                ContainsSubstring("a spline is tangent only"));
  CHECK_THAT(bad[2].reason,
             ContainsSubstring("a point and a line, circle, arc, ellipse"));
  CHECK_THAT(bad[3].reason, ContainsSubstring("two lines, two circles/arcs"));
  CHECK_THAT(bad[4].reason, ContainsSubstring("is no point of an ellipse"));
}

namespace {

// Every number a solve leaves on the curves, in entity order.
std::vector<double> SolvedNumbers(const SketchFeature& s) {
  std::vector<double> out;
  auto v2 = [&](glm::dvec2 p) {
    out.push_back(p.x);
    out.push_back(p.y);
  };
  for (const sketch::Entity& e : s.entities)
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, sketch::Point>)
            v2(x.p);
          else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
            v2(x.center);
            out.insert(out.end(), {x.rMajor, x.rMinor, x.rotation});
          } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
            v2(x.center);
            out.insert(out.end(), {x.rMajor, x.rMinor, x.rotation, x.startParam,
                                   x.endParam});
          } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
            for (glm::dvec2 p : x.poles)
              v2(p);
          }
        },
        e);
  return out;
}

// The sketches whose solve the soft shape holds' second-priority pass decides:
// the v155 golden's (an ellipse turned level and resized, a point put on it),
// and an elliptical arc, a spline and an ellipse solved together.
std::vector<SketchFeature> HeldSketches() {
  std::vector<SketchFeature> out;
  {
    SketchFeature s = MakeSketch();
    const uint32_t el = s.AddEntity(sketch::Ellipse{{0, 0}, 25.0, 10.0, 0.3});
    sketch::Point p{{0, 12}};
    p.construction = true;
    const uint32_t pt = s.AddEntity(p);
    s.constraints.push_back(Con(K::Fixed, {el, 0}));
    s.constraints.push_back(Con(K::Horizontal, {el, 0}));
    s.constraints.push_back(
        Con(K::Distance, {el, 0}, {el, sketch::kEllipseMajorPos}, 30.0));
    s.constraints.push_back(
        Con(K::Distance, {el, 0}, {el, sketch::kEllipseMinorPos}, 10.0));
    s.constraints.push_back(Con(K::PointOnLine, {pt, 0}, {el, 0}));
    out.push_back(std::move(s));
  }
  {
    SketchFeature s = MakeSketch();
    const uint32_t ea =
        s.AddEntity(sketch::EllipseArc{{0.1, 0.2}, 2.3, 1.1, 0.7, 0.3, 2.9});
    const uint32_t el =
        s.AddEntity(sketch::Ellipse{{5.1, 1.3}, 1.7, 0.9, -0.4});
    const uint32_t sp =
        s.AddEntity(sketch::BSpline{{{0, 3}, {1, 5}, {3, 5.5}, {4, 3.3}}});
    const uint32_t p = s.AddEntity(sketch::Point{{2, 5}});
    s.constraints.push_back(Con(K::Horizontal, {el, 0}));
    s.constraints.push_back(
        Con(K::Distance, {el, 0}, {el, sketch::kEllipseMajorPos}, 2.0));
    s.constraints.push_back(Con(K::Distance, {ea, 0}, {ea, 3}, 2.5));
    s.constraints.push_back(Con(K::PointOnLine, {p, 0}, {sp, 0}));
    s.constraints.push_back(Con(K::PointOnLine, {p, 0}, {el, 0}));
    s.constraints.push_back(Con(K::Coincident, {ea, 2}, {sp, 0}));
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<double> SolveFresh(SketchFeature s) {
  sketch::Solver solver;
  solver.Load(s);
  const sketch::SolveResult r = solver.Solve();
  if (r.ok)
    solver.WriteBack(s.entities);
  return SolvedNumbers(s);
}

// Hands the next small allocations back at addresses in a seeded order, as a
// worker thread's own heap (or any earlier work on this one) would: the
// solver's parameters live in separate blocks, and which of them sits at the
// lower address is the heap's business, not the sketch's.
void ScrambleSmallBlocks(unsigned seed) {
  std::vector<void*> blocks;
  for (size_t size : {512u, 576u, 64u, 128u, 256u})
    for (int i = 0; i < 7; i++)
      blocks.push_back(std::malloc(size));
  std::mt19937 rng(seed);
  std::shuffle(blocks.begin(), blocks.end(), rng);
  for (void* b : blocks)
    std::free(b);
}

} // namespace

// A project's parts load on worker threads (ReadProject), and the v155 golden
// hashed differently there about one load in five: PlaneGCS ordered its
// unknowns by their ADDRESSES, so the soft holds' second-priority solve took
// a different path to a different last bit wherever the heap laid the
// solver's blocks out differently.
TEST_CASE("A held curve solves to the same bits wherever its parameters live",
          "[solver][ellipse][spline][parallel]") {
  for (const SketchFeature& s : HeldSketches()) {
    const std::vector<double> want = SolveFresh(s);
    for (unsigned seed = 1; seed <= 48; seed++) {
      ScrambleSmallBlocks(seed);
      INFO("seed " << seed);
      REQUIRE(SolveFresh(s) == want);
    }
  }
}

TEST_CASE("A held curve solves to the same bits on many threads at once",
          "[solver][ellipse][spline][parallel]") {
  for (const SketchFeature& s : HeldSketches()) {
    const std::vector<double> want = SolveFresh(s);
    std::vector<std::vector<double>> got(8 * 6);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++)
      threads.emplace_back([&, t] {
        for (int i = 0; i < 6; i++)
          got[t * 6 + i] = SolveFresh(s);
      });
    for (std::thread& t : threads)
      t.join();
    for (const auto& g : got)
      CHECK(g == want);
  }
}

#endif // HAVE_PLANEGCS

TEST_CASE("An ellipse with its radii the wrong way round is refused on load",
          "[ellipse][io]") {
  const nlohmann::json sk = {
      {"type", "Sketch"},
      {"name", "S"},
      {"id", 1},
      {"plane",
       {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}, {"uAxis", {1, 0, 0}}}},
      {"entities",
       {{{"type", "ellipse"},
         {"id", 1},
         {"center", {0, 0}},
         {"r_major", 1.0},
         {"r_minor", 2.0},
         {"rotation", 0.0}}}},
      {"constraints", nlohmann::json::array()}};
  CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(sk),
                    ContainsSubstring("0 < r_minor <= r_major") &&
                        ContainsSubstring("add 90 degrees"));
}

TEST_CASE("Three clicks with the third outside the second make the second "
          "axis the major one",
          "[ellipse]") {
  const sketch::Ellipse e =
      sketch::EllipseFromClicks({0, 0}, {1, 0}, {0.5, 3.0});
  CHECK_THAT(e.rMajor, WithinAbs(3.0, 1e-12));
  CHECK_THAT(e.rMinor, WithinAbs(1.0, 1e-12));
  // The same curve: its major end is where the third click said.
  const glm::dvec2 end =
      sketch::EllipseAxisEndUV(e.center, e.rMajor, e.rMinor, e.rotation, 3);
  CHECK_THAT(std::abs(end.y), WithinAbs(3.0, 1e-12));
  CHECK_THAT(end.x, WithinAbs(0.0, 1e-12));
}
