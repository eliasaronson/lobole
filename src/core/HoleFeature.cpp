#include "core/HoleFeature.h"

#include "CADKernel.h"
#include "Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/SketchFeature.h"

#include <variant>

HoleFeature::HoleFeature(const std::string& name, uint64_t sketchId,
                         HoleKind kind, double diameter, double depth)
    : sketchId(sketchId), kind(kind), diameter(diameter), depth(depth) {
  m_name = name;
}

bool HoleFeature::Execute(RegenContext& ctx) {
  const auto* sk =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
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

  // Build one tool solid per hole (bore + optional cbore/csink), then union
  // them all into a single cutter.
  CADShape tool;
  for (const glm::vec3& c : centers) {
    CADShape hole = kernel.CreateCylinder(c, axis, r, boreDepth);
    if (kind == HoleKind::Counterbore && cbDiameter > diameter &&
        cbDepth > 1e-9) {
      hole = hole.Union(kernel.CreateCylinder(c, axis, cbDiameter / 2.0,
                                              cbDepth));
    } else if (kind == HoleKind::Countersink && csDiameter > diameter &&
               csDepth > 1e-9) {
      // Cone from the surface (csDiameter/2) tapering down to the bore radius.
      hole = hole.Union(
          kernel.CreateCone(c, axis, csDiameter / 2.0, r, csDepth));
    }
    if (!hole.IsValid()) {
      SetError("hole tool construction failed");
      return false;
    }
    tool = tool.IsValid() ? tool.Union(hole) : std::move(hole);
  }
  if (!tool.IsValid()) {
    SetError("hole tool is empty");
    return false;
  }

  ElementMap toolMap;
  topo::NamePrimitiveElements(tool, std::to_string(GetId()), toolMap);
  if (!ApplyBodyMode(ctx.bodies, std::move(tool), std::move(toolMap),
                     ExtrudeMode::Cut, m_name, m_material, GetId())) {
    SetError("hole cut failed");
    return false;
  }
  return true;
}
