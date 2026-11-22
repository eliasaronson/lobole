// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Outlines generated from known lines and arcs, tilted in the plane and
// sampled with noise as a scan's cut is: the fixtures of the pieces fit
// (test_section_pieces.cpp) and of the candidates it supports
// (test_sketch_candidates.cpp).

#include "core/Angles.h"
#include "core/features/SketchFeature.h"

#include <cmath>
#include <random>
#include <vector>

namespace outlines {

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

inline Prim L(glm::dvec2 a, glm::dvec2 b) {
  return Prim{false, a, b, {}, 0, 0, 0};
}
inline Prim A(glm::dvec2 c, double r, double a0Deg, double sweepDeg) {
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
inline std::vector<Prim> RoundedRect(double w, double h, double r) {
  const double x = w / 2, y = h / 2;
  return {L({-x + r, -y}, {x - r, -y}), A({x - r, -y + r}, r, -90, 90),
          L({x, -y + r}, {x, y - r}),   A({x - r, y - r}, r, 0, 90),
          L({x - r, y}, {-x + r, y}),   A({-x + r, y - r}, r, 90, 90),
          L({-x, y - r}, {-x, -y + r}), A({-x + r, -y + r}, r, 180, 90)};
}

// A slot: centres `len` apart, ends of radius r.
inline std::vector<Prim> Slot(double len, double r) {
  const double x = len / 2;
  return {L({-x, -r}, {x, -r}), A({x, 0}, r, -90, 180), L({x, r}, {-x, r}),
          A({-x, 0}, r, 90, 180)};
}

// A keyhole: a round head of radius R and a straight-sided stem of width w
// down to y = -depth, sharp where they meet.
inline std::vector<Prim> Keyhole(double R, double w, double depth) {
  const double hx = w / 2, hy = -std::sqrt(R * R - hx * hx);
  const double a0 = angles::ToDegrees(std::atan2(hy, hx));
  const double a1 = angles::ToDegrees(std::atan2(hy, -hx)) + 360.0;
  return {A({0, 0}, R, a0, a1 - a0), L({-hx, hy}, {-hx, -depth}),
          L({-hx, -depth}, {hx, -depth}), L({hx, -depth}, {hx, hy})};
}

// A D: a circle of radius R cut by the chord at x = d.
inline std::vector<Prim> DShape(double R, double d) {
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
inline Sampled Sample(const std::vector<Prim>& prims, const Sampling& o,
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
      n += o.waveMm * std::sin(2.0 * angles::kPi * local / o.waveLength + 0.7);
    const glm::dvec2 p = prims[prim].At(local) + prims[prim].Normal(local) * n;
    out.chain.points.push_back(out.Place(p));
  }
  out.chain.closed = true;
  return out;
}

} // namespace outlines
