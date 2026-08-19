#pragma once

#include "cad/SketchPlane.h" // PlaneType
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <string>

// The geometry a split uses as its knife.
enum class SplitKnifeKind {
  Plane, // a base plane / datum plane (see datumId), offset along its normal
  Face,  // a planar face of the body itself, extended to an unbounded plane
};

// Divides one solid body into all the pieces a plane cuts it into, keeping
// every piece as its own body (Onshape's splitPart; import feature-gap #1).
// Boolean-Subtract removes material — this keeps both sides. OCC
// BRepAlgoAPI_Splitter + CADShape::SplitSolids.
//
// The target body is referenced by the stable id of the feature that produced
// it (Body::createdBy), a whole-body reference needing no toponaming. The
// knife is either a plane (a base plane or an earlier DatumPlane by id, offset
// along its normal) or one of the body's own planar faces (an ElementRef,
// extended to an unbounded plane).
//
// v1 identity limitation: every piece is stamped with this feature's id
// (createdBy = GetId()), so a *later* feature that references "the body from
// this split" (fillet/boolean/shell) binds to whichever piece is found first.
// Fine when Split is terminal (the common splitPart case) and for
// export/mass-props, which are per-body. Distinguishing the pieces for
// downstream ops is a follow-up (a body sub-ordinal); see
// docs/interchange/feature-gaps-plan.md. Faces are still individually named
// per-piece ("<id>/bN/...") so each piece stays selectable.
//
// Regeneration fails when the target body is missing, the knife plane does not
// resolve, the face ref no longer resolves or is non-planar, or the cut yields
// fewer than two solids (the plane missed the body).
class SplitFeature : public Feature {
public:
  SplitFeature(const std::string& name, uint64_t bodyId);

  [[nodiscard]] std::string TypeName() const override { return "Split"; }
  bool Execute(RegenContext& ctx) override;

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (bodyId == 0)
      return "pick the body to split";
    if (knife == SplitKnifeKind::Face && face.name.empty())
      return "pick the flat face to cut along";
    return "";
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(offsetExpr, offset);
  }

  uint64_t bodyId = 0; // producing-feature id of the body to split

  SplitKnifeKind knife = SplitKnifeKind::Plane;

  // Plane knife: 0 = one of the three base planes (basePlane), else an earlier
  // DatumPlane by feature id; `offset` shifts the plane signed along its
  // normal (so a base plane can cut a body that does not sit at the origin).
  uint64_t datumId = 0;
  PlaneType basePlane = PlaneType::XY;
  double offset = 0.0;
  std::string offsetExpr;

  // Face knife: a planar face of the target body, extended to an unbounded
  // plane. ref.featureId must be bodyId.
  ElementRef face;
};

// The offset that lands the knife on the body's midpoint along the plane's
// normal — the only offset that is guaranteed to cut a body the plane's
// direction crosses at all.
//
// This exists because "offset" is measured FROM THE CHOSEN PLANE, so a value
// derived for one plane is meaningless for the next: 1.0 above XY is
// mid-body, and 1.0 above a datum that already sits on the body's top face is
// a knife in mid-air. Both the tool's initial default and its re-derivation
// on a plane change go through here.
double CenteredSplitOffset(const glm::vec3& planeOrigin,
                           const glm::vec3& planeNormal, const glm::vec3& bmin,
                           const glm::vec3& bmax);
