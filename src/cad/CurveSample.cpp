// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/CurveSample.h"

#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GCPnts_UniformDeflection.hxx>
#include <gp_Vec.hxx>

namespace cad {

namespace {

// How far the curve's point halfway along a chord's parameters lies off it.
double MidOff(const Adaptor3d_Curve& c, const CurveSample& a,
              const CurveSample& b) {
  const gp_Pnt m = c.Value(0.5 * (a.u + b.u));
  const gp_Vec ab(a.p, b.p), am(a.p, m);
  const double len = ab.Magnitude();
  return len > 0.0 ? ab.Crossed(am).Magnitude() / len : am.Magnitude();
}

// Whether some chord's middle lies well off the curve. Ten times the
// deflection: the quasi-uniform sampler only approximately honours it, and a
// collapsed span strays by the size of the shape, not by a tolerance.
bool Strays(const Adaptor3d_Curve& c, const std::vector<CurveSample>& s,
            double deflection) {
  for (size_t i = 0; i + 1 < s.size(); i++)
    if (MidOff(c, s[i], s[i + 1]) > 10.0 * deflection)
      return true;
  return false;
}

// Appends the points after `a` up to and including `b`, halving the chord
// while its middle lies more than `deflection` off the curve.
// GCPnts_UniformDeflection overshoots too (half as much again on a
// near-closed spline on 7.6), and the fallback is the one sampling here that
// owes nothing to keeping an earlier output.
void Refine(const Adaptor3d_Curve& c, const CurveSample& a,
            const CurveSample& b, double deflection, int depth,
            std::vector<CurveSample>& out) {
  if (depth > 0 && MidOff(c, a, b) > deflection) {
    const double u = 0.5 * (a.u + b.u);
    const CurveSample m{u, c.Value(u)};
    Refine(c, a, m, deflection, depth - 1, out);
    Refine(c, m, b, deflection, depth - 1, out);
    return;
  }
  out.push_back(b);
}

} // namespace

std::vector<CurveSample> SampleCurve(const Adaptor3d_Curve& c,
                                     double deflection) {
  std::vector<CurveSample> s;
  GCPnts_QuasiUniformDeflection quasi(c, deflection);
  if (quasi.IsDone())
    for (int i = 1; i <= quasi.NbPoints(); i++)
      s.push_back({quasi.Parameter(i), quasi.Value(i)});
  if (s.size() >= 2 && !Strays(c, s, deflection))
    return s;
  s.clear();
  GCPnts_UniformDeflection uniform(c, deflection);
  if (!uniform.IsDone() || uniform.NbPoints() < 2)
    return s;
  s.push_back({uniform.Parameter(1), uniform.Value(1)});
  for (int i = 1; i < uniform.NbPoints(); i++) {
    const CurveSample a = s.back(); // a copy: Refine grows `s`
    Refine(c, a, {uniform.Parameter(i + 1), uniform.Value(i + 1)}, deflection,
           8, s);
  }
  return s;
}

} // namespace cad
