#pragma once

#include "core/cam/CamJob.h" // Setup

#include <glm/glm.hpp>

#include <string>

// What a feed means when the part turns (docs/cam/rotary-continuous-plan.md,
// S3).
//
// A move that turns A while it cuts mixes millimetres and degrees, and F is
// one number. Controllers disagree on what it measures, and the two families
// this app posts for are:
//   Combined    -- the length of the whole vector, a degree counted as a
//                  millimetre (Mach3; GRBL-family controllers with an A).
//   LinearFirst -- the XYZ length when there is one, the rotary only when it
//                  moves alone (LinuxCNC, after RS274NGC 2.1.2.5).
// Neither is the path the tool tip travels, which is what the cutter feels.
// So a turning move's F is SCALED by what the controller will measure over
// what the tip travels -- in the post, because it is a fact about the
// controller, and read back the same way everywhere a time is computed.
//
// Its own file, and built without the engine, because the POST needs it and
// the post is part of the document model in every configuration.
namespace cam {

enum class RotaryFeedModel { Combined, LinearFirst };
[[nodiscard]] RotaryFeedModel FeedModelOf(const std::string& postId);

// The path the tool TIP travels from (from, aFrom) to (to, aTo): the linear
// part combined with the arc the part turns under it, at the move's mean
// radius from the axis (setup-space +X). What the cutter feels, and the
// distance the playback slider scrubs along.
[[nodiscard]] double TipLength(const glm::dvec3& from, double aFrom,
                               const glm::dvec3& to, double aTo);
// The length the CONTROLLER divides by F for the same move.
[[nodiscard]] double ControllerLength(const glm::dvec3& from, double aFrom,
                                      const glm::dvec3& to, double aTo,
                                      RotaryFeedModel model);

// How fast each kind of axis can go, as the machine states it. Zero is "not
// stated", and nothing is limited by a number nobody typed. Per KIND of axis
// because that is how the controller limits a move: every axis to its own
// top speed, so a turning cut is held back by A long before it troubles X.
// Capping it at the LINEAR rapid instead -- the first version of this --
// made the rolling pin's spiral a 36-hour job on paper.
struct AxisLimits {
  double linear = 0.0; // mm/min: Machine::rapidFeed
  double rotary = 0.0; // deg/min: Machine::rotaryMaxFeed
};
[[nodiscard]] AxisLimits AxisLimitsOf(const Setup& setup);

// The F to write for a CUTTING move meant to feed at `feed` mm/min at the
// tip. A move whose A does not change comes back unchanged. Held to what the
// axes can do, and the hold is reported rather than hidden: `clamped`, and
// the F it would have needed. `aRate` is the degrees per minute A turns at
// the F returned -- what a machine with no stated top speed is being asked
// for.
struct ScaledFeed {
  double feed = 0.0;
  bool clamped = false;
  double wanted = 0.0;
  double aRate = 0.0;
};
[[nodiscard]] ScaledFeed ControllerFeed(const glm::dvec3& from, double aFrom,
                                        const glm::dvec3& to, double aTo,
                                        double feed, RotaryFeedModel model,
                                        const AxisLimits& limits);

} // namespace cam
