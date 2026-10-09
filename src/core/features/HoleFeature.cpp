// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/HoleFeature.h"

#include "core/parts/AxisStack.h"

#include <cmath>
#include <cstdio>

#include "core/PrintAllowance.h"
#include "core/ThreadStandards.h"
#include "core/parts/FastenerTable.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/ElementOrigin.h"
#include "core/features/SketchFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepTools.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <map>
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
bool ClipToBox(const glm::dvec3& origin, const glm::dvec3& dir,
               const glm::dvec3& lo, const glm::dvec3& hi, double* from,
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

// Does the drilling axis from `c` pass through material between `from` and
// `to`? Exact along the axis line: the line's crossings with each body's
// faces split it into spans that are wholly inside or wholly outside, and
// the middle of each span is classified. Sampling the span at fixed fractions
// cannot be right for every part: a thin ledge 160 mm up a yoke, drilled
// through, sat between the first two of nine samples spread over the whole
// height, and three correct holes were reported as drilling air.
bool AxisMeetsMaterial(const std::vector<Body>& bodies, const glm::dvec3& c,
                       const glm::dvec3& axis, double from, double to) {
  if (to <= from)
    return false;
  const gp_Lin line(gp_Pnt(c.x, c.y, c.z), gp_Dir(axis.x, axis.y, axis.z));
  for (const Body& b : bodies) {
    if (b.reference || !b.shape.IsValid())
      continue;
    std::vector<double> cuts{from, to};
    IntCurvesFace_ShapeIntersector crossings;
    crossings.Load(b.shape.GetShape(), Precision::Confusion());
    crossings.Perform(line, from, to);
    if (crossings.IsDone())
      for (int i = 1; i <= crossings.NbPnt(); ++i)
        cuts.push_back(crossings.WParameter(i));
    std::sort(cuts.begin(), cuts.end());
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      if (cuts[i + 1] - cuts[i] < 1e-6)
        continue;
      const double t = 0.5 * (cuts[i] + cuts[i + 1]);
      if (b.shape.ContainsPoint(glm::dvec3(c + axis * t)))
        return true;
    }
  }
  return false;
}

// The point a twist drill leaves, ISO 235's jobber drills: the drill tables
// a fit is read from (clearance and tap drill sizes) are those drills.
constexpr double kJobberPointDeg = 118.0;
// How many pitches at a blind floor a tap leaves without full thread: the
// lead of the blind-hole tap, ISO 8830 (DIN 2197) chamfer form C, 2-3
// threads.
constexpr double kTapLeadPitches = 3.0;

// The pitch a tapped fit cuts: the stated one, else the table's coarse.
double FitPitch(const std::string& thread, double stated) {
  if (stated > 0.0)
    return stated;
  const std::optional<threads::Standard> table = threads::Lookup(thread);
  return table ? table->pitch : 0.0;
}

} // namespace

double HoleFeature::PointAngleUsed() const {
  if (pointAngleDeg > 0.0)
    return pointAngleDeg;
  return fitThread.empty() ? 180.0 : kJobberPointDeg;
}

double HoleFeature::ThreadDepthUsed() const {
  if (fitKind != "tapped" || fitThread.empty())
    return 0.0;
  if (threadDepth > 0.0)
    return threadDepth;
  if (through)
    return 0.0; // the whole bore
  const double pitch = FitPitch(fitThread, fitPitch);
  // Too shallow for a tap's lead: threaded to the floor, and Execute says so.
  return depth - kTapLeadPitches * pitch >= pitch
             ? depth - kTapLeadPitches * pitch
             : 0.0;
}

bool HoleFeature::Execute(RegenContext& ctx) {
  m_tool = CADShape(); // a failed regen leaves no stale tool to pattern
  m_unpointed.clear();
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
    // A clearance fit names the size the hole must COME OUT at, so on a
    // printed part it is modelled at that size plus the process's hole
    // allowance (docs/tolerances/README.md#printed-parts): an M6 normal hole
    // in PETG prints Ø6.6 from a Ø6.8 model. A tapped hole stays at its tap
    // drill; what a printed thread wants is the thread report's to say.
    if (*fit != parts::Fit::Tapped && ctx.print != nullptr) {
      const auto a =
          printfit::ForBasic(*ctx.print, printfit::Printed::Hole, *bore, &why);
      if (!a) {
        SetError(why);
        return false;
      }
      diameter = a->modelMm;
    }
  }
  if (diameter <= 1e-9 || (!through && depth <= 1e-9)) {
    SetError("hole diameter and depth must be positive");
    return false;
  }
  // The drill point and the thread depth, refused rather than read as
  // something else (docs/modeling/holes-and-threads.md#hole).
  if (pointAngleDeg != 0.0 && (pointAngleDeg < 30.0 || pointAngleDeg > 180.0)) {
    SetError("drill_point_deg is the point's included angle, 30 to 180 "
             "degrees (118 a jobber drill, 180 a flat floor); it was " +
             std::to_string(pointAngleDeg));
    return false;
  }
  const bool tapped = fitKind == "tapped" && !fitThread.empty();
  if (threadDepth != 0.0 && !tapped) {
    SetError("thread_depth is how deep a TAPPED hole is threaded, and this "
             "hole has no tapped fit -- give it fit {thread, kind: tapped}, "
             "or drop thread_depth");
    return false;
  }
  if (threadDepth < 0.0 || (!through && threadDepth > depth + 1e-9)) {
    SetError("thread_depth must be positive and no deeper than the hole (" +
             std::to_string(depth) + " mm)");
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
  std::vector<glm::dvec3> centers;
  std::vector<uint32_t> entities; // the sketch entity each centre came from
  const auto add = [&](const glm::dvec2& uv, uint32_t entity) {
    const glm::dvec3 w = sk->plane.PlaneToWorld(uv);
    for (const glm::dvec3& c : centers)
      if (glm::length(c - w) < 1e-4)
        return;
    centers.push_back(w);
    entities.push_back(entity);
  };
  for (const auto& e : sk->entities) {
    if (const auto* p = std::get_if<sketch::Point>(&e))
      add(p->p, p->id);
    else if (const auto* c = std::get_if<sketch::Circle>(&e))
      add(c->center, c->id);
    else if (const auto* a = std::get_if<sketch::Arc>(&e))
      add(a->center, a->id);
  }
  if (centers.empty()) {
    SetError("hole sketch has no points or circles to place holes at");
    return false;
  }

  const glm::dvec3 axis = -sk->plane.GetNormal(); // drill into the material
  // Through-all: drill well past any body (bbox diagonal of all bodies).
  glm::dvec3 lo(1e30), hi(-1e30);
  for (const auto& b : ctx.bodies) {
    glm::dvec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    lo = glm::min(lo, glm::dvec3(bmin));
    hi = glm::max(hi, glm::dvec3(bmax));
  }
  double boreDepth = depth;
  if (through)
    boreDepth = ctx.bodies.empty() ? depth : glm::length(hi - lo) + 1.0;

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
  // A blind hole's floor is the drill's point when it has one: a cone below
  // the full-diameter depth, its apex the point's height further down.
  const double pointDeg = PointAngleUsed();
  const bool pointed = !through && pointDeg < 180.0 - 1e-6;
  const double pointHeight =
      pointed ? r / std::tan(glm::radians(pointDeg * 0.5)) : 0.0;
  for (size_t site = 0; site < centers.size(); site++) {
    const glm::dvec3& c = centers[site];
    CADShape hole = kernel.CreateCylinder(c, axis, r, boreDepth);
    // Only where the floor is in material: a hole drilled on into a pocket
    // or out of the far side has no floor for a point to be on, and a cone
    // in the air would only rename the faces it touches.
    const glm::dvec3 floor = c + axis * boreDepth;
    bool floorInMaterial = false;
    if (pointed)
      for (const Body& b : ctx.bodies)
        floorInMaterial =
            floorInMaterial ||
            (!b.reference && b.shape.IsValid() &&
             b.shape.ContainsPoint(floor + axis * (pointHeight * 0.5)));
    if (pointed && !floorInMaterial)
      m_unpointed.push_back(entities[site]);
    if (pointed && floorInMaterial && hole.IsValid()) {
      try {
        BRepPrimAPI_MakeCone cone(gp_Ax2(gp_Pnt(floor.x, floor.y, floor.z),
                                         gp_Dir(axis.x, axis.y, axis.z)),
                                  r, 0.0, pointHeight);
        cone.Build();
        hole = cone.IsDone() ? hole.Union(CADShape(cone.Shape())) : CADShape();
      } catch (const Standard_Failure&) {
        hole = CADShape();
      }
    }
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
    for (const glm::dvec3& c : centers) {
      // Asked over the span where material CAN be — the bore clipped to the
      // bodies' box — not over the bore's own length, which for a through
      // hole is a bounding-box diagonal.
      double from = 0.0, to = boreDepth;
      if (!ctx.bodies.empty() && !ClipToBox(c, axis, lo, hi, &from, &to))
        from = to = 0.0; // the bore misses the box entirely: no material
      if (AxisMeetsMaterial(ctx.bodies, c, axis, from, to))
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
  NameByRole(tool, centers, entities, axis, boreDepth, toolMap);
  m_tool = tool;
  m_toolMap = toolMap;
  if (!ApplyBodyModeReporting(ctx.bodies, std::move(tool), std::move(toolMap),
                              ExtrudeMode::Cut, *this)) {
    SetError("hole cut failed");
    return false;
  }
  if (tapped) {
    RecordTapping(ctx.bodies);
    if (!through && threadDepth == 0.0 && ThreadDepthUsed() == 0.0)
      SetWarning("this tapped hole is too shallow for a tap's lead (" +
                 std::to_string(static_cast<int>(kTapLeadPitches)) +
                 " pitches) to leave a full thread above its floor, so it "
                 "is threaded to the floor: drill it deeper, or state "
                 "thread_depth");
  }
  RecordHoles(ctx.bodies);
  return true;
}

// What CAM reads of every bore this feature cut (HoleMark): by its own name
// prefix and role, the way RecordTapping finds them.
void HoleFeature::RecordHoles(std::vector<Body>& bodies) const {
  const bool tapped = fitKind == "tapped" && !fitThread.empty();
  for (Body& body : bodies) {
    if (body.reference || !body.shape.IsValid())
      continue;
    for (const auto& [index, name] : body.elementMap.names) {
      const topo::ElementOrigin o = topo::DecodeElementName(name);
      if (o.ok && o.derived.empty() && o.madeBy == GetId() &&
          o.role == topo::ElementRole::Bore) {
        HoleMark h;
        h.bore = ElementRef{body.createdBy, name};
        h.featureId = GetId();
        h.feature = GetName();
        h.diameter = diameter;
        h.depth = through ? 0.0 : depth;
        h.through = through;
        h.pointAngleDeg =
            through || std::find(m_unpointed.begin(), m_unpointed.end(),
                                 o.entity) != m_unpointed.end()
                ? 180.0
                : PointAngleUsed();
        h.thread = tapped ? fitThread : std::string{};
        body.holes.push_back(std::move(h));
      }
    }
  }
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
// not a thread. The region runs the thread's depth from the mouth
// (ThreadDepthUsed; 0, the whole bore, when through), and carries no
// minimum engagement, so the standard's pitch stands.
void HoleFeature::RecordTapping(std::vector<Body>& bodies) const {
  // What the tap cuts, for whatever reads the bodies (the CAM thread mill):
  // the table's thread at the fit's pitch, right-handed, no class stated.
  const std::optional<threads::Standard> table = threads::Lookup(fitThread);
  const double pitch = fitPitch > 0.0 ? fitPitch : (table ? table->pitch : 0.0);
  std::string designation = table ? table->designation : fitThread;
  if (table && fitPitch > 0.0 && std::abs(fitPitch - table->pitch) > 1e-9) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%sx%g", fitThread.c_str(), fitPitch);
    designation = buf;
  }
  // As deep as it is threaded, from the mouth: a blind hole's tap leaves its
  // lead short of the floor (ThreadDepthUsed); 0 is the whole bore.
  const double length = ThreadDepthUsed();
  for (Body& body : bodies) {
    if (body.reference || !body.shape.IsValid())
      continue;
    for (const auto& [index, name] : body.elementMap.names) {
      const topo::ElementOrigin o = topo::DecodeElementName(name);
      if (o.ok && o.derived.empty() && o.madeBy == GetId() &&
          o.role == topo::ElementRole::Bore) {
        body.threadRegions.push_back(
            {ElementRef{body.createdBy, name}, fitThread, length, 0.0, false});
        if (pitch > 0.0)
          body.threads.push_back({ElementRef{body.createdBy, name}, GetId(),
                                  designation, table ? table->majorDia : 0.0,
                                  pitch, "", true, false, length, false});
      }
    }
  }
}

// Each face of the drilled tool by what it is, keyed by the sketch entity
// its hole was drilled at: `<id>/p<entity>/bore`, `/bottom`, `/counterbore`,
// `/counterbore_floor`, `/countersink`. Positional names interleaved every
// hole's faces, so nothing could ask for "the bores", and adding a point
// renumbered the rest (docs/modeling/toponaming.md#naming-grammar).
void HoleFeature::NameByRole(const CADShape& tool,
                             const std::vector<glm::dvec3>& centers,
                             const std::vector<uint32_t>& entities,
                             const glm::dvec3& axis, double boreDepth,
                             ElementMap& out) const {
  out = {};
  const gp_Dir dir(axis.x, axis.y, axis.z);
  const double r = diameter / 2.0;
  const double tol = 1e-4 * std::max(1.0, boreDepth);
  const auto same = [&](double a, double b) { return std::abs(a - b) < tol; };
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(tool.GetShape(), TopAbs_FACE, faces);
  std::map<std::string, int> seen;
  std::map<size_t, int> unnamed; // per hole
  for (int i = 1; i <= faces.Extent(); i++) {
    const TopoDS_Face& face = TopoDS::Face(faces(i));
    BRepAdaptor_Surface s(face);
    // A point on (or level with) the face, on its hole's axis.
    gp_Pnt p;
    std::string role;
    if (s.GetType() == GeomAbs_Cylinder) {
      const gp_Cylinder cy = s.Cylinder();
      p = cy.Location();
      if (same(cy.Radius(), r))
        role = "bore";
      else if (kind == HoleKind::Counterbore &&
               same(cy.Radius(), cbDiameter / 2))
        role = "counterbore";
    } else if (s.GetType() == GeomAbs_Cone) {
      p = s.Cone().Location();
      role = "countersink"; // or the drill's point, decided below by depth
    } else {
      double u0, u1, v0, v1;
      BRepTools::UVBounds(face, u0, u1, v0, v1);
      p = s.Value((u0 + u1) / 2, (v0 + v1) / 2);
    }
    // Its hole: the centre whose axis it sits on.
    size_t site = 0;
    double best = 1e300;
    for (size_t k = 0; k < centers.size(); k++) {
      const gp_Pnt c(centers[k].x, centers[k].y, centers[k].z);
      const double d = gp_Lin(c, dir).Distance(p);
      if (d < best) {
        best = d;
        site = k;
      }
    }
    if (s.GetType() == GeomAbs_Cone) {
      // A countersink sits at the mouth; the drill's point under the floor.
      const glm::dvec3& c = centers[site];
      const double t = gp_Vec(gp_Pnt(c.x, c.y, c.z), p).Dot(gp_Vec(dir));
      if (same(t, boreDepth))
        role = "point";
    }
    if (s.GetType() == GeomAbs_Plane) {
      const glm::dvec3& c = centers[site];
      const double t = gp_Vec(gp_Pnt(c.x, c.y, c.z), p).Dot(gp_Vec(dir));
      if (same(t, boreDepth))
        role = "bottom";
      else if (kind == HoleKind::Counterbore && same(t, cbDepth))
        role = "counterbore_floor";
    }
    if (role.empty())
      role = "face" + std::to_string(unnamed[site]++);
    std::string name = std::to_string(GetId()) + "/p" +
                       std::to_string(entities[site]) + "/" + role;
    if (const int n = seen[name]++; n > 0) // a role split in two pieces
      name += "." + std::to_string(n);
    out.names[i - 1] = name;
  }
  topo::ApplyFallbackPasses(tool, out);
}
