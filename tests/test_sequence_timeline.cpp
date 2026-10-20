// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The assembly sequence's timeline (core/SequenceTimeline.h): what Play and
// export_assembly_video show
// (docs/assemblies/README.md#playing-and-filming-a-sequence).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/SequenceTimeline.h"

#include <glm/gtc/matrix_transform.hpp>

using Catch::Matchers::WithinAbs;
using sequence::TimelineStep;

namespace {

glm::dvec3 Origin(const glm::dmat4& m) { return glm::dvec3(m[3]); }

double Apart(const glm::dmat4& a, const glm::dmat4& b) {
  double worst = 0.0;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      worst = std::max(worst, std::abs(a[c][r] - b[c][r]));
  return worst;
}

TimelineStep Base(uint64_t occ) {
  TimelineStep s;
  s.occurrences = {occ};
  s.base = true;
  return s;
}

// Goes home down -Z from `travel` above its seat.
TimelineStep Down(std::vector<uint64_t> occ, double travel) {
  TimelineStep s;
  s.occurrences = std::move(occ);
  s.moves = {0.0, 0.0, -1.0};
  s.travelMm = travel;
  return s;
}

} // namespace

TEST_CASE("A part is hidden before its step and home after it", "[sequence]") {
  const auto tl = sequence::Time({Base(1), Down({2}, 40.0)}, 200.0);
  const TimelineStep& s = tl.steps[1];
  const glm::dmat4 seat =
      glm::translate(glm::dmat4(1.0), glm::dvec3(5.0, 6.0, 7.0));
  REQUIRE(s.appear > 0.0);
  CHECK_FALSE(sequence::PoseAt(tl, 2, seat, 0.5 * s.appear).has_value());
  // Shown at its start until it moves: 40 mm up the way it comes from.
  const auto atStart = sequence::PoseAt(tl, 2, seat, s.appear);
  REQUIRE(atStart.has_value());
  CHECK_THAT(Origin(*atStart).z, WithinAbs(47.0, 1e-9));
  const auto after = sequence::PoseAt(tl, 2, seat, s.end + 0.01);
  REQUIRE(after.has_value());
  CHECK(Apart(*after, seat) < 1e-12);
  CHECK(Apart(*sequence::PoseAt(tl, 2, seat, tl.seconds), seat) < 1e-12);
  // The base is there from the start, and stays where it is.
  CHECK(Apart(*sequence::PoseAt(tl, 1, seat, 0.0), seat) < 1e-12);
  // An occurrence no step moves stands at its seat throughout.
  CHECK(Apart(*sequence::PoseAt(tl, 99, seat, 0.0), seat) < 1e-12);
  CHECK(tl.seconds > s.end);
}

TEST_CASE("At its step's midpoint a part is half way along its path",
          "[sequence]") {
  const auto tl = sequence::Time({Base(1), Down({2}, 30.0)}, 300.0);
  const TimelineStep& s = tl.steps[1];
  const double mid = 0.5 * (s.start + s.end);
  const auto p = sequence::PoseAt(tl, 2, glm::dmat4(1.0), mid);
  REQUIRE(p.has_value());
  CHECK_THAT(Origin(*p).z, WithinAbs(15.0, 1e-9));
  CHECK_THAT(Origin(*p).x, WithinAbs(0.0, 1e-12));
}

TEST_CASE("A group moves rigidly", "[sequence]") {
  auto step = Down({2, 3}, 20.0);
  step.point = {10.0, 0.0, 0.0};
  step.turnDeg = 90.0;
  const auto tl = sequence::Time({Base(1), step}, 100.0);
  const TimelineStep& s = tl.steps[1];
  const glm::dmat4 seatA =
      glm::translate(glm::dmat4(1.0), glm::dvec3(10.0, 0.0, 0.0));
  const glm::dmat4 seatB =
      glm::translate(glm::dmat4(1.0), glm::dvec3(30.0, 4.0, 0.0));
  for (double f : {0.0, 0.3, 0.7}) {
    const double t = s.start + f * (s.end - s.start);
    const auto a = sequence::PoseAt(tl, 2, seatA, t);
    const auto b = sequence::PoseAt(tl, 3, seatB, t);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // The two keep their placement relative to each other.
    CHECK(Apart(glm::inverse(*a) * *b, glm::inverse(seatA) * seatB) < 1e-9);
  }
}

TEST_CASE("A thread step turns a turn a pitch", "[sequence]") {
  // A right-hand M6 x 1 nut run 5 mm onto its thread: five turns, right
  // hand about the way it moves.
  const double pitch = 1.0, travel = 5.0;
  auto step = Down({2}, travel);
  step.turnDeg = 360.0 * travel / pitch;
  const auto tl = sequence::Time({Base(1), step}, 120.0);
  const TimelineStep& s = tl.steps[1];
  const glm::dmat4 seat(1.0);
  // Every pitch from the seat it stands as it is seated, turned whole turns.
  for (int k = 1; k <= 4; k++) {
    const double f = 1.0 - k * pitch / travel;
    const auto p =
        sequence::PoseAt(tl, 2, seat, s.start + f * (s.end - s.start));
    REQUIRE(p.has_value());
    CHECK(Apart(*p, glm::translate(glm::dmat4(1.0),
                                   glm::dvec3(0.0, 0.0, k * pitch))) < 1e-9);
  }
  // A quarter pitch out it still has a quarter turn to go: it stands -90
  // deg about the way it moves (-Z) from its seat, which takes +X to +Y.
  const double f = 1.0 - 0.25 * pitch / travel;
  const auto p = sequence::PoseAt(tl, 2, seat, s.start + f * (s.end - s.start));
  REQUIRE(p.has_value());
  const glm::dvec3 x = glm::dvec3(*p * glm::dvec4(1.0, 0.0, 0.0, 1.0));
  CHECK_THAT(x.x, WithinAbs(0.0, 1e-9));
  CHECK_THAT(x.y, WithinAbs(1.0, 1e-9));
  CHECK_THAT(x.z, WithinAbs(0.25 * pitch, 1e-9));
  // No faster than the turn rate allows.
  CHECK(s.end - s.start >= 5.0 / sequence::Timing{}.turnsPerSecond - 1e-9);
}

TEST_CASE("The speed scales with the assembly's size", "[sequence]") {
  const auto small = sequence::Time({Base(1), Down({2}, 40.0)}, 100.0);
  const auto big = sequence::Time({Base(1), Down({2}, 400.0)}, 1000.0);
  CHECK_THAT(big.speedMmPerS / small.speedMmPerS, WithinAbs(10.0, 1e-9));
  // The same share of the assembly takes the same time.
  CHECK_THAT(big.steps[1].end - big.steps[1].start,
             WithinAbs(small.steps[1].end - small.steps[1].start, 1e-9));
  CHECK_THAT(big.seconds, WithinAbs(small.seconds, 1e-9));
  // A short press is still seen moving.
  const auto press = sequence::Time({Base(1), Down({2}, 0.1)}, 1000.0);
  CHECK(press.steps[1].end - press.steps[1].start >=
        sequence::Timing{}.minMoveSeconds - 1e-12);
}
