#include "core/cam/CamRotaryFeed.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cam {

RotaryFeedModel FeedModelOf(const std::string& postId) {
  return postId == "linuxcnc" ? RotaryFeedModel::LinearFirst
                              : RotaryFeedModel::Combined;
}

double TipLength(const glm::dvec3& from, double aFrom, const glm::dvec3& to,
                 double aTo) {
  const double linear = glm::length(to - from);
  const double turn = std::abs(aTo - aFrom);
  if (turn <= 1e-9)
    return linear;
  // The MEAN radius: a spiral's radius changes along a move, and the arc the
  // tip sweeps is the turn times the radius it sweeps it at.
  const double radius =
      0.5 * (std::hypot(from.y, from.z) + std::hypot(to.y, to.z));
  const double arc = turn / 360.0 * 2.0 * std::numbers::pi * radius;
  return std::hypot(linear, arc);
}

double ControllerLength(const glm::dvec3& from, double aFrom,
                        const glm::dvec3& to, double aTo,
                        RotaryFeedModel model) {
  const double linear = glm::length(to - from);
  const double turn = std::abs(aTo - aFrom);
  if (model == RotaryFeedModel::LinearFirst)
    return linear > 1e-9 ? linear : turn;
  return std::hypot(linear, turn);
}

AxisLimits AxisLimitsOf(const Setup& setup) {
  return {setup.machine.rapidFeed, setup.machine.rotaryMaxFeed};
}

ScaledFeed ControllerFeed(const glm::dvec3& from, double aFrom,
                          const glm::dvec3& to, double aTo, double feed,
                          RotaryFeedModel model, const AxisLimits& limits) {
  ScaledFeed out;
  out.feed = out.wanted = feed;
  const double turn = std::abs(aTo - aFrom);
  if (turn <= 1e-9 || feed <= 0.0)
    return out;
  const double tip = TipLength(from, aFrom, to, aTo);
  const double ctrl = ControllerLength(from, aFrom, to, aTo, model);
  if (tip > 1e-12) // turning ON the axis passes nothing by the tip
    out.wanted = feed * ctrl / tip;
  // Each axis moves at F times its share of the controller's length, and
  // may not exceed its own top speed.
  double cap = std::numeric_limits<double>::infinity();
  const double linear = glm::length(to - from);
  if (limits.linear > 0.0 && linear > 1e-9)
    cap = std::min(cap, limits.linear * ctrl / linear);
  if (limits.rotary > 0.0)
    cap = std::min(cap, limits.rotary * ctrl / turn);
  out.feed = std::min(out.wanted, cap);
  out.clamped = out.wanted > cap;
  out.aRate = ctrl > 1e-12 ? out.feed * turn / ctrl : 0.0;
  return out;
}

} // namespace cam
