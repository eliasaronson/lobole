#include "core/features/HoleFeature.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/features/SketchFeature.h"

#include <algorithm>
#include <variant>
#include <vector>

HoleFeature::HoleFeature(const std::string& name, uint64_t sketchId,
                         HoleKind kind, double diameter, double depth)
    : sketchId(sketchId), kind(kind), diameter(diameter), depth(depth) {
  m_name = name;
}

bool HoleFeature::Execute(RegenContext& ctx) {
  const auto* sk = dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sk) {
    SetError("hole needs a sketch of points (missing or suppressed)");
    return false;
  }
  if (diameter <= 1e-9 || (!through && depth <= 1e-9)) {
    SetError("hole diameter and depth must be positive");
    return false;
  }

  // Hole centers = the sketch's Point entities (in plane UV -> world).
  std::vector<glm::vec3> centers;
  for (const auto& e : sk->entities) {
    if (const auto* p = std::get_if<sketch::Point>(&e))
      centers.push_back(sk->plane.PlaneToWorld(p->p));
  }
  if (centers.empty()) {
    SetError("hole sketch has no points to place holes at");
    return false;
  }

  const glm::vec3 axis = -sk->plane.GetNormal(); // drill into the material
  // Through-all: drill well past any body (bbox diagonal of all bodies).
  double boreDepth = depth;
  if (through) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& b : ctx.bodies) {
      glm::vec3 bmin, bmax;
      b.shape.GetBoundingBox(bmin, bmax);
      lo = glm::min(lo, bmin);
      hi = glm::max(hi, bmax);
    }
    boreDepth = ctx.bodies.empty() ? depth : glm::length(hi - lo) + 1.0f;
  }

  auto& kernel = CADKernel::Instance();
  const double r = diameter / 2.0;

  // Build one tool solid per hole (bore + optional cbore/csink), then gather
  // them into ONE cutter.
  //
  // The gather is a compound whenever the holes are disjoint, which is the
  // normal case and the whole point: fusing them one at a time made a
  // 480-hole pattern cost 245 s (BRepAlgoAPI_Fuse re-processes the entire
  // accumulated cutter on every call), against 12 s for the compound — 27x.
  // Each hole's own bore + head DO touch, so those are still fused, twice
  // rather than n times. Measured by scratch/scale_batching.py.
  std::vector<CADShape> tools;
  tools.reserve(centers.size());
  for (const glm::vec3& c : centers) {
    CADShape hole = kernel.CreateCylinder(c, axis, r, boreDepth);
    if (kind == HoleKind::Counterbore && cbDiameter > diameter &&
        cbDepth > 1e-9) {
      hole =
          hole.Union(kernel.CreateCylinder(c, axis, cbDiameter / 2.0, cbDepth));
    } else if (kind == HoleKind::Countersink && csDiameter > diameter &&
               csDepth > 1e-9) {
      // Cone from the surface (csDiameter/2) tapering down to the bore radius.
      hole =
          hole.Union(kernel.CreateCone(c, axis, csDiameter / 2.0, r, csDepth));
    }
    if (!hole.IsValid()) {
      SetError("hole tool construction failed");
      return false;
    }
    tools.push_back(std::move(hole));
  }
  // Disjoint tools go in a compound (fast); overlapping ones must be FUSED
  // first — a boolean against a compound whose pieces interfere with each
  // other fails outright (the 480-hole benchmark, whose holes were 6 mm apart
  // at Ø6, produced no body at all). Overlap is decided on the outer radius
  // of each hole, O(n^2) on a list that is a few hundred long at worst.
  double outer = r;
  if (kind == HoleKind::Counterbore && cbDiameter > diameter)
    outer = std::max(outer, cbDiameter / 2.0);
  else if (kind == HoleKind::Countersink && csDiameter > diameter)
    outer = std::max(outer, csDiameter / 2.0);
  bool anyOverlap = false;
  for (size_t i = 0; i < centers.size() && !anyOverlap; i++) {
    for (size_t j = i + 1; j < centers.size(); j++) {
      if (glm::length(centers[i] - centers[j]) < 2.0 * outer + 1e-6) {
        anyOverlap = true;
        break;
      }
    }
  }
  CADShape tool;
  if (anyOverlap) {
    for (CADShape& piece : tools)
      tool = tool.IsValid() ? tool.Union(piece) : std::move(piece);
  } else {
    tool = CADShape::CompoundOf(tools);
  }
  if (!tool.IsValid()) {
    SetError("hole tool is empty");
    return false;
  }

  ElementMap toolMap;
  topo::NamePrimitiveElements(tool, std::to_string(GetId()), toolMap);
  if (!ApplyBodyModeReporting(ctx.bodies, std::move(tool), std::move(toolMap),
                              ExtrudeMode::Cut, *this)) {
    SetError("hole cut failed");
    return false;
  }
  return true;
}
