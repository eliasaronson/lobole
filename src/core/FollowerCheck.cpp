// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/FollowerCheck.h"

#include "core/Angles.h"
#include "core/Conventions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace follower {

namespace {

constexpr double kTwoPi = angles::kTwoPi;

double Wrap2Pi(double a) {
  a = std::fmod(a, kTwoPi);
  return a < 0.0 ? a + kTwoPi : a;
}
double WrapDeg(double d) {
  d = std::fmod(d, 360.0);
  return d < 0.0 ? d + 360.0 : d;
}

double RpmOf(double radPerS) { return radPerS * 60.0 / kTwoPi; }

// Ranges of consecutive flagged grid points, in cam degrees.
std::vector<Range> RangesOf(const std::vector<double>& deg,
                            const std::vector<bool>& flag) {
  std::vector<Range> out;
  for (size_t i = 0; i < deg.size(); i++) {
    if (!flag[i])
      continue;
    if (!out.empty() && i > 0 && flag[i - 1]) {
      out.back().toDeg = deg[i];
      continue;
    }
    out.push_back({deg[i], deg[i]});
  }
  // A range that runs through 0 is one range.
  if (out.size() > 1 && flag.front() && flag.back()) {
    out.front().fromDeg = out.back().fromDeg;
    out.pop_back();
  }
  return out;
}

} // namespace

namespace {
// Whether cam angle `a` lies in a drop, where the follower is in free fall
// and no contact describes it.
bool InDrop(const contact::Table& t, double a) {
  for (const contact::Table::DropAt& d : t.drops)
    if (Wrap2Pi(a - d.a) <= Wrap2Pi(d.aEnd - d.a))
      return true;
  return false;
}
} // namespace

namespace {

// The free fall from `from` to `to` (b's unit; `unit` to SI), from rest,
// over the mass function. Energy is all there is: d/dt(M q'^2 / 2) =
// Q q', so (1/2) M(q) q'^2 = W(q), gravity's work from the top, and the
// time is the integral of dq / q'. Read at the top alone -- sqrt(2 h M /
// |Q|) -- a lever carried through a loop stands nearest upright there,
// with the most inertia per unit of pull, and the woodcutter's axe fell
// in 155 ms where it falls in 106. Stepped in u = sqrt(distance fallen),
// in which the integrand stays finite at the top, so constant M and Q
// give the closed form exactly.
double FallTime(const std::function<MassAt(double)>& mass, double from,
                double to, double unit) {
  const double h = std::abs(to - from);
  if (h <= 0.0)
    return 0.0;
  const double dir = to > from ? 1.0 : -1.0;
  constexpr int kSteps = 400;
  constexpr int kSub = 8; // the work's sub-steps between two u nodes
  const double du = std::sqrt(h) / kSteps;
  double t = 0.0, work = 0.0, sPrev = 0.0;
  for (int i = 0; i < kSteps; i++) {
    const double u = (i + 0.5) * du;
    const double s = u * u;
    // Gravity's work from the last point to this one (midpoint rule).
    for (int k = 0; k < kSub; k++) {
      const double a = sPrev + (s - sPrev) * (k + 0.5) / kSub;
      work += std::abs(mass(from + dir * a).Q) * (s - sPrev) / kSub * unit;
    }
    sPrev = s;
    const MassAt ms = mass(from + dir * s);
    if (work <= 0.0 || ms.M <= 0.0)
      return std::numeric_limits<double>::infinity();
    const double v = std::sqrt(2.0 * work / ms.M); // SI per second
    t += 2.0 * u * du * unit / v;
  }
  return t;
}

} // namespace

Report Check(const Input& in) {
  Report r;
  const contact::Model& m = *in.model;
  r.turn = m.turn;
  const double tol = m.tolerance;
  const auto touch = [&](double a) {
    return contact::FirstTouch(m.camOutline, m.camSense * a, m.followerOutline,
                               m.path, m.toward, 0.0, tol);
  };

  // ---- per sample: the pressure angle, the radii, bridging ----------------
  r.maxPressureDeg = 0.0;
  double minConvex = 1e300, minConcave = 1e300, folR = 1e300;
  for (const Sample& s : in.samples) {
    if (InDrop(m.table, s.a)) {
      r.inDrop++;
      continue;
    }
    const contact::Touch t = touch(s.a);
    SampleOut o;
    o.index = s.index;
    o.camDeg = WrapDeg(angles::ToDegrees(s.a));
    if (!t.found)
      continue;
    o.pressureDeg = angles::ToDegrees(std::acos(
        std::clamp(std::abs(glm::dot(t.normal, t.motion)), 0.0, 1.0)));
    o.camRadius = t.camRadius;
    o.followerRadius = t.followerRadius;
    o.bridge = t.bridge;
    if (o.pressureDeg > r.maxPressureDeg) {
      r.maxPressureDeg = o.pressureDeg;
      r.maxPressureAtDeg = o.camDeg;
    }
    if (t.camRadius > 0.0)
      minConvex = std::min(minConvex, t.camRadius);
    else if (t.camRadius < 0.0)
      minConcave = std::min(minConcave, -t.camRadius);
    if (t.followerRadius > 0.0)
      folR = std::min(folR, t.followerRadius);
    r.samples.push_back(o);
  }
  r.minCamConvex = minConvex < 1e299 ? minConvex : 0.0;
  r.minCamConcave = minConcave < 1e299 ? minConcave : 0.0;
  r.followerRadius = folR < 1e299 ? folR : 0.0;
  for (const auto& [a0, a1] : m.table.bridges)
    r.bridges.push_back({angles::ToDegrees(a0), angles::ToDegrees(a1)});
  if (in.maxPressureDeg)
    r.pressureOk = r.maxPressureDeg <= *in.maxPressureDeg;

  // ---- the realised motion against the designed one -----------------------
  if (in.designed && !in.samples.empty()) {
    const Designed& d = *in.designed;
    const cammech::PreparedMotion motion(d.motion, d.follower);
    const double sigma = d.ccw ? 1.0 : -1.0;
    const bool turnF = d.follower.motion == cammech::FollowerMotion::Turn;
    // Near a drop the follower rounds the step's corner, which the law
    // does not describe: those samples are left out.
    const double window =
        2.0 + 2.0 * angles::ToDegrees(std::asin(std::clamp(
                        d.follower.radiusMm / std::max(d.baseRadius, 1e-9), 0.0,
                        1.0)));
    std::vector<double> res;
    std::vector<size_t> at;
    for (size_t k = 0; k < in.samples.size(); k++) {
      const Sample& s = in.samples[k];
      if (InDrop(m.table, s.a))
        continue;
      const double c = std::cos(m.camSense * s.a),
                   sn = std::sin(m.camSense * s.a);
      const glm::dmat4 rz(glm::dvec4(c, sn, 0, 0), glm::dvec4(-sn, c, 0, 0),
                          glm::dvec4(0, 0, 1, 0), glm::dvec4(0, 0, 0, 1));
      const glm::dmat4 F = rz * m.camInPlane0 * d.frameInCamPart;
      const glm::dvec3 X(F[0]), Y(F[1]), O(F[3]);
      double theta = 0.0;
      if (!turnF) {
        const glm::dvec3 dir(m.path.dir, 0.0);
        const double beta = std::atan2(glm::dot(dir, Y), glm::dot(dir, X));
        theta = -sigma * beta;
      } else {
        const glm::dvec3 P = glm::dvec3(m.path.pivot, 0.0) - O;
        const double pv = std::atan2(glm::dot(P, Y), glm::dot(P, X));
        theta =
            sigma * (std::atan2(d.follower.pivotYMm, d.follower.pivotXMm) - pv);
      }
      const double deg = WrapDeg(angles::ToDegrees(theta));
      bool nearDrop = false;
      for (const cammech::Drop& dr : motion.Drops()) {
        const double gap = std::abs(WrapDeg(deg - dr.atDeg + 180.0) - 180.0);
        nearDrop = nearDrop || gap < window;
      }
      if (nearDrop)
        continue;
      const contact::Touch t = touch(s.a);
      const double kappa =
          t.found && glm::dot(t.motion, t.point) < 0.0 ? -1.0 : 1.0;
      res.push_back(s.b - kappa * motion.At(deg).s);
      at.push_back(k);
      (void)turnF;
    }
    if (!res.empty()) {
      std::vector<double> sorted = res;
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2,
                       sorted.end());
      r.mateZero = sorted[sorted.size() / 2];
      r.deviationKnown = true;
      r.fitDeviation = d.fitDeviation;
      for (size_t i = 0; i < res.size(); i++) {
        const double dv = std::abs(res[i] - r.mateZero);
        for (SampleOut& o : r.samples)
          if (o.index == in.samples[at[i]].index)
            o.deviation = dv;
        if (dv > r.deviation) {
          r.deviation = dv;
          r.deviationAtDeg = WrapDeg(angles::ToDegrees(in.samples[at[i]].a));
        }
      }
    }
  }

  // ---- gravity and speed ---------------------------------------------------
  if (in.mass) {
    r.gravityKnown = true;
    const double unit = m.turn ? 1.0 : 1e-3; // q to SI
    // The derivatives' step. A touch is exact to some 1e-5 mm, which a
    // second difference over a milliradian made into +-0.03 m/rad^2 and
    // put a liftoff in the middle of a rise (a 2.7 g follower "left" a
    // cycloidal rise at 106 rpm). Over 0.02 rad that noise is a 400th of
    // it, and the truncation, q'''' h^2 / 12, smaller still for any cam
    // that can be made.
    const double h = 0.02; // rad of cam
    const int n = 720;
    const int t = m.toward;
    double bestW = std::numeric_limits<double>::infinity();
    std::vector<double> degs;
    std::vector<bool> off;
    for (int k = 0; k < n; k++) {
      const double a = kTwoPi * k / n;
      bool nearDrop = false;
      for (const contact::Table::DropAt& dr : m.table.drops) {
        const double span = Wrap2Pi(dr.aEnd - dr.a);
        nearDrop = nearDrop || Wrap2Pi(a - dr.a + 4.0 * h) < span + 8.0 * h;
      }
      if (nearDrop)
        continue;
      const contact::Touch t0 = touch(a), tp = touch(a + h), tm = touch(a - h);
      if (!t0.found || !tp.found || !tm.found)
        continue;
      const double q = t0.travel;
      const double q1 = (tp.travel - tm.travel) / (2.0 * h) * unit;
      const double q2 =
          (tp.travel - 2.0 * t0.travel + tm.travel) / (h * h) * unit;
      const MassAt ms = in.mass(q);
      const double B = ms.M * q2 + 0.5 * ms.dM * q1 * q1;
      degs.push_back(angles::ToDegrees(a));
      off.push_back(t * ms.Q <= 0.0);
      if (t * ms.Q <= 0.0)
        continue;
      if (t * B > 0.0) {
        const double w = std::sqrt(t * ms.Q / (t * B));
        if (w < bestW) {
          bestW = w;
          r.liftoffAtDeg = angles::ToDegrees(a);
        }
      }
    }
    r.notHeld = RangesOf(degs, off);
    const double ratio = std::abs(in.driverPerCam);
    if (std::isfinite(bestW)) {
      r.liftoffCamRpm = RpmOf(bestW);
      r.liftoffRpm = *r.liftoffCamRpm * ratio;
    }
    // Each drop: the free fall, and how long the cam holds the level it
    // lands on.
    for (const contact::Table::DropAt& dr : m.table.drops) {
      DropOut o;
      o.atDeg = WrapDeg(angles::ToDegrees(dr.a));
      o.jams = m.toward * (dr.to - dr.from) < 0.0;
      o.height = std::abs(dr.from - dr.to);
      o.fallS = FallTime(in.mass, dr.from, dr.to, unit);
      // Landed while the cam still stands within a few tolerances of the
      // level it fell to: past that, it lands on the next rise.
      const double band = 10.0 * m.table.tolerance;
      double dwell = 0.0;
      for (double da = 1e-4; da < kTwoPi; da += angles::ToRadians(0.1)) {
        if (std::abs(contact::TableValue(m.table, dr.aEnd + da) - dr.to) > band)
          break;
        dwell = da;
      }
      o.dwellDeg = angles::ToDegrees(dwell);
      o.dropCamRpm = std::isfinite(o.fallS) && o.fallS > 0.0
                         ? RpmOf(dwell / o.fallS)
                         : 0.0;
      o.dropRpm = o.dropCamRpm * ratio;
      if (in.speedRpm) {
        const double w = *in.speedRpm / ratio * kTwoPi / 60.0;
        o.turnDeg = angles::ToDegrees(w * o.fallS);
        o.lands = !o.jams && w * o.fallS <= dwell;
      }
      r.drops.push_back(o);
    }
    // The contact force and the shaft's torque at each sample, at the
    // study driver's speed: the same derivatives as the liftoff, at the
    // sample's own angle.
    if (in.speedRpm) {
      const double w = *in.speedRpm / ratio * kTwoPi / 60.0;
      double bestN = -1.0, bestT = -1.0;
      for (size_t k = 0; k < r.samples.size(); k++) {
        SampleOut& o = r.samples[k];
        const double a = angles::ToRadians(o.camDeg);
        bool nearDrop = false;
        for (const contact::Table::DropAt& dr : m.table.drops) {
          const double span = Wrap2Pi(dr.aEnd - dr.a);
          nearDrop = nearDrop || Wrap2Pi(a - dr.a + 4.0 * h) < span + 8.0 * h;
        }
        if (nearDrop)
          continue;
        const contact::Touch t0 = touch(a), tp = touch(a + h),
                             tm = touch(a - h);
        if (!t0.found || !tp.found || !tm.found)
          continue;
        const double cosN = glm::dot(t0.normal, t0.motion);
        const double lever =
            m.turn ? glm::length(t0.point - m.path.pivot) * 1e-3 : 1.0;
        if (std::abs(cosN) * lever < 1e-9)
          continue;
        o.q1 = (tp.travel - tm.travel) / (2.0 * h) * unit;
        o.q2 = (tp.travel - 2.0 * t0.travel + tm.travel) / (h * h) * unit;
        const MassAt ms = in.mass(t0.travel);
        o.forceB = (ms.M * o.q2 + 0.5 * ms.dM * o.q1 * o.q1) * w * w - ms.Q;
        o.forceN = o.forceB / (lever * cosN);
        // The follower pushes the cam with -N n at the contact; the shaft
        // balances its moment about the axis (the plane's Z, through its
        // origin), in the sense a turns it.
        o.camTorque = m.camSense * o.forceN *
                      (t0.point.x * t0.normal.y - t0.point.y * t0.normal.x) *
                      1e-3;
        o.point = t0.point;
        o.normal = t0.normal;
        o.forceKnown = true;
        if (o.forceN > bestN) {
          bestN = o.forceN;
          r.maxForceAt = static_cast<int>(k);
        }
        if (std::abs(o.camTorque) > bestT) {
          bestT = std::abs(o.camTorque);
          r.maxTorqueAt = static_cast<int>(k);
        }
      }
    }
    if (in.speedRpm) {
      bool ok =
          r.notHeld.empty() && (!r.liftoffRpm || *in.speedRpm <= *r.liftoffRpm);
      for (const DropOut& o : r.drops)
        ok = ok && !o.jams && o.lands.value_or(true);
      r.holdsAtSpeed = ok;
    }
  }
  return r;
}

std::vector<Level> Levels(const contact::Model& m, std::string* why) {
  std::vector<Level> out;
  const double deflection = 0.125 * m.tolerance;
  const double z0 = m.z - 0.5 * m.width;
  for (int k = 0; k < kLevels; k++) {
    Level l;
    l.z = z0 + (k + 0.5) / kLevels * m.width;
    std::string w;
    l.cam =
        contact::SectionOutline(m.camPlaced.GetShape(), l.z, deflection, &w);
    l.follower = contact::SectionOutline(m.followerPlaced.GetShape(), l.z,
                                         deflection, &w);
    if (l.cam.pts.empty() || l.follower.pts.empty()) {
      if (why)
        *why = w;
      continue;
    }
    out.push_back(std::move(l));
  }
  return out;
}

Across AcrossWidth(const contact::Model& m, const std::vector<Level>& levels,
                   double a, double b) {
  Across out;
  if (InDrop(m.table, a)) {
    out.inDrop = true;
    return out; // in free fall: nothing to touch
  }
  const contact::Touch mid =
      contact::FirstTouch(m.camOutline, m.camSense * a, m.followerOutline,
                          m.path, m.toward, 0.0, m.tolerance);
  const double tolQ = m.table.tolerance;
  if (mid.found)
    out.gap = std::abs(b - mid.travel);
  for (const Level& l : levels) {
    const contact::Touch t = contact::FirstTouch(
        l.cam, m.camSense * a, l.follower, m.path, m.toward, 0.0, m.tolerance);
    if (!t.found)
      continue;
    // Standing at b, the follower is past this cut's first touch by...
    const double past = m.toward * (b - t.travel);
    if (past > out.clash) {
      out.clash = past;
      out.atZ = l.z - (m.z - 0.5 * m.width);
    }
  }
  out.ok = out.clash <= tolQ && out.gap <= tolQ;
  return out;
}

} // namespace follower
