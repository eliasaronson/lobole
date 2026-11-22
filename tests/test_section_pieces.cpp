// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The pieces fit of a section (docs/sketching/README.md#the-pieces-fit),
// against outlines generated from known lines and arcs, tilted in the
// plane and sampled with noise as a scan's cut is: the pieces come back as
// the generator's count of lines and arcs, each break within a few point
// spacings of the true tangent point, each radius within its stated
// uncertainty -- and those uncertainties hold as uncertainties over many
// noise draws.

#include "SameCore.h"
#include "SectionOutlines.h"

#include "core/Angles.h"
#include "core/sketch/SectionPieces.h"
#include "core/sketch/SketchSection.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kPi = angles::kPi;
using namespace outlines;

double CyclicGap(double a, double b, double total) {
  double d = std::fmod(std::abs(a - b), total);
  return std::min(d, total - d);
}

// The fit against the generator: count by kind, and each found piece
// matched to the primitive whose middle its middle is nearest (arc length).
struct Matched {
  bool countsMatch = false;
  std::vector<int> prim; // per piece
  double worstBreakSpacings = 0.0;
};

Matched Match(const sketch::PiecesFit& f, const std::vector<Prim>& prims,
              const Sampled& smp, double spacing) {
  Matched m;
  size_t lines = 0, arcs = 0, gl = 0, ga = 0;
  for (const auto& p : f.pieces)
    (p.arc ? arcs : lines)++;
  for (const auto& p : prims)
    (p.arc ? ga : gl)++;
  m.countsMatch = lines == gl && arcs == ga && f.pieces.size() == prims.size();
  if (!m.countsMatch)
    return m;
  // Breaks: each primitive's start against the nearest piece start.
  for (const double start : smp.starts) {
    double best = 1e300;
    for (const auto& p : f.pieces)
      best = std::min(best, CyclicGap(smp.s[p.from], start, smp.total));
    m.worstBreakSpacings = std::max(m.worstBreakSpacings, best / spacing);
  }
  for (const auto& p : f.pieces) {
    const size_t n = smp.s.size();
    const double from = smp.s[p.from];
    double len = smp.s[p.to] - from;
    if (len < 0.0)
      len += smp.total;
    (void)n;
    const double mid = std::fmod(from + 0.5 * len, smp.total);
    int best = -1;
    double bestD = 1e300;
    for (size_t i = 0; i < prims.size(); i++) {
      const double pm = smp.starts[i] + 0.5 * prims[i].Length();
      const double d = CyclicGap(mid, pm, smp.total);
      if (d < bestD) {
        bestD = d;
        best = static_cast<int>(i);
      }
    }
    m.prim.push_back(best);
  }
  return m;
}

// A found arc's radius, a found line's direction (radians, mod pi), in the
// order of the pieces.
double Radius(const sketch::Entity& e) {
  if (const auto* a = std::get_if<sketch::Arc>(&e))
    return a->radius;
  if (const auto* c = std::get_if<sketch::Circle>(&e))
    return c->radius;
  return 0.0;
}
glm::dvec2 Centre(const sketch::Entity& e) {
  if (const auto* a = std::get_if<sketch::Arc>(&e))
    return a->center;
  return glm::dvec2(0.0);
}
double LineAngleError(const sketch::Entity& e, const Prim& p, double tilt) {
  const auto* l = std::get_if<sketch::Line>(&e);
  if (l == nullptr)
    return 1e300;
  const glm::dvec2 d = l->b - l->a;
  const glm::dvec2 g = p.b - p.a;
  double err = std::atan2(d.y, d.x) - (std::atan2(g.y, g.x) + tilt);
  err = std::remainder(err, kPi);
  return std::abs(err);
}

struct Case {
  const char* name;
  std::vector<Prim> prims;
  Sampling sampling;
};

std::vector<Case> Cases() {
  std::vector<Case> c;
  c.push_back({"rounded rectangle 60 x 40, r 5", RoundedRect(60, 40, 5), {}});
  c.push_back({"slot 40 long, r 6", Slot(40, 6), {}});
  c.push_back({"keyhole r 10, stem 8 wide", Keyhole(10, 8, 30), {}});
  c.push_back({"D r 15, chord at 8", DShape(15, 8), {}});
  // A 1 mm round is 1.6 mm of outline: at 0.2 mm it is eight points, two
  // or three of which the walls' tangent ends take; at 0.1 mm, sixteen.
  Sampling fine;
  fine.spacing = 0.1;
  // ...and the scan fine enough to show one: 0.01 mm of noise. At 0.03 the
  // walls take the round's tangent ends, and two in five draws read one
  // round as a chamfer (docs/sketching/README.md#the-pieces-fit).
  fine.noise = 0.01;
  c.push_back(
      {"rectangle 60 x 40 with 1 mm rounds", RoundedRect(60, 40, 1), fine});
  Sampling wave;
  wave.wavy = 0;
  wave.waveMm = 0.05;
  std::vector<Prim> sharp = RoundedRect(60, 40, 0.0);
  std::vector<Prim> box;
  for (const Prim& p : sharp)
    if (!p.arc)
      box.push_back(p);
  c.push_back(
      {"rectangle 60 x 40, a 0.05 mm wave on its bottom wall", box, wave});
  return c;
}

} // namespace

TEST_CASE("The pieces fit finds the generator's lines and arcs",
          "[sketch][pieces]") {
  for (const Case& cs : Cases()) {
    for (const bool correlated : {false, true}) {
      Sampling o = cs.sampling;
      o.correlated = correlated;
      const Sampled smp = Sample(cs.prims, o, 1234);
      const sketch::PiecesFit f = sketch::FitPieces(smp.chain, 0, 0.1, 0.5);
      INFO(cs.name << (correlated ? ", correlated noise" : ", independent")
                   << ": " << f.pieces.size() << " pieces, noise "
                   << f.noise.sigma << " correlation " << f.noise.correlation);
      const Matched m = Match(f, cs.prims, smp, o.spacing);
      std::string kinds;
      for (const auto& p : f.pieces)
        kinds += std::string(p.arc ? " A" : " L") + std::to_string(p.points) +
                 "@" + std::to_string(p.from) + "(" +
                 std::to_string(p.sigmaMm) + ")";
      INFO("kinds " << kinds);
      REQUIRE(m.countsMatch);
      // Each break within a few point spacings of the true tangent point.
      CHECK(m.worstBreakSpacings <= 4.0);
      std::printf("pieces: %s%s: %zu pieces, worst break %.2f spacings, "
                  "noise %.4f mm (true %.3f), correlation %.2f\n",
                  cs.name, correlated ? " (correlated)" : "", f.pieces.size(),
                  m.worstBreakSpacings, f.noise.sigma, o.noise,
                  f.noise.correlation);
      for (size_t i = 0; i < f.pieces.size(); i++) {
        const auto& p = f.pieces[i];
        const Prim& g = cs.prims[static_cast<size_t>(m.prim[i])];
        REQUIRE(p.arc == g.arc);
        if (p.arc) {
          const double r = Radius(f.entities[i]);
          INFO("radius " << r << " sigma " << p.radiusSigmaMm << " true "
                         << g.r);
          const glm::dvec2 c = smp.Place(g.c);
          {
            CHECK(std::abs(r - g.r) <= 3.0 * p.radiusSigmaMm);
            CHECK(std::abs(Centre(f.entities[i]).x - c.x) <=
                  3.0 * p.centreSigmaMm.x);
            CHECK(std::abs(Centre(f.entities[i]).y - c.y) <=
                  3.0 * p.centreSigmaMm.y);
          }
          std::printf("  arc r %.4f +- %.4f (true %.3f)\n", r, p.radiusSigmaMm,
                      g.r);
        } else {
          const double err = LineAngleError(f.entities[i], g, smp.tilt);
          INFO("angle error " << angles::ToDegrees(err) << " deg, sigma "
                              << angles::ToDegrees(p.angleSigma));
          // A wavy wall's line is the wave's mean, not the generator's.
          if (static_cast<int>(m.prim[i]) != o.wavy)
            CHECK(err <= 4.0 * p.angleSigma); // one of ~40, at 2 sigma 96 %
        }
        CHECK(p.sigmaMm < 2.0 * o.noise + o.waveMm);
      }
    }
  }
}

TEST_CASE("The pieces fit's uncertainties hold over many noise draws",
          "[sketch][pieces][slow]") {
  // Over seeded draws, how often the true radius, centre and line angle
  // fall within two of the fit's own sigmas. A Gaussian's is 95.4 %.
  struct Rate {
    int in = 0, all = 0;
    double Of() const { return all ? double(in) / all : 0.0; }
  };
  // The 1 mm rounds apart: a few points decide where they touch the walls.
  for (const bool correlated : {false, true}) {
    for (const bool small : {false, true}) {
      Rate radius, centre, angle;
      int mismatched = 0, drawn = 0;
      const int draws = 150;
      for (const Case& cs : Cases()) {
        if (cs.sampling.wavy >= 0)
          continue; // its line is the wave's, not the generator's
        if ((cs.sampling.spacing < 0.2) != small)
          continue;
        for (int seed = 0; seed < draws; seed++) {
          Sampling o = cs.sampling;
          o.correlated = correlated;
          o.tiltDeg = 7.0 + 0.9 * seed;
          const Sampled smp = Sample(cs.prims, o, 1000 + seed);
          const sketch::PiecesFit f = sketch::FitPieces(smp.chain, 0, 0.1, 0.5);
          const Matched m = Match(f, cs.prims, smp, o.spacing);
          drawn++;
          if (!m.countsMatch) {
            mismatched++;
            continue;
          }
          for (size_t i = 0; i < f.pieces.size(); i++) {
            const auto& p = f.pieces[i];
            const Prim& g = cs.prims[static_cast<size_t>(m.prim[i])];
            if (p.arc != g.arc)
              continue;
            if (p.arc) {
              radius.all++;
              radius.in += std::abs(Radius(f.entities[i]) - g.r) <=
                           2.0 * p.radiusSigmaMm;
              const glm::dvec2 c = smp.Place(g.c);
              centre.all++;
              centre.in += std::abs(Centre(f.entities[i]).x - c.x) <=
                           2.0 * p.centreSigmaMm.x;
            } else {
              angle.all++;
              angle.in += LineAngleError(f.entities[i], g, smp.tilt) <=
                          2.0 * p.angleSigma;
            }
          }
        }
      }
      if (drawn == 0)
        continue;
      std::printf("pieces coverage (%s noise, %s): radius %.3f of %d, centre "
                  "x %.3f of %d, line angle %.3f of %d; %d of %d draws did "
                  "not match the generator's count\n",
                  correlated ? "correlated" : "independent",
                  small ? "1 mm rounds" : "the outlines", radius.Of(),
                  radius.all, centre.Of(), centre.all, angle.Of(), angle.all,
                  mismatched, drawn);
      // About as often as a Gaussian's two sigmas: neither far under (the
      // sigmas claim too much) nor at one (they claim too little). A small
      // round's sigmas are its fit held tangent to the walls
      // (sketch::FitTangentRound), which carries where it touches them:
      // 92-94 % (81 % and 68 % from the round's own points alone).
      const double floor = small ? 0.88 : 0.9;
      CHECK(radius.Of() > floor);
      CHECK(radius.Of() < 0.99);
      CHECK(centre.Of() > floor);
      CHECK(centre.Of() < 0.99);
      CHECK(angle.Of() > 0.9);
      CHECK(angle.Of() < 0.99);
      // The count: right in all but a few draws in a hundred.
      CHECK(mismatched * 20 < drawn);
    }
  }
}

TEST_CASE("The pieces fit draws an exact cut as its own pieces",
          "[sketch][pieces]") {
  // A B-Rep's section: no noise at all, the floor standing in for it.
  Sampling o;
  o.noise = 0.0;
  o.spacing = 0.05;
  const auto prims = RoundedRect(60, 40, 5);
  const Sampled smp = Sample(prims, o, 7);
  const sketch::PiecesFit f = sketch::FitPieces(smp.chain, 0, 0.1, 0.5);
  const Matched m = Match(f, prims, smp, o.spacing);
  REQUIRE(m.countsMatch);
  CHECK(f.deviation < 1e-3); // the noise floor's thousandth
  // Corners meet: each piece's end is the next one's start.
  for (size_t i = 0; i < f.entities.size(); i++) {
    const size_t j = (i + 1) % f.entities.size();
    const glm::dvec2 e = sketch::EntityPointUV(f.entities[i], f.ends[i].second);
    const glm::dvec2 s = sketch::EntityPointUV(f.entities[j], f.ends[j].first);
    CHECK(glm::length(e - s) < 1e-4);
  }
}

TEST_CASE("The pieces fit's detail trades pieces for fit", "[sketch][pieces]") {
  // A wall bowed by 0.3 mm over 60: at the least detail one line, at the
  // most an arc or more pieces than that.
  std::vector<Prim> box;
  for (const Prim& p : RoundedRect(60, 40, 0.0))
    if (!p.arc)
      box.push_back(p);
  Sampling o;
  o.wavy = 0;
  o.waveMm = 0.3;
  o.waveLength = 120.0;
  const Sampled smp = Sample(box, o, 99);
  const auto few = sketch::FitPieces(smp.chain, 0, 0.1, 0.0);
  const auto many = sketch::FitPieces(smp.chain, 0, 0.1, 1.0);
  INFO(few.pieces.size() << " at detail 0, " << many.pieces.size()
                         << " at detail 1");
  CHECK(few.pieces.size() <= many.pieces.size());
}

TEST_CASE("What the pieces fit costs on a realistic section",
          "[sketch][pieces]") {
  // A scan's cut of tens of thousands of points: a 600 x 400 rounded
  // rectangle sampled every 0.05 mm with 0.02 mm of noise. A cost check (CPU
  // seconds, docs/ui/testing.md#tests-that-time-things), with the greedy
  // arcs fit on the same chain for scale, both on one core. (Sampled every
  // 0.02 mm, closer than the noise is deep, a round's tangent stretch can
  // come back as a line of its own: docs/sketching/README.md#the-pieces-fit.)
  Sampling o;
  o.spacing = 0.05;
  o.noise = 0.02;
  const auto prims = RoundedRect(600, 400, 8);
  const Sampled smp = Sample(prims, o, 5);
  REQUIRE(smp.chain.points.size() > 20000);
  const SameCore held;
  const auto cpu = [] { return double(std::clock()) / CLOCKS_PER_SEC; };
  double t0 = cpu();
  const auto f = sketch::FitPieces(smp.chain, 0, 0.1, 0.5);
  const double pieces = cpu() - t0;
  t0 = cpu();
  const auto g = sketch::FitSection({smp.chain}, sketch::SectionFit::Arcs, 0.1);
  const double arcs = cpu() - t0;
  std::printf("pieces fit of %zu points: %.3f s of CPU, %zu pieces; the "
              "arcs fit %.3f s, %zu entities\n",
              smp.chain.points.size(), pieces, f.pieces.size(), arcs,
              g.entities.size());
  std::string kinds;
  for (const auto& p : f.pieces)
    kinds += std::string(p.arc ? " A" : " L") + std::to_string(p.points);
  INFO("kinds" << kinds);
  CHECK(f.pieces.size() == prims.size());
  // Measured at 0.1 s on a performance core; ten times that is the
  // budget, an efficiency core's three to five times with room.
  CHECK(pieces < 1.0);
}
