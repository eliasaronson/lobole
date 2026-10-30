// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/AxisRef.h"
#include "core/Feature.h"
#include "core/MeshFit.h"

#include <glm/glm.hpp>
#include <string>

enum class DatumAxisKind {
  TwoPoints,
  SketchLine,
  GlobalX,
  GlobalY,
  GlobalZ,
  FromFit
};

// A named model-space axis as its own history feature (modeling M2). Produces
// a cached origin + unit direction that features reference through
// AxisRef::Kind::DatumAxis (revolve, angled datum plane, later 3D patterns).
// Toponaming-free: built from world points, a global axis, or a sketch line by
// stable id — never a B-Rep edge — or (FromFit) the axis of a cylinder or
// cone refitted to a body's triangles round a stored point every
// regeneration (docs/modeling/README.md#datums-on-a-scan). Contributes no body;
// a degenerate definition fails Execute, stopping regeneration there
// (datum-plane contract).
class DatumAxisFeature : public Feature {
public:
  DatumAxisFeature(const std::string& name,
                   DatumAxisKind kind = DatumAxisKind::TwoPoints);

  [[nodiscard]] std::string TypeName() const override { return "DatumAxis"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DatumAxisFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<DatumAxisFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(sketchId);          // SketchLine kind; 0 for the base axes
    visit(fit.bodyFeatureId); // FromFit: the body's feature
  }

  [[nodiscard]] bool IsDatum() const override { return true; }

  [[nodiscard]] const glm::dvec3& Origin() const { return m_origin; }
  [[nodiscard]] const glm::dvec3& Direction() const { return m_dir; } // unit

  DatumAxisKind kind = DatumAxisKind::TwoPoints;
  glm::dvec3 p0{0.0}, p1{1.0, 0.0, 0.0};    // TwoPoints (world space)
  uint64_t sketchId = 0;                    // SketchLine: sketch feature id
  uint32_t entityId = 0;                    // SketchLine: stable entity id
  // FromFit: the body, the seed point and the kind (cylinder or cone).
  meshfit::FitRef fit;
  // What the last Execute fitted ({} when it failed or is not FromFit).
  [[nodiscard]] const nlohmann::json& FitReport() const { return m_fitReport; }

private:
  glm::dvec3 m_origin{0.0};
  glm::dvec3 m_dir{1.0, 0.0, 0.0};
  nlohmann::json m_fitReport;
};
