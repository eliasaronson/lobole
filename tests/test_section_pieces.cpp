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

// One primitive of an outline: a line a -> b, or an arc about c of radius r
// from angle a0 through the signed sweep.
struct Prim {
  bool arc = false;
  glm::dvec2 a{0.0}, b{0.0};
  glm::dvec2 c{0.0};
  double r = 0.0, a0 = 0.0, sweep = 0.0;

  double Length() const {
    return arc ? r * std::abs(sweep) : glm::length(b - a);
  }
  glm::dvec2 At(double s) const {
    if (!arc)
      return a + (b - a) * (s / Length());
    const double t = a0 + sweep * (s / Length());
    return c + r * glm::dvec2(std::cos(t), std::sin(t));
  }
  glm::dvec2 Normal(double s) const {
    if (!arc) {
      const glm::dvec2 d = glm::normalize(b - a);
      return {-d.y, d.x};
    }
    return glm::normalize(At(s) - c);
  }
};

Prim L(glm::dvec2 a, glm::dvec2 b) { return Prim{false, a, b, {}, 0, 0, 0}; }
Prim A(glm::dvec2 c, double r, double a0Deg, double sweepDeg) {
  return Prim{true,
              {},
              {},
              c,
              r,
              angles::ToRadians(a0Deg),
              angles::ToRadians(sweepDeg)};
}

// A rounded rectangle w x h, corner radius r, run counter-clockwise from
// the bottom side.
std::vector<Prim> RoundedRect(double w, double h, double r) {
  const double x = w / 2, y = h / 2;
  return {L({-x + r, -y}, {x - r, -y}), A({x - r, -y + r}, r, -90, 90),
          L({x, -y + r}, {x, y - r}),   A({x - r, y - r}, r, 0, 90),
          L({x - r, y}, {-x + r, y}),   A({-x + r, y - r}, r, 90, 90),
          L({-x, y - r}, {-x, -y + r}), A({-x + r, -y + r}, r, 180, 90)};
}

// A slot: centres `len` apart, ends of radius r.
std::vector<Prim> Slot(double len, double r) {
  const double x = len / 2;
  return {L({-x, -r}, {x, -r}), A({x, 0}, r, -90, 180), L({x, r}, {-x, r}),
          A({-x, 0}, r, 90, 180)};
}

// A keyhole: a round head of radius R and a straight-sided stem of width w
// down to y = -depth, sharp where they meet.
std::vector<Prim> Keyhole(double R, double w, double depth) {
  const double hx = w / 2, hy = -std::sqrt(R * R - hx * hx);
  const double a0 = angles::ToDegrees(std::atan2(hy, hx));
  const double a1 = angles::ToDegrees(std::atan2(hy, -hx)) + 360.0;
  return {A({0, 0}, R, a0, a1 - a0), L({-hx, hy}, {-hx, -depth}),
          L({-hx, -depth}, {hx, -depth}), L({hx, -depth}, {hx, hy})};
}

// A D: a circle of radius R cut by the chord at x = d.
std::vector<Prim> DShape(double R, double d) {
  const double half = angles::ToDegrees(std::acos(d / R));
  const double y = std::sqrt(R * R - d * d);
  return {A({0, 0}, R, half, 360.0 - 2.0 * half), L({d, -y}, {d, y})};
}

struct Sampled {
  sketch::CutChain chain;
  std::vector<double> s;      // each point's arc length along the outline
  std::vector<double> starts; // each primitive's start (arc length)
  double total = 0.0;
  double tilt = 0.0;
  glm::dvec2 shift{0.0};
  glm::dvec2 Place(glm::dvec2 p) const {
    const double c = std::cos(tilt), sn = std::sin(tilt);
    return glm::dvec2(c * p.x - sn * p.y, sn * p.x + c * p.y) + shift;
  }
};

struct Sampling {
  double spacing = 0.2;    // mm between points, jittered
  double noise = 0.03;     // mm, normal to the outline
  bool correlated = false; // neighbouring points share their noise
  double tiltDeg = 17.0;
  glm::dvec2 shift{3.2, -1.7};
  // A low wave on primitive `wavy`: amplitude and wavelength (mm).
  int wavy = -1;
  double waveMm = 0.0, waveLength = 25.0;
};

// The outline sampled as a scan's cut is: about `spacing` apart (each step
// jittered by a third), moved along the outline's normal by noise.
Sampled Sample(const std::vector<Prim>& prims, const Sampling& o,
               uint32_t seed) {
  Sampled out;
  out.tilt = angles::ToRadians(o.tiltDeg);
  out.shift = o.shift;
  for (const Prim& p : prims) {
    out.starts.push_back(out.total);
    out.total += p.Length();
  }
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> jitter(-1.0 / 3.0, 1.0 / 3.0);
  std::normal_distribution<double> gauss(0.0, 1.0);
  double s = o.spacing * (0.5 + 0.5 * jitter(rng));
  std::vector<double> e;
  while (s < out.total) {
    out.s.push_back(s);
    s += o.spacing * (1.0 + jitter(rng));
  }
  e.resize(out.s.size() + 2);
  for (double& v : e)
    v = gauss(rng);
  size_t prim = 0;
  for (size_t k = 0; k < out.s.size(); k++) {
    while (prim + 1 < prims.size() && out.s[k] >= out.starts[prim + 1])
      prim++;
    const double local = out.s[k] - out.starts[prim];
    double n =
        o.correlated ? (e[k] + e[k + 1] + e[k + 2]) / std::sqrt(3.0) : e[k + 1];
    n *= o.noise;
    if (static_cast<int>(prim) == o.wavy)
      n += o.waveMm * std::sin(2.0 * kPi * local / o.waveLength + 0.7);
    const glm::dvec2 p = prims[prim].At(local) + prims[prim].Normal(local) * n;
    out.chain.points.push_back(out.Place(p));
  }
  out.chain.closed = true;
  return out;
}

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
          if (o.spacing < 0.2) {
            // A small round's sigma leaves out where its tangent points
            // are (the coverage test measures by how much): held to a
            // tenth of its radius instead.
            CHECK(std::abs(r - g.r) <= 0.1 * g.r);
          } else {
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
  // The small rounds apart: their sigmas do not carry where their tangent
  // points are, which a few points decide, so they are reported, not held.
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
      if (small)
        continue;
      // About as often as a Gaussian's two sigmas: neither far under (the
      // sigmas claim too much) nor at one (they claim too little).
      CHECK(radius.Of() > 0.9);
      CHECK(radius.Of() < 0.99);
      CHECK(centre.Of() > 0.9);
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
