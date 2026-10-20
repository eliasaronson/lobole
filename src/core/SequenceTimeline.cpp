// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/SequenceTimeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace sequence {
namespace {

// Rigid placements: position lerped, orientation slerped (the motion
// playback's blend).
glm::dmat4 Blend(const glm::dmat4& a, const glm::dmat4& b, double f) {
  const glm::dquat qa = glm::normalize(glm::quat_cast(glm::dmat3(a)));
  const glm::dquat qb = glm::normalize(glm::quat_cast(glm::dmat3(b)));
  const glm::dvec3 p = glm::mix(glm::dvec3(a[3]), glm::dvec3(b[3]), f);
  return glm::translate(glm::dmat4(1.0), p) *
         glm::mat4_cast(glm::slerp(qa, qb, f));
}

// How long a pose takes: the largest turn of what is shown by then at the
// pose rate, its largest slide at the timeline's speed, stride by stride.
// Nothing shown moving: no time at all.
double PoseSeconds(const TimelineStep& s,
                   const std::map<uint64_t, size_t>& shown, double speed,
                   const Timing& timing) {
  double worst = 0.0;
  for (const auto& [occ, first] : s.strides.front()) {
    if (!shown.contains(occ))
      continue;
    double turned = 0.0, slid = 0.0;
    glm::dmat4 prev = first;
    for (size_t k = 1; k < s.strides.size(); k++) {
      const auto it = s.strides[k].find(occ);
      if (it == s.strides[k].end())
        continue;
      const glm::dmat4& now = it->second;
      slid += glm::length(glm::dvec3(now[3]) - glm::dvec3(prev[3]));
      const glm::dquat dq =
          glm::normalize(glm::quat_cast(glm::dmat3(now))) *
          glm::inverse(glm::normalize(glm::quat_cast(glm::dmat3(prev))));
      turned += glm::degrees(2.0 * std::acos(std::min(1.0, std::abs(dq.w))));
      prev = now;
    }
    worst = std::max({worst, slid / speed,
                      timing.poseDegPerSecond > 0.0
                          ? turned / timing.poseDegPerSecond
                          : 0.0});
  }
  return worst;
}

} // namespace

Timeline Time(std::vector<TimelineStep> steps, double sizeMm,
              const Timing& timing) {
  Timeline tl;
  tl.sizeMm = sizeMm;
  tl.speedMmPerS =
      sizeMm > 0.0 ? sizeMm / std::max(timing.crossSeconds, 1e-6) : 1.0;
  double cursor = 0.0;
  for (size_t i = 0; i < steps.size(); i++) {
    TimelineStep& s = steps[i];
    if (s.IsPose()) {
      s.appear = s.start = cursor;
      const double move = PoseSeconds(s, tl.stepOf, tl.speedMmPerS, timing);
      s.end =
          s.start + (move > 1e-9 ? std::max(move, timing.minMoveSeconds) : 0.0);
      cursor = s.end;
      tl.poses.push_back(i);
      continue;
    }
    s.appear = cursor;
    s.start = cursor + timing.holdSeconds;
    double move = 0.0;
    if (!s.base && s.travelMm > 0.0) {
      move = std::max(s.travelMm / tl.speedMmPerS, timing.minMoveSeconds);
      if (s.turnDeg != 0.0 && timing.turnsPerSecond > 0.0)
        move =
            std::max(move, std::abs(s.turnDeg) / 360.0 / timing.turnsPerSecond);
    }
    s.end = s.start + move;
    cursor = s.end;
    for (uint64_t o : s.occurrences)
      tl.stepOf.emplace(o, i); // the first step that moves it
  }
  tl.seconds = cursor + timing.endSeconds;
  tl.steps = std::move(steps);
  return tl;
}

std::vector<uint64_t> MovingAt(const Timeline& tl, double t) {
  std::vector<uint64_t> out;
  for (const TimelineStep& s : tl.steps) {
    if (!s.IsPose()) {
      if (s.appear <= t && t < s.end)
        out.insert(out.end(), s.occurrences.begin(), s.occurrences.end());
      continue;
    }
    if (t < s.start || t >= s.end)
      continue;
    // What its solve moves, of what is shown by then.
    for (const auto& [occ, first] : s.strides.front()) {
      const auto shown = tl.stepOf.find(occ);
      if (shown != tl.stepOf.end() && tl.steps[shown->second].appear > t)
        continue;
      for (size_t k = 1; k < s.strides.size(); k++) {
        const auto it = s.strides[k].find(occ);
        if (it == s.strides[k].end())
          continue;
        double moved = 0.0;
        for (int c = 0; c < 4; c++)
          moved = std::max(moved, glm::length(it->second[c] - first[c]));
        if (moved > 1e-6) {
          out.push_back(occ);
          break;
        }
      }
    }
  }
  return out;
}

double RemainingAt(const TimelineStep& s, double t) {
  if (s.base || s.travelMm <= 0.0 || t >= s.end)
    return 0.0;
  if (t <= s.start || s.end <= s.start)
    return s.travelMm;
  return s.travelMm * (1.0 - (t - s.start) / (s.end - s.start));
}

glm::dmat4 HomeAt(const Timeline& tl, uint64_t occurrence,
                  const glm::dmat4& seat, double t) {
  glm::dmat4 home = seat;
  for (size_t i : tl.poses) {
    const TimelineStep& s = tl.steps[i];
    if (t < s.start)
      break;
    const auto at = [&](size_t k) {
      const auto it = s.strides[k].find(occurrence);
      return it == s.strides[k].end() ? home : it->second;
    };
    const size_t n = s.strides.size();
    if (t >= s.end || n < 2) {
      home = at(n - 1);
      continue;
    }
    const double pos = (t - s.start) / (s.end - s.start) * double(n - 1);
    const size_t k = std::min(static_cast<size_t>(pos), n - 2);
    return Blend(at(k), at(k + 1), pos - double(k));
  }
  return home;
}

std::optional<glm::dmat4> PoseAt(const Timeline& tl, uint64_t occurrence,
                                 const glm::dmat4& seat, double t) {
  const glm::dmat4 home = HomeAt(tl, occurrence, seat, t);
  const auto it = tl.stepOf.find(occurrence);
  if (it == tl.stepOf.end())
    return home;
  const TimelineStep& s = tl.steps[it->second];
  if (t < s.appear)
    return std::nullopt;
  const double d = RemainingAt(s, t);
  if (d <= 0.0)
    return home;
  const glm::dvec3 m = glm::normalize(s.moves);
  // Still to turn by the share of the path still to go: back by that much.
  const double angle = s.travelMm > 0.0 ? -s.turnDeg * d / s.travelMm : 0.0;
  glm::dmat4 about(1.0);
  if (angle != 0.0)
    about = glm::translate(glm::dmat4(1.0), s.point) *
            glm::rotate(glm::dmat4(1.0), glm::radians(angle), m) *
            glm::translate(glm::dmat4(1.0), -s.point);
  return glm::translate(glm::dmat4(1.0), -m * d) * about * home;
}

} // namespace sequence
