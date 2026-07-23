#include "core/features/DatumPlaneFeature.h"

#include "common/Logger.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/SketchFeature.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace {

// Near-parallel test for unit vectors; the tolerance is generous because a
// nearly-degenerate plane is as useless as an exactly-degenerate one.
bool NearlyParallel(const glm::vec3& a, const glm::vec3& b) {
  return std::abs(glm::dot(a, b)) > 0.999;
}

} // namespace

bool ResolveAxis(const RegenContext& ctx, const AxisRef& ref,
                 glm::vec3& outOrigin, glm::vec3& outDir) {
  switch (ref.kind) {
    case AxisRef::Kind::X:
      outOrigin = glm::vec3(0.0f);
      outDir = glm::vec3(1.0f, 0.0f, 0.0f);
      return true;
    case AxisRef::Kind::Y:
      outOrigin = glm::vec3(0.0f);
      outDir = glm::vec3(0.0f, 1.0f, 0.0f);
      return true;
    case AxisRef::Kind::Z:
      outOrigin = glm::vec3(0.0f);
      outDir = glm::vec3(0.0f, 0.0f, 1.0f);
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
      const glm::vec3 a = sketch->plane.PlaneToWorld(line->a);
      const glm::vec3 b = sketch->plane.PlaneToWorld(line->b);
      const glm::vec3 d = b - a;
      if (glm::dot(d, d) < 1e-12f) {
        return false; // zero-length line
      }
      outOrigin = a;
      outDir = glm::normalize(d);
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
  switch (kind) {
    case DatumPlaneKind::Offset: {
      const auto parent = ResolveParentPlane(ctx, parentId, basePlane);
      if (!parent) {
        LOG_ERROR("Datum plane '{}': parent plane not found", GetName());
        return false;
      }
      const double d = flip ? -offset : offset;
      m_outPlane = SketchPlane(
          parent->GetOrigin() + parent->GetNormal() * static_cast<float>(d),
          parent->GetNormal(), parent->GetUAxis());
      return true;
    }
    case DatumPlaneKind::Angled: {
      const auto parent = ResolveParentPlane(ctx, parentId, basePlane);
      if (!parent) {
        LOG_ERROR("Datum plane '{}': parent plane not found", GetName());
        return false;
      }
      glm::vec3 axisOrigin{0.0f};
      glm::vec3 axisDir{1.0f, 0.0f, 0.0f};
      if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
        LOG_ERROR("Datum plane '{}': axis not found", GetName());
        return false;
      }
      if (NearlyParallel(axisDir, parent->GetNormal())) {
        LOG_ERROR("Datum plane '{}': axis is parallel to the parent normal",
                  GetName());
        return false;
      }
      const float rad = glm::radians(static_cast<float>(angleDeg));
      const glm::mat3 rot(glm::rotate(glm::mat4(1.0f), rad, axisDir));
      // The rotated plane hinges on the axis: the axis lies in the plane
      // (uAxis follows it) and the parent normal tips around it.
      m_outPlane = SketchPlane(axisOrigin, rot * parent->GetNormal(), axisDir);
      return true;
    }
    case DatumPlaneKind::ThroughPoints: {
      const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
      if (glm::dot(n, n) < 1e-12f) {
        LOG_ERROR("Datum plane '{}': points are collinear", GetName());
        return false;
      }
      m_outPlane = SketchPlane(p0, glm::normalize(n), glm::normalize(p1 - p0));
      return true;
    }
  }
  return false;
}
