#pragma once

// The rotary as INSTALLED, and what holds a part in it
// (docs/cam/rotary.md#how-the-rotary-is-installed,
// docs/cam/rotary.md#chuck-and-tailstock).
//
// Inside the app a part in the rotary lies along setup +X, the chuck at its
// low end, and A turns right-handed about it. `MountOf` says what that is on
// the machine -- a quarter turn about Z, a sign on A, a letter -- and is the
// ONE mapping the post writes by and the reader reads back by, so a Y-mounted
// rotary driven as B is posted, simulated and verified as the same thing.

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace cam {

// Whether this setup's part is held in the rotary: stated, or derived from a
// rotary op, an op with its own index list, or a non-zero index.
[[nodiscard]] bool HeldInRotary(const Setup& setup);

// A rotary index in degrees as an operation's list holds it
// (docs/cam/rotary.md#one-operation-at-several-indices): in [0, 360), and
// what `SolveIndexA` returns as 89.99999999 read as the 90 it is.
[[nodiscard]] double NormalizeIndexDeg(double degrees);

// Adds `target`, presented to the spindle at index `atDeg`, to `op`, cut at
// the setup's own index `setupDeg` until now when its list is empty: the
// list becomes the setup's index and `atDeg` (sorted, each once), unless
// both are the same index, when the list stays empty. The target is added
// once. False when the op already held both.
[[nodiscard]] bool AddIndexedTarget(Operation& op, const Target& target,
                                    double atDeg, double setupDeg);

// From setup space to the machine: `quarter` turns of +90 degrees about Z,
// A multiplied by `sign`, written as `word`. Identity for a setup on the bed
// and for a machine that does not state its mount.
struct Mount {
  int quarter = 0;
  double sign = 1.0;
  char word = 'A';
};
[[nodiscard]] Mount MountOf(const Setup& setup);
[[nodiscard]] glm::dvec3 ToMachine(const Mount& m, const glm::dvec3& p);
[[nodiscard]] glm::dvec3 ToSetup(const Mount& m, const glm::dvec3& p);

// The machine's travel spans along setup X, Y, Z: X and Y swapped for a
// rotary that lies along machine Y.
[[nodiscard]] glm::dvec3 TravelSpanInSetup(const Setup& setup);

// Empty when this setup can be posted; else why not, in words. A setup held
// in the rotary of a machine that does not say how the rotary is installed is
// refused: posting would guess, and the guess drives the cutter across a
// part that lies the other way.
[[nodiscard]] std::string MountRefusal(const Setup& setup);

// The chuck and the tailstock round the axis, in setup space: nothing may
// come nearer the axis than `RadiusAt(x)`. From the stock's ends: the chuck
// from its low end to `chuckTo` (the jaw tips), the tailstock's centre from
// its high end, a cone out to the centre's base, then its body.
struct HoldingEnvelope {
  bool ok = false; // false: not held in the rotary, or no stock to measure
  double x0 = 0.0, x1 = 0.0; // the stock's ends along the axis
  double chuckTo = 0.0;      // the jaw tips
  double chuckR = 0.0;
  bool tail = false;
  double coneLen = 0.0, coneR = 0.0, bodyR = 0.0;
  double clearance = 0.0;
  // Which sizes are the defaults, not measured: said by the verdict.
  std::vector<std::string> defaulted;
  // The keep-out radius at x: 0 where nothing holds the part.
  [[nodiscard]] double RadiusAt(double x) const;
  // The window along X a tool of radius `r` may cut in.
  [[nodiscard]] double MinX(double r) const { return chuckTo + r + clearance; }
  [[nodiscard]] double MaxX(double r) const {
    return tail ? x1 - r - clearance : 1e300;
  }
};
[[nodiscard]] HoldingEnvelope
HoldingEnvelopeOf(const Setup& setup, const std::vector<Body>& framed);

// How far a vertical tool of radius `r`, its tip at setup-space `p`, reaches
// into the envelope: the envelope's radius over the tool's X slab less the
// tool's nearest approach to the axis. <= 0 is clear.
[[nodiscard]] double HoldingDepth(const HoldingEnvelope& env,
                                  const glm::dvec3& p, double r);

} // namespace cam
