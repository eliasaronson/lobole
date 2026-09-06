#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"

#include <clipper2/clipper.h>

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
// (docs/architecture-review-2026-09-06.md, stage 2).

namespace cam {

using namespace detail;

namespace {

// The face `ref` names, and the body it belongs to. Both picks below need the
// same walk, and doing it twice was how the two disagreed about which face a
// name meant.
bool FindFace(const std::vector<Body>& bodies, const ElementRef& ref,
              TopoDS_Face& face, glm::vec3& centre, glm::vec3& normal) {
  if (!ref.IsSet())
    return false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      return false;
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
                            const ElementRef& ref, double deflection) {
  FaceRegion out;
  if (!ref.IsSet()) {
    out.why = "nothing picked";
    return out;
  }
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx) {
      out.why = "that face is gone from the model";
      return out;
    }
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
  out.why = "that face is not on a body this job can machine";
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
                          glm::dvec2& onWall) {
  if (!ref.IsSet())
    return false;
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx)
      return false;
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

bool IsWallFace(const std::vector<Body>& bodies, const ElementRef& ref) {
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  if (FindFace(bodies, ref, face, centre, normal))
    return std::abs(normal.z) < 0.01f;
  glm::dvec2 onWall(0.0);
  return FindVerticalWallFace(bodies, ref, face, onWall);
}

WallContour PickedWallContour(const std::vector<Body>& bodies,
                              const ElementRef& ref, double deflection) {
  WallContour out;
  TopoDS_Face face;
  glm::vec3 centre(0.0f), normal(0.0f);
  glm::dvec2 at(0.0);
  if (FindFace(bodies, ref, face, centre, normal)) {
    if (std::abs(normal.z) >= 0.01f) {
      out.why = "pick a wall — that face is level, so it is a floor to clear "
                "rather than a contour to follow";
      return out;
    }
    at = {centre.x, centre.y};
  } else if (FindVerticalWallFace(bodies, ref, face, at)) {
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
  Bnd_Box box;
  BRepBndLib::Add(face, box);
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
