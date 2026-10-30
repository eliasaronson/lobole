// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/DatumPlaneFeature.h"

#include "common/Logger.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace {

// Near-parallel test for unit vectors; the tolerance is generous because a
// nearly-degenerate plane is as useless as an exactly-degenerate one.
bool NearlyParallel(const glm::dvec3& a, const glm::dvec3& b) {
  return std::abs(glm::dot(a, b)) > 0.999;
}

} // namespace

bool ResolveAxis(const RegenContext& ctx, const AxisRef& ref,
                 glm::dvec3& outOrigin, glm::dvec3& outDir) {
  switch (ref.kind) {
  case AxisRef::Kind::X:
    outOrigin = glm::dvec3(0.0);
    outDir = glm::dvec3(1.0, 0.0, 0.0);
    return true;
  case AxisRef::Kind::Y:
    outOrigin = glm::dvec3(0.0);
    outDir = glm::dvec3(0.0, 1.0, 0.0);
    return true;
  case AxisRef::Kind::Z:
    outOrigin = glm::dvec3(0.0);
    outDir = glm::dvec3(0.0, 0.0, 1.0);
    return true;
  case AxisRef::Kind::SketchLine: {
    const auto* sketch =
        dynamic_cast<const SketchFeature*>(ctx.FindById(ref.sketchId));
    if (sketch == nullptr) {
      return false;
    }
    const size_t index = sketch->IndexOfEntity(ref.entityId);
    if (index == SIZE_MAX) {
      return false;
    }
    const auto* line = std::get_if<sketch::Line>(&sketch->entities[index]);
    if (line == nullptr) {
      return false;
    }
    const glm::dvec3 a = sketch->plane.PlaneToWorld(line->a);
    const glm::dvec3 b = sketch->plane.PlaneToWorld(line->b);
    const glm::dvec3 d = b - a;
    if (glm::dot(d, d) < 1e-12) {
      return false; // zero-length line
    }
    outOrigin = a;
    outDir = glm::normalize(d);
    return true;
  }
  case AxisRef::Kind::Custom: {
    const glm::dvec3 d = ref.direction;
    if (glm::dot(d, d) < 1e-18) {
      return false; // no direction to turn about
    }
    outOrigin = glm::dvec3(ref.origin);
    outDir = glm::dvec3(glm::normalize(d));
    return true;
  }
  case AxisRef::Kind::DatumAxis: {
    const auto* datum =
        dynamic_cast<const DatumAxisFeature*>(ctx.FindById(ref.datumAxisId));
    if (datum == nullptr) {
      return false; // earlier feature, so it already Executed successfully
    }
    outOrigin = datum->Origin();
    outDir = datum->Direction();
    return true;
  }
  }
  return false;
}

std::optional<SketchPlane> ResolveParentPlane(const RegenContext& ctx,
                                              uint64_t parentId,
                                              PlaneType basePlane) {
  if (parentId == 0) {
    return SketchPlane(basePlane);
  }
  const auto* datum =
      dynamic_cast<const DatumPlaneFeature*>(ctx.FindById(parentId));
  if (datum == nullptr) {
    return std::nullopt;
  }
  return datum->OutputPlane();
}

DatumPlaneFeature::DatumPlaneFeature(const std::string& name,
                                     DatumPlaneKind kind)
    : kind(kind) {
  m_name = name;
}

bool DatumPlaneFeature::Execute(RegenContext& ctx) {
  // What a failed evaluation leaves is the default, as on a fresh object
  // -- never the last good one, which a dependent would build on.
  m_outPlane = SketchPlane(PlaneType::XY);
  m_fitReport = nlohmann::json();
  switch (kind) {
  case DatumPlaneKind::Offset: {
    const auto parent = ResolveParentPlane(ctx, parentId, basePlane);
    if (!parent) {
      SetError("parent plane not found");
      return false;
    }
    const double d = flip ? -offset : offset;
    m_outPlane = SketchPlane(parent->GetOrigin() + parent->GetNormal() * d,
                             parent->GetNormal(), parent->GetUAxis());
    return true;
  }
  case DatumPlaneKind::Angled: {
    const auto parent = ResolveParentPlane(ctx, parentId, basePlane);
    if (!parent) {
      SetError("parent plane not found");
      return false;
    }
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDir{1.0, 0.0, 0.0};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      SetError("axis not found");
      return false;
    }
    if (NearlyParallel(axisDir, parent->GetNormal())) {
      SetError("axis is parallel to the parent normal");
      return false;
    }
    const double rad = glm::radians(angleDeg);
    const glm::dmat3 rot(glm::rotate(glm::dmat4(1.0), rad, axisDir));
    // The rotated plane hinges on the axis: the axis lies in the plane
    // (uAxis follows it) and the parent normal tips around it.
    m_outPlane = SketchPlane(axisOrigin, rot * parent->GetNormal(), axisDir);
    return true;
  }
  case DatumPlaneKind::ThroughPoints: {
    const glm::dvec3 n = glm::cross(p1 - p0, p2 - p0);
    if (glm::dot(n, n) < 1e-12) {
      SetError("the three points are collinear");
      return false;
    }
    m_outPlane = SketchPlane(p0, glm::normalize(n), glm::normalize(p1 - p0));
    return true;
  }
  case DatumPlaneKind::FromFit: {
    if (fit.kind != mesh::Kind::Plane) {
      SetError(std::string("a datum plane fits a plane, not a ") +
               mesh::KindName(fit.kind) +
               " (a cylinder's or cone's axis is a DatumAxis from_fit)");
      return false;
    }
    meshfit::Result r;
    std::string error;
    if (!meshfit::Refit(ctx, fit, r, error)) {
      SetError(error);
      return false;
    }
    const glm::dvec3 normal = glm::normalize(r.region.fit.axis);
    // The plane's own X: the world's X laid into it, else the world's Y --
    // fixed by the world, so a refit turns it only as far as the normal.
    glm::dvec3 u = glm::dvec3(1, 0, 0) - normal.x * normal;
    if (glm::length(u) < 0.5)
      u = glm::dvec3(0, 1, 0) - normal.y * normal;
    m_outPlane = SketchPlane(r.region.fit.Project(fit.pointMm), normal,
                             glm::normalize(u));
    m_fitReport = meshfit::ResultJson(r);
    return true;
  }
  }
  return false;
}
