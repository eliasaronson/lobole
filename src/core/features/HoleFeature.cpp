#include "core/features/HoleFeature.h"

#include "core/parts/AxisStack.h"

#include <cstdio>

#include "core/parts/FastenerTable.h"

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

namespace {
// "a, b, c and 2 more" — the error-style rule for a list that could be long.
std::string Join(const std::vector<std::string>& items, size_t show) {
  std::string out;
  for (size_t i = 0; i < items.size() && i < show; i++)
    out += (out.empty() ? "" : ", ") + items[i];
  if (items.size() > show)
    out += " and " + std::to_string(items.size() - show) + " more";
  return out;
}
} // namespace

bool HoleFeature::Execute(RegenContext& ctx) {
  const auto* sk = dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sk) {
    SetError(
        "hole needs a sketch of points or circles (missing or suppressed)");
    return false;
  }
  // A recorded fit DERIVES the diameter (docs/modeling/hole-standards.md).
  // Done here rather than at the call site so it survives everything that
  // rebuilds a feature — undo, a reload, a configuration override, an
  // expression on the thread — instead of being a number somebody typed once
  // from a table and never re-derived.
  if (!fitThread.empty()) {
    const std::optional<parts::Fit> fit = parts::FitFromName(fitKind);
    if (!fit) {
      SetError("\"" + fitKind +
               "\" is not a fit — a hole is drilled close, normal or loose "
               "for a clearance, or tapped for a thread");
      return false;
    }
    std::string why;
    const std::optional<double> bore =
        parts::HoleDiameter(fitThread, *fit, fitPitch, &why);
    if (!bore) {
      SetError(why);
      return false;
    }
    diameter = *bore;
  }
  if (diameter <= 1e-9 || (!through && depth <= 1e-9)) {
    SetError("hole diameter and depth must be positive");
    return false;
  }

  // Hole centers = the sketch's Point entities and its Circles' centres (in
  // plane UV -> world). Circles count so that a hole can be placed on
  // another body's hole WITHOUT copying its coordinates: external geometry
  // of a Derived ghost's hole edge projects to a (construction) circle, and
  // that circle is the centre. The SO-101 gripper dogfood carried every
  // servo and camera hole centre by hand before this.
  // Arcs count too, and coincident centres merge: a hole whose wall the
  // source split in two (the camera PCB's) projects as two half-arcs, and a
  // cylinder projected along its axis as two circles.
  std::vector<glm::vec3> centers;
  const auto add = [&](const glm::vec2& uv) {
    const glm::vec3 w = sk->plane.PlaneToWorld(uv);
    for (const glm::vec3& c : centers)
      if (glm::length(c - w) < 1e-4f)
        return;
    centers.push_back(w);
  };
  for (const auto& e : sk->entities) {
    if (const auto* p = std::get_if<sketch::Point>(&e))
      add(p->p);
    else if (const auto* c = std::get_if<sketch::Circle>(&e))
      add(c->center);
    else if (const auto* a = std::get_if<sketch::Arc>(&e))
      add(a->center);
  }
  if (centers.empty()) {
    SetError("hole sketch has no points or circles to place holes at");
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

  // A hole that meets no material removes nothing and used to say nothing:
  // regen.ok, no warning, and a body that looks drilled from the outside.
  // That silence is how a sketch whose plane origin already carried the
  // pattern's offset — so every centre landed 7.6 mm off the part — passed
  // two rounds of "it built fine" (docs/modeling/derived-gripper-dogfood.md).
  // The probe is the fastener stack's: a bundle of rays just outside the
  // bore wall, so a centre beside a part still counts as meeting it.
  {
    std::vector<parts::PlacedBody> placed;
    for (const Body& b : ctx.bodies)
      if (!b.reference && b.shape.IsValid())
        placed.push_back({&b, glm::mat4(1.0f), "", 0, 0});
    std::vector<std::string> empty;
    for (const glm::vec3& c : centers) {
      if (!parts::TraverseAxis(placed, glm::dvec3(c), glm::dvec3(axis), r,
                               boreDepth)
               .empty())
        continue;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c.x, c.y, c.z);
      empty.push_back(buf);
    }
    // SOME of them missing is the case nothing else reports: the cut still
    // removes material, so the body-mode "removed nothing" warning stays
    // quiet and the feature looks clean. (All of them missing is that
    // warning's job, and it says it better; it is set after this one.)
    if (!empty.empty())
      SetWarning(std::to_string(empty.size()) + " of " +
                 std::to_string(centers.size()) +
                 " hole centres drill empty space and remove nothing: " +
                 Join(empty, 4) +
                 ". A sketch's coordinates are measured FROM its plane "
                 "origin (world = origin + u*U + v*V).");
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
