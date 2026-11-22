// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A spline cut in two stays the one curve
// (docs/sketching/README.md#a-spline-cut-exactly, #smooth): two pieces on the
// parent's knots, joined Coincident + Smooth, with the freedom of the uncut
// spline with one knot inserted. Points are checked against OpenCASCADE's
// evaluation of the original knots, never against the code under test.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchSpline.h"
#include "core/sketch/SketchSplit.h"

#include <Geom2d_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using FK = sketch::ConstraintKind;
using sketch::BSpline;

namespace {

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

glm::dvec2 At(const Handle(Geom2d_BSplineCurve) & c, double u) {
  const gp_Pnt2d p = c->Value(u);
  return {p.X(), p.Y()};
}

BSpline Wave(int degree) {
  BSpline s;
  s.degree = degree;
  for (int k = 0; k < 7; k++)
    s.poles.push_back(glm::dvec2(2.0 * k, k % 2 == 0 ? 0.0 : 3.0 + k));
  return s;
}

const sketch::Entity& Get(const SketchFeature& sk, uint32_t id) {
  return sk.entities[sk.IndexOfEntity(id)];
}
const BSpline& Spline(const SketchFeature& sk, uint32_t id) {
  return std::get<BSpline>(Get(sk, id));
}

sketch::SolveResult Solve(SketchFeature& sk) {
  sketch::Solver s;
  s.Load(sk);
  const sketch::SolveResult r = s.Solve();
  REQUIRE(r.ok);
  CHECK(r.conflictingConstraints.empty());
  CHECK(r.redundantConstraints.empty());
  CHECK(s.UnsupportedConstraints().empty());
  s.WriteBack(sk.entities);
  return r;
}

// Drags pole `which` of `id` to `to` the way the GUI does, and writes the
// result back.
void Drag(SketchFeature& sk, sketch::PointRef ref, glm::dvec2 to) {
  sketch::Solver s;
  s.Load(sk);
  REQUIRE(s.BeginDrag({ref}));
  s.SetDragTargets({to});
  for (int i = 0; i < 30; i++)
    REQUIRE(s.SolveDrag().ok);
  s.WriteBack(sk.entities);
}

// The first and second derivatives of each piece at the joint, in the
// shared parameter.
std::pair<glm::dvec2, glm::dvec2> JointJump(const BSpline& a,
                                            const BSpline& b) {
  const double u = sketch::SplineDomain(a).second;
  const double eps = 1e-12;
  return {sketch::BSplineDerivUV(a, u - eps) - sketch::BSplineDerivUV(b, u),
          sketch::BSplineDeriv2UV(a, u - eps) - sketch::BSplineDeriv2UV(b, u)};
}

} // namespace

TEST_CASE("One cut gives two splines on the parent's knots, together the "
          "parent",
          "[sketch][spline][split][knots]") {
  std::vector<BSpline> shapes{Wave(1), Wave(2), Wave(3)};
  BSpline uneven = Wave(3);
  uneven.knots = {1, 1, 1, 1, 1.4, 2.9, 3.0, 5, 5, 5, 5};
  shapes.push_back(uneven);
  for (const BSpline& s : shapes) {
    const Handle(Geom2d_BSplineCurve) occ = Occ(s);
    const auto [lo, hi] = sketch::SplineDomain(s);
    const std::vector<double> U = sketch::SplineKnots(s);
    std::vector<double> at;
    for (const double t : {0.07, 0.31, 0.5, 0.93})
      at.push_back(lo + t * (hi - lo));
    at.push_back(U[s.degree + 1]); // on a knot
    for (const double u : at) {
      INFO("degree " << s.degree << (s.knots.empty() ? "" : " uneven")
                     << ", u = " << u);
      const auto cut = sketch::CutSpline(s, u);
      REQUIRE(cut);
      REQUIRE(cut->pieces.size() == 2);
      REQUIRE(cut->joints.size() == 1);
      CHECK(cut->before == 1);
      for (size_t i = 0; i < 2; i++) {
        const BSpline& piece = cut->pieces[i].spline;
        CHECK(sketch::SplineKnotsRefusal(piece).empty());
        const auto [plo, phi] = sketch::SplineDomain(piece);
        CHECK(plo == (i == 0 ? lo : u));
        CHECK(phi == (i == 0 ? u : hi));
        // The piece in the parent's parameter IS the parent there.
        for (int k = 0; k <= 100; k++) {
          const double v = plo + (phi - plo) * k / 100.0;
          CHECK(glm::length(sketch::BSplineEvalUV(piece, v) - At(occ, v)) <
                1e-9);
        }
        for (size_t k = 0; k < cut->pieces[i].source.size(); k++)
          if (cut->pieces[i].source[k] >= 0)
            CHECK(piece.poles[k] == s.poles[cut->pieces[i].source[k]]);
      }
      const bool onKnot =
          std::find(U.begin() + s.degree + 1, U.end() - s.degree - 1, u) !=
          U.end() - s.degree - 1;
      const sketch::PieceJoint want =
          s.degree == 1
              ? (onKnot ? sketch::PieceJoint::C0 : sketch::PieceJoint::G1)
              : sketch::PieceJoint::Smooth;
      CHECK(cut->joints[0] == want);
      // Only the degree - 1 poles of the cut's own span are replaced.
      int replaced = 0;
      std::vector<bool> kept(s.poles.size(), false);
      for (const auto& piece : cut->pieces)
        for (int src : piece.source)
          if (src >= 0)
            kept[static_cast<size_t>(src)] = true;
      for (bool k : kept)
        replaced += k ? 0 : 1;
      CHECK(replaced <= std::max(0, s.degree - 1));
    }
  }
}

TEST_CASE("A cubic cut once gains two degrees of freedom and stays the "
          "uncut curve with that knot inserted",
          "[sketch][spline][split][knots]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const BSpline wave = Wave(3);
  const uint32_t sp = sk.AddEntity(wave);
  const int dof0 = Solve(sk).dof;
  CHECK(dof0 == 14);
  const double u = 2.5; // mid-span [2, 3]
  std::string why;
  const auto ids =
      sk.SplitEntity(sp, {{sketch::BSplineEvalUV(wave, u), 0}}, &why);
  INFO(why);
  REQUIRE(ids.size() == 2);
  int smooth = 0;
  for (const auto& c : sk.constraints)
    smooth += c.kind == FK::SplineJoint ? 1 : 0;
  CHECK(smooth == 1);
  const std::vector<sketch::Entity> cut = sk.entities;
  CHECK(Solve(sk).dof == dof0 + 2);
  // Nothing moved in the solve.
  for (uint32_t id : ids)
    CHECK(Spline(sk, id).poles ==
          std::get<BSpline>(cut[sk.IndexOfEntity(id)]).poles);

  // The uncut spline with u inserted once: 8 poles, the original pole 5
  // its pole 6.
  BSpline inserted = wave;
  std::vector<double> U = sketch::SplineKnots(wave);
  sketch::InsertKnot(U, inserted.poles, 3, u);
  inserted.knots = U;
  REQUIRE(inserted.poles[6] == wave.poles[5]);
  const uint32_t right = ids[1];
  const BSpline& r0 = Spline(sk, right);
  // Pole 5 of the original sits in the right piece.
  int at = -1;
  for (size_t k = 0; k < r0.poles.size(); k++)
    if (r0.poles[k] == wave.poles[5])
      at = static_cast<int>(k);
  REQUIRE(at >= 0);

  SECTION("a pole dragged: the pieces stay on the uncut curve with the same "
          "knot and the same drag") {
    SketchFeature dragged = sk;
    Drag(dragged, {right, static_cast<uint8_t>(at)},
         wave.poles[5] + glm::dvec2(1.5, -2.0));
    BSpline moved = inserted;
    moved.poles[6] = Spline(dragged, right).poles[static_cast<size_t>(at)];
    CHECK(glm::length(moved.poles[6] - wave.poles[5]) > 1.0); // it moved
    const Handle(Geom2d_BSplineCurve) occ = Occ(moved);
    for (uint32_t id : ids) {
      const BSpline& piece = Spline(dragged, id);
      const auto [lo, hi] = sketch::SplineDomain(piece);
      for (int k = 0; k <= 100; k++) {
        const double v = lo + (hi - lo) * k / 100.0;
        CHECK(glm::length(sketch::BSplineEvalUV(piece, v) - At(occ, v)) < 1e-9);
      }
    }
  }

  SECTION("the pole beside the joint dragged: the joint carries the curve "
          "with it, and with Smooth deleted the pieces part") {
    for (const bool keepSmooth : {true, false}) {
      INFO((keepSmooth ? "with Smooth" : "Smooth deleted"));
      SketchFeature dragged = sk;
      if (!keepSmooth)
        std::erase_if(dragged.constraints, [](const sketch::Constraint& c) {
          return c.kind == FK::SplineJoint;
        });
      const glm::dvec2 from = Spline(dragged, right).poles[1];
      Drag(dragged, {right, 1}, from + glm::dvec2(0.8, 1.2));
      CHECK(glm::length(Spline(dragged, right).poles[1] - from) > 0.5);
      const auto [d1, d2] =
          JointJump(Spline(dragged, ids[0]), Spline(dragged, right));
      const double scale = glm::length(sketch::BSplineDerivUV(
          Spline(dragged, right),
          sketch::SplineDomain(Spline(dragged, right)).first));
      if (keepSmooth) {
        CHECK(glm::length(d1) < 1e-6 * scale);
        CHECK(glm::length(d2) < 1e-6 * scale);
      } else {
        CHECK(glm::length(d1) > 1e-2 * scale);
      }
    }
  }
}

TEST_CASE("Smooth survives the file and refuses what it cannot join, by name",
          "[sketch][spline][knots]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const auto cut = sketch::CutSpline(Wave(3), 2.5);
  REQUIRE(cut);
  const uint32_t a = sk.AddEntity(cut->pieces[0].spline);
  const uint32_t b = sk.AddEntity(cut->pieces[1].spline);
  const uint8_t aEnd =
      static_cast<uint8_t>(cut->pieces[0].spline.poles.size() - 1);
  const uint32_t q = sk.AddEntity(
      BSpline{{{30, 0}, {32, 3}, {34, 0}}, 2}); // a quadratic, loose
  const uint32_t poly = sk.AddEntity(BSpline{{{40, 0}, {42, 3}, {44, 0}}, 1});
  const uint32_t poly2 = sk.AddEntity(BSpline{{{44, 0}, {46, 3}, {48, 0}}, 1});
  const uint32_t l = sk.AddEntity(sketch::Line{{-5, 0}, {0, 0}});
  sk.constraints.push_back({FK::Coincident, {a, aEnd}, {b, 0}, 0.0});
  sk.constraints.push_back({FK::Coincident, {poly, 2}, {poly2, 0}, 0.0});
  sk.constraints.push_back({FK::Coincident, {l, 1}, {a, 0}, 0.0});
  sk.constraints.push_back({FK::SplineJoint, {a, aEnd}, {b, 0}, 0.0});
  Solve(sk);
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  CHECK(j.at("constraints").back().at("kind") == "smooth");
  const auto back = DocumentIO::FeatureFromJson(j);
  const auto* sk2 = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(sk2);
  CHECK(sk2->constraints.back().kind == FK::SplineJoint);

  const auto refusal = [&](sketch::Constraint c) {
    SketchFeature t = sk;
    t.constraints.push_back(c);
    sketch::Solver s;
    s.Load(t);
    const auto& u = s.UnsupportedConstraints();
    return u.empty() ? std::string() : u.front().reason;
  };
  CHECK_THAT(refusal({FK::SplineJoint, {l, 1}, {a, 0}, 0.0}),
             ContainsSubstring("`tangent` or `curvature`"));
  CHECK_THAT(refusal({FK::SplineJoint, {a, aEnd}, {q, 0}, 0.0}),
             ContainsSubstring("smooth joins two splines of one degree"));
  CHECK_THAT(refusal({FK::SplineJoint, {poly, 2}, {poly2, 0}, 0.0}),
             ContainsSubstring("degree-1 spline is a polyline"));
  CHECK_THAT(refusal({FK::SplineJoint, {a, 1}, {b, 0}, 0.0}),
             ContainsSubstring("must be an END of each spline"));
  CHECK_THAT(refusal({FK::SplineJoint, {a, 0}, {b, 0}, 0.0}),
             ContainsSubstring("join them with a coincident first"));
}

TEST_CASE("A closed spline cut once is two pieces, smooth at the cut and "
          "coincident at the seam",
          "[sketch][spline][split][knots]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  BSpline loop;
  loop.poles = {{0, 0}, {6, -2}, {10, 3}, {6, 9}, {-1, 7}, {-3, 2}, {0, 0}};
  const uint32_t sp = sk.AddEntity(loop);
  const uint32_t l = sk.AddEntity(sketch::Line{{3, -5}, {3, 15}});
  const int dof0 = Solve(sk).dof;
  std::string why;
  const auto ids = sk.SplitEntity(sp, {{{3, 8}, l}}, &why);
  INFO(why);
  REQUIRE(ids.size() == 2);
  int smooth = 0, seam = 0;
  for (const auto& c : sk.constraints) {
    smooth += c.kind == FK::SplineJoint ? 1 : 0;
    seam += c.kind == FK::Coincident && c.a.entity == ids[1] &&
                    c.b.entity == ids[0] && c.b.which == 0
                ? 1
                : 0;
  }
  CHECK(smooth == 1);
  CHECK(seam == 1);
  // Held on the line at the crossing: the cut adds what the inserted knot
  // does, less the one the crossing takes.
  CHECK(Solve(sk).dof == dof0 + 1);
}

TEST_CASE("The owner's split: a spline crossed by a line, cut there, "
          "extrudes to the volume it did whole",
          "[sketch][spline][split][knots]") {
  const auto build = [](bool split) {
    Document doc;
    auto own = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
    const BSpline s{
        {{-3, -1}, {-2, 1.5}, {-0.5, -1.5}, {1, 1.8}, {2.2, -0.5}, {3, 1}}};
    const uint32_t sp = own->AddEntity(s);
    // Closed below it: down, across, up.
    const uint32_t down = own->AddEntity(sketch::Line{{3, 1}, {3, -3}});
    const uint32_t across = own->AddEntity(sketch::Line{{3, -3}, {-3, -3}});
    const uint32_t up = own->AddEntity(sketch::Line{{-3, -3}, {-3, -1}});
    sketch::Line cross{{0.6, -2.5}, {0.6, 2.5}};
    cross.construction = true;
    const uint32_t x = own->AddEntity(cross);
    own->constraints.push_back({FK::Coincident, {sp, 5}, {down, 0}, 0.0});
    own->constraints.push_back({FK::Coincident, {down, 1}, {across, 0}, 0.0});
    own->constraints.push_back({FK::Coincident, {across, 1}, {up, 0}, 0.0});
    own->constraints.push_back({FK::Coincident, {up, 1}, {sp, 0}, 0.0});
    if (split) {
      std::string why;
      const auto xs = sketch::Crossings(Get(*own, sp), Get(*own, x));
      REQUIRE(xs.size() == 1);
      const auto ids = own->SplitEntity(sp, {{xs[0], x}}, &why);
      INFO(why);
      REQUIRE(ids.size() == 2);
    }
    doc.AddFeature(std::move(own));
    const uint64_t skId = doc.GetFeatures().back()->GetId();
    doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", skId, 2.0,
                                                    ExtrudeMode::NewBody));
    const bool ok = doc.Regenerate();
    for (const auto& f : doc.GetFeatures())
      UNSCOPED_INFO(f->GetName() << ": " << f->GetError());
    REQUIRE(ok);
    REQUIRE(doc.GetBodies().size() == 1);
    return doc.GetBodies()[0].shape.Volume();
  };
  const double whole = build(false);
  CHECK(whole > 1.0);
  CHECK_THAT(build(true), WithinRel(whole, 1e-9));
}

TEST_CASE("A fillet of a line and a spline leaves one spline",
          "[sketch][spline][fillet][knots]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t sp =
      sk.AddEntity(BSpline{{{10, 0}, {9, 4}, {6, 6}, {3, 8}, {0, 8}}});
  sk.constraints.push_back({FK::Coincident, {l, 1}, {sp, 0}, 0.0});
  const BSpline before = Spline(sk, sp);
  std::string why;
  SketchFeature::CutReport report;
  const uint32_t arc =
      sk.FilletCorner(l, sp, 1.5, glm::dvec2(10, 0), &why, nullptr, &report);
  INFO(why);
  REQUIRE(arc != 0);
  int splines = 0;
  for (const auto& e : sk.entities)
    splines += std::holds_alternative<BSpline>(e) ? 1 : 0;
  CHECK(splines == 1);
  REQUIRE(report.replaced.size() == 1);
  CHECK(report.replaced[0].pieces == std::vector<uint32_t>{sp});
  // What is left is the original curve from the tangent point on, in the
  // original's parameter.
  const BSpline& left = Spline(sk, sp);
  const Handle(Geom2d_BSplineCurve) occ = Occ(before);
  const auto [lo, hi] = sketch::SplineDomain(left);
  CHECK(hi == sketch::SplineDomain(before).second);
  for (int k = 0; k <= 50; k++) {
    const double v = lo + (hi - lo) * k / 50.0;
    CHECK(glm::length(sketch::BSplineEvalUV(left, v) - At(occ, v)) < 1e-9);
  }
  Solve(sk);
}
