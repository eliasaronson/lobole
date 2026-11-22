// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchSpline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sketch {

void InsertKnot(std::vector<double>& U, std::vector<glm::dvec2>& P, int p,
                double u, std::vector<int>* source) {
  const int nn = static_cast<int>(P.size());
  // The span u lies in, U[k] <= u < U[k+1] -- past the last pole's span
  // when u is an unclamped end of the domain, which the formula allows.
  int k = p;
  while (k + 2 < static_cast<int>(U.size()) && U[k + 1] <= u)
    k++;
  const int sMul = static_cast<int>(std::count(U.begin(), U.end(), u));
  std::vector<glm::dvec2> Q(P.size() + 1);
  std::vector<int> S;
  if (source)
    S.assign(P.size() + 1, -1);
  for (int i = 0; i <= k - p; i++) {
    Q[i] = P[i];
    if (source)
      S[i] = (*source)[i];
  }
  for (int i = k - p + 1; i <= k - sMul; i++) {
    const double alpha = (u - U[i]) / (U[i + p] - U[i]);
    Q[i] = alpha * P[i] + (1.0 - alpha) * P[i - 1];
    if (source)
      S[i] = alpha == 1.0 ? (*source)[i] : alpha == 0.0 ? (*source)[i - 1] : -1;
  }
  for (int i = k - sMul; i < nn; i++) {
    Q[i + 1] = P[i];
    if (source)
      S[i + 1] = (*source)[i];
  }
  U.insert(U.begin() + k + 1, u);
  P = std::move(Q);
  if (source)
    *source = std::move(S);
}

namespace {

// de Boor on any knot vector, degree `p` >= 0, u clamped to the domain.
glm::dvec2 Eval(const std::vector<double>& U, const std::vector<glm::dvec2>& P,
                int p, double u) {
  const int n = static_cast<int>(P.size());
  if (n == 0)
    return glm::dvec2(0.0);
  u = std::clamp(u, U[p], U[n]);
  int k = p;
  while (k < n - 1 && U[k + 1] <= u)
    k++;
  std::vector<glm::dvec2> d(p + 1);
  for (int j = 0; j <= p; j++)
    d[j] = P[j + k - p];
  for (int r = 1; r <= p; r++)
    for (int j = p; j >= r; j--) {
      const double den = U[j + 1 + k - r] - U[j + k - p];
      const double a = den > 0.0 ? (u - U[j + k - p]) / den : 0.0;
      d[j] = (1.0 - a) * d[j - 1] + a * d[j];
    }
  return d[p];
}

// The hodograph of (U, P, p): knots U without its ends, degree p - 1.
void Hodograph(std::vector<double>& U, std::vector<glm::dvec2>& P, int& p) {
  std::vector<glm::dvec2> Q;
  for (size_t i = 0; i + 1 < P.size(); i++) {
    const double den = U[i + p + 1] - U[i + 1];
    Q.push_back(den > 0.0 ? (p / den) * (P[i + 1] - P[i]) : glm::dvec2(0.0));
  }
  U = std::vector<double>(U.begin() + 1, U.end() - 1);
  P = std::move(Q);
  p -= 1;
}

glm::dvec2 Deriv(const BSpline& s, double u, int order) {
  const int n = static_cast<int>(s.poles.size());
  int p = BSplineDegree(s);
  if (p < order)
    return glm::dvec2(0.0);
  std::vector<double> U = BSplineKnots(n, p);
  std::vector<glm::dvec2> P = s.poles;
  for (int i = 0; i < order; i++)
    Hodograph(U, P, p);
  return Eval(U, P, p, u);
}

} // namespace

glm::dvec2 BSplineDerivUV(const BSpline& s, double u) { return Deriv(s, u, 1); }
glm::dvec2 BSplineDeriv2UV(const BSpline& s, double u) {
  return Deriv(s, u, 2);
}

bool SplineClosed(const BSpline& s) {
  return s.poles.size() >= 3 &&
         glm::length(s.poles.front() - s.poles.back()) < 1e-9;
}

double SplineParamNearest(const BSpline& s, glm::dvec2 p) {
  const int n = static_cast<int>(s.poles.size());
  const int deg = BSplineDegree(s);
  if (n < 2 || deg == 0)
    return 0.0;
  const double umax = static_cast<double>(n - deg);
  const int samples = 64 * (n - deg);
  double best = 0.0, bestD = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= samples; i++) {
    const double u = umax * i / samples;
    const double d = glm::length(BSplineEvalUV(s, u) - p);
    if (d < bestD) {
      bestD = d;
      best = u;
    }
  }
  double u = best;
  for (int it = 0; it < 50; it++) {
    const glm::dvec2 r = BSplineEvalUV(s, u) - p;
    const glm::dvec2 d1 = BSplineDerivUV(s, u), d2 = BSplineDeriv2UV(s, u);
    const double f = glm::dot(r, d1);
    const double df = glm::dot(d1, d1) + glm::dot(r, d2);
    if (!(df > 0.0))
      break;
    const double next = std::clamp(u - f / df, 0.0, umax);
    if (std::abs(next - u) < 1e-15) {
      u = next;
      break;
    }
    // Only a step that does not move away is taken.
    if (glm::length(BSplineEvalUV(s, next) - p) > glm::length(r) + 1e-15)
      break;
    u = next;
  }
  return u;
}

std::optional<SplineCut> CutSpline(const BSpline& s, double u) {
  const int n = static_cast<int>(s.poles.size());
  const int p = BSplineDegree(s);
  if (n < 2 || p == 0)
    return std::nullopt;
  const int m = n - p;
  if (!(u > 0.0 && u < m))
    return std::nullopt;
  const double nearestKnot = std::round(u);
  const bool onKnot = std::abs(u - nearestKnot) < 1e-9;
  if (onKnot)
    u = nearestKnot;

  // The breaks between pieces, each raised to multiplicity p.
  std::vector<double> breaks;
  if (onKnot) {
    breaks.push_back(u);
  } else {
    const double k = std::floor(u);
    if (k > 0.0)
      breaks.push_back(k);
    breaks.push_back(u);
    if (k + 1.0 < m)
      breaks.push_back(k + 1.0);
  }
  std::vector<double> U = BSplineKnots(n, p);
  std::vector<glm::dvec2> P = s.poles;
  std::vector<int> src(n);
  for (int i = 0; i < n; i++)
    src[i] = i;
  for (double b : breaks)
    while (std::count(U.begin(), U.end(), b) < p)
      InsertKnot(U, P, p, b, &src);

  // The pole the curve passes through at break b: the one before b's first
  // knot (p equal knots there), the first pole at the start.
  const auto poleAt = [&](double b) -> int {
    if (b <= 0.0)
      return 0;
    const auto it = std::find(U.begin(), U.end(), b);
    return static_cast<int>(it - U.begin()) - 1;
  };
  std::vector<double> all{0.0};
  all.insert(all.end(), breaks.begin(), breaks.end());
  all.push_back(static_cast<double>(m));
  SplineCut cut;
  for (size_t i = 0; i + 1 < all.size(); i++) {
    const int a = poleAt(all[i]), c = poleAt(all[i + 1]);
    SplinePiece piece;
    piece.spline.degree = p;
    piece.spline.construction = s.construction;
    piece.spline.poles.assign(P.begin() + a, P.begin() + c + 1);
    piece.source.assign(src.begin() + a, src.begin() + c + 1);
    cut.pieces.push_back(std::move(piece));
    if (all[i + 1] <= u)
      cut.before = cut.pieces.size();
    if (i + 2 < all.size()) {
      const double b = all[i + 1];
      // Mid-span the curve is a polynomial; at a knot it is C^(p-1).
      const int smooth = b == u && !onKnot ? p + 1 : p - 1;
      cut.joints.push_back(smooth >= 2   ? PieceJoint::G2
                           : smooth == 1 ? PieceJoint::G1
                                         : PieceJoint::C0);
    }
  }
  // Mid-span at degree 1 the two pieces are collinear: tangent, not G2.
  if (p == 1)
    for (size_t j = 0; j < cut.joints.size(); j++)
      if (cut.joints[j] == PieceJoint::G2)
        cut.joints[j] = PieceJoint::G1;
  return cut;
}

} // namespace sketch
