#pragma once

#include "core/cam/CamJob.h" // Setup

#include <glm/glm.hpp>

#include <string>

// What a feed means when the part turns
// (docs/cam/rotary.md#feed-on-a-turning-cut).
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

// --- one segment, one answer ---------------------------------------------
//
// The post, the cycle time, the playback clock and the verdict all walk
// moves asking the same questions -- is this a cut that turns the part, what
// F does it get, how long does it take -- and each used to answer them in
// its own loop. The rule lives here now, once.

// Two consecutive moves, whatever they came from (a toolpath or a file).
struct RotarySegment {
  glm::dvec3 from{0.0};
  double aFrom = 0.0;
  bool fromHasA = false;
  glm::dvec3 to{0.0};
  double aTo = 0.0;
  bool toHasA = false;
  bool rapid = false;
  double feed = 0.0; // the move's own F, mm/min; 0 = none stated
};
[[nodiscard]] RotarySegment SegmentOf(const Move& from, const Move& to);

// A CUT that changes A: the one kind of move whose F is scaled.
[[nodiscard]] bool TurnsWhileCutting(const RotarySegment& s);
// The path the tool tip travels: TipLength when both ends state an A, the
// straight distance otherwise. What the playback slider scrubs along.
[[nodiscard]] double SegmentLength(const RotarySegment& s);

// A PLANNED move -- a toolpath's, `feed` meant at the tip. The F to post (a
// turning cut's scaled and held; any other move's own), and the minutes the
// controller takes over it. A rapid is timed at `rapidFeed` when one is
// stated and not at all otherwise, because an invented traverse rate is an
// invented cycle time.
struct PlannedFeed {
  ScaledFeed feed;
  double minutes = 0.0;
};
[[nodiscard]] PlannedFeed PlanFeed(const RotarySegment& s, double rapidFeed,
                                   RotaryFeedModel model,
                                   const AxisLimits& limits);
// A POSTED move, whose F was already scaled for the controller: the minutes
// it takes. Not PlanFeed on the file's F -- that would scale it twice.
[[nodiscard]] double PostedMinutes(const RotarySegment& s, double rapidFeed,
                                   RotaryFeedModel model);

} // namespace cam
