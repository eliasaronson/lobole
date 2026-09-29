#include "core/features/HoleFeature.h"

#include "core/parts/AxisStack.h"

#include <cmath>
#include <cstdio>

#include "core/parts/FastenerTable.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/features/SketchFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>

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
// The span of a ray that lies inside an axis-aligned box, as [from, to]
// along `dir`. False when the ray misses it altogether.
bool ClipToBox(const glm::vec3& origin, const glm::vec3& dir,
               const glm::vec3& lo, const glm::vec3& hi, double* from,
               double* to) {
  double t0 = *from, t1 = *to;
  for (int i = 0; i < 3; i++) {
    if (std::abs(dir[i]) < 1e-9) {
      if (origin[i] < lo[i] - 1e-6 || origin[i] > hi[i] + 1e-6)
        return false;
      continue;
    }
    double a = (lo[i] - origin[i]) / dir[i];
    double b = (hi[i] - origin[i]) / dir[i];
    if (a > b)
      std::swap(a, b);
    t0 = std::max(t0, a);
    t1 = std::min(t1, b);
    if (t0 > t1)
      return false;
  }
  *from = t0;
  *to = t1;
  return true;
}

} // namespace

bool HoleFeature::Execute(RegenContext& ctx) {
  m_tool = CADShape(); // a failed regen leaves no stale tool to pattern
  m_toolMap = {};
  const auto* sk = dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sk) {
    SetError(
        "hole needs a sketch of points or circles (missing or suppressed)");
    return false;
  }
  // A recorded fit DERIVES the diameter
  // (docs/modeling/holes-and-threads.md#hole). Done here rather than at the
  // call site so it survives everything that rebuilds a feature — undo, a
  // reload, a configuration override, an expression on the thread — instead of
  // being a number somebody typed once from a table and never re-derived.
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
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const auto& b : ctx.bodies) {
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
  double boreDepth = depth;
  if (through)
    boreDepth = ctx.bodies.empty() ? depth : glm::length(hi - lo) + 1.0f;

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
  // rather than n times. Measured by driving a 480-hole plate over the RPC.
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
  // two rounds of "it built fine" (docs/modeling/holes-and-threads.md#hole).
  //
  // The test is a CLASSIFICATION of points down the bore, not a ray cast.
  // The first version reused the fastener stack's ray bundle, whose rays are
  // offset just outside the bore wall — and that bundle documents its own
  // assumption, that a ray starts OUTSIDE the material. A hole drilled into
  // a boss breaks it: the offset rays begin inside the boss, every one is
  // discarded as "started inside", and four holes that had visibly drilled
  // four standoffs were reported as drilling air (the SO-101 gripper rebuilt
  // on the SDK; docs/modeling/holes-and-threads.md#hole). A false warning is
  // worse than none, so this asks the question directly instead.
  {
    std::vector<std::string> empty;
    for (const glm::vec3& c : centers) {
      bool meets = false;
      // Sampled over the span where material CAN be — the bore clipped to
      // the bodies' box — not over the bore's own length. A through hole's
      // bore is a bounding-box DIAGONAL long, so nine points spread over it
      // all start past a thin plate and every one of them reports air: a
      // carbon frame drilled twelve correct holes and warned about all
      // twelve. The clip is what makes the spacing scale with the MATERIAL.
      double from = 0.0, to = boreDepth;
      if (!ctx.bodies.empty() && !ClipToBox(c, axis, lo, hi, &from, &to))
        from = to = 0.0; // the bore misses the box entirely: no material
      for (int i = 1; i <= 9 && !meets && to > from; i++) {
        const glm::vec3 p = c + axis * float(from + (to - from) * (0.1 * i));
        for (const Body& b : ctx.bodies) {
          if (b.reference || !b.shape.IsValid())
            continue;
          if (b.shape.ContainsPoint(p)) {
            meets = true;
            break;
          }
        }
      }
      if (meets)
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
  m_tool = tool;
  m_toolMap = toolMap;
  if (!ApplyBodyModeReporting(ctx.bodies, std::move(tool), std::move(toolMap),
                              ExtrudeMode::Cut, *this)) {
    SetError("hole cut failed");
    return false;
  }
  if (fitKind == "tapped" && !fitThread.empty())
    RecordTapping(ctx.bodies);
  return true;
}

// A hole drilled FOR a thread IS threaded.
//
// `fit: {thread: "M6", kind: "tapped"}` already derives the 5 mm tap drill,
// which is the whole of what the geometry can say — but until this, nothing
// downstream knew what the hole was for, because only a separate `Thread`
// feature ever wrote a thread region. So a screw in a correctly drilled,
// correctly declared tapped hole came back as 40 mm3 of CLASH, with
// `engaged_thread_pairs: 0` the only hint and no refusal anywhere: the
// clearance check relieves an engaged length only between threads it was
// told about (docs/modeling/holes-and-threads.md#hole).
//
// Every bore this feature cut, by its own name prefix and its radius — a
// counterbore or a countersink on the same hole is a different radius and is
// not a thread. The region carries no length, so it is the whole bore, and
// no minimum engagement, so the standard's pitch stands.
void HoleFeature::RecordTapping(std::vector<Body>& bodies) const {
  const std::string mine = std::to_string(GetId()) + "/";
  const double want = diameter / 2.0;
  for (Body& body : bodies) {
    if (body.reference || !body.shape.IsValid())
      continue;
    int index = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), index++) {
      const std::string* name = body.elementMap.NameOf(index);
      if (name == nullptr || name->rfind(mine, 0) != 0)
        continue;
      BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
      if (surf.GetType() != GeomAbs_Cylinder ||
          std::abs(surf.Cylinder().Radius() - want) > 1e-6)
        continue;
      body.threadRegions.push_back(
          {ElementRef{body.createdBy, *name}, fitThread, 0.0, 0.0, false});
    }
  }
}
