// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/CamProfileFeature.h"

#include "core/Angles.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/features/FrameFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

std::string Num(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.4g", v);
  return b;
}

// The cam's placement: its plane's origin and axes in part space.
struct Placement {
  gp_Pnt o;
  gp_Dir x, y, z;
  double radius = 0.0; // the picked cylinder's; 0 from a Frame
  double length = 0.0; // its span along the axis; 0 from a Frame
  Body* body = nullptr;
};

bool ResolveHub(RegenContext& ctx, const ElementRef& face, Placement& out,
                std::string& why) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(), [&](const Body& b) {
        return b.createdBy == face.featureId;
      });
  if (body == ctx.bodies.end()) {
    why = "the hub's body no longer exists";
    return false;
  }
  const auto idx = body->elementMap.Resolve(face.name);
  if (!idx) {
    why = topo::UnresolvedReason(ctx, face, topo::ElementKind::Face,
                                 "the hub's face");
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
    why = "the hub's face could not be resolved";
    return false;
  }
  BRepAdaptor_Surface surf(occFace);
  if (surf.GetType() != GeomAbs_Cylinder) {
    why = "pick the CYLINDRICAL outside of the shaft or hub the cam goes on";
    return false;
  }
  const gp_Cylinder cyl = surf.Cylinder();
  double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
  if (v1 < v0)
    std::swap(v0, v1);
  BRepGProp_Face gface(occFace);
  gp_Pnt p;
  gp_Vec n;
  gface.Normal(0.5 * (surf.FirstUParameter() + surf.LastUParameter()),
               0.5 * (v0 + v1), p, n);
  const gp_Dir d = cyl.Axis().Direction();
  const gp_Vec toP(cyl.Location(), p);
  const gp_Vec radial = toP - gp_Vec(d.XYZ() * toP.Dot(gp_Vec(d)));
  if (n.Dot(radial) <= 0.0) {
    why = "the face is a bore: a cam goes on the OUTSIDE of a shaft or hub";
    return false;
  }
  out.z = d;
  out.x = cyl.Position().XDirection();
  out.y = out.z.Crossed(out.x);
  out.o = cyl.Location().Translated(gp_Vec(d) * v0);
  out.radius = cyl.Radius();
  out.length = v1 - v0;
  out.body = &*body;
  return true;
}

gp_Pnt At(const Placement& pl, glm::dvec2 p) {
  return pl.o.Translated(gp_Vec(pl.x) * p.x + gp_Vec(pl.y) * p.y);
}

} // namespace

CamProfileFeature::CamProfileFeature(const std::string& name, ElementRef face)
    : face(std::move(face)) {
  m_name = name;
}

bool CamProfileFeature::Execute(RegenContext& ctx) {
  report = {};
  if (face.name.empty() && frameId == 0)
    return true; // previewing: nothing picked yet
  if (!face.name.empty() && frameId != 0) {
    SetError("a cam is placed by its hub's face OR by a frame, not both");
    return false;
  }
  Placement pl;
  std::string why;
  if (!face.name.empty()) {
    if (!ResolveHub(ctx, face, pl, why)) {
      SetError(why);
      return false;
    }
  } else {
    const Feature* ref = ctx.FindById(frameId);
    const auto* fr = dynamic_cast<const FrameFeature*>(ref);
    if (fr == nullptr) {
      SetError("frame " + std::to_string(frameId) +
               " is not a Frame feature before this one");
      return false;
    }
    if (!fr->GetError().empty()) {
      SetError("its frame failed: " + fr->GetError());
      return false;
    }
    const glm::dvec3 o = fr->Origin(), x = fr->XAxis(), z = fr->ZAxis();
    pl.o = gp_Pnt(o.x, o.y, o.z);
    pl.x = gp_Dir(x.x, x.y, x.z);
    pl.z = gp_Dir(z.x, z.y, z.z);
    pl.y = pl.z.Crossed(pl.x);
    if (!newBody) {
      SetError("a cam placed by a frame is its own body; adding it to a hub "
               "needs the hub's face picked");
      return false;
    }
  }

  // The defaults, each derived from the pick and named in the report.
  Report r;
  r.baseRadius = baseRadius;
  if (!(baseRadius > 0.0)) {
    if (baseRadius < 0.0) {
      SetError("base_radius_mm is positive, or omitted to take the hub's");
      return false;
    }
    if (pl.radius <= 0.0) {
      SetError("a cam placed by a frame needs base_radius_mm: there is no "
               "hub to take it from");
      return false;
    }
    r.baseRadius = pl.radius;
    r.derived.push_back("base_radius_mm");
  }
  r.width = width;
  if (!(width > 0.0)) {
    if (width < 0.0) {
      SetError("width_mm is positive, or omitted to take the hub's length");
      return false;
    }
    if (pl.length <= 0.0) {
      SetError("a cam placed by a frame needs width_mm: there is no hub to "
               "take it from");
      return false;
    }
    r.width = pl.length;
    r.derived.push_back("width_mm");
  }
  r.follower = follower;
  if (!followerSet) {
    r.follower = {};
    r.follower.kind = cammech::FollowerKind::Roller;
    r.follower.motion = cammech::FollowerMotion::Slide;
    r.follower.radiusMm = r.baseRadius / 3.0;
    r.derived.push_back("follower");
  }
  r.motion = motion;
  if (motion.lift.empty() && motion.segments.empty()) {
    if (r.follower.motion == cammech::FollowerMotion::Turn) {
      SetError("a turning follower's motion is stated (segments with "
               "rise_deg, or a lift table in degrees): no rise in degrees "
               "follows from the hub");
      return false;
    }
    const double h = r.baseRadius / 2.0;
    using cammech::Law;
    r.motion.segments = {{120.0, h, Law::Cycloidal},
                         {180.0, 0.0, Law::Dwell},
                         {300.0, -h, Law::Cycloidal},
                         {360.0, 0.0, Law::Dwell}};
    r.derived.push_back("segments");
  }

  cammech::Profile prof =
      cammech::BuildProfile(r.motion, r.follower, r.baseRadius, ccw, 1.0);
  if (!prof.error.empty()) {
    SetError(prof.error);
    return false;
  }

  try {
    // One periodic B-spline through the envelope when there is no drop;
    // else one per span, joined by the drops' step faces.
    BRepBuilderAPI_MakeWire wire;
    std::vector<Handle(Geom_BSplineCurve)> curves;
    double dev = 0.0;
    for (const cammech::ProfileSpan& sp : prof.spans) {
      const int n = static_cast<int>(sp.points.size());
      Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n);
      for (int i = 0; i < n; i++)
        pts->SetValue(i + 1, At(pl, sp.points[static_cast<size_t>(i)]));
      GeomAPI_Interpolate interp(pts, prof.closed, 1e-9);
      interp.Perform();
      if (!interp.IsDone()) {
        SetError("fitting the cam's profile failed");
        return false;
      }
      Handle(Geom_BSplineCurve) c = interp.Curve();
      for (glm::dvec2 m : sp.mid) {
        GeomAPI_ProjectPointOnCurve proj(At(pl, m), c);
        if (proj.NbPoints() > 0)
          dev = std::max(dev, proj.LowerDistance());
      }
      curves.push_back(c);
    }
    for (size_t k = 0; k < curves.size(); k++) {
      wire.Add(BRepBuilderAPI_MakeEdge(curves[k]).Edge());
      if (prof.closed)
        break;
      // The step from this span's end to the next span's start.
      const Handle(Geom_BSplineCurve) & next = curves[(k + 1) % curves.size()];
      const gp_Pnt a = curves[k]->Value(curves[k]->LastParameter());
      const gp_Pnt b = next->Value(next->FirstParameter());
      if (r.follower.motion == cammech::FollowerMotion::Turn) {
        // Along the follower's arc: about the pivot as the cam stands at
        // the drop.
        const double th =
            angles::ToRadians(prof.drops[(k + 1) % prof.drops.size()].atDeg);
        const double sg = ccw ? -1.0 : 1.0;
        const glm::dvec2 pv{r.follower.pivotXMm, r.follower.pivotYMm};
        const double c = std::cos(sg * th), s = std::sin(sg * th);
        const glm::dvec2 piv{c * pv.x - s * pv.y, s * pv.x + c * pv.y};
        const gp_Pnt P = At(pl, piv);
        const gp_Vec va(P, a), vb(P, b);
        const double ang = va.AngleWithRef(vb, gp_Vec(pl.z));
        const gp_Pnt mid = a.Rotated(gp_Ax1(P, pl.z), 0.5 * ang);
        if (std::abs(ang) > 1e-9)
          wire.Add(
              BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(a, mid, b).Value())
                  .Edge());
        else
          wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
      } else {
        wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge());
      }
    }
    if (!wire.IsDone()) {
      SetError("the cam's profile does not close");
      return false;
    }
    BRepBuilderAPI_MakeFace mf(wire.Wire(), Standard_True);
    if (!mf.IsDone()) {
      SetError("the cam's profile does not make a face");
      return false;
    }
    BRepPrimAPI_MakePrism prism(mf.Face(), gp_Vec(pl.z) * r.width);
    if (!prism.IsDone()) {
      SetError("extruding the cam failed");
      return false;
    }
    CADShape solid(prism.Shape());
    ElementMap map;
    topo::NamePrimitiveElements(solid, std::to_string(GetId()), map);
    if (newBody) {
      ctx.bodies.push_back({m_name, std::move(solid), m_material, GetId()});
      ctx.bodies.back().elementMap = std::move(map);
    } else {
      Body& hub = *pl.body;
      CheckedBoolean fused =
          CheckedBooleanNamed(m_name, BooleanOp::Union, hub.name, hub.shape,
                              hub.elementMap, solid, map, GetId());
      if (!fused.refusal.empty() || !fused.shape.IsValid()) {
        SetError("adding the cam to its hub failed: " +
                 (fused.refusal.empty() ? fused.error : fused.refusal));
        return false;
      }
      if (fused.fuzzy > 0.0)
        SetWarning(fused.warning);
      hub.shape = std::move(fused.shape);
      hub.elementMap = std::move(fused.map);
    }
    for (cammech::ProfileSpan& sp : prof.spans) {
      sp.points.clear();
      sp.mid.clear();
    }
    r.profile = std::move(prof);
    r.fitDeviation = dev;
    r.frame = glm::dmat4(glm::dvec4(pl.x.X(), pl.x.Y(), pl.x.Z(), 0.0),
                         glm::dvec4(pl.y.X(), pl.y.Y(), pl.y.Z(), 0.0),
                         glm::dvec4(pl.z.X(), pl.z.Y(), pl.z.Z(), 0.0),
                         glm::dvec4(pl.o.X(), pl.o.Y(), pl.o.Z(), 1.0));
    r.valid = true;
    report = std::move(r);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("the cam failed: ") + e.GetMessageString());
    return false;
  }
}

nlohmann::json CamProfileFeature::ReportJson() const {
  if (!report.valid)
    return nullptr;
  const cammech::Profile& p = report.profile;
  const bool turn = report.follower.motion == cammech::FollowerMotion::Turn;
  const double unit = turn ? angles::kDegPerRad : 1.0;
  const char* sfx = turn ? "_deg" : "_mm";
  const auto range = cammech::MotionRange(report.motion, report.follower);
  nlohmann::json drops = nlohmann::json::array();
  for (const cammech::Drop& d : p.drops)
    drops.push_back({{"at_deg", d.atDeg},
                     {std::string("height") + sfx, (d.from - d.to) * unit}});
  nlohmann::json out{
      {"base_radius_mm", report.baseRadius},
      {"width_mm", report.width},
      {"max_radius_mm", p.maxRadiusMm},
      {std::string("rise") + sfx, (range.second - range.first) * unit},
      {"fit_deviation_mm", report.fitDeviation},
      {"max_pressure_angle_deg", p.maxPressureAngleDeg},
      {"max_pressure_at_deg", p.maxPressureAtDeg},
      {"min_convex_radius_mm", p.minConvexRadiusMm},
      {"drops", drops},
      {"follower",
       {{"kind", cammech::FollowerKindName(report.follower.kind)},
        {"motion", cammech::FollowerMotionName(report.follower.motion)},
        {"radius_mm", report.follower.radiusMm}}},
      {"derived", report.derived}};
  if (p.minConcaveRadiusMm > 0.0)
    out["min_concave_radius_mm"] = p.minConcaveRadiusMm;
  if (report.follower.kind == cammech::FollowerKind::Flat)
    out["flat_face_width_mm"] = p.flatWidthMm;
  return out;
}
