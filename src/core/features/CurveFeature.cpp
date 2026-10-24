// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/CurveFeature.h"
#include "core/HelicalSweep.h"
#include "core/ModelSpace.h"

#include "common/Logger.h"
#include "core/Angles.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

gp_Pnt ToPnt(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }

// Tessellates a built edge into a world-space polyline with GCPnts adaptive
// sampling (angular + curvature deflection), the same shape the viewport draws
// and a good-enough representation for measure. Returns false on an empty
// sampling.
bool TessellateEdge(const TopoDS_Edge& edge, std::vector<glm::dvec3>& out) {
  BRepAdaptor_Curve adaptor(edge);
  GCPnts_TangentialDeflection tess(adaptor, /*angularDeflection=*/0.1,
                                   /*curvatureDeflection=*/0.05,
                                   /*minPoints=*/2);
  const int n = tess.NbPoints();
  if (n < 2)
    return false;
  out.clear();
  out.reserve(n);
  for (int i = 1; i <= n; i++) {
    const gp_Pnt p = tess.Value(i);
    out.emplace_back(p.X(), p.Y(), p.Z());
  }
  return true;
}

// The same, for a wire of several edges: each edge sampled in order, without
// repeating the point where two of them meet.
bool TessellateWire(const TopoDS_Wire& wire, std::vector<glm::dvec3>& out) {
  out.clear();
  for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next()) {
    std::vector<glm::dvec3> piece;
    if (!TessellateEdge(TopoDS::Edge(ex.Current()), piece))
      return false;
    for (size_t i = 0; i < piece.size(); i++) {
      if (i == 0 && !out.empty() && glm::length(piece[i] - out.back()) < 1e-6)
        continue;
      out.push_back(piece[i]);
    }
  }
  return out.size() >= 2;
}

// Clamped-uniform B-spline through control poles in world space (the 3D twin of
// SketchProfile's MakeBSplineCurve). Degree is reduced so few poles stay valid.
Handle(Geom_BSplineCurve)
    ControlPolyCurve(const std::vector<glm::dvec3>& poles, int degree) {
  const int nPoles = static_cast<int>(poles.size());
  const int p = std::clamp(degree > 0 ? degree : 3, 1, nPoles - 1);
  TColgp_Array1OfPnt occPoles(1, nPoles);
  for (int i = 0; i < nPoles; i++)
    occPoles.SetValue(i + 1, ToPnt(poles[i]));
  const int nDistinct = nPoles - p + 1;
  TColStd_Array1OfReal knots(1, nDistinct);
  TColStd_Array1OfInteger mults(1, nDistinct);
  for (int i = 0; i < nDistinct; i++) {
    knots.SetValue(i + 1, static_cast<double>(i));
    mults.SetValue(i + 1, 1);
  }
  mults.SetValue(1, p + 1);
  mults.SetValue(nDistinct, p + 1);
  return new Geom_BSplineCurve(occPoles, knots, mults, p);
}

// A bent tube's centreline: straights between the corners, each corner turned
// on a former of `radius`. Line-arc-line, exactly — which is what a bender
// makes and what a bend radius MEANS.
//
// Each corner eats `radius * tan(turn/2)` of straight on either side of it, so
// two corners closer together than that cannot both be made: the refusal says
// which pair and by how much, because the fix is a longer straight or a
// tighter former and the caller has to know which.
TopoDS_Wire BendPathWire(const std::vector<glm::dvec3>& raw, double radius,
                         std::vector<CurveFeature::Bend>& bends, double& length,
                         std::string* why) {
  const auto fail = [&](const std::string& s) {
    if (why)
      *why = s;
    return TopoDS_Wire();
  };
  std::vector<glm::dvec3> p;
  for (const glm::dvec3& d : raw)
    if (p.empty() || glm::length(d - p.back()) > 1e-9)
      p.push_back(d);
  if (p.size() < 2)
    return fail("a bent path needs at least two distinct points - the corners "
                "the tube is bent at, in order");
  if (radius <= 1e-9)
    return fail("a bent path needs a positive bend radius: the former the "
                "tube is bent round, measured to its CENTRELINE");

  // Per corner: the turn, and how much straight it eats on each side.
  struct Corner {
    glm::dvec3 in, out; // unit directions arriving and leaving
    double turn = 0.0;  // radians, > 0
    double tangent = 0.0;
  };
  std::vector<Corner> corners(p.size());
  for (size_t i = 1; i + 1 < p.size(); i++) {
    Corner& c = corners[i];
    c.in = glm::normalize(p[i] - p[i - 1]);
    c.out = glm::normalize(p[i + 1] - p[i]);
    const double dot = std::clamp(glm::dot(c.in, c.out), -1.0, 1.0);
    c.turn = std::acos(dot);
    if (c.turn < 1e-9)
      continue; // collinear: no bend here, the straights merge
    if (c.turn > M_PI - 1e-6)
      return fail("the path doubles back on itself at point " +
                  std::to_string(i + 1) +
                  "; a tube cannot be bent 180 "
                  "degrees on a former");
    c.tangent = radius * std::tan(c.turn / 2.0);
  }
  for (size_t i = 1; i + 1 < p.size(); i++) {
    const double run = glm::length(p[i + 1] - p[i]);
    const double need =
        corners[i].tangent + (i + 2 < p.size() ? corners[i + 1].tangent : 0.0);
    if (need > run + 1e-9) {
      char buf[256]; // the sentence with two point numbers and two lengths
      std::snprintf(buf, sizeof buf,
                    "the straight between points %zu and %zu is %.3g mm and "
                    "the bends at its ends need %.3g mm of it: use a longer "
                    "straight or a tighter former",
                    i + 1, i + 2, run, need);
      return fail(buf);
    }
  }

  BRepBuilderAPI_MakeWire wire;
  glm::dvec3 from = p.front();
  double straight = 0.0;
  glm::dvec3 lastPlane(0.0);
  for (size_t i = 1; i + 1 < p.size(); i++) {
    const Corner& c = corners[i];
    if (c.turn < 1e-9)
      continue;
    const glm::dvec3 a = p[i] - c.in * c.tangent;
    const glm::dvec3 b = p[i] + c.out * c.tangent;
    if (glm::length(a - from) > 1e-9) {
      wire.Add(BRepBuilderAPI_MakeEdge(ToPnt(from), ToPnt(a)).Edge());
      straight += glm::length(a - from);
      length += glm::length(a - from);
    }
    // The arc through its own midpoint: the centre lies along the internal
    // bisector at radius / cos(turn/2), and the midpoint is on the ray from
    // there back to the corner.
    const glm::dvec3 bisector = glm::normalize(c.out - c.in);
    const glm::dvec3 centre = p[i] + bisector * (radius / std::cos(c.turn / 2));
    const glm::dvec3 mid = centre + glm::normalize(p[i] - centre) * radius;
    const Handle(Geom_TrimmedCurve) arc =
        GC_MakeArcOfCircle(ToPnt(a), ToPnt(mid), ToPnt(b)).Value();
    if (arc.IsNull())
      return fail("the bend at point " + std::to_string(i + 1) +
                  " could not be made into an arc");
    wire.Add(BRepBuilderAPI_MakeEdge(arc).Edge());
    length += radius * c.turn;

    CurveFeature::Bend bend;
    bend.corner = p[i];
    bend.angleDeg = angles::ToDegrees(c.turn);
    bend.radius = radius;
    bend.straightBefore = straight;
    // The ROLL a bender turns the tube through between one bend and the next:
    // the angle between the two bend planes, which is the third number on a
    // bending schedule and zero for a flat path.
    const glm::dvec3 plane = glm::normalize(glm::cross(c.in, c.out));
    if (glm::length(lastPlane) > 0.5) {
      const double dot = std::clamp(glm::dot(lastPlane, plane), -1.0, 1.0);
      bend.rollDeg = angles::ToDegrees(std::acos(dot));
    }
    lastPlane = plane;
    bends.push_back(bend);
    from = b;
    straight = 0.0;
  }
  if (glm::length(p.back() - from) > 1e-9) {
    wire.Add(BRepBuilderAPI_MakeEdge(ToPnt(from), ToPnt(p.back())).Edge());
    length += glm::length(p.back() - from);
  }
  if (!wire.IsDone())
    return fail("the bent path's segments do not join up");
  return wire.Wire();
}

} // namespace

CurveFeature::CurveFeature(const std::string& name, Curve3DKind kind)
    : kind(kind) {
  m_name = name;
}

void CurveFeature::VisitExprs(const ExprVisitor& visit) {
  if (kind == Curve3DKind::Helix) {
    visit(radiusExpr, radius);
    visit(pitchExpr, pitch);
    visit(turnsExpr, turns);
    visit(startDegExpr, startDeg);
  }
}

glm::dvec3 HelixReference(const glm::dvec3& axis) {
  // OpenCASCADE's own choice for gp_Ax3(P, V), asked of it rather than
  // re-derived, so every helix that predates start_deg keeps its start.
  const glm::dvec3 d =
      glm::dot(axis, axis) > 1e-18 ? glm::normalize(axis) : glm::dvec3(0, 0, 1);
  const gp_Dir x = gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(d.x, d.y, d.z)).XDirection();
  return {x.X(), x.Y(), x.Z()};
}

std::string CurveFeature::WhyIncomplete() const {
  if (kind != Curve3DKind::BentPath)
    return {};
  std::vector<Bend> bends;
  double length = 0.0;
  std::string why;
  if (BendPathWire(points, radius, bends, length, &why).IsNull())
    return why.empty() ? "the bent path could not be built" : why;
  return {};
}

bool CurveFeature::Execute(RegenContext& ctx) {
  (void)ctx;
  m_polyline.clear();
  m_edgeShape = CADShape();
  m_bends.clear();
  m_length = 0.0;

  // Outside the model space (core/ModelSpace.h) a curve is refused by name,
  // as a sketch is: a point at 2^53 mm made a sweep along it take seconds.
  // A helix of more turns than that space holds coils of a micrometre is
  // refused the same way.
  const auto inSpace = [](const glm::dvec3& v) {
    return InModelSpace(v.x) && InModelSpace(v.y) && InModelSpace(v.z);
  };
  if (!std::all_of(points.begin(), points.end(), inSpace) ||
      !inSpace(axisOrigin) || !inSpace(axisDir) || !InModelSpace(radius) ||
      !InModelSpace(pitch) || !InModelSpace(pitch * turns) ||
      !(std::abs(turns) <= 1e4)) {
    SetError("a point, the axis, the radius, the pitch or the turns are "
             "outside the " +
             std::to_string(static_cast<long>(kModelSpaceMm)) +
             " mm model space (core/ModelSpace.h; at most 10000 turns)");
    return false;
  }

  try {
    TopoDS_Edge edge;
    switch (kind) {
    case Curve3DKind::FitSpline: {
      if (points.size() < 2) {
        SetError("3D fit-spline needs at least 2 points");
        return false;
      }
      Handle(TColgp_HArray1OfPnt) pts =
          new TColgp_HArray1OfPnt(1, static_cast<int>(points.size()));
      for (int i = 0; i < static_cast<int>(points.size()); i++)
        pts->SetValue(i + 1, ToPnt(points[i]));
      GeomAPI_Interpolate interp(pts, /*periodic=*/closed, /*tol=*/1e-6);
      interp.Perform();
      if (!interp.IsDone()) {
        SetError("3D fit-spline interpolation failed (coincident points?)");
        return false;
      }
      edge = BRepBuilderAPI_MakeEdge(interp.Curve()).Edge();
      break;
    }
    case Curve3DKind::ControlPoly: {
      if (points.size() < 2) {
        SetError("3D control-poly spline needs at least 2 poles");
        return false;
      }
      edge = BRepBuilderAPI_MakeEdge(ControlPolyCurve(points, degree)).Edge();
      break;
    }
    case Curve3DKind::BentPath: {
      std::string why;
      const TopoDS_Wire wire =
          BendPathWire(points, radius, m_bends, m_length, &why);
      if (wire.IsNull()) {
        SetError(why);
        return false;
      }
      m_edgeShape = CADShape(wire);
      if (!TessellateWire(wire, m_polyline)) {
        SetError("the bent path could not be tessellated");
        return false;
      }
      return true;
    }
    case Curve3DKind::Helix: {
      if (radius <= 1e-9 || pitch <= 1e-9 || turns <= 1e-9) {
        SetError("helix needs positive radius, pitch, and turns");
        return false;
      }
      const glm::dvec3 d = glm::length(axisDir) > 1e-9 ? glm::normalize(axisDir)
                                                       : glm::dvec3(0, 0, 1);
      // The reference is named (HelixReference) and the start turned from it,
      // right-handed about the axis: the parameter-space line starts at
      // u = start_deg instead of 0.
      const glm::dvec3 ref = HelixReference(glm::dvec3(d));
      gp_Ax3 axis(ToPnt(axisOrigin), gp_Dir(d.x, d.y, d.z),
                  gp_Dir(ref.x, ref.y, ref.z));
      // The one helix builder, shared with the modelled thread
      // (core/HelicalSweep.h).
      edge = HelixEdge(axis, radius, pitch, turns, rightHanded,
                       angles::ToRadians(startDeg));
      break;
    }
    }

    if (edge.IsNull()) {
      SetError("curve produced no edge");
      return false;
    }
    if (!TessellateEdge(edge, m_polyline)) {
      SetError("curve could not be tessellated");
      return false;
    }
    m_edgeShape = CADShape(edge);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("curve failed: ") + e.GetMessageString());
    LOG_ERROR("CurveFeature '{}': {}", m_name, GetError());
    return false;
  }
}
