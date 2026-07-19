#include "core/features/ThreadFeature.h"

#include "Logger.h"
#include "core/ThreadStandards.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

ThreadFeature::ThreadFeature(const std::string& name, ElementRef face,
                             std::string designation, double pitch)
    : face(std::move(face)), designation(std::move(designation)),
      pitch(pitch) {
  m_name = name;
}

bool ThreadFeature::Execute(RegenContext& ctx) {
  overlay = {};
  if (face.name.empty())
    return true; // previewing: no face picked yet
  if (pitch <= 1e-6) {
    SetError("thread pitch must be positive");
    return false;
  }

  const auto body = std::find_if(
      ctx.bodies.begin(), ctx.bodies.end(),
      [&](const Body& b) { return b.createdBy == face.featureId; });
  if (body == ctx.bodies.end()) {
    SetError("the threaded body no longer exists");
    return false;
  }
  const auto idx = body->elementMap.Resolve(face.name);
  if (!idx) {
    SetError("threaded face '" + face.name + "' no longer exists");
    return false;
  }
  TopoDS_Face occFace;
  int i = 0;
  for (TopExp_Explorer ex(body->shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == *idx) {
      occFace = TopoDS::Face(ex.Current());
      break;
    }
  if (occFace.IsNull()) {
    SetError("threaded face could not be resolved");
    return false;
  }

  try {
    BRepAdaptor_Surface surf(occFace);
    if (surf.GetType() != GeomAbs_Cylinder) {
      SetError("thread needs a cylindrical face");
      return false;
    }
    const gp_Cylinder cyl = surf.Cylinder();
    const double r = cyl.Radius();

    // A known designation must roughly fit the face it annotates.
    if (const auto std_ = threads::Lookup(designation)) {
      pitch = std_->pitch; // the table is authoritative for known series
      const double lo = threads::MinorDiameter(*std_) * 0.5 * 0.8;
      const double hi = std_->majorDia * 0.5 * 1.2;
      if (r < lo || r > hi) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "%s does not fit this face (face %.3f, thread %.3f dia)",
                 designation.c_str(), 2.0 * r, std_->majorDia);
        SetError(buf);
        return false;
      }
    }

    // External boss vs hole wall: the face normal at mid-parameters points
    // away from the axis on a boss, toward it inside a hole.
    const double uMid = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const double vMid = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    BRepGProp_Face gface(occFace);
    gp_Pnt p;
    gp_Vec n;
    gface.Normal(uMid, vMid, p, n);
    const gp_Pnt axisLoc = cyl.Location();
    const gp_Dir axisDir = cyl.Axis().Direction();
    const gp_Vec toP(axisLoc, p);
    const gp_Vec axial(axisDir.XYZ() * toP.Dot(gp_Vec(axisDir)));
    const gp_Vec radial = toP - axial;
    const bool external = n.Dot(radial) > 0.0;

    // The threaded span along the axis: the face's V range (V is axial for a
    // cylinder), optionally clipped to `length` from the V-min end.
    double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
    if (v1 < v0)
      std::swap(v0, v1);
    if (length > 1e-9)
      v1 = std::min(v1, v0 + length);

    // Drafting convention: the dashed circle sits at the thread depth —
    // minor diameter on a boss, major diameter inside a hole. Both are
    // pitch-derived (ISO 68 fundamental triangle), so custom designations
    // work without a table row.
    const double depth = 1.0825 * pitch * 0.5;
    overlay.valid = true;
    overlay.origin = glm::vec3(axisLoc.X(), axisLoc.Y(), axisLoc.Z());
    overlay.dir = glm::normalize(
        glm::vec3(axisDir.X(), axisDir.Y(), axisDir.Z()));
    overlay.radius = static_cast<float>(external ? r - depth : r + depth);
    overlay.faceRadius = static_cast<float>(r);
    overlay.v0 = static_cast<float>(v0);
    overlay.v1 = static_cast<float>(v1);
    overlay.external = external;

    if (modeled) {
      // Reserved (docs/modeling/thread-rib.md): the helical cut ships as a
      // follow-up. The error never takes the document down and the cosmetic
      // overlay above stays drawn.
      SetError("modeled threads are not implemented yet (cosmetic only)");
      return false;
    }
    return true; // cosmetic: geometry untouched
  } catch (const Standard_Failure& e) {
    SetError(std::string("thread failed: ") + e.GetMessageString());
    LOG_ERROR("ThreadFeature '{}': {}", m_name, GetError());
    return false;
  }
}
