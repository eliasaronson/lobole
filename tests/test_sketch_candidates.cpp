// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The relations a traced section supports
// (docs/sketching/README.md#relations-the-scan-supports), on outlines
// generated from known lines and arcs and sampled with noise as a scan's
// cut is: what is offered is what the generator holds within the fit's own
// uncertainty, and nothing it does not; accepting adds them as constraints
// while the solve keeps every piece on its points.

#include "SectionOutlines.h"

#include "core/Angles.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SectionTrace.h"
#include "core/sketch/SketchCandidates.h"
#include "core/sketch/SketchOrientation.h"
#include "core/sketch/SketchSolver.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace outlines;

namespace {

// A slot whose ends are r1 (at +len/2) and r2 (at -len/2): its sides are
// tangent to both, so they are 2 asin((r1 - r2) / len) from parallel.
std::vector<Prim> TaperedSlot(double len, double r1, double r2) {
  const glm::dvec2 c1(len / 2, 0), c2(-len / 2, 0);
  const double s = (r1 - r2) / len, cc = std::sqrt(1.0 - s * s);
  const glm::dvec2 nb(s, -cc), nt(s, cc);
  const double ab = angles::ToDegrees(std::atan2(nb.y, nb.x));
  const double at = angles::ToDegrees(std::atan2(nt.y, nt.x));
  return {L(c2 + r2 * nb, c1 + r1 * nb), A(c1, r1, ab, at - ab),
          L(c1 + r1 * nt, c2 + r2 * nt), A(c2, r2, at, ab + 360.0 - at)};
}

// A bar w x h with rounds of radius r at its lower-left and upper-right
// corners and sharp corners at the other two, run counter-clockwise.
std::vector<Prim> TwoRoundBar(double w, double h, double r) {
  const double x = w / 2, y = h / 2;
  return {L({-x + r, -y}, {x, -y}),    L({x, -y}, {x, y - r}),
          A({x - r, y - r}, r, 0, 90), L({x - r, y}, {-x, y}),
          L({-x, y}, {-x, -y + r}),    A({-x + r, -y + r}, r, 180, 90)};
}

std::vector<Prim> Boss(double d) { return {A({0, 0}, d / 2, 0, 360)}; }

// A sketch holding the sampled outline as its section's cut (as the
// section's regeneration would leave it), traced and solved.
SketchFeature Traced(const std::vector<Sampled>& chains, double tol = 0.1) {
  SketchFeature sk("Traced", SketchPlane(PlaneType::XY));
  sk.AddSectionGeometry({}, tol, sketch::SectionFit::Pieces);
  sketch::ExternalGeom& ext = sk.external.back();
  ext.usedToleranceMm = tol;
  for (const Sampled& s : chains)
    ext.cut.push_back(s.chain);
  const sketch::TraceResult tr = sketch::TraceSection(sk, 0, 0.5);
  REQUIRE(tr.error.empty());
  sketch::Solver solver;
  solver.Load(sk);
  REQUIRE(solver.Solve().ok);
  solver.WriteBack(sk.entities);
  return sk;
}

uint32_t TracedOfKind(const SketchFeature& sk, bool arc, int nth = 0) {
  for (const sketch::TracedFrom& t : sk.traced) {
    const auto& e = sk.entities[sk.IndexOfEntity(t.entity)];
    if ((std::holds_alternative<sketch::Arc>(e) ||
         std::holds_alternative<sketch::Circle>(e)) == arc &&
        nth-- == 0)
      return t.entity;
  }
  return 0;
}

const sketch::Candidate* Find(const std::vector<sketch::Candidate>& cs,
                              sketch::CandidateKind k) {
  for (const sketch::Candidate& c : cs)
    if (c.kind == k)
      return &c;
  return nullptr;
}

void Print(const char* what, const std::vector<sketch::Candidate>& cs) {
  std::printf("candidates (%s):\n", what);
  for (const sketch::Candidate& c : cs)
    std::printf("  %-28s off %+.4f %s, %.2f sigmas%s\n", c.key.c_str(), c.off,
                c.degrees ? "deg" : "mm", c.sigmas,
                sketch::IsSize(c.kind)
                    ? (" (nominal " + std::to_string(c.nominal) + ")").c_str()
                    : "");
}

Sampling Noisy(double noise, double spacing, uint32_t tiltDeg = 0) {
  Sampling o;
  o.noise = noise;
  o.spacing = spacing;
  o.tiltDeg = tiltDeg;
  o.shift = {0.0, 0.0};
  return o;
}

} // namespace

TEST_CASE("Sides a fraction of a degree from parallel are offered parallel",
          "[sketch][candidates]") {
  // A slot 12 mm between centres, 0.2 degrees off parallel (its ends
  // 0.021 mm apart in radius), 0.05 mm of noise every 0.2 mm: parallel
  // and its ends' equal radius are offered, under three sigmas.
  const double len = 12.0, r = 4.0;
  const double dr = len * std::sin(angles::ToRadians(0.1));
  int parallel = 0, equal = 0;
  const int draws = 5;
  for (uint32_t seed = 0; seed < draws; seed++) {
    const SketchFeature sk =
        Traced({Sample(TaperedSlot(len, r + dr / 2, r - dr / 2),
                       Noisy(0.05, 0.2, 17), 40 + seed)});
    REQUIRE(sk.traced.size() == 4);
    const auto cs = sketch::FindCandidates(sk, {});
    if (seed == 0)
      Print("slot 0.2 deg off parallel", cs);
    parallel += Find(cs, sketch::CandidateKind::Parallel) != nullptr;
    equal += Find(cs, sketch::CandidateKind::EqualRadius) != nullptr;
  }
  // Under three sigmas a true relation is missed about once in 370 draws;
  // the 0.2 degrees and 0.021 mm are themselves 1-2 sigmas here.
  CHECK(parallel >= draws - 1);
  CHECK(equal >= draws - 1);
}

TEST_CASE("Sides three degrees from parallel are not", "[sketch][candidates]") {
  const double len = 12.0, r = 4.0;
  const double dr = len * std::sin(angles::ToRadians(1.5));
  for (uint32_t seed = 0; seed < 5; seed++) {
    const SketchFeature sk =
        Traced({Sample(TaperedSlot(len, r + dr / 2, r - dr / 2),
                       Noisy(0.05, 0.2, 17), 50 + seed)});
    const auto cs = sketch::FindCandidates(sk, {});
    if (seed == 0)
      Print("slot 3 deg off parallel", cs);
    CHECK(Find(cs, sketch::CandidateKind::Parallel) == nullptr);
    CHECK(Find(cs, sketch::CandidateKind::EqualRadius) == nullptr);
  }
}

TEST_CASE("Equal rounds at opposite ends of a part are not offered equal",
          "[sketch][candidates]") {
  // Two 3 mm rounds 100 mm apart with sharp corners between: neither
  // meets, neighbours nor faces the other, so their equal radius holds by
  // chance and is not offered; each one's own 3 mm is.
  for (uint32_t seed = 0; seed < 3; seed++) {
    const SketchFeature sk = Traced(
        {Sample(TwoRoundBar(100, 20, 3), Noisy(0.03, 0.2, 11), 60 + seed)});
    const auto cs = sketch::FindCandidates(sk, {});
    if (seed == 0)
      Print("two rounds at opposite ends", cs);
    CHECK(Find(cs, sketch::CandidateKind::EqualRadius) == nullptr);
    int radii = 0;
    for (const sketch::Candidate& c : cs)
      if (c.kind == sketch::CandidateKind::Radius) {
        radii++;
        CHECK(c.nominal == 3.0);
      }
    CHECK(radii == 2);
  }
}

TEST_CASE("A boss offers its round diameter only within its evidence",
          "[sketch][candidates]") {
  // 0.1 mm of noise every 0.5 mm: the diameter is known to about 0.02 mm,
  // so 11.97 is 12 and 11.7 is neither 12 nor 11.5.
  for (uint32_t seed = 0; seed < 5; seed++) {
    {
      const SketchFeature sk =
          Traced({Sample(Boss(11.97), Noisy(0.1, 0.5), 70 + seed)});
      const auto cs = sketch::FindCandidates(sk, {});
      if (seed == 0)
        Print("boss 11.97", cs);
      const sketch::Candidate* d = Find(cs, sketch::CandidateKind::Diameter);
      CHECK(d != nullptr);
      if (d != nullptr)
        CHECK(d->nominal == 12.0);
    }
    {
      const SketchFeature sk =
          Traced({Sample(Boss(11.7), Noisy(0.1, 0.5), 80 + seed)});
      const auto cs = sketch::FindCandidates(sk, {});
      CHECK(Find(cs, sketch::CandidateKind::Diameter) == nullptr);
    }
  }
}

TEST_CASE("An inch document's sizes step in inches", "[sketch][candidates]") {
  const sketch::SizeSteps mm = sketch::StepsFor(units::Length::Millimetre);
  const sketch::SizeSteps in = sketch::StepsFor(units::Length::Inch);
  CHECK(mm.smallStep == 0.5);
  CHECK(mm.largeStep == 1.0);
  CHECK(std::abs(in.largeStep - 25.4 / 16) < 1e-12);
  // A 1/2" boss (12.7 mm) at 0.05 mm noise: 1/2" in an inch document, and
  // 12.7 mm offered nothing in a millimetre one (it is 13 less 0.3).
  const SketchFeature sk = Traced({Sample(Boss(12.7), Noisy(0.05, 0.3), 90)});
  sketch::CandidateOptions o;
  o.unit = units::Length::Inch;
  const sketch::Candidate* d =
      Find(sketch::FindCandidates(sk, o), sketch::CandidateKind::Diameter);
  REQUIRE(d != nullptr);
  CHECK(std::abs(d->nominal - 12.7) < 1e-9);
  CHECK(Find(sketch::FindCandidates(sk, {}), sketch::CandidateKind::Diameter) ==
        nullptr);
}

TEST_CASE("Accepting a slot's relations keeps it on its points",
          "[sketch][candidates]") {
  const double len = 12.0, r = 4.0;
  const double dr = len * std::sin(angles::ToRadians(0.1));
  SketchFeature sk = Traced({Sample(TaperedSlot(len, r + dr / 2, r - dr / 2),
                                    Noisy(0.05, 0.2, 17), 40)});
  const size_t before = sk.constraints.size();
  const sketch::AcceptResult res = sketch::AcceptCandidates(sk, {});
  for (const auto& a : res.accepted)
    std::printf("accepted %s (%.2f sigmas, moved %.4f mm)\n",
                a.candidate.key.c_str(), a.candidate.sigmas, a.moveMm);
  for (const auto& l : res.left)
    std::printf("left %s: %s\n", l.candidate.key.c_str(), l.reason.c_str());
  CHECK(sk.constraints.size() == before + res.accepted.size());
  // What was accepted is a basis: a row it implies is left as one the
  // sketch already fixes. Through the solve, the slot holds its relations:
  // its sides parallel, its ends of a radius and tangent at all four
  // joints.
  for (const auto& l : res.left)
    CHECK_THAT(l.reason, ContainsSubstring("already fixes"));
  const uint32_t a0 = TracedOfKind(sk, true, 0), a1 = TracedOfKind(sk, true, 1);
  const uint32_t l0 = TracedOfKind(sk, false, 0),
                 l1 = TracedOfKind(sk, false, 1);
  const auto& la = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l0)]);
  const auto& lb = std::get<sketch::Line>(sk.entities[sk.IndexOfEntity(l1)]);
  const glm::dvec2 da = glm::normalize(la.b - la.a),
                   db = glm::normalize(lb.b - lb.a);
  CHECK(std::abs(da.x * db.y - da.y * db.x) < 1e-6);
  const auto& aa = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(a0)]);
  const auto& ab = std::get<sketch::Arc>(sk.entities[sk.IndexOfEntity(a1)]);
  CHECK(std::abs(aa.radius - ab.radius) < 1e-6);
  CHECK(std::abs(aa.radius - 4.0) < 1e-6);
  for (const auto* l : {&la, &lb})
    for (const auto* a : {&aa, &ab}) {
      // Tangent: the line's distance from the centre is the radius.
      const glm::dvec2 d = glm::normalize(l->b - l->a);
      const double dist =
          std::abs(d.x * (a->center.y - l->a.y) - d.y * (a->center.x - l->a.x));
      CHECK(std::abs(dist - a->radius) < 1e-6);
    }
  // Its fit report within the noise...
  for (const sketch::TracedMiss& m : sketch::SectionFitReport(sk)) {
    INFO("entity " << m.from.entity << " sigma " << m.miss.sigmaMm);
    CHECK(m.cut);
    CHECK(m.miss.explained);
    CHECK(m.miss.sigmaMm < 1.5 * 0.05);
  }
  // ...and every accepted dimension at its nominal.
  sketch::Solver solver;
  solver.Load(sk);
  REQUIRE(solver.Solve().ok);
  for (size_t k = before; k < sk.constraints.size(); k++) {
    const sketch::Constraint& c = sk.constraints[k];
    if (!sketch::IsDimension(c.kind))
      continue;
    const auto measured = sketch::MeasureDimension(sk, k);
    REQUIRE(measured.has_value());
    CHECK(std::abs(*measured - c.value) < 1e-6);
  }
}

TEST_CASE("A relation that drags a piece off its points is left",
          "[sketch][candidates]") {
  // A slot 40 mm between centres and 3 degrees from parallel: forced
  // parallel (past any sigma the evidence gives it), its sides swing half a
  // millimetre off their points at the ends.
  const double len = 40.0, r = 6.0;
  const double dr = len * std::sin(angles::ToRadians(1.5));
  SketchFeature sk = Traced({Sample(TaperedSlot(len, r + dr / 2, r - dr / 2),
                                    Noisy(0.05, 0.2, 17), 41)});
  const uint32_t a = TracedOfKind(sk, false, 0), b = TracedOfKind(sk, false, 1);
  sketch::AcceptOptions o;
  o.maxSigmas = 1e9;
  o.only = {"parallel:" + std::to_string(std::min(a, b)) + "," +
            std::to_string(std::max(a, b))};
  const size_t before = sk.constraints.size();
  const sketch::AcceptResult res = sketch::AcceptCandidates(sk, o);
  REQUIRE(res.unknown.empty());
  REQUIRE(res.left.size() == 1);
  INFO(res.left[0].reason);
  CHECK_THAT(res.left[0].reason, ContainsSubstring("off its points"));
  CHECK(res.left[0].moveMm > 0.3);
  CHECK(sk.constraints.size() == before);
}

TEST_CASE("A relation the sketch already fixes is left as redundant",
          "[sketch][candidates]") {
  // A rounded rectangle, square to the axes: its four rounds are offered
  // equal pairwise, and once three pairs hold the rest repeat them.
  SketchFeature sk =
      Traced({Sample(RoundedRect(60, 40, 5), Noisy(0.03, 0.2), 42)});
  const sketch::AcceptResult res = sketch::AcceptCandidates(sk, {});
  int redundant = 0;
  for (const auto& l : res.left) {
    std::printf("left %s: %s\n", l.candidate.key.c_str(), l.reason.c_str());
    redundant += l.reason.find("already fixes") != std::string::npos;
  }
  CHECK(redundant >= 1);
  // ...and the four rounds are one radius.
  std::vector<double> radii;
  for (const sketch::Entity& e : sk.entities)
    if (const auto* a = std::get_if<sketch::Arc>(&e);
        a != nullptr && !sketch::IsConstruction(e))
      radii.push_back(a->radius);
  REQUIRE(radii.size() == 4);
  for (double r : radii)
    CHECK(std::abs(r - radii[0]) < 1e-6);
}

TEST_CASE("Accepting refuses keys no candidate has", "[sketch][candidates]") {
  SketchFeature sk =
      Traced({Sample(RoundedRect(60, 40, 5), Noisy(0.03, 0.2), 43)});
  sketch::AcceptOptions o;
  o.only = {"parallel:9999,10000"};
  const size_t before = sk.constraints.size();
  const sketch::AcceptResult res = sketch::AcceptCandidates(sk, o);
  CHECK(res.unknown.size() == 1);
  CHECK(res.accepted.empty());
  CHECK(sk.constraints.size() == before);
}
