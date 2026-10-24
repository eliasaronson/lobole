// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/ConstraintEndCurvature.h"

#ifdef HAVE_PLANEGCS

#include "core/features/SketchFeature.h" // BSplineKnots

#include <cmath>

namespace sketch {

double EndCurvatureFactor(int poles, int degree, bool atStart) {
  const int n = poles, p = degree;
  if (p < 2 || n < p + 1)
    return 0.0;
  std::vector<double> U = BSplineKnots(n, p);
  if (!atStart) {
    // The same curve read from its other end: the knot vector mirrored.
    const double last = U.back();
    std::vector<double> rev(U.size());
    for (size_t i = 0; i < U.size(); i++)
      rev[i] = last - U[U.size() - 1 - i];
    U = std::move(rev);
  }
  const double k1 = p / (U[p + 1] - U[1]);
  const double q1 = p / (U[p + 2] - U[2]);
  const double k2 = (p - 1) / (U[p + 1] - U[2]);
  return k2 * q1 / (k1 * k1);
}

ConstraintEndCurvature*
ConstraintEndCurvature::SplineArc(const std::array<GCS::Point, 3>& ends,
                                  double factor, double* radius, double sigma) {
  auto* c = new ConstraintEndCurvature;
  for (const GCS::Point& p : ends) {
    c->pvec.push_back(p.x);
    c->pvec.push_back(p.y);
  }
  c->pvec.push_back(radius);
  c->origpvec = c->pvec;
  c->m_factorA = factor;
  c->m_sigma = sigma;
  c->rescale();
  return c;
}

ConstraintEndCurvature* ConstraintEndCurvature::SplineSpline(
    const std::array<GCS::Point, 3>& a, double factorA,
    const std::array<GCS::Point, 3>& b, double factorB, double sigma,
    double length) {
  auto* c = new ConstraintEndCurvature;
  for (const auto* side : {&a, &b})
    for (const GCS::Point& p : *side) {
      c->pvec.push_back(p.x);
      c->pvec.push_back(p.y);
    }
  c->origpvec = c->pvec;
  c->m_twoSplines = true;
  c->m_factorA = factorA;
  c->m_factorB = factorB;
  c->m_sigma = sigma;
  c->m_length = length > 0.0 ? length : 1.0;
  c->rescale();
  return c;
}

double ConstraintEndCurvature::Kappa(size_t base, double factor,
                                     double* dk) const {
  const double p0x = *pvec[base], p0y = *pvec[base + 1];
  const double p1x = *pvec[base + 2], p1y = *pvec[base + 3];
  const double p2x = *pvec[base + 4], p2y = *pvec[base + 5];
  const double d0x = p1x - p0x, d0y = p1y - p0y;
  const double d1x = p2x - p1x, d1y = p2y - p1y;
  const double cross = d0x * d1y - d0y * d1x;
  const double L2 = d0x * d0x + d0y * d0y;
  const double L = std::sqrt(L2);
  if (!(L > 0.0)) {
    for (int i = 0; i < 6; i++)
      dk[i] = 0.0;
    return 0.0;
  }
  const double L3 = L2 * L, L5 = L3 * L2;
  // d kappa / d d0 and d kappa / d d1 (factor applied below).
  const double gd0x = d1y / L3 - 3.0 * cross * d0x / L5;
  const double gd0y = -d1x / L3 - 3.0 * cross * d0y / L5;
  const double gd1x = -d0y / L3;
  const double gd1y = d0x / L3;
  // d0 = P1 - P0, d1 = P2 - P1.
  dk[0] = -gd0x * factor;
  dk[1] = -gd0y * factor;
  dk[2] = (gd0x - gd1x) * factor;
  dk[3] = (gd0y - gd1y) * factor;
  dk[4] = gd1x * factor;
  dk[5] = gd1y * factor;
  return factor * cross / L3;
}

void ConstraintEndCurvature::errorgrad(double* err, double* grad,
                                       double* param) {
  double dA[6], dB[6];
  const double kA = Kappa(0, m_factorA, dA);
  if (!m_twoSplines) {
    const double r = *pvec[6];
    if (err)
      *err = kA * r - m_sigma;
    if (grad) {
      // A parameter can sit in pvec more than once once the solver has
      // merged coincident points: every slot it fills contributes.
      double g = 0.0;
      for (size_t i = 0; i < 6; i++)
        if (pvec[i] == param)
          g += dA[i] * r;
      if (pvec[6] == param)
        g += kA;
      *grad = g;
    }
    return;
  }
  const double kB = Kappa(6, m_factorB, dB);
  if (err)
    *err = (kA - m_sigma * kB) * m_length;
  if (grad) {
    double g = 0.0;
    for (size_t i = 0; i < 6; i++) {
      if (pvec[i] == param)
        g += dA[i];
      if (pvec[6 + i] == param)
        g -= m_sigma * dB[i];
    }
    *grad = g * m_length;
  }
}

} // namespace sketch

#endif // HAVE_PLANEGCS
