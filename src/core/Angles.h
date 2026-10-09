// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Degrees and radians, stated ONCE.
//
// The app stores angles in RADIANS inside geometry structs and DEGREES in
// every field whose name ends `Deg`/`_deg` — the file format, the RPC surface
// and the expression language all speak degrees, the sketch solver and the
// mate frames speak radians, and the boundary between them is where every
// angle bug this project has shipped actually lived:
//
//   - `set_mate(rotation=60)` pinned a mate to 60 RADIANS (3437.7 degrees,
//     197.7 folded) and was diagnosed as a loop-solver branch bug TWICE.
//   - `end_angle: 90` swept 116.6 degrees: a plausible arc and the wrong part.
//   - `pattern(angle=90)` was documented as a total sweep in degrees and
//     consumed as a per-step angle in radians — two errors in one number.
//
// So the conversion factor is not a literal anyone retypes. Before this header
// there were four independent definitions of it (Expression.cpp,
// SerializeFeatures.cpp, RemoteAssembly.cpp, RemoteMotion.cpp) and eleven more
// sites spelling `180.0 / M_PI` by hand, among 77 conversion sites in all —
// exactly the state the world up-axis was in before core/Conventions.h, and
// tests/test_conventions.cpp now fails on a new bare conversion literal the
// same way tests/test_world_up.cpp does for (0,1,0).
//
// A bare `180.0 / M_PI` elsewhere in the tree is a claim that the value is NOT
// an angle conversion. If it converts an angle, it belongs routed through here.
//
// Deliberately dependency-free (no glm, no json, no <cmath>) so it costs
// nothing to include anywhere an angle crosses a boundary.
namespace angles {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

// The factor itself, both ways round. Named for what they multiply BY, so
// `deg * kRadPerDeg` reads as the conversion it is.
inline constexpr double kDegPerRad = 180.0 / kPi;
inline constexpr double kRadPerDeg = kPi / 180.0;

constexpr double ToRadians(double degrees) { return degrees * kRadPerDeg; }
constexpr double ToDegrees(double radians) { return radians * kDegPerRad; }

// A full turn is the ceiling on every angle that describes a SWEEP (an arc, a
// pattern's total span, a revolve): more than a turn is a circle, so it is not
// a drawable sweep whatever was meant. It is the one unit check that rests on
// geometry rather than on guessing what the caller had in mind, which is why
// the arc parser and the pattern parser both refuse on it and then name the
// likely cause. A number that was meant as degrees trips it from 7 upward.
constexpr bool IsPlausibleSweep(double radians) {
  return radians >= -kTwoPi && radians <= kTwoPi;
}

} // namespace angles
