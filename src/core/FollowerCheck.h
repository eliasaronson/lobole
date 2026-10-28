// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
// Whether a follower stays on its cam
// (docs/assemblies/relations.md#the-follower-check): over a motion study's
// stored samples, the pressure angle and the radii at the contact, the realised
// motion against the one the cam was cut for, and — from the masses that move
// with the follower — the speed above which gravity can no longer hold it on,
// and whether each drop lands in time. Pure: the contact model, the samples and
// a mass function in, a report out. The check across the width of one sample is
// `AcrossWidth`.

#include "core/CamProfileMath.h"
#include "core/ContactFollower.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace follower {

// One stored sample: the study's driver, the cam's angle a (mate a, rad)
// and the follower's DOF b (mm or rad), as the run left them.
struct Sample {
  size_t index = 0;
  double driver = 0.0, a = 0.0, b = 0.0;
};

// The generalised inertia and gravity force of everything that moves with
// the follower's DOF, at q (b's unit): SI — M in kg (a slide) or kg m^2 (a
// turn), Q in N or N m along +q, dM per metre or per radian of q.
struct MassAt {
  double M = 0.0, Q = 0.0, dM = 0.0;
};

// The motion the cam was cut for, when its body is a CamProfile's.
struct Designed {
  cammech::MotionSpec motion;
  cammech::FollowerSpec follower;
  bool ccw = true;
  double baseRadius = 0.0;
  double fitDeviation = 0.0;    // mm
  glm::dmat4 frameInCamPart{1}; // the feature's frame in the cam part
};

struct Input {
  const contact::Model* model = nullptr;
  std::vector<Sample> samples;
  std::function<MassAt(double q)> mass; // empty: no gravity section
  std::optional<Designed> designed;
  double driverPerCam = 1.0;            // d driver / d a, from the samples
  std::optional<double> speedRpm;       // the study driver's
  std::optional<double> maxPressureDeg; // the caller's limit
};

struct Range {
  double fromDeg = 0.0, toDeg = 0.0; // cam angles
};

struct SampleOut {
  size_t index = 0;
  double camDeg = 0.0;
  double pressureDeg = 0.0;
  double camRadius = 0.0, followerRadius = 0.0; // signed: + convex
  bool bridge = false;
  double deviation = 0.0; // b's unit; only with a design
};

struct DropOut {
  double atDeg = 0.0;  // cam angle
  double height = 0.0; // b's unit
  double fallS = 0.0;
  double dwellDeg = 0.0; // how long the cam holds the landing level
  double dropCamRpm = 0.0, dropRpm = 0.0; // the speed it just lands in time
  std::optional<double> turnDeg;          // at speedRpm: cam turn while falling
  std::optional<bool> lands;
};

struct Report {
  std::string error;
  bool turn = false; // b turns: its unit is radians (degrees reported)
  std::vector<SampleOut> samples;
  double maxPressureDeg = 0.0, maxPressureAtDeg = 0.0;
  std::optional<bool> pressureOk; // only with a limit
  double minCamConvex = 0.0, minCamConcave = 0.0, followerRadius = 0.0;
  std::vector<Range> bridges;
  bool deviationKnown = false;
  double deviation = 0.0, deviationAtDeg = 0.0, mateZero = 0.0;
  double fitDeviation = 0.0;
  bool gravityKnown = false;
  std::optional<double> liftoffCamRpm, liftoffRpm; // empty: never leaves
  double liftoffAtDeg = 0.0;
  std::vector<Range> notHeld; // gravity pushes it off: any speed
  std::vector<DropOut> drops;
  std::optional<bool> holdsAtSpeed;
};

Report Check(const Input& in);

// The check across the width, of one sample: the cam and the follower cut
// at `kLevels` heights spread over the width they share (the solve cuts
// only the middle), each solved for its first touch at the sample's a. The
// follower, standing at b, may enter none of them by more than the contact
// tolerance; `clash` is the deepest it does, in b's unit, and `level` the
// height (mm from the shared width's start). Sections are cut once per
// model (`Levels`).
inline constexpr int kLevels = 9;
struct Level {
  double z = 0.0; // in the plane frame
  contact::Outline cam, follower;
};
std::vector<Level> Levels(const contact::Model& m, std::string* why);
struct Across {
  bool ok = true;
  double clash = 0.0; // b's unit
  double atZ = 0.0;   // mm across the shared width
  double gap = 0.0;   // b's unit: b against the middle cut's solve
};
Across AcrossWidth(const contact::Model& m, const std::vector<Level>& levels,
                   double a, double b);

} // namespace follower
