// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/DatumAxisFeature.h"

#include "common/Logger.h"

#include <glm/geometric.hpp>

DatumAxisFeature::DatumAxisFeature(const std::string& name, DatumAxisKind kind)
    : kind(kind) {
  m_name = name;
}

bool DatumAxisFeature::Execute(RegenContext& ctx) {
  // What a failed evaluation leaves is the default, as on a fresh object
  // -- never the last good one, which a dependent would build on.
  m_origin = glm::dvec3(0.0);
  m_dir = glm::dvec3(1.0, 0.0, 0.0);
  m_fitReport = nlohmann::json();
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
  case DatumAxisKind::FromFit: {
    if (fit.kind != mesh::Kind::Cylinder && fit.kind != mesh::Kind::Cone) {
      SetError(std::string("a datum axis fits a cylinder or a cone, not a ") +
               mesh::KindName(fit.kind) +
               (fit.kind == mesh::Kind::Plane
                    ? " (a plane is a DatumPlane from_fit)"
                    : " (a sphere has no axis: fit_primitive gives its "
                      "centre)"));
      return false;
    }
    meshfit::Result r;
    std::string error;
    if (!meshfit::Refit(ctx, fit, r, error)) {
      SetError(error);
      return false;
    }
    const mesh::FitResult& f = r.region.fit;
    m_dir = glm::normalize(meshfit::CanonicalAxis(f));
    // A cone's apex; a cylinder's axis point nearest the seed.
    m_origin = f.kind == mesh::Kind::Cone
                   ? f.origin
                   : f.origin + glm::dot(fit.pointMm - f.origin, m_dir) * m_dir;
    m_fitReport = meshfit::ResultJson(r);
    return true;
  }
  }
  return false;
}
