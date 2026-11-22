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
  int p = BSplineDegree(s);
  if (p < order)
    return glm::dvec2(0.0);
  std::vector<double> U = SplineKnots(s);
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
  const auto [lo, hi] = SplineDomain(s);
  const int samples = 64 * SplineSpans(s);
  double best = lo, bestD = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= samples; i++) {
    const double u = lo + (hi - lo) * i / samples;
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
    const double next = std::clamp(u - f / df, lo, hi);
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

bool UniformUpToScale(const std::vector<double>& U, int p) {
  const int n = static_cast<int>(U.size()) - p - 1;
  if (p < 1 || n < p + 1)
    return false;
  const double a = U[p], b = U[n];
  if (!(b > a))
    return false;
  for (int i = 0; i <= p; i++)
    if (U[i] != a || U[n + i] != b)
      return false;
  const double step = (b - a) / (n - p);
  for (int i = 1; i < n - p; i++)
    if (!(std::abs(U[p + i] - (a + i * step)) <= 1e-12 * (b - a)))
      return false;
  return true;
}

BSpline SliceSpline(const std::vector<double>& U,
                    const std::vector<glm::dvec2>& P, int p, double b0,
                    double b1, const std::vector<int>* src,
                    std::vector<int>* srcOut) {
  const auto first = [&](double v) {
    return static_cast<int>(std::find(U.begin(), U.end(), v) - U.begin());
  };
  const auto mult = [&](double v) {
    return static_cast<int>(std::count(U.begin(), U.end(), v));
  };
  // The pole the curve passes through at b0, read leaving it, and at b1,
  // read arriving: past b0's knots by their excess over p, and the one
  // before b1's first.
  const int start = first(b0) + mult(b0) - p - 1;
  const int end = first(b1) - 1;
  BSpline out;
  out.degree = p;
  out.poles.assign(P.begin() + start, P.begin() + end + 1);
  if (srcOut && src)
    srcOut->assign(src->begin() + start, src->begin() + end + 1);
  out.knots.assign(static_cast<size_t>(p + 1), b0);
  for (double k : U)
    if (k > b0 && k < b1)
      out.knots.push_back(k);
  out.knots.insert(out.knots.end(), static_cast<size_t>(p + 1), b1);
  return out;
}

std::optional<SplineCut> CutSpline(const BSpline& s, double u) {
  const int n = static_cast<int>(s.poles.size());
  const int p = BSplineDegree(s);
  if (n < 2 || p == 0)
    return std::nullopt;
  const auto [lo, hi] = SplineDomain(s);
  if (!(u > lo && u < hi))
    return std::nullopt;
  std::vector<double> U = SplineKnots(s);
  // The distinct interior knots, and the one a cut that close is on.
  std::vector<double> inner;
  for (int i = p + 1; i < n; i++)
    if (inner.empty() || inner.back() != U[i])
      inner.push_back(U[i]);
  const double tol = 1e-9 * (hi - lo) / std::max(1, SplineSpans(s));
  bool onKnot = false;
  for (double k : inner)
    if (std::abs(u - k) < tol) {
      u = k;
      onKnot = true;
    }

  // How many times u is already a knot: what the uncut curve's continuity
  // there is (C^(p-m); mid-span the spline with u inserted once, C^(p-1)).
  const int had = static_cast<int>(std::count(U.begin(), U.end(), u));
  std::vector<glm::dvec2> P = s.poles;
  std::vector<int> src(n);
  for (int i = 0; i < n; i++)
    src[i] = i;
  while (std::count(U.begin(), U.end(), u) < p)
    InsertKnot(U, P, p, u, &src);
  SplineCut cut;
  for (const auto& [b0, b1] : {std::pair{lo, u}, std::pair{u, hi}}) {
    SplinePiece piece;
    piece.spline = SliceSpline(U, P, p, b0, b1, &src, &piece.source);
    piece.spline.construction = s.construction;
    BSpline plain = piece.spline;
    plain.knots.clear();
    if (SplineKnots(plain) == piece.spline.knots)
      piece.spline.knots.clear(); // the default, stored as no knots
    cut.pieces.push_back(std::move(piece));
  }
  cut.before = 1;
  const int continuity = p - std::max(had, 1);
  cut.joints.push_back(p == 1 && had == 0 ? PieceJoint::G1
                       : p >= 2 && continuity >= std::min(2, p - 1)
                           ? PieceJoint::Smooth
                       : continuity >= 1 ? PieceJoint::G1
                                         : PieceJoint::C0);
  return cut;
}

} // namespace sketch
