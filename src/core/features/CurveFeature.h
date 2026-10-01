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
  // A bent TUBE's centreline: straights between `points`, each corner turned
  // on a former of `radius`. Exactly what a bender is programmed with, and
  // exactly line-arc-line — where a fitted spline through sampled arc points
  // is neither, has no radius anything can read back, and bows between the
  // points on the straights (docs/tube/README.md#members-and-their-paths).
  BentPath,
};

// Where a helix on `axis` starts when its start_deg is 0: the direction
// OpenCASCADE's gp_Ax3(P, V) chooses, written out so it is OUR rule and can be
// documented — +X for a Z axis, +Z for a Y axis, -Z... (see the .cpp). Every
// helix made before start_deg existed started here, so none of them moves.
[[nodiscard]] glm::dvec3 HelixReference(const glm::dvec3& axis);

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
// The name a kind is written and refused by. One spelling: the serializer
// and the RPC surface both read this, so a new kind cannot be spelled two
// ways (the `bent_path` round shipped a kind the format history did not
// know, and that is the family of mistake).
[[nodiscard]] inline const char* Curve3DKindName(Curve3DKind kind) {
  switch (kind) {
  case Curve3DKind::FitSpline:
    return "fit_spline";
  case Curve3DKind::Helix:
    return "helix";
  case Curve3DKind::ControlPoly:
    return "control_poly";
  case Curve3DKind::BentPath:
    return "bent_path";
  }
  return "fit_spline";
}

class CurveFeature : public Feature {
public:
  CurveFeature(const std::string& name,
               Curve3DKind kind = Curve3DKind::FitSpline);

  std::string TypeName() const override { return "Curve3D"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CurveFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // A bent path whose corners cannot be bent (a straight too short for the
  // bends at its ends, a doubled-back corner) is refused when it is ADDED,
  // not committed broken: it names nothing, so its geometry can only come
  // out one way, and a broken path committed anyway went on to fail the pipe
  // swept along it with a second, false message
  // (docs/tube/README.md#members-and-their-paths).
  [[nodiscard]] std::string WhyIncomplete() const override;

  // Names no other feature: a 3D curve is its own points.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  [[nodiscard]] bool IsDatum() const override { return true; }
  void VisitExprs(const ExprVisitor& visit) override;

  // Result of the last Execute (transient, rebuilt each regeneration, never
  // serialized — the defining parameters below are the persistent state).
  [[nodiscard]] const std::vector<glm::dvec3>& Polyline() const {
    return m_polyline;
  }
  // Holds a single TopoDS_Edge; downstream sweep/loft extract it. Null shape
  // when Execute failed.
  [[nodiscard]] const CADShape& EdgeShape() const { return m_edgeShape; }

  Curve3DKind kind = Curve3DKind::FitSpline;

  // FitSpline: fit points to interpolate. ControlPoly: control poles.
  std::vector<glm::dvec3> points;
  int degree = 3;      // ControlPoly effective degree (clamped to points-1)
  bool closed = false; // FitSpline periodic (ControlPoly ignores in v1)

  // BentPath: one bend record per corner, filled by Execute — the schedule a
  // shop bends from, and what a check reads. Transient, like the polyline.
  struct Bend {
    glm::dvec3 corner{0.0};
    double angleDeg = 0.0;       // how far the tube turns at this corner
    double radius = 0.0;         // the former's radius (the centreline's)
    double straightBefore = 0.0; // free length before it, mm
    double rollDeg = 0.0;        // rotation about the tube from the last bend
  };
  [[nodiscard]] const std::vector<Bend>& Bends() const { return m_bends; }
  [[nodiscard]] double Length() const { return m_length; }

  // Helix parameters (axis frame + geometry).
  glm::dvec3 axisOrigin{0.0};
  glm::dvec3 axisDir{0.0, 0.0, 1.0};
  double radius = 1.0;
  double pitch = 1.0; // rise along the axis per full turn
  double turns = 1.0;
  bool rightHanded = true;
  // Where the helix starts round its axis (v128), degrees, right-handed about
  // the axis from HelixReference(axisDir). 0 is where every helix started
  // before the field existed.
  double startDeg = 0.0;
  std::string radiusExpr, pitchExpr, turnsExpr; // expression-driven (v42)
  std::string startDegExpr;                     // v128

private:
  std::vector<glm::dvec3> m_polyline;
  CADShape m_edgeShape;
  std::vector<Bend> m_bends;
  double m_length = 0.0;
};
