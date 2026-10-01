#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

// Turning a PICK into a region: the face a click lands on, the vertical wall
// behind it, and the contour that comes out.

// One of six files split from CamToolpath.cpp
// (docs/architecture.md#source-layout).

namespace cam {

using namespace detail;

namespace {

// The face `ref` names, and the body it belongs to. Both picks below need the
// same walk, and doing it twice was how the two disagreed about which face a
// name meant.
bool FindFace(const std::vector<Body>& bodies, const ElementRef& ref,
              TopoDS_Face& face, glm::vec3& centre, glm::vec3& normal,
              uint64_t jobPartId) {
  if (!ref.IsSet())
    return false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    // A body that belongs to a DIFFERENT part of the job is not this pick.
    // One with no job part at all (a plain part document, which is what every
    // caller outside a job passes) has nothing to disambiguate, so it stays:
    // the qualifier narrows a job's parts, it does not require one.
    if (jobPartId != 0 && body.jobPartId != 0 && body.jobPartId != jobPartId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      continue;
    glm::vec3 uAxis(0.0f);
    if (!body.shape.FacePlane(*idx, centre, normal, uAxis))
      return false;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++)
      if (i == *idx) {
        face = TopoDS::Face(ex.Current());
        return !face.IsNull();
      }
    return false;
  }
  return false;
}

} // namespace

namespace detail {

// Lives here rather than with the rest of the loop geometry because this
// file is built whether or not CAM is enabled and CamGeometry.cpp is not:
// flattening a wire needs OpenCASCADE, not Clipper2, and a pick has to
// resolve to a contour in either build (CMakeLists.txt).
// Walks one wire into an ordered polyline, honouring edge orientation so the
// points come out in sequence rather than in map order.
std::vector<glm::dvec2> WireToLoop(const TopoDS_Wire& wire, double deflection) {
  std::vector<glm::dvec2> pts;
  BRepTools_WireExplorer ex(wire);
  for (; ex.More(); ex.Next()) {
    const TopoDS_Edge& edge = ex.Current();
    BRepAdaptor_Curve curve(edge);
    GCPnts_QuasiUniformDeflection sampler(curve, deflection);
    if (!sampler.IsDone())
      continue;
    const int n = sampler.NbPoints();
    const bool reversed = edge.Orientation() == TopAbs_REVERSED;
    for (int k = 0; k < n; k++) {
      const gp_Pnt p = sampler.Value(reversed ? n - k : k + 1);
      const glm::dvec2 xy(p.X(), p.Y());
      // The last point of one edge is the first of the next.
      if (!pts.empty() && glm::length(pts.back() - xy) < 1e-9)
        continue;
      pts.push_back(xy);
    }
  }
  if (pts.size() > 2 && glm::length(pts.front() - pts.back()) < 1e-9)
    pts.pop_back();
  return pts;
}

// The shortest distance from `p` to a loop's boundary. Which loop a wall
// belongs to is decided by this rather than by containment: a wall lies ON
// its own contour, and "on" is unreliable to test with point-in-polygon.
double DistanceToLoop(const std::vector<glm::dvec2>& pts, const glm::dvec2& p) {
  double best = std::numeric_limits<double>::max();
  for (size_t i = 0; i < pts.size(); i++) {
    const glm::dvec2 a = pts[i], b = pts[(i + 1) % pts.size()];
    const glm::dvec2 ab = b - a;
    const double len2 = glm::dot(ab, ab);
    const double t =
        len2 < 1e-18 ? 0.0 : std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
    best = std::min(best, glm::length(p - (a + ab * t)));
  }
  return best;
}

} // namespace detail

FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                            const ElementRef& ref, double deflection,
                            uint64_t jobPartId) {
  FaceRegion out;
  if (!ref.IsSet()) {
    out.why = "nothing picked";
    return out;
  }
  bool sawTheFeature = false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    // A body that belongs to a DIFFERENT part of the job is not this pick.
    // One with no job part at all (a plain part document, which is what every
    // caller outside a job passes) has nothing to disambiguate, so it stays:
    // the qualifier narrows a job's parts, it does not require one.
    if (jobPartId != 0 && body.jobPartId != 0 && body.jobPartId != jobPartId)
      continue;
    sawTheFeature = true;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      continue; // a part is several bodies; only one owns the name
    glm::vec3 centre(0.0f), normal(0.0f), uAxis(0.0f);
    if (!body.shape.FacePlane(*idx, centre, normal, uAxis)) {
      out.why = "pick a flat face — that one is curved";
      return out;
    }
    // A 2.5D job cuts in Z, so a face it can machine to is level. A tilted
    // face has no single depth, and guessing one puts the cut somewhere the
    // user did not point at.
    if (std::abs(normal.z) < 0.99f) {
      out.why = "pick a level face — a tilted one has no single depth";
      return out;
    }
    // Level and facing DOWN is a ceiling: the spindle comes from above and
    // cannot reach it. It was pocketed anyway, as if the cutter went through
    // the part to get there (a rotary mitre indexed the wrong way round).
    if (normal.z < 0.0f) {
      out.why = "that face points down, away from the spindle — turn the "
                "part over (lay_face_up, flip, or the rotary 180° round)";
      return out;
    }
    // Walk to the face in the same explorer order FacePlane uses.
    TopoDS_Face face;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++)
      if (i == *idx) {
        face = TopoDS::Face(ex.Current());
        break;
      }
    if (face.IsNull()) {
      out.why = "that face is gone from the model";
      return out;
    }
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
      const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
      Loop loop;
      loop.pts = WireToLoop(wire, deflection);
      if (loop.pts.size() < 3)
        continue;
      // A wire of one circular edge is a BORE, and its diameter is the
      // circle's -- asked of the geometry rather than measured off the
      // flattened polygon.
      int edges = 0;
      double diameter = 0.0;
      for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
        edges++;
        BRepAdaptor_Curve curve(ex.Current());
        if (curve.GetType() == GeomAbs_Circle)
          diameter = 2.0 * curve.Circle().Radius();
        else
          diameter = 0.0;
      }
      loop.boreDiameter = edges == 1 ? diameter : 0.0;
      // Inner wires are islands: material standing in the pocket, which the
      // clearing region has to go around rather than through.
      loop.hole = !wire.IsSame(outer);
      if (loop.hole)
        out.loops.push_back(std::move(loop));
      else
        out.loops.insert(out.loops.begin(), std::move(loop));
    }
    if (out.loops.empty()) {
      out.why = "that face has no usable boundary";
      return out;
    }
    out.z = centre.z;
    out.ok = true;
    return out;
  }
  out.why = sawTheFeature ? "that face is gone from the model"
                          : "that face is not on a body this job can machine";
  return out;
}

// A picked face that is a WALL: vertical everywhere, so a 3-axis contour can
// follow it at any depth. Planar walls arrive through FindFace; this is
// everything else — the cylinder of a round bore, and equally the surface of
// extrusion an ellipse or a spline opening produces.
//
// Asking the SHAPE ("is it a cylinder?") was the original gate, and it was
// the only thing that decided which openings could be machined: the
// section-loop machinery downstream works on polygons out of a Z section and
// never cared what curve produced them. So a round hole cut out and an
// elliptical one was refused, for no reason that survives being looked at.
//
// Asking the NORMAL instead — is it horizontal over the whole face? — admits
// any extruded curve and keeps every refusal the shape test made: a floor or
// ceiling points at ±Z, a cone or a draughted wall tilts, a cylinder lying on
// its side sweeps through ±Z, a sphere likewise. None of those is a contour a
// 3-axis job can follow at a chosen depth.
//
// `onWall` is a point ON the wall, which is what the nearest-loop pick below
// needs: a bore's axis centre would sit a full radius from its own contour
// and lose to the 0.5 mm tolerance.
bool FindVerticalWallFace(const std::vector<Body>& bodies,
                          const ElementRef& ref, TopoDS_Face& face,
                          glm::dvec2& onWall, uint64_t jobPartId) {
  if (!ref.IsSet())
    return false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    // A body that belongs to a DIFFERENT part of the job is not this pick.
    // One with no job part at all (a plain part document, which is what every
    // caller outside a job passes) has nothing to disambiguate, so it stays:
    // the qualifier narrows a job's parts, it does not require one.
    if (jobPartId != 0 && body.jobPartId != 0 && body.jobPartId != jobPartId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      continue;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++) {
      if (i != *idx)
        continue;
      face = TopoDS::Face(ex.Current());
      if (face.IsNull())
        return false;
      double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
      BRepTools::UVBounds(face, u0, u1, v0, v1);
      if (u1 - u0 < 1e-12 || v1 - v0 < 1e-12)
        return false;
      BRepAdaptor_Surface surf(face);
      BRepLProp_SLProps props(surf, 1, 1e-7);
      // A grid rather than one sample: a surface of extrusion is vertical
      // everywhere, but a cone is vertical along no direction at all and a
      // single unlucky sample on a nearly-vertical draught would pass.
      constexpr int kSamples = 5;
      bool any = false;
      for (int a = 0; a <= kSamples; a++)
        for (int b = 0; b <= kSamples; b++) {
          const double u = u0 + (u1 - u0) * a / kSamples;
          const double v = v0 + (v1 - v0) * b / kSamples;
          props.SetParameters(u, v);
          if (!props.IsNormalDefined())
            continue;
          if (std::abs(props.Normal().Z()) > 0.01)
            return false;
          any = true;
        }
      if (!any)
        return false; // no normal anywhere: not something to follow
      const gp_Pnt mid = surf.Value(0.5 * (u0 + u1), 0.5 * (v0 + v1));
      onWall = {mid.X(), mid.Y()};
      return true;
    }
    return false;
  }
  return false;
}

bool IsWallFace(const std::vector<Body>& bodies, const ElementRef& ref,
                uint64_t jobPartId) {
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  if (FindFace(bodies, ref, face, centre, normal, jobPartId))
    return std::abs(normal.z) < 0.01f;
  glm::dvec2 onWall(0.0);
  return FindVerticalWallFace(bodies, ref, face, onWall, jobPartId);
}

WallContour PickedWallContour(const std::vector<Body>& bodies,
                              const ElementRef& ref, double deflection,
                              uint64_t jobPartId) {
  WallContour out;
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  glm::dvec2 at(0.0);
  if (FindFace(bodies, ref, face, centre, normal, jobPartId)) {
    if (std::abs(normal.z) >= 0.01f) {
      out.why = "pick a wall — that face is level, so it is a floor to clear "
                "rather than a contour to follow";
      return out;
    }
    at = {centre.x, centre.y};
  } else if (FindVerticalWallFace(bodies, ref, face, at, jobPartId)) {
    // A curved wall — a bore, a boss, an elliptical or splined opening. The
    // section-loop machinery below is shape-agnostic, so these need nothing
    // more than being let in.
  } else {
    out.why = "pick a wall — a face that stands vertical, so a contour can "
              "follow it at depth; that one is not (a floor or ceiling, a "
              "tapered or draughted side, or not on a body this job can "
              "machine)";
    return out;
  }
  // Exact, not BRepBndLib::Add's box widened by the face's tolerance: that
  // put a cylinder's top 0.05 mm over the part and a chamfer's edge with it.
  Bnd_Box box;
  BRepBndLib::AddOptimal(face, box, false, false);
  if (box.IsVoid()) {
    out.why = "that face has no extent";
    return out;
  }
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  box.Get(x0, y0, z0, x1, y1, z1);
  out.topZ = z1;
  out.bottomZ = z0;
  if (z1 - z0 < 1e-6) {
    out.why = "that wall has no height to cut";
    return out;
  }
  // Section where the wall certainly exists. Its own mid-height, not the
  // model's: a wall that stops half way up belongs to a different contour
  // than the one at the part's waist.
  const std::vector<Loop> loops =
      SectionLoops(bodies, (z0 + z1) * 0.5, deflection);
  double best = std::numeric_limits<double>::max();
  const Loop* pick = nullptr;
  for (const Loop& l : loops) {
    const double d = DistanceToLoop(l.pts, at);
    if (d < best) {
      best = d;
      pick = &l;
    }
  }
  // A tolerance in millimetres, not a fraction: the wall centre lies on its
  // own contour, so anything further away is a different loop entirely.
  if (pick == nullptr || best > 0.5) {
    out.why = "that wall is not on this part's cross-section at its own "
              "height — nothing here can follow it";
    return out;
  }
  out.loop = *pick;
  out.opening = pick->hole;
  out.at = at;
  out.ok = true;
  return out;
}

} // namespace cam
