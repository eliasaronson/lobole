// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/SequenceTimeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace sequence {

Timeline Time(std::vector<TimelineStep> steps, double sizeMm,
              const Timing& timing) {
  Timeline tl;
  tl.sizeMm = sizeMm;
  tl.speedMmPerS =
      sizeMm > 0.0 ? sizeMm / std::max(timing.crossSeconds, 1e-6) : 1.0;
  double cursor = 0.0;
  for (size_t i = 0; i < steps.size(); i++) {
    TimelineStep& s = steps[i];
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

double RemainingAt(const TimelineStep& s, double t) {
  if (s.base || s.travelMm <= 0.0 || t >= s.end)
    return 0.0;
  if (t <= s.start || s.end <= s.start)
    return s.travelMm;
  return s.travelMm * (1.0 - (t - s.start) / (s.end - s.start));
}

std::optional<glm::dmat4> PoseAt(const Timeline& tl, uint64_t occurrence,
                                 const glm::dmat4& seat, double t) {
  const auto it = tl.stepOf.find(occurrence);
  if (it == tl.stepOf.end())
    return seat;
  const TimelineStep& s = tl.steps[it->second];
  if (t < s.appear)
    return std::nullopt;
  const double d = RemainingAt(s, t);
  if (d <= 0.0)
    return seat;
  const glm::dvec3 m = glm::normalize(s.moves);
  // Still to turn by the share of the path still to go: back by that much.
  const double angle = s.travelMm > 0.0 ? -s.turnDeg * d / s.travelMm : 0.0;
  glm::dmat4 about(1.0);
  if (angle != 0.0)
    about = glm::translate(glm::dmat4(1.0), s.point) *
            glm::rotate(glm::dmat4(1.0), glm::radians(angle), m) *
            glm::translate(glm::dmat4(1.0), -s.point);
  return glm::translate(glm::dmat4(1.0), -m * d) * about * seat;
}

} // namespace sequence
