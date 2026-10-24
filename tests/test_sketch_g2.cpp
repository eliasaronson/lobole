// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Curvature continuity (G2) and the arc-length dimension
// (docs/sketching/README.md#curvature-continuity,
// docs/sketching/README.md#arc-length). Every case reads the outcome back
// through OpenCASCADE from the wire the sketch builds -- the curvature on both
// sides of a joint, the length of an edge -- not the solver's own numbers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchOrientation.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepLProp_CLProps.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

#include <cmath>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
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

sketch::SolveResult SolveInPlace(SketchFeature& s) {
  sketch::Solver solver;
  solver.Load(s);
  for (const auto& u : solver.UnsupportedConstraints())
    FAIL("refused: " << u.reason);
  const sketch::SolveResult r = solver.Solve();
  CHECK(r.conflictingConstraints.empty());
  CHECK(r.redundantConstraints.empty());
  if (r.ok)
    solver.WriteBack(s.entities);
  return r;
}

// One side of a joint as OCC reads it on the built wire: the curvature and
// the centre of curvature (the point the curve bends toward), and the
// direction the edge leaves the joint in.
struct Side {
  double curvature = 0.0;
  gp_Pnt centre;
  gp_Dir away;
};

std::vector<Side> SidesAt(const TopoDS_Wire& wire, glm::dvec2 at) {
  std::vector<Side> out;
  const gp_Pnt p(at.x, at.y, 0.0);
  for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
    BRepAdaptor_Curve c(e);
    for (const bool first : {true, false}) {
      const double u = first ? c.FirstParameter() : c.LastParameter();
      if (c.Value(u).Distance(p) > 1e-6)
        continue;
      BRepLProp_CLProps props(c, u, 2, 1e-12);
      Side s;
      s.curvature = props.Curvature();
      if (s.curvature > 1e-12)
        props.CentreOfCurvature(s.centre);
      gp_Dir t;
      props.Tangent(t);
      s.away = first ? t : t.Reversed();
      out.push_back(s);
    }
  }
  return out;
}

TopoDS_Wire Wire(const SketchFeature& s) {
  const auto loops = sketch::BuildLoops(s.entities);
  REQUIRE(loops.size() == 1);
  const TopoDS_Wire w = sketch::BuildProfileWire(s.plane, loops);
  REQUIRE(!w.IsNull());
  return w;
}

// G2 at `at`: two sides, leaving in opposite directions (tangent), with the
// same curvature, bending toward the same centre.
void CheckG2(const TopoDS_Wire& wire, glm::dvec2 at) {
  const std::vector<Side> sides = SidesAt(wire, at);
  REQUIRE(sides.size() == 2);
  CHECK(sides[0].away.Dot(sides[1].away) < -1.0 + 1e-10);
  const double k0 = sides[0].curvature, k1 = sides[1].curvature;
  INFO("curvatures " << k0 << " and " << k1);
  CHECK(std::abs(k0 - k1) <= 1e-6 * std::max({k0, k1, 1e-3}));
  if (k0 > 1e-9 && k1 > 1e-9)
    CHECK(sides[0].centre.Distance(sides[1].centre) <= 1e-6 / k0);
}

// A spline running out of the end of a fixed line, back round to its start.
SketchFeature SplineLine(uint32_t* spline, uint32_t* line) {
  SketchFeature s = MakeSketch();
  const uint32_t ln = s.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{10, 0}, {13, 1}, {14, 5}, {8, 8}, {0, 6}}});
  const uint32_t back = s.AddEntity(sketch::Line{{0, 6}, {0, 0}});
  s.constraints.push_back(Con(K::Fixed, {ln, 0}));
  s.constraints.push_back(Con(K::Fixed, {ln, 1}));
  s.constraints.push_back(Con(K::Fixed, {sp, 3}));
  s.constraints.push_back(Con(K::Fixed, {sp, 4}));
  s.constraints.push_back(Con(K::Coincident, {ln, 1}, {sp, 0}));
  s.constraints.push_back(Con(K::Coincident, {sp, 4}, {back, 0}));
  s.constraints.push_back(Con(K::Coincident, {back, 1}, {ln, 0}));
  *spline = sp;
  *line = ln;
  return s;
}

// An arc (fixed) whose end a spline leaves from, closed by two lines.
SketchFeature SplineArc(uint32_t* spline, uint32_t* arc) {
  SketchFeature s = MakeSketch();
  const uint32_t ar =
      s.AddEntity(sketch::Arc{{0, 0}, 5.0, 0.0, M_PI / 2.0}); // (5,0)->(0,5)
  const uint32_t sp = s.AddEntity(
      sketch::BSpline{{{0, 5}, {-3, 5.4}, {-5.5, 4.0}, {-7, 2}, {-7, -2}}});
  const uint32_t l1 = s.AddEntity(sketch::Line{{-7, -2}, {5, -2}});
  const uint32_t l2 = s.AddEntity(sketch::Line{{5, -2}, {5, 0}});
  s.constraints.push_back(Con(K::Fixed, {ar, 0}));
  s.constraints.push_back(Con(K::Fixed, {ar, 1}));
  s.constraints.push_back(Con(K::Fixed, {ar, 2}));
  s.constraints.push_back(Con(K::Fixed, {sp, 3}));
  s.constraints.push_back(Con(K::Fixed, {sp, 4}));
  s.constraints.push_back(Con(K::Coincident, {ar, 2}, {sp, 0}));
  s.constraints.push_back(Con(K::Coincident, {sp, 4}, {l1, 0}));
  s.constraints.push_back(Con(K::Coincident, {l1, 1}, {l2, 0}));
  s.constraints.push_back(Con(K::Coincident, {l2, 1}, {ar, 1}));
  *spline = sp;
  *arc = ar;
  return s;
}

// Two splines joined end to end, closed underneath by three lines.
SketchFeature SplineSpline(uint32_t* a, uint32_t* b) {
  SketchFeature s = MakeSketch();
  const uint32_t sa =
      s.AddEntity(sketch::BSpline{{{0, 0}, {1, 3}, {3, 2.5}, {4, 1}, {5, 0}}});
  const uint32_t sb = s.AddEntity(
      sketch::BSpline{{{5, 0}, {6, -1.5}, {8, -1}, {9, 0.5}, {10, 0}}});
  const uint32_t l1 = s.AddEntity(sketch::Line{{10, 0}, {10, -4}});
  const uint32_t l2 = s.AddEntity(sketch::Line{{10, -4}, {0, -4}});
  const uint32_t l3 = s.AddEntity(sketch::Line{{0, -4}, {0, 0}});
  for (uint8_t k : {0, 1, 2})
    s.constraints.push_back(Con(K::Fixed, {sa, k}));
  for (uint8_t k : {3, 4})
    s.constraints.push_back(Con(K::Fixed, {sb, k}));
  s.constraints.push_back(Con(K::Fixed, {sa, 4}));
  s.constraints.push_back(Con(K::Coincident, {sa, 4}, {sb, 0}));
  s.constraints.push_back(Con(K::Coincident, {sb, 4}, {l1, 0}));
  s.constraints.push_back(Con(K::Coincident, {l1, 1}, {l2, 0}));
  s.constraints.push_back(Con(K::Coincident, {l2, 1}, {l3, 0}));
  s.constraints.push_back(Con(K::Coincident, {l3, 1}, {sa, 0}));
  *a = sa;
  *b = sb;
  return s;
}

} // namespace

TEST_CASE("G2 between a spline and a line: the joint is straight on both sides",
          "[solver][spline][g2]") {
  uint32_t sp = 0, ln = 0;
  SketchFeature s = SplineLine(&sp, &ln);
  // Before: neither tangent nor G2 -- the spline leaves at 18 degrees.
  CHECK(SidesAt(Wire(s), {10, 0})[0].away.Dot(
            SidesAt(Wire(s), {10, 0})[1].away) > -0.99);
  s.constraints.push_back(Con(K::Curvature, {sp, 0}, {ln, 0}));
  REQUIRE(SolveInPlace(s).ok);
  CheckG2(Wire(s), {10, 0});
  // The joint did not move to get there.
  CHECK(glm::length(std::get<sketch::BSpline>(s.entities[1]).poles[0] -
                    glm::dvec2(10, 0)) < 1e-9);
}

TEST_CASE("G2 between a spline and an arc matches the arc's radius",
          "[solver][spline][arc][g2]") {
  uint32_t sp = 0, ar = 0;
  SketchFeature s = SplineArc(&sp, &ar);
  {
    // Tangent alone is G1: the curvature still jumps at the joint.
    SketchFeature g1 = s;
    g1.constraints.push_back(Con(K::Tangent, {sp, 0}, {ar, 0}));
    REQUIRE(SolveInPlace(g1).ok);
    const auto sides = SidesAt(Wire(g1), {0, 5});
    REQUIRE(sides.size() == 2);
    CHECK(sides[0].away.Dot(sides[1].away) < -1.0 + 1e-10);
    CHECK(std::abs(sides[0].curvature - sides[1].curvature) > 0.05);
  }
  // Either order of the refs.
  const bool arcFirst = GENERATE(false, true);
  s.constraints.push_back(arcFirst ? Con(K::Curvature, {ar, 0}, {sp, 0})
                                   : Con(K::Curvature, {sp, 0}, {ar, 0}));
  REQUIRE(SolveInPlace(s).ok);
  const TopoDS_Wire w = Wire(s);
  CheckG2(w, {0, 5});
  for (const Side& side : SidesAt(w, {0, 5}))
    CHECK_THAT(side.curvature, WithinRel(0.2, 1e-6)); // 1 / r, r = 5
  // And it profiles into a solid.
  const CADShape prism =
      sketch::BuildPrism(s.plane, sketch::BuildLoops(s.entities), 1.0, false);
  CHECK(prism.IsValid());
}

TEST_CASE("G2 between two splines", "[solver][spline][g2]") {
  uint32_t a = 0, b = 0;
  SketchFeature s = SplineSpline(&a, &b);
  s.constraints.push_back(Con(K::Curvature, {a, 0}, {b, 0}));
  REQUIRE(SolveInPlace(s).ok);
  CheckG2(Wire(s), {5, 0});
}

TEST_CASE("G2 survives the file and solves to the same bits on many threads",
          "[solver][spline][g2][parallel][io]") {
  uint32_t a = 0, b = 0;
  SketchFeature s = SplineSpline(&a, &b);
  s.constraints.push_back(Con(K::Curvature, {a, 0}, {b, 0}));
  const auto json = DocumentIO::FeatureToJson(s);
  CHECK(json.dump().find("\"curvature\"") != std::string::npos);
  auto back = DocumentIO::FeatureFromJson(json);
  const auto* loaded = dynamic_cast<SketchFeature*>(back.get());
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->constraints.back().kind == K::Curvature);

  const auto solve = [](SketchFeature f) {
    sketch::Solver solver;
    solver.Load(f);
    if (solver.Solve().ok)
      solver.WriteBack(f.entities);
    std::vector<double> out;
    for (const auto& e : f.entities)
      if (const auto* sp = std::get_if<sketch::BSpline>(&e))
        for (glm::dvec2 p : sp->poles)
          out.insert(out.end(), {p.x, p.y});
    return out;
  };
  const std::vector<double> want = solve(*loaded);
  std::vector<std::vector<double>> got(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++)
    threads.emplace_back([&, t] { got[t] = solve(*loaded); });
  for (std::thread& t : threads)
    t.join();
  for (const auto& g : got)
    CHECK(g == want);
}

TEST_CASE("G2 is refused, naming why, where it cannot hold",
          "[solver][g2][refusals]") {
  SketchFeature s = MakeSketch();
  const uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {5, 0}});
  const uint32_t l2 = s.AddEntity(sketch::Line{{5, 0}, {9, 1}});
  const uint32_t a1 = s.AddEntity(sketch::Arc{{9, 4}, 3.0, -M_PI / 2, 0.0});
  const uint32_t a2 = s.AddEntity(sketch::Arc{{15, 4}, 3.0, M_PI, 1.5 * M_PI});
  const uint32_t poly =
      s.AddEntity(sketch::BSpline{{{0, 0}, {-2, 3}, {-4, 0}}, /*degree=*/1});
  const uint32_t loose =
      s.AddEntity(sketch::BSpline{{{20, 0}, {21, 2}, {23, 2}, {24, 0}}});
  s.constraints.push_back(Con(K::Coincident, {l1, 1}, {l2, 0}));
  s.constraints.push_back(Con(K::Coincident, {l2, 1}, {a1, 1}));
  s.constraints.push_back(Con(K::Coincident, {a1, 2}, {a2, 1}));
  s.constraints.push_back(Con(K::Coincident, {l1, 0}, {poly, 0}));
  const auto refusal = [&](sketch::Constraint c) {
    SketchFeature t = s;
    t.constraints.push_back(c);
    sketch::Solver solver;
    solver.Load(t);
    const auto& u = solver.UnsupportedConstraints();
    REQUIRE(u.size() == 1);
    CHECK(u[0].index == t.constraints.size() - 1);
    CHECK_THAT(u[0].reason, ContainsSubstring("curvature takes a spline"));
    return u[0].reason;
  };
  CHECK_THAT(refusal(Con(K::Curvature, {l1, 0}, {l2, 0})),
             ContainsSubstring("collinear"));
  CHECK_THAT(refusal(Con(K::Curvature, {l2, 0}, {a1, 0})),
             ContainsSubstring("infinite"));
  CHECK_THAT(refusal(Con(K::Curvature, {a1, 0}, {a2, 0})),
             ContainsSubstring("one circle"));
  CHECK_THAT(refusal(Con(K::Curvature, {poly, 0}, {l1, 0})),
             ContainsSubstring("degree-1"));
  CHECK_THAT(refusal(Con(K::Curvature, {loose, 0}, {l1, 0})),
             ContainsSubstring("do not share an end"));
}

TEST_CASE("An arc-length dimension sets the length of the built edge",
          "[solver][arc][arc_length]") {
  SketchFeature s = MakeSketch();
  // A slot-like profile: an arc closed by two radii (lines to its centre).
  const uint32_t ar = s.AddEntity(sketch::Arc{{0, 0}, 10.0, 0.2, 1.4});
  const uint32_t l1 = s.AddEntity(sketch::Line{{0, 0}, {9.8, 2.0}});
  const uint32_t l2 = s.AddEntity(sketch::Line{{1.7, 9.85}, {0, 0}});
  s.constraints.push_back(Con(K::Fixed, {ar, 0}));
  s.constraints.push_back(Con(K::Coincident, {l1, 0}, {ar, 0}));
  s.constraints.push_back(Con(K::Coincident, {l1, 1}, {ar, 1}));
  s.constraints.push_back(Con(K::Coincident, {ar, 2}, {l2, 0}));
  s.constraints.push_back(Con(K::Coincident, {l2, 1}, {ar, 0}));
  s.constraints.push_back(Con(K::Radius, {ar, 0}, {}, 10.0));
  s.constraints.push_back(Con(K::ArcLength, {ar, 0}, {}, 15.0));
  REQUIRE(SolveInPlace(s).ok);

  const auto edgeLength = [&](const SketchFeature& f) {
    const TopoDS_Wire w = Wire(f);
    double best = 0.0;
    for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
      if (c.GetType() == GeomAbs_Circle)
        best = GCPnts_AbscissaPoint::Length(c);
    }
    return best;
  };
  CHECK_THAT(edgeLength(s), WithinRel(15.0, 1e-9));
  CHECK_THAT(*sketch::MeasureDimension(s, s.constraints.size() - 1),
             WithinRel(15.0, 1e-9));

  // A value edit steps it, as any dimension's.
  SketchFeature edited = s;
  const size_t dim = edited.constraints.size() - 1;
  edited.constraints[dim].value = 40.0; // past a half turn of r = 10
  sketch::Solver solver;
  solver.Load(edited);
  const auto out = solver.SolveValueEdit({{dim, 15.0, 40.0}});
  REQUIRE(out.Accepted());
  solver.WriteBack(edited.entities);
  CHECK_THAT(edgeLength(edited), WithinRel(40.0, 1e-9));

  // And it is a dimension: driven, it adds no equation and measures.
  SketchFeature driven = s;
  driven.constraints[dim].driven = true;
  driven.constraints[dim].value = 0.0;
  sketch::Solver ds;
  ds.Load(driven);
  REQUIRE(ds.UnsupportedConstraints().empty());
  CHECK_THAT(*sketch::MeasureDimension(driven, dim), WithinRel(15.0, 1e-9));
}

TEST_CASE("An arc-length dimension names the right kind for what it refuses",
          "[solver][arc_length][refusals]") {
  SketchFeature s = MakeSketch();
  const uint32_t ln = s.AddEntity(sketch::Line{{0, 0}, {5, 0}});
  const uint32_t ci = s.AddEntity(sketch::Circle{{0, 0}, 3.0});
  const uint32_t el =
      s.AddEntity(sketch::EllipseArc{{0, 0}, 4.0, 2.0, 0.0, 0.0, 1.0});
  const uint32_t sp =
      s.AddEntity(sketch::BSpline{{{0, 0}, {1, 2}, {3, 2}, {4, 0}}});
  const auto refusal = [&](uint32_t id) {
    SketchFeature t = s;
    t.constraints.push_back(Con(K::ArcLength, {id, 0}, {}, 5.0));
    sketch::Solver solver;
    solver.Load(t);
    REQUIRE(solver.UnsupportedConstraints().size() == 1);
    return solver.UnsupportedConstraints()[0].reason;
  };
  CHECK_THAT(refusal(ln), ContainsSubstring("`distance`"));
  CHECK_THAT(refusal(ci), ContainsSubstring("`diameter`"));
  CHECK_THAT(refusal(el), ContainsSubstring("no arc-length dimension"));
  CHECK_THAT(refusal(sp), ContainsSubstring("no arc-length dimension"));
}

#endif // HAVE_PLANEGCS
