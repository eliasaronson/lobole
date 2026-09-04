#pragma once

#include "core/AxisRef.h"
#include "core/Feature.h"

#include <glm/glm.hpp>
#include <string>

enum class DatumAxisKind { TwoPoints, SketchLine, GlobalX, GlobalY, GlobalZ };

// A named model-space axis as its own history feature (modeling M2). Produces
// a cached origin + unit direction that features reference through
// AxisRef::Kind::DatumAxis (revolve, angled datum plane, later 3D patterns).
// Toponaming-free: built from world points, a global axis, or a sketch line by
// stable id — never a B-Rep edge. Contributes no body; a degenerate definition
// fails Execute, stopping regeneration there (datum-plane contract).
class DatumAxisFeature : public Feature {
public:
  DatumAxisFeature(const std::string& name,
                   DatumAxisKind kind = DatumAxisKind::TwoPoints);

  [[nodiscard]] std::string TypeName() const override { return "DatumAxis"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DatumAxisFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  [[nodiscard]] bool IsDatum() const override { return true; }

  [[nodiscard]] const glm::vec3& Origin() const { return m_origin; }
  [[nodiscard]] const glm::vec3& Direction() const { return m_dir; } // unit

  DatumAxisKind kind = DatumAxisKind::TwoPoints;
  glm::vec3 p0{0.0f}, p1{1.0f, 0.0f, 0.0f}; // TwoPoints (world space)
  uint64_t sketchId = 0;                    // SketchLine: sketch feature id
  uint32_t entityId = 0;                    // SketchLine: stable entity id

private:
  glm::vec3 m_origin{0.0f};
  glm::vec3 m_dir{1.0f, 0.0f, 0.0f};
};
