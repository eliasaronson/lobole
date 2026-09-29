#include "core/cam/CamMount.h"

#include "core/cam/CamNames.h" // CutAtIndices
#include "core/cam/CamToolpath.h"
#include "core/cam/CamToolpathInternal.h" // StockBoundsFramed

#include <algorithm>
#include <cmath>

namespace cam {

bool HeldInRotary(const Setup& setup) {
  switch (setup.heldIn) {
  case Setup::HeldIn::Rotary:
    return true;
  case Setup::HeldIn::Bed:
    return false;
  default:
    break;
  }
  if (!setup.machine.hasRotaryAxis)
    return false;
  if (std::abs(setup.rotationA) > 1e-9)
    return true;
  for (const Operation& op : setup.operations)
    if (op.type == OpType::SurfaceRotary || CutAtIndices(op))
      return true;
  return false;
}

namespace {
glm::dvec2 Turn(int quarter, const glm::dvec2& v) {
  switch (((quarter % 4) + 4) % 4) {
  case 1:
    return {-v.y, v.x};
  case 2:
    return {-v.x, -v.y};
  case 3:
    return {v.y, -v.x};
  default:
    return v;
  }
}
} // namespace

Mount MountOf(const Setup& setup) {
  Mount out;
  const RotaryMount& r = setup.machine.rotary;
  if (!r.Stated() || !HeldInRotary(setup))
    return out;
  out.word = r.word;
  // Setup +X (the part's axis, away from the chuck) onto the machine's
  // `along`, pointing away from the end the chuck is at.
  if (r.along == 'x')
    out.quarter = r.chuckEnd < 0 ? 0 : 2;
  else
    out.quarter = r.chuckEnd < 0 ? 1 : 3;
  // A right-handed turn about setup +X takes the top of the part toward
  // setup -Y. Where that lands on the machine, against where the top is
  // SEEN to go on a positive jog, is the sign.
  const glm::dvec2 internal = Turn(out.quarter, {0.0, -1.0});
  const glm::dvec2 seen = r.topAxis == 'x' ? glm::dvec2(r.topSign, 0.0)
                                           : glm::dvec2(0.0, r.topSign);
  out.sign = glm::dot(internal, seen) >= 0.0 ? 1.0 : -1.0;
  return out;
}

glm::dvec3 ToMachine(const Mount& m, const glm::dvec3& p) {
  const glm::dvec2 xy = Turn(m.quarter, {p.x, p.y});
  return {xy.x, xy.y, p.z};
}

glm::dvec3 ToSetup(const Mount& m, const glm::dvec3& p) {
  const glm::dvec2 xy = Turn(-m.quarter, {p.x, p.y});
  return {xy.x, xy.y, p.z};
}

glm::dvec3 TravelSpanInSetup(const Setup& setup) {
  const Machine& mc = setup.machine;
  glm::dvec3 span = mc.travelMax - mc.travelMin;
  if (MountOf(setup).quarter % 2 == 1)
    std::swap(span.x, span.y);
  return span;
}

std::string MountRefusal(const Setup& setup) {
  if (!HeldInRotary(setup))
    return "";
  const Machine& mc = setup.machine;
  const std::string name = mc.name.empty() ? std::string("this setup's machine")
                                           : "machine \"" + mc.name + "\"";
  if (!mc.hasRotaryAxis)
    return "this setup holds its part in the rotary, and " + name +
           " has no rotary axis";
  if (!mc.rotary.Stated())
    return "this setup holds its part in the rotary, and " + name +
           " does not say how its rotary is installed: which machine axis "
           "it lies along, which end the chuck is at, the letter the "
           "controller drives it with, and which way the top of the part "
           "moves on a positive jog (the machine's `rotary`). Posting would "
           "guess -- and a wrong guess drives the cutter across a part that "
           "lies the other way";
  return "";
}

double HoldingEnvelope::RadiusAt(double x) const {
  if (!ok)
    return 0.0;
  if (chuckR > 0.0 && x <= chuckTo)
    return chuckR;
  if (tail && x >= x1) {
    const double along = x - x1;
    return along >= coneLen ? bodyR : coneR * along / std::max(coneLen, 1e-9);
  }
  return 0.0;
}

HoldingEnvelope HoldingEnvelopeOf(const Setup& setup,
                                  const std::vector<Body>& framed) {
  HoldingEnvelope env;
  if (!HeldInRotary(setup))
    return env;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, framed, lo, hi))
    return env;
  const RotaryHolding& h = setup.machine.holding;
  const auto size = [&](double stated, double fallback, const char* what) {
    if (stated > 0.0)
      return stated;
    env.defaulted.push_back(what);
    return fallback;
  };
  env.ok = true;
  env.x0 = lo.x;
  env.x1 = hi.x;
  if (setup.chuck == 0) {
    env.chuckTo = -1e300; // held some other way: no jaws at this end
  } else {
    const double grip =
        setup.gripLength > 0.0
            ? setup.gripLength
            : size(h.gripLength, 15.0, "the chuck's grip (15 mm)");
    env.chuckTo = env.x0 + grip;
    env.chuckR =
        size(h.chuckDiameter, 90.0, "the chuck's diameter (90 mm)") * 0.5;
  }
  env.tail = setup.tailstock == 1 || (setup.tailstock == -1 && h.hasTailstock);
  if (env.tail) {
    env.coneLen =
        size(h.centreLength, 25.0, "the tailstock centre's length (25 mm)");
    env.coneR = size(h.centreDiameter, 14.0,
                     "the tailstock centre's diameter (14 mm)") *
                0.5;
    env.bodyR =
        size(h.tailBodyDiameter, 60.0, "the tailstock's body (60 mm)") * 0.5;
  }
  env.clearance = h.clearance > 0.0 ? h.clearance : 3.0;
  return env;
}

double HoldingDepth(const HoldingEnvelope& env, const glm::dvec3& p, double r) {
  if (!env.ok)
    return 0.0;
  // The envelope's widest over the tool's slab along X.
  double reach = 0.0;
  const double a = p.x - r, b = p.x + r;
  if (env.chuckR > 0.0 && a <= env.chuckTo)
    reach = std::max(reach, env.chuckR);
  if (env.tail && b >= env.x1)
    reach = std::max(reach, env.RadiusAt(b));
  if (reach <= 0.0)
    return 0.0;
  // The tool: a cylinder of radius r from its tip up. Its nearest approach to
  // the axis is the tip's height with the lateral offset less its radius --
  // or zero when the tip is below the axis and the cylinder straddles it.
  const double lateral = std::max(0.0, std::abs(p.y) - r);
  const double nearest =
      p.z >= 0.0 ? std::hypot(lateral, p.z) : (lateral > 0.0 ? lateral : 0.0);
  return reach - nearest;
}

} // namespace cam
