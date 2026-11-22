// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Smooth (ConstraintKind::SplineJoint, docs/sketching/README.md#smooth): two
// splines of one degree that a Coincident joins end to end continue each
// other PARAMETRICALLY -- the first and (from degree 3) second derivatives
// agree at the joint, each read in its own spline's parameter, so two
// pieces cut from one spline (which keep the parent's knots) stay the one
// curve with a knot inserted. Linear in the poles: a clamped spline's
// derivatives at an end are fixed combinations of its three end poles,
//   C'  = k1 (P1 - P0),   C'' = k2 (q1 (P2 - P1) - k1 (P1 - P0)),
// k1 = p / (U[p+1] - U[1]), q1 = p / (U[p+2] - U[2]),
// k2 = (p-1) / (U[p+1] - U[2]), with P0 the end pole and U read away from
// that end (mirrored at a last end). Added through System::addConstraint,
// beside ConstraintEndCurvature, so the vendored PlaneGCS is not modified.

#ifdef HAVE_PLANEGCS

#include "Constraints.h"

#include <array>
#include <vector>

namespace sketch {

// The weights on (P0, P1, P2) of the derivative of `order` (1 or 2) read
// AWAY from an end of a clamped spline of degree `degree` on `knots` (the
// full vector), at its first end (`atStart`) or its last.
std::array<double, 3> EndDerivativeWeights(const std::vector<double>& knots,
                                           int degree, bool atStart, int order);

class ConstraintSplineJoint : public GCS::Constraint {
public:
  // One coordinate (`axis` 0 = x, 1 = y) of one derivative equation: the
  // first derivatives read away from the joint are opposite, the second
  // equal. `a`, `b`: each side's three poles from the joint; `wa`, `wb`
  // their EndDerivativeWeights; `scale` makes the error a length.
  ConstraintSplineJoint(const std::array<GCS::Point, 3>& a,
                        const std::array<double, 3>& wa,
                        const std::array<GCS::Point, 3>& b,
                        const std::array<double, 3>& wb, int order, int axis,
                        double scale);

  void errorgrad(double* err, double* grad, double* param) override;

private:
  std::array<double, 6> m_w{}; // on pvec[0..5]: A's three, then B's three
};

} // namespace sketch

#endif // HAVE_PLANEGCS
