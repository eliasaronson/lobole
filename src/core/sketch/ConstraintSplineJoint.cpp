// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/ConstraintSplineJoint.h"

#ifdef HAVE_PLANEGCS

namespace sketch {

std::array<double, 3> EndDerivativeWeights(const std::vector<double>& knots,
                                           int degree, bool atStart,
                                           int order) {
  const int p = degree;
  std::vector<double> U = knots;
  if (!atStart) {
    // The same curve read from its other end: the knot vector mirrored.
    const double last = U.back();
    std::vector<double> rev(U.size());
    for (size_t i = 0; i < U.size(); i++)
      rev[i] = last - U[U.size() - 1 - i];
    U = std::move(rev);
  }
  if (p < order || U.size() < static_cast<size_t>(p + 3))
    return {0.0, 0.0, 0.0};
  const double k1 = p / (U[p + 1] - U[1]);
  if (order == 1)
    return {-k1, k1, 0.0};
  const double q1 = p / (U[p + 2] - U[2]);
  const double k2 = (p - 1) / (U[p + 1] - U[2]);
  return {k2 * k1, -k2 * (q1 + k1), k2 * q1};
}

ConstraintSplineJoint::ConstraintSplineJoint(const std::array<GCS::Point, 3>& a,
                                             const std::array<double, 3>& wa,
                                             const std::array<GCS::Point, 3>& b,
                                             const std::array<double, 3>& wb,
                                             int order, int axis,
                                             double scale) {
  for (const auto* side : {&a, &b})
    for (const GCS::Point& p : *side)
      pvec.push_back(axis == 0 ? p.x : p.y);
  origpvec = pvec;
  // Order 1: away-derivatives opposite, A + B = 0; order 2: equal, A - B.
  const double sb = order == 1 ? 1.0 : -1.0;
  for (int i = 0; i < 3; i++) {
    m_w[i] = wa[i] * scale;
    m_w[3 + i] = sb * wb[i] * scale;
  }
  rescale();
}

void ConstraintSplineJoint::errorgrad(double* err, double* grad,
                                      double* param) {
  if (err) {
    double e = 0.0;
    for (size_t i = 0; i < 6; i++)
      e += m_w[i] * *pvec[i];
    *err = e;
  }
  if (grad) {
    // A parameter can sit in pvec more than once once the solver has
    // merged coincident points (the joint pole is both sides' P0).
    double g = 0.0;
    for (size_t i = 0; i < 6; i++)
      if (pvec[i] == param)
        g += m_w[i];
    *grad = g;
  }
}

} // namespace sketch

#endif // HAVE_PLANEGCS
