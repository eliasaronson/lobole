#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"
#include "cad/SketchPlane.h" // PlaneType

#include <cstdint>
#include <string>
#include <vector>

// Tapers faces of one body by a draft angle (modeling M8): each face named by
// a stable ElementRef is tilted about its intersection line with a neutral
// plane, so a mould/casting can be pulled out (positive angle widens toward
// the neutral plane). BRepOffsetAPI_DraftAngle. All refs must address the
// same body (ref.featureId). The neutral plane is a base plane (XY/XZ/YZ) at
// the body's location — v1 keeps it simple; a datum-plane neutral is later.
// The body is modified in place; the drafted result gets fresh primitive-
// style names (draft reshapes faces). Fails per-feature when a ref no longer
// resolves or OCC rejects the taper (angle too large / self-intersecting).
class DraftFeature : public Feature {
public:
  DraftFeature(const std::string& name, uint64_t bodyId,
               std::vector<ElementRef> faces, double angleDeg,
               PlaneType neutralPlane = PlaneType::XY);

  std::string TypeName() const override { return "Draft"; }
  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(angleExpr, angleDeg);
  }

  uint64_t bodyId = 0;
  std::vector<ElementRef> faces;
  double angleDeg = 5.0;
  std::string angleExpr;
  PlaneType neutralPlane = PlaneType::XY; // pull direction = this plane's normal
};
