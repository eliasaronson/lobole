// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mate limits (docs/assemblies/README.md#mate-limits): which values of a
// mate type a range may bound, where each stands, and the one wording every
// refusal of a value past a range uses -- set_mate's, a study's, a
// sequence's and the solve's own.

#include "core/AssemblySolve.h"

#include "core/Angles.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace assembly {

namespace {

// A value AT its bound fits: a loop projected onto a limit puts it there
// exactly, and degrees converted to radians on both sides agree to far
// better than this.
constexpr double kRangeSlack = 1e-9;

const char* TypeWord(MateType t) {
  switch (t) {
  case MateType::Fastened:
    return "fastened";
  case MateType::Revolute:
    return "revolute";
  case MateType::Slider:
    return "slider";
  case MateType::Cylindrical:
    return "cylindrical";
  case MateType::Planar:
    return "planar";
  case MateType::Ball:
    return "ball";
  }
  return "?";
}

std::string MateLabel(const Mate& m) {
  return "mate " + std::to_string(m.id) +
         (m.name.empty() ? std::string() : " ('" + m.name + "')");
}

bool IsAngle(RangedValue v) {
  return v == RangedValue::Rotation || v == RangedValue::Cone;
}

} // namespace

std::vector<RangedValue> RangedValuesFor(MateType type) {
  switch (type) {
  case MateType::Revolute:
    return {RangedValue::Rotation};
  case MateType::Slider:
    return {RangedValue::Offset};
  case MateType::Cylindrical:
    return {RangedValue::Rotation, RangedValue::Offset};
  case MateType::Planar:
    return {RangedValue::Rotation, RangedValue::Offset, RangedValue::Dx,
            RangedValue::Dy};
  case MateType::Ball:
    return {RangedValue::Cone};
  case MateType::Fastened:
    break;
  }
  return {};
}

const char* RangedValueName(RangedValue v) {
  switch (v) {
  case RangedValue::Rotation:
    return "rotation";
  case RangedValue::Offset:
    return "offset";
  case RangedValue::Dx:
    return "dx";
  case RangedValue::Dy:
    return "dy";
  case RangedValue::Cone:
    return "cone";
  }
  return "?";
}

std::string FormatRangedValue(RangedValue v, double value) {
  char buf[64];
  if (IsAngle(v))
    std::snprintf(buf, sizeof buf, "%.6g°", angles::ToDegrees(value));
  else
    std::snprintf(buf, sizeof buf, "%.6g mm", value);
  return buf;
}

std::optional<MateRange> RangeOf(const Mate& m, RangedValue v) {
  switch (v) {
  case RangedValue::Rotation:
    return m.rotationRange;
  case RangedValue::Offset:
    return m.offsetRange;
  case RangedValue::Dx:
    return m.dxRange;
  case RangedValue::Dy:
    return m.dyRange;
  case RangedValue::Cone:
    if (m.coneRad)
      return MateRange{0.0, *m.coneRad};
    return std::nullopt;
  }
  return std::nullopt;
}

double RangedValueOf(const Mate& m, RangedValue v) {
  switch (v) {
  case RangedValue::Rotation:
    return m.rotation;
  case RangedValue::Offset:
    return m.offset;
  case RangedValue::Dx:
    return m.dx;
  case RangedValue::Dy:
    return m.dy;
  case RangedValue::Cone: {
    // The tilt of b's Z: the angle ballRotation turns the Z axis through.
    const glm::dvec3 z = glm::normalize(m.ballRotation) * glm::dvec3(0, 0, 1);
    return std::acos(std::clamp(z.z, -1.0, 1.0));
  }
  }
  return 0.0;
}

std::string MateRangeProblem(const Mate& m) {
  const std::vector<RangedValue> freed = RangedValuesFor(m.type);
  std::string frees;
  for (RangedValue v : freed)
    frees += (frees.empty() ? "" : ", ") + std::string(RangedValueName(v));
  for (RangedValue v : {RangedValue::Rotation, RangedValue::Offset,
                        RangedValue::Dx, RangedValue::Dy, RangedValue::Cone}) {
    const std::optional<MateRange> r = RangeOf(m, v);
    if (!r)
      continue;
    if (std::find(freed.begin(), freed.end(), v) == freed.end())
      return MateLabel(m) + " is " + TypeWord(m.type) + ", which " +
             (freed.empty() ? std::string("frees nothing to limit")
                            : "limits only its " + frees) +
             "; it has no " + RangedValueName(v) + " range";
    if (!std::isfinite(r->min) || !std::isfinite(r->max))
      return MateLabel(m) + ": a " + RangedValueName(v) +
             " range is two finite numbers";
    if (v == RangedValue::Cone) {
      if (r->max < 0.0 || r->max > angles::ToRadians(180.0))
        return MateLabel(m) + ": a cone's half-angle is 0..180°, not " +
               FormatRangedValue(v, r->max);
      continue;
    }
    if (r->min > r->max)
      return MateLabel(m) + ": its " + RangedValueName(v) + " range's min " +
             FormatRangedValue(v, r->min) + " is above its max " +
             FormatRangedValue(v, r->max);
  }
  return {};
}

std::string RangeRefusal(const Mate& m, RangedValue v, double value) {
  const std::optional<MateRange> r = RangeOf(m, v);
  if (!r)
    return {};
  if (value >= r->min - kRangeSlack && value <= r->max + kRangeSlack)
    return {};
  if (v == RangedValue::Cone)
    return MateLabel(m) + ": a tilt of " + FormatRangedValue(v, value) +
           " is outside its cone of " + FormatRangedValue(v, r->max);
  return MateLabel(m) + ": " + RangedValueName(v) + " " +
         FormatRangedValue(v, value) + " is outside its range " +
         FormatRangedValue(v, r->min) + " .. " + FormatRangedValue(v, r->max);
}

std::string RangeRefusal(const Mate& m, MateDof dof, double value) {
  return RangeRefusal(
      m, dof == MateDof::Rotation ? RangedValue::Rotation : RangedValue::Offset,
      value);
}

std::string MateOutsideRange(const Mate& m) {
  for (RangedValue v : RangedValuesFor(m.type))
    if (std::string why = RangeRefusal(m, v, RangedValueOf(m, v)); !why.empty())
      return why;
  return {};
}

std::string AssemblyOutsideRange(const AssemblyDocument& doc) {
  for (const Mate& m : doc.GetMates()) {
    if (doc.MateSuppressed(m.id))
      continue;
    if (std::string why = MateOutsideRange(m); !why.empty())
      return why;
  }
  return {};
}

double ClampToRange(const std::optional<MateRange>& r, double value) {
  if (!r)
    return value;
  return std::clamp(value, r->min.value, std::max(r->min.value, r->max.value));
}

glm::dquat ClampToCone(const std::optional<ExprAngle>& coneRad,
                       const glm::dquat& ballRotation) {
  if (!coneRad)
    return ballRotation;
  const glm::dquat q = glm::normalize(ballRotation);
  // Swing-twist about Z: q = swing * twist, the twist a turn about Z and the
  // swing what tilts Z. Only the swing is limited.
  glm::dquat twist(q.w, 0.0, 0.0, q.z);
  const double tl = glm::length(twist);
  twist = tl > 1e-12 ? twist / tl : glm::dquat(1.0, 0.0, 0.0, 0.0);
  const glm::dquat swing = q * glm::conjugate(twist);
  const glm::dvec3 axis(swing.x, swing.y, swing.z);
  const double s = glm::length(axis);
  const double tilt = 2.0 * std::atan2(s, std::abs(swing.w));
  if (tilt <= *coneRad || s < 1e-15)
    return q;
  const glm::dvec3 dir = (swing.w < 0.0 ? -axis : axis) / s;
  return glm::normalize(glm::angleAxis(coneRad->value, dir) * twist);
}

} // namespace assembly
