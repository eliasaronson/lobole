#include "core/features/CurveFeature.h"

#include "Logger.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepLib.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Geom2d_Line.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Lin2d.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <cmath>

namespace {

gp_Pnt ToPnt(const glm::vec3& v) { return gp_Pnt(v.x, v.y, v.z); }

// Tessellates a built edge into a world-space polyline with GCPnts adaptive
// sampling (angular + curvature deflection), the same shape the viewport draws
// and a good-enough representation for measure. Returns false on an empty
// sampling.
bool TessellateEdge(const TopoDS_Edge& edge, std::vector<glm::vec3>& out) {
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
    out.emplace_back(static_cast<float>(p.X()), static_cast<float>(p.Y()),
                     static_cast<float>(p.Z()));
  }
  return true;
}

// Clamped-uniform B-spline through control poles in world space (the 3D twin of
// SketchProfile's MakeBSplineCurve). Degree is reduced so few poles stay valid.
Handle(Geom_BSplineCurve) ControlPolyCurve(const std::vector<glm::vec3>& poles,
                                           int degree) {
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
  }
}

bool CurveFeature::Execute(RegenContext& ctx) {
  (void)ctx;
  m_polyline.clear();
  m_edgeShape = CADShape();

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
    case Curve3DKind::Helix: {
      if (radius <= 1e-9 || pitch <= 1e-9 || turns <= 1e-9) {
        SetError("helix needs positive radius, pitch, and turns");
        return false;
      }
      const glm::vec3 d = glm::length(axisDir) > 1e-9f
                              ? glm::normalize(axisDir)
                              : glm::vec3(0, 0, 1);
      gp_Ax3 axis(ToPnt(axisOrigin), gp_Dir(d.x, d.y, d.z));
      Handle(Geom_CylindricalSurface) cyl =
          new Geom_CylindricalSurface(axis, radius);
      // Helix as a straight line in the cylinder's (u=angle, v=height) param
      // space: slope k = pitch/2pi rises v by `pitch` per 2pi of angle. A unit
      // 2D direction means the line's own parameter w maps u = w/|(1,k)|, so we
      // run w to 2pi*turns*|(1,k)| to sweep exactly `turns` turns. Left-handed
      // reverses the angular sense (u decreases while height still rises).
      const double k = pitch / (2.0 * M_PI);
      const double norm = std::sqrt(1.0 + k * k);
      const double wLast = 2.0 * M_PI * turns * norm;
      gp_Dir2d dir(rightHanded ? 1.0 : -1.0, k);
      Handle(Geom2d_Line) line2d = new Geom2d_Line(gp_Pnt2d(0.0, 0.0), dir);
      edge = BRepBuilderAPI_MakeEdge(line2d, cyl, 0.0, wLast).Edge();
      BRepLib::BuildCurve3d(edge); // materialize the 3D curve for downstream use
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
