#include "core/tube/Check.h"

#include "core/Angles.h"

#include <cmath>
#include <sstream>

namespace tube {
namespace {

std::string Mm(double v) {
  std::ostringstream s;
  s.precision(2);
  s << std::fixed << v;
  return s.str();
}

std::string Pct(double fraction) {
  std::ostringstream s;
  s.precision(1);
  s << std::fixed << fraction * 100.0;
  return s.str();
}

// The straight left AFTER the last bend: what the whole centreline develops
// to, less every straight before a bend and every bend's own arc. It is the
// length the bender has to grip, and no bend record carries it.
double Tail(const Member& m) {
  double used = 0.0;
  for (const Bend& b : m.bends) {
    used += b.straightBefore;
    used += BendArcLength(angles::ToRadians(b.angleDeg), b.radius);
  }
  return m.length - used;
}

} // namespace

CheckResult Check(const std::vector<Member>& members, const TubeRules& rules) {
  CheckResult out;
  const double od = rules.outerDiameter;
  const double wall = rules.wallThickness;
  const double minR = MinBendRadius(rules.material, od, wall);
  const double minStraight = rules.EffectiveMinStraight();
  const double tail = rules.EffectiveTailLength();
  const double maxThin = rules.EffectiveMaxThinning();

  // Ovality is the other thing a bent tube is judged on, and it is not
  // geometry: it depends on whether the bender ran a mandrel, a wiper die and
  // how hard the pressure die was set. This build has no tooling model, so it
  // says so rather than implying one.
  out.notChecked.emplace_back(
      "ovality at the bend: it depends on the tooling (mandrel, wiper and "
      "pressure die), which this build has no model of");
  out.notChecked.emplace_back(
      "springback: the overbend a machine needs is a property of that machine "
      "and its setup, not of the part");

  for (const Member& m : members) {
    out.membersChecked++;
    for (size_t i = 0; i < m.bends.size(); i++) {
      const Bend& b = m.bends[i];
      out.bendsChecked++;
      const int n = static_cast<int>(i) + 1;

      if (b.radius + 1e-9 < minR) {
        out.violations.push_back(
            {"bend_below_minimum_radius",
             "bend " + std::to_string(n) + " of " + m.name + " is on a " +
                 Mm(b.radius) + " mm former; " + rules.size + " " +
                 rules.material + " takes " + Mm(minR) + " mm (" +
                 MinBendRadiusSource(rules.material, od, wall) + ")",
             m.name, n, b.radius, minR});
      } else if (!rules.formerRadii.empty()) {
        // Only worth asking once the radius is legal: a former nobody owns is
        // a different problem from a former that would collapse the tube.
        const auto die = rules.FormerFor(b.radius);
        if (!die.has_value() || std::abs(*die - b.radius) > 1e-6) {
          std::string owned;
          for (double r : rules.formerRadii)
            owned += owned.empty() ? Mm(r) : ", " + Mm(r);
          out.violations.push_back({"radius_not_in_die_set",
                                    "bend " + std::to_string(n) + " of " +
                                        m.name + " wants a " + Mm(b.radius) +
                                        " mm former; this bender has " + owned +
                                        " mm",
                                    m.name, n, b.radius, die.value_or(0.0)});
        }
      }

      const double thin = WallThinning(od, wall, b.radius);
      if (thin > maxThin + 1e-9) {
        out.violations.push_back(
            {"wall_thinning_over_limit",
             "bend " + std::to_string(n) + " of " + m.name +
                 " thins the "
                 "outer wall to " +
                 Mm(WallAfterBend(od, wall, b.radius)) + " mm, " + Pct(thin) +
                 "% under " + Mm(wall) + " — the limit is " + Pct(maxThin) +
                 "%",
             m.name, n, thin, maxThin});
      }

      // The straight before the FIRST bend is an end of the tube: it is
      // gripped, not clamped between two bends, so it answers to the tail
      // rule. Every later one has to hold the clamp die.
      const double need = i == 0 ? tail : minStraight;
      const char* kind = i == 0 ? "tail_below_gripper_minimum"
                                : "straight_below_clamp_minimum";
      if (b.straightBefore + 1e-9 < need) {
        out.violations.push_back(
            {kind,
             "bend " + std::to_string(n) + " of " + m.name + " has " +
                 Mm(b.straightBefore) + " mm of straight before it; " +
                 (i == 0 ? "the bender needs " + Mm(need) + " mm to grip"
                         : "the clamp die needs " + Mm(need) + " mm"),
             m.name, n, b.straightBefore, need});
      }
    }
    if (!m.bends.empty()) {
      const double t = Tail(m);
      if (t + 1e-9 < tail) {
        out.violations.push_back({"tail_below_gripper_minimum",
                                  m.name + " ends " + Mm(t) +
                                      " mm past its last bend; the bender "
                                      "needs " +
                                      Mm(tail) + " mm to grip",
                                  m.name, static_cast<int>(m.bends.size()), t,
                                  tail});
      }
    }
  }
  return out;
}

} // namespace tube
