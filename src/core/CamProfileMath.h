// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
// The math of a disc cam (mechanism, not machining): a follower motion as a
// law of the cam's angle, and the profile that produces it for a stated
// follower (docs/modeling/cam-profiles.md). Pure — no OpenCASCADE, no
// document — so the CamProfile feature builds from it and the follower
// check (FollowerCheck.h) evaluates the designed motion without a regen.
//
// Conventions. The cam's own frame is X, Y in its plane and Z its axis. At
// cam angle theta = 0 the follower sits on +X: a sliding follower moves
// along +X (away from the axis) on the line y = offset; a turning
// follower's pivot is at (pivot_x, pivot_y) and its contact point swings on
// the arm about it. The cam turns by theta about +Z (`ccw`) or -Z; seen
// from the cam, the follower then stands at angle -theta (or +theta).
// Lift is mm for a sliding follower and RADIANS for a turning one (degrees
// only at the surface); positive lift moves the follower away from the
// axis.

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <string>
#include <utility>
#include <vector>

namespace cammech {

enum class Law { Cycloidal, Harmonic, Poly345, Dwell, Drop };
const char* LawName(Law l);
// Empty when `name` is not a law; the caller refuses, naming kLawNames.
bool LawFromName(const std::string& name, Law* out);
inline constexpr const char* kLawNames =
    "cycloidal, harmonic, poly345, dwell, drop";

struct Segment {
  double toDeg = 0.0; // where the segment ends, degrees of cam turn
  double rise = 0.0;  // mm (slide) or degrees (turn); negative falls
  Law law = Law::Cycloidal;
};

enum class FollowerKind { Roller, Flat, Knife };
enum class FollowerMotion { Slide, Turn };
const char* FollowerKindName(FollowerKind k);
const char* FollowerMotionName(FollowerMotion m);

struct FollowerSpec {
  FollowerKind kind = FollowerKind::Roller;
  FollowerMotion motion = FollowerMotion::Slide;
  double radiusMm = 0.0; // roller radius; a knife's tip radius (0 = sharp)
  double offsetMm = 0.0; // slide: the line's offset from the axis (along Y)
  double pivotXMm = 0.0; // turn: the pivot, in the cam's plane
  double pivotYMm = 0.0;
  double armMm = 0.0; // turn: pivot to the contact point (a roller's centre)
};

// The follower motion: a table (`lift`, [[angle_deg, lift]], the cam
// relation's form; lift mm or degrees) or a segment list. Exactly one.
struct MotionSpec {
  std::vector<std::pair<double, double>> lift;
  std::vector<Segment> segments;
};

// Why `m` is not a motion for `f` (empty when it is): one of lift/segments;
// a table that wraps a turn; segments that rise past each other, close the
// turn at 360 and return to their start; a drop of zero width; `rise_deg`
// only on a turning follower.
std::string MotionError(const MotionSpec& m, const FollowerSpec& f);
// Why `f` is not a follower a cam can be cut for (empty when it is).
std::string FollowerError(const FollowerSpec& f, double baseRadiusMm);

struct Drop {
  double atDeg = 0.0; // cam angle of the step
  double from = 0.0;  // lift just before (mm or rad)
  double to = 0.0;    // lift just after
};

// The designed lift at cam angle `deg` (mm or rad), with its first and
// second derivatives per RADIAN of cam turn. At a drop the value is the one
// AFTER the step (the follower has landed).
struct LiftAt {
  double s = 0.0, ds = 0.0, dds = 0.0;
};
// A motion prepared once (segment starts accumulated, a table converted to
// the internal unit, the base circle found) for repeated evaluation.
class PreparedMotion {
public:
  PreparedMotion(MotionSpec m, const FollowerSpec& f);
  // `before` asks for the side of a drop at exactly `deg` before the step.
  LiftAt At(double deg, bool before = false) const;
  const std::vector<Drop>& Drops() const { return drops_; }

private:
  LiftAt Raw(double deg, bool before) const;
  std::vector<Drop> RawDrops() const;
  MotionSpec m_;
  double unit_ = 1.0;
  std::vector<std::pair<double, double>> table_;
  double offset_ = 0.0;
  std::vector<Drop> drops_;
};

LiftAt EvaluateMotion(const MotionSpec& m, const FollowerSpec& f, double deg);
std::vector<Drop> MotionDrops(const MotionSpec& m, const FollowerSpec& f);
// The lift's range over a turn (mm or rad).
std::pair<double, double> MotionRange(const MotionSpec& m,
                                      const FollowerSpec& f);

struct ThetaRange {
  double fromDeg = 0.0, toDeg = 0.0;
};

// One continuous run of the profile in the cam's frame, from `fromDeg` to
// `toDeg` of cam turn (a whole turn when there is no drop). Points every
// `stepDeg`; `mid` are the exact envelope at each step's midpoint, against
// which a fitted curve's deviation is measured.
struct ProfileSpan {
  double fromDeg = 0.0, toDeg = 0.0;
  std::vector<glm::dvec2> points;
  std::vector<glm::dvec2> mid;
};

struct Profile {
  std::string error;   // why it was not built (undercut, an impossible
                       // follower); the fields below still report
  bool closed = false; // one periodic span (no drop)
  std::vector<ProfileSpan> spans; // a drop's step face joins span k's end
                                  // to span k+1's start (wrapping)
  std::vector<Drop> drops;
  std::vector<ThetaRange> undercut;
  double minBaseRadiusMm = 0.0; // the smallest that clears the undercut
  double maxPressureAngleDeg = 0.0, maxPressureAtDeg = 0.0;
  double minConvexRadiusMm = 0.0;  // the profile's, 0 when none
  double minConcaveRadiusMm = 0.0; // 0 when the profile has no hollow
  double flatWidthMm = 0.0;        // flat face: the contact's travel on it
  double flatContactMinMm = 0.0, flatContactMaxMm = 0.0;
  double maxRadiusMm = 0.0;
};

Profile BuildProfile(const MotionSpec& m, const FollowerSpec& f,
                     double baseRadiusMm, bool ccw, double stepDeg = 1.0);

// The pressure angle (radians) the designed motion gives at `deg`: between
// the contact normal and the follower's direction of motion there.
double DesignedPressureAngle(const MotionSpec& m, const FollowerSpec& f,
                             double baseRadiusMm, bool ccw, double deg);

// The follower's reference point (roller centre, knife tip centre, or the
// flat face's foot on its line) in the GROUND frame at cam angle `deg`,
// before the cam's turn is undone. Exposed for tests.
glm::dvec2 PitchPointGround(const MotionSpec& m, const FollowerSpec& f,
                            double baseRadiusMm, double deg);

} // namespace cammech
