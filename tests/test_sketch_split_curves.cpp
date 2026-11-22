// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Split and Fillet on ellipses, elliptical arcs and splines
// (docs/sketching/README.md#split, #fillet, #a-spline-cut-exactly).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Units.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchFillet.h"
#include "core/sketch/SketchSolver.h"
#include "core/sketch/SketchSpline.h"
#include "core/sketch/SketchSplit.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>
#include <string>

using Catch::Matchers::WithinAbs;
using FK = sketch::ConstraintKind;

namespace {

// What a cut dropped, as the person reads it (mm, degrees).
std::vector<std::string> Said(const std::vector<SketchFeature::CutDrop>& d) {
  std::vector<std::string> out;
  for (const SketchFeature::CutDrop& x : d)
    out.push_back(sketch::DescribeCutDrop(x, units::System{}));
  return out;
}

sketch::SolveResult Solved(SketchFeature& sk) {
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

int SolvedDof(SketchFeature& sk) { return Solved(sk).dof; }

// The farthest any point of `piece` lies from `curve`, along a fine sample.
double OffCurve(const sketch::Entity& piece, const sketch::Entity& curve) {
  double worst = 0.0;
  std::vector<glm::dvec2> pts;
  if (const auto* bs = std::get_if<sketch::BSpline>(&piece)) {
    pts = sketch::BSplineSampleUV(*bs, 64);
  } else if (const auto* ea = std::get_if<sketch::EllipseArc>(&piece)) {
    pts = sketch::EllipseArcSampleUV(*ea, 256);
  } else {
    pts = sketch::TessellateEntity(piece);
  }
  for (const glm::dvec2& p : pts)
    worst = std::max(worst, sketch::NearestOnCurve(curve, p)->distance);
  return worst;
}

bool SameGeometry(const std::vector<sketch::Entity>& a,
                  const std::vector<sketch::Entity>& b, double tol) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    const auto pa = sketch::TessellateEntity(a[i]);
    const auto pb = sketch::TessellateEntity(b[i]);
    if (pa.size() != pb.size())
      return false;
    for (size_t k = 0; k < pa.size(); k++)
      if (glm::length(pa[k] - pb[k]) > tol)
        return false;
  }
  return true;
}

const sketch::Entity& Get(const SketchFeature& sk, uint32_t id) {
  return sk.entities[sk.IndexOfEntity(id)];
}

// One split checked whole: the pieces, in order, together the original
// curve (each on it within 1e-9 mm, the lengths adding up); the sketch
// solving clean at the DOF it had plus `dofUp` (-1: any, for a spline's
// extra poles), nothing moving in the solve; either piece made
// construction leaving the rest where it was; the ids surviving a round
// trip through the file.
std::vector<uint32_t>
CheckSplit(SketchFeature& sk, uint32_t id,
           const std::vector<SketchFeature::SplitCut>& cuts, int dofUp,
           SketchFeature::CutReport* report = nullptr) {
  const int dof0 = SolvedDof(sk);
  const sketch::Entity original = Get(sk, id);
  std::string why;
  const std::vector<uint32_t> ids = sk.SplitEntity(id, cuts, &why, report);
  INFO(why);
  REQUIRE(ids.size() >= 2);
  CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
  double length = 0.0;
  for (uint32_t p : ids) {
    const sketch::Entity& e = Get(sk, p);
    CHECK(OffCurve(e, original) < 1e-9);
    length += sketch::CurveLength(e);
  }
  CHECK_THAT(length, WithinAbs(sketch::CurveLength(original),
                               1e-5 * sketch::CurveLength(original)));
  const std::vector<sketch::Entity> cut = sk.entities;
  const int dof1 = SolvedDof(sk);
  if (dofUp >= 0)
    CHECK(dof1 == dof0 + dofUp);
  else
    CHECK(dof1 >= dof0);
  CHECK(SameGeometry(sk.entities, cut, 1e-9));
  for (uint32_t p : ids) {
    SketchFeature c = sk;
    sketch::SetConstruction(c.entities[c.IndexOfEntity(p)], true);
    SolvedDof(c);
    CHECK(SameGeometry(c.entities, cut, 1e-9));
  }
  const nlohmann::json j = DocumentIO::FeatureToJson(sk);
  const auto back = DocumentIO::FeatureFromJson(j);
  const auto* sk2 = dynamic_cast<const SketchFeature*>(back.get());
  REQUIRE(sk2 != nullptr);
  for (uint32_t p : ids) {
    REQUIRE(sk2->IndexOfEntity(p) != SIZE_MAX);
    CHECK(SameGeometry({Get(*sk2, p)}, {Get(sk, p)}, 1e-12));
  }
  return ids;
}

sketch::BSpline Wave(int degree, glm::dvec2 at = {0, 0}) {
  sketch::BSpline s;
  s.degree = degree;
  for (int k = 0; k < 7; k++)
    s.poles.push_back(at + glm::dvec2(2.0 * k, k % 2 == 0 ? 0.0 : 3.0 + k));
  return s;
}

} // namespace

TEST_CASE("A spline cut at any parameter is the same curve",
          "[sketch][spline][split]") {
  for (const int degree : {1, 2, 3}) {
    const sketch::BSpline s = Wave(degree);
    const int m = static_cast<int>(s.poles.size()) - degree;
    for (const double u : {0.37, 1.0, 1.5, m - 0.2, 0.5 * m, m - 1.0}) {
      INFO("degree " << degree << ", u = " << u);
      const auto cut = sketch::CutSpline(s, u);
      REQUIRE(cut);
      REQUIRE(cut->joints.size() + 1 == cut->pieces.size());
      // Walk the pieces in order: each starts where the last ended, and
      // the curve they make, read at the arc-length-free parameter each
      // piece covers, is the original.
      std::vector<glm::dvec2> along, orig;
      for (const sketch::SplinePiece& piece : cut->pieces) {
        for (const glm::dvec2& p : sketch::BSplineSampleUV(piece.spline, 64))
          along.push_back(p);
        // Every pole the cut kept is where it was.
        for (size_t k = 0; k < piece.source.size(); k++)
          if (piece.source[k] >= 0)
            CHECK(piece.spline.poles[k] == s.poles[piece.source[k]]);
      }
      for (size_t i = 0; i + 1 < cut->pieces.size(); i++)
        CHECK(glm::length(cut->pieces[i].spline.poles.back() -
                          cut->pieces[i + 1].spline.poles.front()) < 1e-12);
      double worst = 0.0;
      for (const glm::dvec2& p : along)
        worst = std::max(worst, sketch::NearestOnCurve(s, p)->distance);
      CHECK(worst < 1e-12 * 20.0); // the poles run to ~20 mm
      // The cut lies where it was asked for.
      const glm::dvec2 at = sketch::BSplineEvalUV(s, u);
      CHECK(glm::length(cut->pieces[cut->before - 1].spline.poles.back() - at) <
            1e-12 * 20.0);
      CHECK(glm::length(cut->pieces.front().spline.poles.front() -
                        s.poles.front()) == 0.0);
      CHECK(glm::length(cut->pieces.back().spline.poles.back() -
                        s.poles.back()) == 0.0);
    }
    CHECK_FALSE(sketch::CutSpline(s, 0.0));
    CHECK_FALSE(sketch::CutSpline(s, m));
  }
}

TEST_CASE("Split a spline where a line crosses it and at a free point",
          "[sketch][spline][split]") {
  for (const bool crossing : {true, false}) {
    INFO((crossing ? "at the crossing" : "free"));
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t sp = sk.AddEntity(Wave(3));
    const uint32_t l = sk.AddEntity(sketch::Line{{5, -2}, {5, 12}});
    // A line running into the spline's first end, tangent there; the
    // curvature joint moves with the end it is at.
    const uint32_t in = sk.AddEntity(sketch::Line{{-1, -2}, {0, 0}});
    sk.constraints.push_back({FK::Coincident, {in, 1}, {sp, 0}, 0.0});
    sk.constraints.push_back({FK::Tangent, {in, 0}, {sp, 0}, 0.0});
    SolvedDof(sk); // the tangent turns the line onto the spline's leg
    const sketch::Entity original = Get(sk, sp);
    const glm::dvec2 at =
        crossing ? glm::dvec2(5, 4) : sketch::BSplineEvalUV(Wave(3), 2.3);
    std::vector<SketchFeature::SplitCut> cuts{{at, crossing ? l : 0u}};
    SketchFeature::CutReport report;
    const auto ids = CheckSplit(sk, sp, cuts, -1, &report);
    CHECK(report.dropped.empty());
    // The tangent went with the first end.
    bool tangent = false;
    for (const auto& k : sk.constraints)
      tangent |= k.kind == FK::Tangent && k.a.entity == in &&
                 k.b.entity == ids.front();
    CHECK(tangent);
    // Pieces in order: the first starts at the original's start.
    const auto& first = std::get<sketch::BSpline>(Get(sk, ids.front()));
    const auto& last = std::get<sketch::BSpline>(Get(sk, ids.back()));
    CHECK(glm::length(first.poles.front() -
                      std::get<sketch::BSpline>(original).poles.front()) <
          1e-9);
    CHECK(glm::length(last.poles.back() -
                      std::get<sketch::BSpline>(original).poles.back()) < 1e-9);
    if (crossing) {
      bool held = false;
      for (const auto& k : sk.constraints)
        held |= k.kind == FK::PointOnLine && k.b.entity == l;
      CHECK(held);
    }
  }
}

TEST_CASE("A spline's cut keeps the constraints on the poles it leaves and "
          "names the ones it drops",
          "[sketch][spline][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const sketch::BSpline wave = Wave(3);
  const uint32_t sp = sk.AddEntity(wave);
  for (uint8_t k = 0; k < wave.poles.size(); k++)
    sk.constraints.push_back({FK::Fixed, {sp, k}, {0, 0}, 0.0});
  sk.entityExprs[sp]["p3x"] = "6";
  sk.entityExprs[sp]["p0y"] = "0";
  const double u = 1.5;
  const auto cut = sketch::CutSpline(wave, u);
  REQUIRE(cut);
  std::set<int> kept;
  for (const auto& piece : cut->pieces)
    for (int s : piece.source)
      if (s >= 0)
        kept.insert(s);
  SketchFeature::CutReport report;
  std::string why;
  const auto ids =
      sk.SplitEntity(sp, {{sketch::BSplineEvalUV(wave, u), 0}}, &why, &report);
  INFO(why);
  REQUIRE(ids.size() == cut->pieces.size());
  // Each kept pole's fixed points at the same coordinates on its piece.
  int fixedLeft = 0;
  for (const auto& k : sk.constraints)
    if (k.kind == FK::Fixed) {
      fixedLeft++;
      const glm::dvec2 p =
          sketch::EntityPointUV(Get(sk, k.a.entity), k.a.which);
      bool somewhere = false;
      for (const glm::dvec2& q : wave.poles)
        somewhere |= q == p;
      CHECK(somewhere);
    }
  CHECK(fixedLeft == static_cast<int>(kept.size()));
  // Said as a person counts poles, from one; named to a script by its ids.
  const auto nth = [](int k) {
    const char* suffix[] = {"st", "nd", "rd"};
    return std::to_string(k + 1) + (k < 3 ? suffix[k] : "th");
  };
  std::vector<std::string> expected;
  std::vector<int> constraints;
  for (int k = 0; k < static_cast<int>(wave.poles.size()); k++)
    if (!kept.count(k)) {
      expected.push_back("fixed on the spline's " + nth(k) + " pole");
      constraints.push_back(k);
    }
  if (!kept.count(3)) {
    expected.push_back("the expression on the spline's 4th pole's x");
    constraints.push_back(-1);
  }
  CHECK(Said(report.dropped) == expected);
  REQUIRE(report.dropped.size() == constraints.size());
  for (size_t i = 0; i < constraints.size(); i++) {
    const SketchFeature::CutDrop& d = report.dropped[i];
    CHECK(d.constraint == constraints[i]);
    CHECK(d.spline == sp);
    if (d.constraint >= 0)
      CHECK(d.poles == std::vector<size_t>{static_cast<size_t>(d.constraint)});
    else
      CHECK(d.field == "p3x");
  }
  REQUIRE(kept.count(0));
  CHECK(sk.entityExprs[ids.front()].at("p0y") == "0");
}

TEST_CASE("A dimension survives a spline's cut wherever what it measured "
          "still exists, and the dry run names the one it drops first",
          "[sketch][spline][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const sketch::BSpline wave = Wave(3);
  const uint32_t sp = sk.AddEntity(wave);
  const double u = 1.5;
  const auto cut = sketch::CutSpline(wave, u);
  REQUIRE(cut);
  std::set<int> kept;
  for (const auto& piece : cut->pieces)
    for (int s : piece.source)
      if (s >= 0)
        kept.insert(s);
  // A pole the cut replaces, and one it leaves away from the cut.
  int replaced = -1, far = -1;
  for (int k = 1; k + 1 < static_cast<int>(wave.poles.size()); k++) {
    if (!kept.count(k) && replaced < 0)
      replaced = k;
    if (kept.count(k))
      far = k;
  }
  REQUIRE(replaced > 0);
  REQUIRE(far > 0);
  // An anchor fixed at the origin side, and a point placed on the spline.
  const uint32_t anchor = sk.AddEntity(sketch::Point{{-3.0, -4.0}});
  sk.constraints.push_back({FK::Fixed, {anchor, 0}, {0, 0}, 0.0});
  const glm::dvec2 onAt = sketch::BSplineEvalUV(wave, 3.4);
  const uint32_t on = sk.AddEntity(sketch::Point{onAt});
  sk.constraints.push_back({FK::PointOnLine, {on, 0}, {sp, 0}, 0.0});
  const auto dist = [&](const sketch::PointRef& r) {
    return glm::length(sketch::EntityPointUV(Get(sk, r.entity), r.which) -
                       sketch::EntityPointUV(Get(sk, anchor), 0));
  };
  struct Dim {
    sketch::PointRef to;
    double value;
    size_t index;
  };
  std::vector<Dim> dims;
  for (const sketch::PointRef& r :
       {sketch::PointRef{sp, 0},
        sketch::PointRef{sp, static_cast<uint8_t>(far)},
        sketch::PointRef{on, 0},
        sketch::PointRef{sp, static_cast<uint8_t>(replaced)}}) {
    dims.push_back({r, dist(r), sk.constraints.size()});
    sk.constraints.push_back({FK::Distance, {anchor, 0}, r, dims.back().value});
  }
  SolvedDof(sk);

  // The dry run: what the cut would drop, nothing changed.
  const glm::dvec2 at = sketch::BSplineEvalUV(wave, u);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  const std::vector<SketchFeature::CutDrop> dry = sk.SplitDropsAt(sp, at);
  CHECK(DocumentIO::FeatureToJson(sk) == before);
  REQUIRE(dry.size() == 1);
  // By its kind and value, and the pole counted from one; the ids for a
  // script.
  CHECK(Said(dry)[0] == "distance " + units::FormatNumber(dims[3].value, 3) +
                            " mm on the spline's " +
                            std::to_string(replaced + 1) +
                            (replaced + 1 == 2   ? "nd"
                             : replaced + 1 == 3 ? "rd"
                                                 : "th") +
                            " pole");
  CHECK(dry[0].constraint == static_cast<int>(dims[3].index));
  CHECK(dry[0].spline == sp);
  CHECK(dry[0].poles == std::vector<size_t>{static_cast<size_t>(replaced)});
  CHECK(dry[0].kind == FK::Distance);
  CHECK(sk.SplitDropsAt(sk.AddEntity(sketch::Line{{0, -9}, {1, -9}}), at)
            .empty()); // a line's cut drops nothing of this kind

  // The cut drops exactly that, and the rest keep their values.
  SketchFeature::CutReport report;
  std::string why;
  const auto ids = sk.SplitEntity(sp, {{at, 0}}, &why, &report);
  INFO(why);
  REQUIRE(ids.size() == 2);
  CHECK(Said(report.dropped) == Said(dry));
  REQUIRE(report.dropped.size() == 1);
  CHECK(report.dropped[0].constraint == dry[0].constraint);
  Solved(sk);
  int distances = 0;
  for (const auto& c : sk.constraints) {
    if (c.kind != FK::Distance)
      continue;
    distances++;
    const double measured =
        glm::length(sketch::EntityPointUV(Get(sk, c.a.entity), c.a.which) -
                    sketch::EntityPointUV(Get(sk, c.b.entity), c.b.which));
    CHECK_THAT(measured, WithinAbs(c.value, 1e-6));
    bool known = false;
    for (size_t d = 0; d < 3; d++)
      known |= std::abs(c.value - dims[d].value) < 1e-12;
    CHECK(known);
  }
  CHECK(distances == 3);

  // A distance to the new joint drives where the joint lies.
  const auto& first = std::get<sketch::BSpline>(Get(sk, ids.front()));
  const sketch::PointRef joint{ids.front(),
                               static_cast<uint8_t>(first.poles.size() - 1)};
  const double now = dist(joint);
  sk.constraints.push_back({FK::Distance, {anchor, 0}, joint, now + 0.75});
  Solved(sk);
  CHECK_THAT(dist(joint), WithinAbs(now + 0.75, 1e-6));
  const auto& second = std::get<sketch::BSpline>(Get(sk, ids.back()));
  CHECK(glm::length(sketch::EntityPointUV(Get(sk, joint.entity), joint.which) -
                    second.poles.front()) < 1e-6); // still one curve
}

TEST_CASE("A closed spline takes one cut; its seam is the other",
          "[sketch][spline][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  sketch::BSpline loop;
  loop.poles = {{0, 0}, {6, -2}, {10, 3}, {6, 9}, {-1, 7}, {-3, 2}, {0, 0}};
  const uint32_t sp = sk.AddEntity(loop);
  const uint32_t l = sk.AddEntity(sketch::Line{{3, -5}, {3, 15}});
  std::string why;
  CHECK(sk.SplitEntity(sp, {{{3, 1}, l}, {{3, 7}, l}}, &why).empty());
  CHECK(why.find("takes one cut (a closed spline's seam is its other)") !=
        std::string::npos);
  CHECK(sk.SplitEntity(sp, {{{0, 0}}}, &why).empty());
  CHECK(why.find("the seam of spline") != std::string::npos);
  const auto ids = CheckSplit(sk, sp, {{{3, 8}, l}}, -1);
  // The seam: the last piece's end joined to the first piece's start.
  bool seam = false;
  for (const auto& k : sk.constraints)
    seam |= k.kind == FK::Coincident && k.a.entity == ids.back() &&
            k.b.entity == ids.front() && k.b.which == 0;
  CHECK(seam);
}

TEST_CASE("Split an ellipse at two crossings and at two free points",
          "[sketch][ellipse][split]") {
  struct Case {
    const char* what;
    glm::dvec2 a, b; // the crossing line (or none)
    glm::dvec2 c1, c2;
    bool crossing;
  };
  // Through the centre along the major axis: the cuts a diameter apart, on
  // the axis ends -- where an equal radius or a concentric would fall one
  // equation short.
  for (const Case& c :
       {Case{"a diameter on the major axis",
             {-9, 0},
             {9, 0},
             {6, 0},
             {-6, 0},
             true},
        Case{"a diameter on the minor axis",
             {0, -9},
             {0, 9},
             {0, 3},
             {0, -3},
             true},
        Case{"off the centre", {-9, 1}, {9, 2.5}, {5, 2}, {-5, 1}, true},
        Case{"free", {}, {}, {3, 2.6}, {-4, -2.2}, false}}) {
    INFO(c.what);
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t el = sk.AddEntity(sketch::Ellipse{{0, 0}, 6.0, 3.0, 0.0});
    uint32_t l = 0;
    if (c.crossing)
      l = sk.AddEntity(sketch::Line{c.a, c.b});
    const auto ids =
        CheckSplit(sk, el, {{c.c1, l}, {c.c2, l}}, c.crossing ? 0 : 2);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == el);
    CHECK(std::holds_alternative<sketch::EllipseArc>(Get(sk, ids[0])));
    CHECK(std::holds_alternative<sketch::EllipseArc>(Get(sk, ids[1])));
  }
  // Turned, the same.
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t el = sk.AddEntity(sketch::Ellipse{{2, 1}, 5.0, 2.0, 0.6});
  const uint32_t l = sk.AddEntity(sketch::Line{{-8, -6}, {10, 8}});
  const auto xs = sketch::Crossings(Get(sk, el), Get(sk, l));
  REQUIRE(xs.size() == 2);
  CheckSplit(sk, el, {{xs[0], l}, {xs[1], l}}, 0);
}

TEST_CASE("Split an elliptical arc at a crossing and at a free point",
          "[sketch][ellipse][split]") {
  struct Case {
    const char* what;
    glm::dvec2 a, b, at;
    bool crossing;
  };
  for (const Case& c :
       {Case{"at the minor axis end", {0, -1}, {0, 9}, {0, 3}, true},
        Case{"between the axis ends", {-9, 1}, {9, 2}, {4.5, 1.5}, true},
        Case{"free", {}, {}, {3, 2.6}, false},
        Case{"free at the minor axis end", {}, {}, {0, 3}, false}}) {
    INFO(c.what);
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t ea = sk.AddEntity(
        sketch::EllipseArc{{0, 0}, 6.0, 3.0, 0.0, -0.3, M_PI - 0.2});
    // The far end is joined to a line: its coincident goes with piece 2.
    const glm::dvec2 farEnd =
        sketch::EllipseArcPointUV(std::get<sketch::EllipseArc>(Get(sk, ea)), 2);
    const uint32_t tail =
        sk.AddEntity(sketch::Line{farEnd, farEnd - glm::dvec2(0, 4)});
    sk.constraints.push_back({FK::Coincident, {ea, 2}, {tail, 0}, 0.0});
    uint32_t l = 0;
    if (c.crossing)
      l = sk.AddEntity(sketch::Line{c.a, c.b});
    const auto ids = CheckSplit(sk, ea, {{c.at, l}}, c.crossing ? 0 : 1);
    REQUIRE(ids.size() == 2);
    bool moved = false;
    for (const auto& k : sk.constraints)
      moved |= k.kind == FK::Coincident && k.a.entity == ids[1] &&
               k.a.which == 2 && k.b.entity == tail;
    CHECK(moved);
  }
}

TEST_CASE("Split refuses a circle-shaped ellipse, naming why",
          "[sketch][ellipse][split]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t el = sk.AddEntity(sketch::Ellipse{{0, 0}, 3.0, 3.0, 0.0});
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  std::string why;
  CHECK(sk.SplitEntity(el, {{{3, 0}}, {{-3, 0}}}, &why).empty());
  CHECK(why.find("circle-shaped") != std::string::npos);
  CHECK(DocumentIO::FeatureToJson(sk) == before);
}

namespace {

// The unit direction curve `e` runs in at its point `p` (either sense).
glm::dvec2 DirectionAt(const sketch::Entity& e, glm::dvec2 p) {
  return sketch::TangentAt(e, p);
}

// The fillet checked: each tangent point on its curve within 1e-9 mm, the
// arc tangent to both there, the sketch solving clean, and the profile
// extruding.
void CheckFillet(std::unique_ptr<SketchFeature> sk, uint32_t a, uint32_t b,
                 double radius, glm::dvec2 corner) {
  const sketch::Entity ca = Get(*sk, a), cb = Get(*sk, b);
  SolvedDof(*sk);
  std::string why;
  SketchFeature::CutReport report;
  const uint32_t arcId =
      sk->FilletCorner(a, b, radius, corner, &why, nullptr, &report);
  INFO(why);
  REQUIRE(arcId != 0);
  const auto check = [&](double tol) {
    const auto& arc = std::get<sketch::Arc>(Get(*sk, arcId));
    CHECK_THAT(arc.radius, WithinAbs(radius, tol));
    for (const sketch::Entity* o : {&ca, &cb}) {
      // The arc end on this curve.
      const glm::dvec2 e1 = sketch::ArcPointUV(arc, 1),
                       e2 = sketch::ArcPointUV(arc, 2);
      const double d1 = sketch::NearestOnCurve(*o, e1)->distance;
      const double d2 = sketch::NearestOnCurve(*o, e2)->distance;
      const glm::dvec2 t = d1 < d2 ? e1 : e2;
      CHECK(std::min(d1, d2) < tol);
      const glm::dvec2 radial = glm::normalize(t - arc.center);
      CHECK(std::abs(glm::dot(radial, DirectionAt(*o, t))) < 1e-7);
    }
  };
  check(1e-9);
  Solved(*sk);
  check(1e-9);
  Document doc;
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", sid, 2.0, ExtrudeMode::NewBody));
  CHECK(doc.GetFeature(0)->GetError().empty());
  CHECK(doc.GetFeature(1)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() > 0.0);
}

void Join(SketchFeature& sk, uint32_t a, uint8_t wa, uint32_t b, uint8_t wb) {
  sk.constraints.push_back({FK::Coincident, {a, wa}, {b, wb}, 0.0});
}

} // namespace

TEST_CASE("Fillet a line and a spline", "[sketch][spline][fillet]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l1 = sk->AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t sp =
      sk->AddEntity(sketch::BSpline{{{10, 0}, {9, 4}, {6, 6}, {3, 8}, {0, 8}}});
  const uint32_t l2 = sk->AddEntity(sketch::Line{{0, 8}, {0, 0}});
  Join(*sk, l1, 1, sp, 0);
  Join(*sk, sp, 4, l2, 0);
  Join(*sk, l2, 1, l1, 0);
  CheckFillet(std::move(sk), l1, sp, 1.5, {10, 0});
}

TEST_CASE("Fillet two splines", "[sketch][spline][fillet]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t s1 =
      sk->AddEntity(sketch::BSpline{{{0, 0}, {4, -1}, {8, 1}, {12, 0}}});
  const uint32_t s2 =
      sk->AddEntity(sketch::BSpline{{{12, 0}, {11, 4}, {8, 7}, {4, 8}}});
  const uint32_t l = sk->AddEntity(sketch::Line{{4, 8}, {0, 0}});
  Join(*sk, s1, 3, s2, 0);
  Join(*sk, s2, 3, l, 0);
  Join(*sk, l, 1, s1, 0);
  CheckFillet(std::move(sk), s1, s2, 1.0, {12, 0});
}

TEST_CASE("Fillet a line and an elliptical arc", "[sketch][ellipse][fillet]") {
  auto sk = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t l1 = sk->AddEntity(sketch::Line{{0, 0}, {6, 0}});
  const uint32_t ea =
      sk->AddEntity(sketch::EllipseArc{{0, 0}, 6.0, 3.0, 0.0, 0.0, M_PI / 2.0});
  const uint32_t l2 = sk->AddEntity(sketch::Line{{0, 3}, {0, 0}});
  Join(*sk, l1, 1, ea, 1);
  Join(*sk, ea, 2, l2, 0);
  Join(*sk, l2, 1, l1, 0);
  CheckFillet(std::move(sk), l1, ea, 0.8, {6, 0});
}

TEST_CASE("A fillet too large on a spline names the largest that fits, and "
          "that fits",
          "[sketch][spline][fillet]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t l = sk.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  const uint32_t sp =
      sk.AddEntity(sketch::BSpline{{{10, 0}, {10, 2}, {8, 3}, {7, 3}}});
  Join(sk, l, 1, sp, 0);
  const nlohmann::json before = DocumentIO::FeatureToJson(sk);
  std::string why;
  double largest = 0.0;
  CHECK(sk.FilletCorner(l, sp, 50.0, glm::dvec2(10, 0), &why, &largest) == 0);
  INFO(why);
  CHECK(why.find("is the largest that does") != std::string::npos);
  CHECK(DocumentIO::FeatureToJson(sk) == before);
  REQUIRE(largest > 0.0);
  CHECK(sk.FilletCorner(l, sp, largest, glm::dvec2(10, 0), &why) != 0);
}

TEST_CASE("The owner's corner: a spline, a vertical line and two collinear "
          "horizontal lines; either sector the spline bounds fillets it",
          "[sketch][spline][fillet]") {
  for (const glm::dvec2 cursor :
       {glm::dvec2(-1.0, -2.0), glm::dvec2(2.0, -1.0)}) {
    INFO("cursor " << cursor.x << ", " << cursor.y);
    SketchFeature sk("S", SketchPlane(PlaneType::XY));
    const uint32_t left = sk.AddEntity(sketch::Line{{-5, 0}, {0, 0}});
    const uint32_t right = sk.AddEntity(sketch::Line{{0, 0}, {5, 0}});
    const uint32_t up = sk.AddEntity(sketch::Line{{0, 0}, {0, 5}});
    const uint32_t sp =
        sk.AddEntity(sketch::BSpline{{{0, 0}, {1, -3}, {4, -4}, {6, -2}}});
    Join(sk, left, 1, right, 0);
    Join(sk, right, 0, up, 0);
    Join(sk, up, 0, sp, 0);
    const auto corner = sketch::CornerAt(sk.entities, {0, 0}, cursor, 0.1);
    REQUIRE(corner);
    const uint32_t other =
        corner->a.entity == sp ? corner->b.entity : corner->a.entity;
    CHECK((corner->a.entity == sp || corner->b.entity == sp));
    CHECK(other == (cursor.x < 0 ? left : right));
    std::string why;
    SketchFeature::CutReport report;
    const uint32_t arc =
        sk.FilletCorner(corner->a.entity, corner->b.entity, 0.5, corner->vertex,
                        &why, nullptr, &report);
    INFO(why);
    REQUIRE(arc != 0);
    REQUIRE(report.replaced.size() == 1);
    CHECK(report.replaced[0].spline == sp);
    Solved(sk);
  }
}

TEST_CASE("Splitting a spline is one undo step", "[sketch][spline][split]") {
  Document doc;
  auto own = std::make_unique<SketchFeature>("S", SketchPlane(PlaneType::XY));
  const uint32_t sp = own->AddEntity(Wave(3));
  doc.AddFeature(std::move(own));
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  const nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  REQUIRE(sk->SplitEntity(sp, {{sketch::BSplineEvalUV(Wave(3), 2.3)}}).size() >=
          2);
  const nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  CommandStack stack;
  stack.Execute(std::make_unique<ModifyFeatureCommand>(0, before, after), doc);
  stack.Undo(doc);
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  CHECK(DocumentIO::FeatureToJson(*sk) == before);
  stack.Redo(doc);
  sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  CHECK(DocumentIO::FeatureToJson(*sk) == after);
}
