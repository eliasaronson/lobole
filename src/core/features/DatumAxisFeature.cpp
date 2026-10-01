#include "core/features/DatumAxisFeature.h"

#include "common/Logger.h"

#include <glm/geometric.hpp>

DatumAxisFeature::DatumAxisFeature(const std::string& name, DatumAxisKind kind)
    : kind(kind) {
  m_name = name;
}

bool DatumAxisFeature::Execute(RegenContext& ctx) {
  switch (kind) {
  case DatumAxisKind::GlobalX:
    m_origin = glm::dvec3(0.0);
    m_dir = glm::dvec3(1.0, 0.0, 0.0);
    return true;
  case DatumAxisKind::GlobalY:
    m_origin = glm::dvec3(0.0);
    m_dir = glm::dvec3(0.0, 1.0, 0.0);
    return true;
  case DatumAxisKind::GlobalZ:
    m_origin = glm::dvec3(0.0);
    m_dir = glm::dvec3(0.0, 0.0, 1.0);
    return true;
  case DatumAxisKind::TwoPoints: {
    const glm::dvec3 d = p1 - p0;
    if (glm::dot(d, d) < 1e-12) {
      SetError("the two points are coincident");
      return false;
    }
    m_origin = p0;
    m_dir = glm::normalize(d);
    return true;
  }
  case DatumAxisKind::SketchLine: {
    // Reuse the shared sketch-line resolution (FindById + zero-length guard).
    AxisRef ref;
    ref.kind = AxisRef::Kind::SketchLine;
    ref.sketchId = sketchId;
    ref.entityId = entityId;
    if (!ResolveAxis(ctx, ref, m_origin, m_dir)) {
      SetError("sketch line not found or degenerate");
      return false;
    }
    return true;
  }
  }
  return false;
}
