// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Three sketch edits that save the user work (docs/sketching/README.md): the
// first dimension rescaling a sketch drawn by eye, the open ends of a profile
// that does not close, and trimming every span a dragged path crosses.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/features/SketchFeature.h"
#include "core/sketch/SketchOrientation.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"

#include <cmath>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using K = sketch::ConstraintKind;

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

// A rough L-shaped outline, drawn by eye: six lines joined end to end.
SketchFeature RoughL(std::vector<uint32_t>* ids) {
  SketchFeature s = MakeSketch();
  const std::vector<glm::dvec2> pts = {{1.1, 0.9}, {7.3, 1.2}, {7.0, 3.4},
                                       {3.2, 3.1}, {3.4, 8.2}, {0.8, 7.9}};
  for (size_t i = 0; i < pts.size(); i++)
    ids->push_back(
        s.AddEntity(sketch::Line{pts[i], pts[(i + 1) % pts.size()]}));
  for (size_t i = 0; i < pts.size(); i++)
    s.constraints.push_back(
        Con(K::Coincident, {(*ids)[i], 1}, {(*ids)[(i + 1) % ids->size()], 0}));
  return s;
}

std::vector<glm::dvec2> Corners(const SketchFeature& s) {
  std::vector<glm::dvec2> out;
  for (const auto& e : s.entities)
    if (const auto* l = std::get_if<sketch::Line>(&e))
      out.push_back(l->a);
  return out;
}

} // namespace

TEST_CASE("The first dimension scales the drawn shape about its centre",
          "[sketch][rescale]") {
  std::vector<uint32_t> ids;
  SketchFeature s = RoughL(&ids);
  const std::vector<glm::dvec2> before = Corners(s);
  sketch::Constraint width = Con(K::Distance, {ids[0], 0}, {ids[0], 1}, 60.0);
  const double drawn = *sketch::MeasureDimension(s.entities, width);
  const sketch::RescaleOutcome r =
      sketch::RescaleForFirstDimension(s, width, 60.0);
  REQUIRE(r.scaled);
  CHECK(r.why.empty());
  CHECK_THAT(r.factor, WithinRel(60.0 / drawn, 1e-12));
  // The anchor is the bounding box's centre, and stays put.
  CHECK_THAT(r.anchor.x, WithinAbs(0.5 * (0.8 + 7.3), 1e-12));
  CHECK_THAT(r.anchor.y, WithinAbs(0.5 * (0.9 + 8.2), 1e-12));
  const std::vector<glm::dvec2> after = Corners(s);
  // Every ratio and angle the user drew is kept: each corner is the old one
  // scaled about the anchor.
  for (size_t i = 0; i < before.size(); i++) {
    const glm::dvec2 want = r.anchor + (before[i] - r.anchor) * r.factor;
    CHECK(glm::length(after[i] - want) < 1e-9);
  }
  // Then the dimension applies with nothing left to move.
  s.constraints.push_back(width);
  sketch::Solver solver;
  solver.Load(s);
  REQUIRE(solver.Solve().ok);
  std::vector<sketch::Entity> solved;
  solver.WriteBack(solved);
  const auto loops = sketch::BuildLoops(solved);
  REQUIRE(loops.size() == 1);
  CHECK_THAT(std::abs(loops[0].SignedArea()),
             WithinRel(std::abs(sketch::BuildLoops(s.entities)[0].SignedArea()),
                       1e-9));
  CHECK_THAT(*sketch::MeasureDimension(solved, width), WithinRel(60.0, 1e-9));
}

TEST_CASE("The first dimension scales about the one pinned point",
          "[sketch][rescale]") {
  std::vector<uint32_t> ids;
  SketchFeature s = RoughL(&ids);
  // The corner the user put on the origin (a coincident to the datum).
  s.entities[0] = sketch::Line{{0, 0}, {7.3, 1.2}};
  sketch::SetEntityId(s.entities[0], ids[0]);
  std::get<sketch::Line>(s.entities[5]).b = {0, 0};
  s.constraints.push_back(
      Con(K::Coincident, {ids[0], 0}, {sketch::kOrigin, 0}));
  const sketch::Constraint r12 =
      Con(K::Distance, {ids[1], 0}, {ids[1], 1}, 12.0);
  const auto r = sketch::RescaleForFirstDimension(s, r12, 12.0);
  REQUIRE(r.scaled);
  CHECK(glm::length(r.anchor) < 1e-12);
  CHECK(glm::length(std::get<sketch::Line>(s.entities[0]).a) < 1e-12);
}

TEST_CASE("Only the first length dimension rescales", "[sketch][rescale]") {
  std::vector<uint32_t> ids;
  SketchFeature s = RoughL(&ids);
  const sketch::Constraint w = Con(K::Distance, {ids[0], 0}, {ids[0], 1}, 60);
  const sketch::Constraint h = Con(K::Distance, {ids[4], 0}, {ids[4], 1}, 30);

  SECTION("a second dimension does not") {
    s.constraints.push_back(w);
    const auto before = Corners(s);
    const auto r = sketch::RescaleForFirstDimension(s, h, 30.0);
    CHECK_FALSE(r.scaled);
    CHECK(r.why.empty());
    CHECK(Corners(s) == before);
  }
  SECTION("re-editing the only one does") {
    s.constraints.push_back(w);
    CHECK(sketch::RescaleForFirstDimension(s, s.constraints.back(), 90.0,
                                           s.constraints.size() - 1)
              .scaled);
  }
  SECTION("an angle does not") {
    const sketch::Constraint ang = Con(K::Angle, {ids[0], 0}, {ids[1], 0}, 1.5);
    CHECK_FALSE(sketch::RescaleForFirstDimension(s, ang, 1.5).scaled);
  }
  SECTION("a sketch with an expression-driven constraint does not") {
    sketch::Constraint e = Con(K::Distance, {ids[2], 0}, {ids[2], 1}, 4.0);
    e.valueExpr = "wall";
    e.driven = false;
    s.constraints.push_back(e);
    CHECK_FALSE(sketch::RescaleForFirstDimension(s, w, 60.0).scaled);
  }
  SECTION("two pinned points do not, and say so") {
    s.constraints.push_back(Con(K::Fixed, {ids[0], 0}));
    s.constraints.push_back(Con(K::Fixed, {ids[2], 0}));
    const auto before = Corners(s);
    const auto r = sketch::RescaleForFirstDimension(s, w, 60.0);
    CHECK_FALSE(r.scaled);
    CHECK_THAT(r.why, ContainsSubstring("2 points are pinned"));
    CHECK(Corners(s) == before);
  }
  SECTION("a linear pattern's millimetre step does not, and says so") {
    s.AddLinearPattern({ids[0]}, {20.0, 0.0}, 3, /*link=*/true);
    const auto r = sketch::RescaleForFirstDimension(s, w, 60.0);
    CHECK_FALSE(r.scaled);
    CHECK_THAT(r.why, ContainsSubstring("linear pattern"));
  }
}

TEST_CASE("Open ends: a chain, a T, an overlap and a closed loop",
          "[sketch][profile][open_ends]") {
  SECTION("an open chain has two ends") {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::Line{{0, 0}, {10, 0}});
    s.AddEntity(sketch::Line{{10, 0}, {10, 5}});
    s.AddEntity(sketch::Line{{10, 5}, {0, 5}});
    const auto r = sketch::OpenEnds(s.entities);
    REQUIRE(r.open.size() == 2);
    CHECK(r.branched.empty());
    CHECK_THAT(sketch::OpenEndsWords(r), ContainsSubstring("2 open ends"));
    CHECK(r.gaps.empty()); // 5 apart on a 10 x 5 outline: not "almost"
  }
  SECTION("an almost-closed loop: two ends just apart, joined as a gap") {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::Line{{0, 0}, {10, 0}});
    s.AddEntity(sketch::Line{{10, 0}, {10, 5}});
    s.AddEntity(sketch::Line{{10, 5}, {0, 5}});
    s.AddEntity(sketch::Line{{0, 5}, {0, 0.001}});
    REQUIRE(sketch::BuildLoops(s.entities).empty());
    const auto r = sketch::OpenEnds(s.entities);
    REQUIRE(r.open.size() == 2);
    REQUIRE(r.gaps.size() == 1);
    CHECK(glm::length(r.gaps[0].first - r.gaps[0].second) < 0.0011);
  }
  SECTION("a T: three edges meet at one point") {
    SketchFeature s = MakeSketch();
    s.AddRectangle({0, 0}, {10, 5});
    s.AddEntity(sketch::Line{{10, 0}, {14, -2}});
    const auto r = sketch::OpenEnds(s.entities);
    CHECK(r.open.size() == 1);
    REQUIRE(r.branched.size() == 1);
    CHECK(glm::length(r.branched[0] - glm::dvec2(10, 0)) < 1e-9);
  }
  SECTION("an overlap: a line drawn twice") {
    SketchFeature s = MakeSketch();
    s.AddRectangle({0, 0}, {10, 5});
    s.AddEntity(sketch::Line{{0, 0}, {10, 0}});
    const auto r = sketch::OpenEnds(s.entities);
    CHECK(r.open.empty());
    CHECK(r.branched.size() == 2);
  }
  SECTION("a closed loop the greedy chainer reads as open") {
    // Two triangles sharing the vertex (5, 5): every vertex is met an even
    // number of times. BuildLoops' walk takes the first edge that fits at
    // the shared vertex and can strand part of the figure.
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::Line{{0, 0}, {5, 5}});
    s.AddEntity(sketch::Line{{5, 5}, {10, 0}});
    s.AddEntity(sketch::Line{{5, 5}, {10, 10}});
    s.AddEntity(sketch::Line{{10, 0}, {0, 0}});
    s.AddEntity(sketch::Line{{10, 10}, {0, 10}});
    s.AddEntity(sketch::Line{{0, 10}, {5, 5}});
    const auto r = sketch::OpenEnds(s.entities);
    CHECK(r.open.empty());
    CHECK(r.branched.size() == 1); // the bow tie's waist: four edges
  }
  SECTION("construction geometry and closed curves are not ends") {
    SketchFeature s = MakeSketch();
    s.AddEntity(sketch::Circle{{0, 0}, 3.0});
    sketch::Line c{{0, 0}, {5, 5}};
    c.construction = true;
    s.AddEntity(c);
    CHECK(sketch::OpenEnds(s.entities).Empty());
  }
}

TEST_CASE("A dragged path trims every span it crosses, in one call",
          "[sketch][trim]") {
  // Three verticals cut in half by one horizontal, and a lone circle. A path
  // across their tops crosses the three upper halves and the circle.
  SketchFeature s = MakeSketch();
  std::vector<uint32_t> v;
  for (double x : {2.0, 5.0, 8.0})
    v.push_back(s.AddEntity(sketch::Line{{x, 0}, {x, 10}}));
  const uint32_t h = s.AddEntity(sketch::Line{{0, 5}, {10, 5}});
  const uint32_t ci = s.AddEntity(sketch::Circle{{12, 8}, 1.0});
  const std::vector<glm::dvec2> path = {{1, 8}, {6, 8.5}, {12.5, 8}};
  std::vector<sketch::Entity> removed;
  CHECK(s.TrimAlongPath(path, &removed) == 4);
  REQUIRE(removed.size() == 4);
  // What the preview showed is what went: each vertical's upper half.
  for (size_t i = 0; i < 3; i++) {
    const auto& l = std::get<sketch::Line>(removed[i]);
    CHECK_THAT(std::min(l.a.y, l.b.y), WithinAbs(5.0, 1e-9));
    CHECK_THAT(std::max(l.a.y, l.b.y), WithinAbs(10.0, 1e-9));
  }
  CHECK(std::holds_alternative<sketch::Arc>(removed[3]));
  for (uint32_t id : v) {
    const auto& l = std::get<sketch::Line>(s.entities[s.IndexOfEntity(id)]);
    CHECK_THAT(std::max(l.a.y, l.b.y), WithinAbs(5.0, 1e-9));
  }
  CHECK(s.IndexOfEntity(h) != SIZE_MAX);  // not crossed, untouched
  CHECK(s.IndexOfEntity(ci) == SIZE_MAX); // no crossing bracketed it: gone
  CHECK(s.entities.size() == 4);
}

TEST_CASE("A path that crosses one span twice trims it once",
          "[sketch][trim]") {
  SketchFeature s = MakeSketch();
  const uint32_t a = s.AddEntity(sketch::Line{{0, 0}, {10, 0}});
  s.AddEntity(sketch::Line{{5, -5}, {5, 5}});
  // Down through the left half and back up through it again.
  const std::vector<glm::dvec2> path = {{2, 1}, {2, -1}, {3, -1}, {3, 1}};
  CHECK(s.TrimAlongPath(path) == 1);
  // The left half [0, 5] went; the right half is still there.
  bool right = false;
  for (const auto& e : s.entities)
    if (const auto* l = std::get_if<sketch::Line>(&e))
      right |= (std::abs(l->a.y) < 1e-12 && std::abs(l->b.y) < 1e-12 &&
                std::max(l->a.x, l->b.x) > 9.99);
  CHECK(right);
  // The trimmed piece took the id with it; the survivor is the split's
  // second piece.
  CHECK(s.IndexOfEntity(a) == SIZE_MAX);
  CHECK(s.entities.size() == 2);
}
