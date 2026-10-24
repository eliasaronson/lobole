// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The curvature half of a G2 joint, as a PlaneGCS constraint of our own
// (docs/sketching/README.md#curvature-continuity): added through the public
// System::addConstraint, so the vendored solver is not modified
// (THIRD_PARTY.md).
//
// A clamped spline's signed curvature at an end depends on its three poles
// nearest that end and the knot spacing there:
//   C'  = k1 d0,   C'' = k2 (q1 d1 - k1 d0),   d0 = P1 - P0, d1 = P2 - P1
//   kappa = C' x C'' / |C'|^3 = c (d0 x d1) / |d0|^3,   c = k2 q1 / k1^2
// with P0 the end pole and the curve read AWAY from that end. `c` is a
// constant of the knot vector (EndCurvatureFactor), so the error and its
// gradient are closed-form in the six pole coordinates.

#ifdef HAVE_PLANEGCS

#include "Constraints.h"

#include <array>

namespace sketch {

// c above for a clamped-uniform spline of `poles` poles and degree `degree`
// (>= 2), at its first end (`atStart`) or its last.
double EndCurvatureFactor(int poles, int degree, bool atStart);

class ConstraintEndCurvature : public GCS::Constraint {
public:
  // A spline end against an arc: kappa * r = sigma (sigma = +1 or -1, the
  // side read from the geometry at load).
  static ConstraintEndCurvature*
  SplineArc(const std::array<GCS::Point, 3>& ends, double factor,
            double* radius, double sigma);
  // Two spline ends: (kappa_a - sigma kappa_b) * length = 0, `length` a
  // constant that makes the error dimensionless.
  static ConstraintEndCurvature*
  SplineSpline(const std::array<GCS::Point, 3>& a, double factorA,
               const std::array<GCS::Point, 3>& b, double factorB, double sigma,
               double length);

  void errorgrad(double* err, double* grad, double* param) override;

private:
  ConstraintEndCurvature() = default;
  // kappa of the three poles at pvec[base..base+5], and d kappa / d each of
  // those six coordinates.
  double Kappa(size_t base, double factor, double* dk) const;

  bool m_twoSplines = false;
  double m_factorA = 0.0, m_factorB = 0.0;
  double m_sigma = 1.0;
  double m_length = 1.0;
};

} // namespace sketch

#endif // HAVE_PLANEGCS
