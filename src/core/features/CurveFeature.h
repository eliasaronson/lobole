#pragma once

#include "core/Feature.h" // Feature + CADShape (via CADKernel.h)

#include <glm/glm.hpp>
#include <string>
#include <vector>

// How a Curve3D builds its geometry. Stored as a plain enum (not a variant) so
// each kind's parameters live as members, mirroring DatumAxisFeature — the
// serializer emits only the active kind's fields.
enum class Curve3DKind {
  FitSpline,   // interpolate through `points` (OCC GeomAPI_Interpolate)
  Helix,       // procedural helix on an axis
  ControlPoly, // B-spline with `points` as control poles (clamped, uniform)
};

// A free 3D curve as its own history feature — the first Designer geometry that
// is NOT a sketch entity and NOT a solid body (curves-3d-plan Phase 0). Like a
// datum, it contributes no body: Execute caches a resulting OCC edge plus a
// tessellated polyline that downstream features (Sweep path, Loft guide) and
// the viewport read back. A degenerate definition fails Execute, stopping
// regeneration there (the datum contract).
//
// Reuses the 2D-spline geometry machinery conceptually: FitSpline/ControlPoly
// end in a Geom_BSplineCurve, Helix in a curve-on-surface edge. All three
// tessellate through the same GCPnts sampler into `m_polyline`.
class CurveFeature : public Feature {
public:
  CurveFeature(const std::string& name,
               Curve3DKind kind = Curve3DKind::FitSpline);

  std::string TypeName() const override { return "Curve3D"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CurveFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: a 3D curve is its own points.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  [[nodiscard]] bool IsDatum() const override { return true; }
  void VisitExprs(const ExprVisitor& visit) override;

  // Result of the last Execute (transient, rebuilt each regeneration, never
  // serialized — the defining parameters below are the persistent state).
  [[nodiscard]] const std::vector<glm::vec3>& Polyline() const {
    return m_polyline;
  }
  // Holds a single TopoDS_Edge; downstream sweep/loft extract it. Null shape
  // when Execute failed.
  [[nodiscard]] const CADShape& EdgeShape() const { return m_edgeShape; }

  Curve3DKind kind = Curve3DKind::FitSpline;

  // FitSpline: fit points to interpolate. ControlPoly: control poles.
  std::vector<glm::vec3> points;
  int degree = 3;      // ControlPoly effective degree (clamped to points-1)
  bool closed = false; // FitSpline periodic (ControlPoly ignores in v1)

  // Helix parameters (axis frame + geometry).
  glm::vec3 axisOrigin{0.0f};
  glm::vec3 axisDir{0.0f, 0.0f, 1.0f};
  double radius = 1.0;
  double pitch = 1.0; // rise along the axis per full turn
  double turns = 1.0;
  bool rightHanded = true;
  std::string radiusExpr, pitchExpr, turnsExpr; // expression-driven (v42)

private:
  std::vector<glm::vec3> m_polyline;
  CADShape m_edgeShape;
};
