// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/CamProfileMath.h"

#include "core/Angles.h"
#include "core/Occurrence.h" // CamLift: the cam relation's interpolation

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace cammech {

namespace {

constexpr double kPi = angles::kPi;

std::string Num(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.4g", v);
  return b;
}

glm::dvec2 J(glm::dvec2 v) { return {-v.y, v.x}; }
glm::dvec2 Rot(double a, glm::dvec2 v) {
  const double c = std::cos(a), s = std::sin(a);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}
double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

bool IsTurn(const FollowerSpec& f) { return f.motion == FollowerMotion::Turn; }
// A turning follower's lift is held in radians; the surface says degrees.
double LiftUnit(const FollowerSpec& f) {
  return IsTurn(f) ? angles::kRadPerDeg : 1.0;
}

// The follower's reference point in the ground frame and its first two
// derivatives per radian of cam turn: a roller's or knife tip's centre (the
// pitch point). Not used for a flat face.
struct Pitch {
  glm::dvec2 c, dc, ddc;
  glm::dvec2 motionDir; // unit: where positive lift moves the point
  std::string error;
};

struct TurnGeometry {
  glm::dvec2 pivot;
  double phi0 = 0.0;  // the arm's angle at zero lift
  double kappa = 1.0; // +1 when positive lift turns the arm ccw
  std::string error;
};

TurnGeometry SolveTurn(const FollowerSpec& f, double primeR) {
  TurnGeometry g;
  g.pivot = {f.pivotXMm, f.pivotYMm};
  const double d = glm::length(g.pivot);
  const double L = f.armMm;
  // The arm's circle about the pivot meets the prime circle where the
  // follower stands at zero lift.
  if (!(d > 0.0) || !(L > 0.0) || primeR > d + L || L > d + primeR ||
      d > L + primeR) {
    g.error = "the arm (" + Num(L) + " mm from a pivot " + Num(d) +
              " mm off the axis) cannot reach the cam's base circle (" +
              Num(primeR) + " mm, the follower's radius included)";
    return g;
  }
  // Law of cosines for the angle at the pivot between "toward the axis" and
  // the arm; two solutions, mirror images about the pivot's line. Take the
  // one nearer the follower's side (+X).
  const double cosA = (d * d + L * L - primeR * primeR) / (2 * d * L);
  const double A = std::acos(std::clamp(cosA, -1.0, 1.0));
  const double toAxis = std::atan2(-g.pivot.y, -g.pivot.x);
  double best = 0.0, bestScore = -1e300;
  for (double sgn : {1.0, -1.0}) {
    const double phi = toAxis + sgn * A;
    const glm::dvec2 c = g.pivot + L * glm::dvec2{std::cos(phi), std::sin(phi)};
    const double score = c.x / primeR - std::abs(c.y) / primeR * 1e-3;
    if (score > bestScore) {
      bestScore = score;
      best = phi;
    }
  }
  g.phi0 = best;
  const glm::dvec2 c0 =
      g.pivot + L * glm::dvec2{std::cos(best), std::sin(best)};
  const glm::dvec2 perp{-std::sin(best), std::cos(best)};
  const double radial = glm::dot(glm::normalize(c0), perp);
  if (std::abs(radial) < 1e-3) {
    g.error = "the arm points at the cam's axis: turning it does not move "
              "the follower toward or away from the cam";
    return g;
  }
  g.kappa = radial > 0 ? 1.0 : -1.0;
  return g;
}

Pitch PitchAt(const LiftAt& l, const FollowerSpec& f, double baseR,
              const TurnGeometry* tg) {
  Pitch p;
  const double primeR = baseR + f.radiusMm;
  if (!IsTurn(f)) {
    const double s0 =
        std::sqrt(std::max(0.0, primeR * primeR - f.offsetMm * f.offsetMm));
    p.c = {s0 + l.s, f.offsetMm};
    p.dc = {l.ds, 0.0};
    p.ddc = {l.dds, 0.0};
    p.motionDir = {1.0, 0.0};
    return p;
  }
  const double phi = tg->phi0 + tg->kappa * l.s;
  const glm::dvec2 u{std::cos(phi), std::sin(phi)};
  const glm::dvec2 up{-u.y, u.x};
  const double L = f.armMm;
  p.c = tg->pivot + L * u;
  p.dc = L * tg->kappa * l.ds * up;
  p.ddc = L * tg->kappa * l.dds * up - L * l.ds * l.ds * u;
  p.motionDir = tg->kappa * up;
  return p;
}

struct Sampled {
  glm::dvec2 contactCam;      // the profile point, in the cam's frame
  double pressure = 0.0;      // radians
  double profileRadius = 0.0; // signed: + convex, - concave, 0 straight
  bool undercut = false;
  double flatContact = 0.0;
};

Sampled SampleAt(const PreparedMotion& mo, const FollowerSpec& f, double baseR,
                 double sigma, const TurnGeometry* tg, double deg,
                 bool before) {
  const LiftAt l = mo.At(deg, before);
  const double th = angles::ToRadians(deg);
  Sampled out;
  if (f.kind == FollowerKind::Flat) {
    const double h = baseR + l.s;
    // Ground: the face's line x = h; the profile's point on it sits at
    // y = dh/dalpha with alpha = -sigma theta.
    out.flatContact = -sigma * l.ds;
    out.contactCam = Rot(-sigma * th, glm::dvec2{h, out.flatContact});
    out.profileRadius = h + l.dds;
    out.undercut = out.profileRadius <= 0.0;
    out.pressure = 0.0;
    return out;
  }
  const Pitch p = PitchAt(l, f, baseR, tg);
  // In the ground frame (the cam's rotation undone): the pitch curve's
  // tangent and second derivative as the cam sees them.
  const glm::dvec2 T = p.dc - sigma * J(p.c);
  const glm::dvec2 A = p.ddc - 2.0 * sigma * J(p.dc) - p.c;
  const double tl = glm::length(T);
  const double o = -sigma; // the pitch curve winds against the cam's turn
  const glm::dvec2 nOut = o * glm::dvec2{T.y, -T.x} / tl;
  const double k = Cross(T, A) / (tl * tl * tl);
  const double r = f.radiusMm;
  const bool convex = k * o > 0.0;
  const double rhoP = std::abs(k) > 1e-15 ? 1.0 / std::abs(k) : 1e15;
  out.undercut = r > 0.0 && convex && rhoP <= r;
  out.profileRadius = convex ? rhoP - r : -(rhoP + r);
  if (std::abs(k) <= 1e-15)
    out.profileRadius = 0.0;
  out.contactCam = Rot(-sigma * th, p.c - r * nOut);
  out.pressure =
      std::acos(std::clamp(std::abs(glm::dot(nOut, p.motionDir)), 0.0, 1.0));
  return out;
}

bool AnyUndercut(const PreparedMotion& mo, const FollowerSpec& f, double baseR,
                 double sigma, double stepDeg) {
  TurnGeometry tg;
  if (IsTurn(f)) {
    tg = SolveTurn(f, baseR + f.radiusMm);
    if (!tg.error.empty())
      return true;
  }
  for (double d = 0.0; d < 360.0; d += stepDeg)
    if (SampleAt(mo, f, baseR, sigma, &tg, d, false).undercut)
      return true;
  return false;
}

} // namespace

PreparedMotion::PreparedMotion(MotionSpec m, const FollowerSpec& f)
    : m_(std::move(m)), unit_(LiftUnit(f)) {
  if (!m_.lift.empty()) {
    table_ = m_.lift;
    for (auto& [a, l] : table_)
      l *= unit_;
  }
  double lo = std::numeric_limits<double>::infinity();
  for (int k = 0; k < 1440; k++)
    lo = std::min(lo, Raw(k * 0.25, false).s);
  for (const Drop& d : RawDrops())
    lo = std::min({lo, d.from, d.to});
  offset_ = std::isfinite(lo) ? lo : 0.0;
  drops_ = RawDrops();
  for (Drop& d : drops_) {
    d.from -= offset_;
    d.to -= offset_;
  }
}

LiftAt PreparedMotion::At(double deg, bool before) const {
  LiftAt r = Raw(deg, before);
  r.s -= offset_;
  return r;
}

std::vector<Drop> PreparedMotion::RawDrops() const {
  std::vector<Drop> out;
  double a0 = 0.0, l0 = 0.0;
  for (const Segment& s : m_.segments) {
    const double rise = s.rise * unit_;
    if (s.law == Law::Drop)
      out.push_back({std::fmod(a0, 360.0), l0, l0 + rise});
    a0 = s.toDeg;
    l0 += rise;
  }
  std::sort(out.begin(), out.end(),
            [](const Drop& a, const Drop& b) { return a.atDeg < b.atDeg; });
  return out;
}

LiftAt PreparedMotion::Raw(double deg, bool before) const {
  deg = std::fmod(deg, 360.0);
  if (deg < 0.0)
    deg += 360.0;
  if (!table_.empty()) {
    LiftAt r;
    const double rad = angles::ToRadians(deg);
    r.s = CamLift(table_, rad, &r.ds);
    const double h = 1e-4;
    double sp = 0.0, sm = 0.0;
    CamLift(table_, rad + h, &sp);
    CamLift(table_, rad - h, &sm);
    r.dds = (sp - sm) / (2.0 * h);
    return r;
  }
  // A drop at the very start of the turn is a drop at 360 seen from the
  // end: `before` there means the end of the last segment.
  if (before && deg == 0.0)
    deg = 360.0;
  double a0 = 0.0, l0 = 0.0;
  for (const Segment& s : m_.segments) {
    const double rise = s.rise * unit_;
    if (s.law == Law::Drop) {
      // The step belongs to the segment that follows it, unless the
      // caller asked for the side before it.
      if (deg == a0 && before)
        return {l0, 0.0, 0.0};
      l0 += rise;
      continue;
    }
    const bool last = (&s == &m_.segments.back());
    if (deg < s.toDeg || (last && deg <= s.toDeg) ||
        (before && deg == s.toDeg)) {
      const double beta = angles::ToRadians(s.toDeg - a0);
      const double u = beta > 0.0 ? (deg - a0) / (s.toDeg - a0) : 1.0;
      LiftAt r;
      const double h = rise;
      switch (s.law) {
      case Law::Cycloidal:
        r.s = h * (u - std::sin(2 * kPi * u) / (2 * kPi));
        r.ds = h / beta * (1 - std::cos(2 * kPi * u));
        r.dds = 2 * kPi * h / (beta * beta) * std::sin(2 * kPi * u);
        break;
      case Law::Harmonic:
        r.s = h / 2 * (1 - std::cos(kPi * u));
        r.ds = kPi * h / (2 * beta) * std::sin(kPi * u);
        r.dds = kPi * kPi * h / (2 * beta * beta) * std::cos(kPi * u);
        break;
      case Law::Poly345: {
        const double u2 = u * u, u3 = u2 * u;
        r.s = h * (10 * u3 - 15 * u3 * u + 6 * u3 * u2);
        r.ds = h / beta * (30 * u2 - 60 * u3 + 30 * u3 * u);
        r.dds = h / (beta * beta) * (60 * u - 180 * u2 + 120 * u3);
        break;
      }
      case Law::Dwell:
      case Law::Drop:
        break;
      }
      r.s += l0;
      return r;
    }
    a0 = s.toDeg;
    l0 += rise;
  }
  return {l0, 0.0, 0.0};
}

const char* LawName(Law l) {
  switch (l) {
  case Law::Cycloidal:
    return "cycloidal";
  case Law::Harmonic:
    return "harmonic";
  case Law::Poly345:
    return "poly345";
  case Law::Dwell:
    return "dwell";
  case Law::Drop:
    return "drop";
  }
  return "?";
}

bool LawFromName(const std::string& name, Law* out) {
  for (Law l :
       {Law::Cycloidal, Law::Harmonic, Law::Poly345, Law::Dwell, Law::Drop})
    if (name == LawName(l)) {
      *out = l;
      return true;
    }
  return false;
}

const char* FollowerKindName(FollowerKind k) {
  switch (k) {
  case FollowerKind::Roller:
    return "roller";
  case FollowerKind::Flat:
    return "flat";
  case FollowerKind::Knife:
    return "knife";
  }
  return "?";
}

const char* FollowerMotionName(FollowerMotion m) {
  return m == FollowerMotion::Turn ? "turn" : "slide";
}

std::string MotionError(const MotionSpec& m, const FollowerSpec& f) {
  if (m.lift.empty() == m.segments.empty())
    return "a cam profile's motion is EITHER `lift` (a table of [angle_deg, "
           "lift]) OR `segments` (a list of {to_deg, rise, law})";
  const std::string unit = IsTurn(f) ? "rise_deg" : "rise_mm";
  if (!m.lift.empty()) {
    if (m.lift.size() < 2)
      return "a lift table needs at least two points";
    for (size_t k = 0; k < m.lift.size(); k++) {
      const auto& [a, l] = m.lift[k];
      if (!std::isfinite(a) || !std::isfinite(l))
        return "a lift table's points are numbers";
      if (a < 0.0 || a >= 360.0)
        return "a lift table's angles lie in [0, 360) degrees: it wraps "
               "one turn (point " +
               std::to_string(k) + " is " + Num(a) + ")";
      if (k > 0 && !(a > m.lift[k - 1].first))
        return "a lift table's angles rise, each past the last (point " +
               std::to_string(k) + " does not)";
    }
    return {};
  }
  double a0 = 0.0, sum = 0.0;
  for (size_t k = 0; k < m.segments.size(); k++) {
    const Segment& s = m.segments[k];
    const std::string at = "segment " + std::to_string(k);
    if (!std::isfinite(s.toDeg) || !std::isfinite(s.rise))
      return at + "'s to_deg and " + unit + " are numbers";
    if (s.law == Law::Drop) {
      if (std::abs(s.toDeg - a0) > 1e-9)
        return at +
               " is a drop, a step with no width: its to_deg is where "
               "the segment before it ended (" +
               Num(a0) + ")";
      if (!(s.rise < 0.0))
        return at + " is a drop: the follower falls, so its " + unit +
               " is negative";
      if (a0 >= 360.0 - 1e-9 && k + 1 == m.segments.size() && k == 0)
        return at + ": a drop needs a motion around it";
    } else {
      if (!(s.toDeg > a0))
        return at + " ends at " + Num(s.toDeg) +
               " degrees, not past where "
               "the one before it ended (" +
               Num(a0) + ")";
      if (s.law == Law::Dwell && s.rise != 0.0)
        return at + " is a dwell: it does not rise";
      if (s.law != Law::Dwell && s.rise == 0.0)
        return at + " is a " + LawName(s.law) +
               " rise with no rise; a segment that holds still is a dwell";
    }
    a0 = s.toDeg;
    sum += s.rise;
  }
  if (std::abs(a0 - 360.0) > 1e-9)
    return "the segments end at " + Num(a0) +
           " degrees; they close one turn, ending at 360";
  if (std::abs(sum) > 1e-9)
    return "the segments rise " + Num(sum) + (IsTurn(f) ? " degrees" : " mm") +
           " over the turn; they return to where they started (rises and "
           "falls sum to zero)";
  return {};
}

std::string FollowerError(const FollowerSpec& f, double baseRadiusMm) {
  if (!(baseRadiusMm > 0.0))
    return "the base radius is positive";
  if (f.kind == FollowerKind::Flat && IsTurn(f))
    return "a flat-faced follower is cut for a sliding follower; a turning "
           "one takes `roller` or `knife`";
  if (f.kind == FollowerKind::Roller && !(f.radiusMm > 0.0))
    return "a roller follower's radius_mm is positive";
  if (f.kind == FollowerKind::Knife && f.radiusMm < 0.0)
    return "a knife follower's tip radius_mm is zero (sharp) or positive";
  if (!IsTurn(f)) {
    if (f.kind != FollowerKind::Flat &&
        std::abs(f.offsetMm) >= baseRadiusMm + f.radiusMm)
      return "the follower's line (offset " + Num(f.offsetMm) +
             " mm) misses the cam's base circle (" +
             Num(baseRadiusMm + f.radiusMm) +
             " mm, the follower's radius "
             "included)";
    return {};
  }
  return SolveTurn(f, baseRadiusMm + f.radiusMm).error;
}

LiftAt EvaluateMotion(const MotionSpec& m, const FollowerSpec& f, double deg) {
  return PreparedMotion(m, f).At(deg, false);
}

std::vector<Drop> MotionDrops(const MotionSpec& m, const FollowerSpec& f) {
  return PreparedMotion(m, f).Drops();
}

std::pair<double, double> MotionRange(const MotionSpec& m,
                                      const FollowerSpec& f) {
  const PreparedMotion mo(m, f);
  double lo = 1e300, hi = -1e300;
  for (int k = 0; k < 1440; k++) {
    const double s = mo.At(k * 0.25, false).s;
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  for (const Drop& d : mo.Drops()) {
    hi = std::max({hi, d.from, d.to});
    lo = std::min({lo, d.from, d.to});
  }
  return {lo, hi};
}

glm::dvec2 PitchPointGround(const MotionSpec& m, const FollowerSpec& f,
                            double baseRadiusMm, double deg) {
  const PreparedMotion mo(m, f);
  const LiftAt l = mo.At(deg, false);
  if (f.kind == FollowerKind::Flat)
    return {baseRadiusMm + l.s, f.offsetMm};
  TurnGeometry tg;
  if (IsTurn(f))
    tg = SolveTurn(f, baseRadiusMm + f.radiusMm);
  return PitchAt(l, f, baseRadiusMm, &tg).c;
}

double DesignedPressureAngle(const MotionSpec& m, const FollowerSpec& f,
                             double baseRadiusMm, bool ccw, double deg) {
  const PreparedMotion mo(m, f);
  TurnGeometry tg;
  if (IsTurn(f))
    tg = SolveTurn(f, baseRadiusMm + f.radiusMm);
  return SampleAt(mo, f, baseRadiusMm, ccw ? 1.0 : -1.0, &tg, deg, false)
      .pressure;
}

Profile BuildProfile(const MotionSpec& m, const FollowerSpec& f,
                     double baseRadiusMm, bool ccw, double stepDeg) {
  Profile out;
  if (std::string e = MotionError(m, f); !e.empty()) {
    out.error = e;
    return out;
  }
  if (std::string e = FollowerError(f, baseRadiusMm); !e.empty()) {
    out.error = e;
    return out;
  }
  const double sigma = ccw ? 1.0 : -1.0;
  const PreparedMotion mo(m, f);
  TurnGeometry tg;
  if (IsTurn(f))
    tg = SolveTurn(f, baseRadiusMm + f.radiusMm);
  out.drops = mo.Drops();

  // The spans between drops (one closed span when there is none).
  std::vector<std::pair<double, double>> spans;
  if (out.drops.empty()) {
    out.closed = true;
    spans.push_back({0.0, 360.0});
  } else {
    for (size_t k = 0; k < out.drops.size(); k++) {
      const double a = out.drops[k].atDeg;
      const double b = k + 1 < out.drops.size() ? out.drops[k + 1].atDeg
                                                : out.drops[0].atDeg + 360.0;
      spans.push_back({a, b});
    }
  }

  double minConvex = 1e300, minConcave = 1e300;
  double flatLo = 1e300, flatHi = -1e300;
  bool inUndercut = false;
  ThetaRange cur;
  for (auto [a, b] : spans) {
    ProfileSpan sp;
    sp.fromDeg = a;
    sp.toDeg = b;
    const int n = std::max(2, static_cast<int>(std::ceil((b - a) / stepDeg)));
    const double h = (b - a) / n;
    for (int i = 0; i <= n; i++) {
      if (out.closed && i == n)
        break; // periodic: the end is the start
      const double d = a + i * h;
      const bool before = (i == n);
      const Sampled smp = SampleAt(mo, f, baseRadiusMm, sigma, &tg, d, before);
      sp.points.push_back(smp.contactCam);
      out.maxRadiusMm = std::max(out.maxRadiusMm, glm::length(smp.contactCam));
      if (smp.pressure > angles::ToRadians(out.maxPressureAngleDeg)) {
        out.maxPressureAngleDeg = angles::ToDegrees(smp.pressure);
        out.maxPressureAtDeg = std::fmod(d, 360.0);
      }
      if (smp.profileRadius > 0.0)
        minConvex = std::min(minConvex, smp.profileRadius);
      else if (smp.profileRadius < 0.0)
        minConcave = std::min(minConcave, -smp.profileRadius);
      flatLo = std::min(flatLo, smp.flatContact);
      flatHi = std::max(flatHi, smp.flatContact);
      if (smp.undercut && !inUndercut) {
        inUndercut = true;
        cur.fromDeg = std::fmod(d, 360.0);
      }
      if (inUndercut) {
        cur.toDeg = std::fmod(d, 360.0);
        if (!smp.undercut) {
          inUndercut = false;
          out.undercut.push_back(cur);
        }
      }
      if (i < n) {
        const Sampled mid =
            SampleAt(mo, f, baseRadiusMm, sigma, &tg, d + 0.5 * h, false);
        sp.mid.push_back(mid.contactCam);
      }
    }
    out.spans.push_back(std::move(sp));
  }
  if (inUndercut)
    out.undercut.push_back(cur);
  out.minConvexRadiusMm = minConvex < 1e299 ? minConvex : 0.0;
  out.minConcaveRadiusMm = minConcave < 1e299 ? minConcave : 0.0;
  if (f.kind == FollowerKind::Flat) {
    out.flatContactMinMm = flatLo;
    out.flatContactMaxMm = flatHi;
    out.flatWidthMm = flatHi - flatLo;
  }

  if (!out.undercut.empty()) {
    // The smallest base radius that clears it: a flat face's is closed
    // form, a roller's is searched (the pitch curve's radius of curvature
    // grows with the base radius).
    double need = baseRadiusMm;
    if (f.kind == FollowerKind::Flat) {
      double worst = 0.0;
      for (int k = 0; k < 3600; k++) {
        const LiftAt l = mo.At(k * 0.1, false);
        worst = std::max(worst, -(l.s + l.dds));
      }
      need = worst;
    } else {
      double lo = baseRadiusMm, hi = baseRadiusMm * 2.0;
      while (AnyUndercut(mo, f, hi, sigma, stepDeg) && hi < 1e5)
        hi *= 2.0;
      while (hi - lo > 0.005) {
        const double midR = 0.5 * (lo + hi);
        (AnyUndercut(mo, f, midR, sigma, stepDeg) ? lo : hi) = midR;
      }
      need = hi;
    }
    out.minBaseRadiusMm = std::ceil(need * 100.0) / 100.0 + 0.01;
    std::string ranges;
    for (const ThetaRange& r : out.undercut) {
      if (!ranges.empty())
        ranges += ", ";
      ranges += Num(r.fromDeg) + "-" + Num(r.toDeg);
    }
    out.error = std::string("the profile is undercut (the ") +
                (f.kind == FollowerKind::Flat
                     ? "face cannot follow it"
                     : "follower is wider than the curve it rides") +
                ") at cam angles " + ranges +
                " degrees; a base radius of at least " +
                Num(out.minBaseRadiusMm) +
                " mm clears it, or a gentler law or a longer segment";
  }
  return out;
}

} // namespace cammech
