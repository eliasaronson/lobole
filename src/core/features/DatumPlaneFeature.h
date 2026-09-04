#pragma once

#include "cad/SketchPlane.h"
#include "core/AxisRef.h"
#include "core/Feature.h"

#include <optional>
#include <string>

enum class DatumPlaneKind { Offset, Angled, ThroughPoints };

// Model-level reference plane (modeling M1). Produces a SketchPlane that
// downstream sketches reference by this feature's stable id (see
// SketchFeature::planeRef), so upstream edits move dependent sketches —
// unlike the baked face-planes this replaces. All variants are
// toponaming-free: parents are base planes or earlier datums by id, axes are
// global axes or sketch lines by stable ids.
//
// Contributes no bodies. A missing/degenerate input fails Execute, stopping
// regeneration there (same contract as ExtrudeFeature's missing sketch);
// the cached output plane keeps its last good value for UI overlays.
class DatumPlaneFeature : public Feature {
public:
  DatumPlaneFeature(const std::string& name,
                    DatumPlaneKind kind = DatumPlaneKind::Offset);

  [[nodiscard]] std::string TypeName() const override { return "DatumPlane"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DatumPlaneFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  [[nodiscard]] bool IsDatum() const override { return true; }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(offsetExpr, offset);
    visit(angleExpr, angleDeg);
  }

  // The plane this datum evaluated to on the last successful Execute.
  [[nodiscard]] const SketchPlane& OutputPlane() const { return m_outPlane; }

  DatumPlaneKind kind = DatumPlaneKind::Offset;

  // Parent plane: an earlier DatumPlane by feature id, or (when 0) one of
  // the three base planes. Used by Offset and Angled.
  uint64_t parentId = 0;
  PlaneType basePlane = PlaneType::XY;

  // Offset: signed distance along the parent normal; flip negates.
  double offset = 2.0;
  std::string offsetExpr;
  bool flip = false;

  // Angled: parent plane rotated about the axis, degrees (UI convention).
  AxisRef axis;
  double angleDeg = 45.0;
  std::string angleExpr;

  // ThroughPoints: three non-collinear world-space points.
  glm::vec3 p0{0.0f}, p1{1.0f, 0.0f, 0.0f}, p2{0.0f, 1.0f, 0.0f};

private:
  SketchPlane m_outPlane{PlaneType::XY};
};

// The parent plane a datum builds on: base plane when parentId == 0, else an
// earlier DatumPlane's output. nullopt if the reference doesn't resolve.
std::optional<SketchPlane> ResolveParentPlane(const RegenContext& ctx,
                                              uint64_t parentId,
                                              PlaneType basePlane);
