#include "core/features/PipeFeature.h"

#include "Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/features/SweepFeature.h" // ResolveSweepPathWire, SweepProfileAlongSpine

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

namespace {

// A circular wire of `radius` centered at the spine's start point and lying in
// the plane normal to the spine there — the section a pipe sweeps. Null wire if
// the spine has no edge or a degenerate start tangent.
TopoDS_Wire StartSectionCircle(const TopoDS_Wire& spine, double radius) {
  TopExp_Explorer exp(spine, TopAbs_EDGE);
  if (!exp.More())
    return TopoDS_Wire();
  BRepAdaptor_Curve curve(TopoDS::Edge(exp.Current()));
  gp_Pnt p;
  gp_Vec tangent;
  curve.D1(curve.FirstParameter(), p, tangent);
  if (tangent.Magnitude() < 1e-9)
    return TopoDS_Wire();
  gp_Ax2 axis(p, gp_Dir(tangent));
  BRepBuilderAPI_MakeEdge edge(gp_Circ(axis, radius));
  if (!edge.IsDone())
    return TopoDS_Wire();
  BRepBuilderAPI_MakeWire wire(edge.Edge());
  return wire.IsDone() ? wire.Wire() : TopoDS_Wire();
}

} // namespace

PipeFeature::PipeFeature(const std::string& name, uint64_t pathId,
                         double outerDiameter, double wallThickness,
                         ExtrudeMode mode)
    : pathId(pathId), outerDiameter(outerDiameter),
      wallThickness(wallThickness), mode(mode) {
  m_name = name;
}

void PipeFeature::VisitExprs(const ExprVisitor& visit) {
  visit(outerDiameterExpr, outerDiameter);
  visit(wallThicknessExpr, wallThickness);
}

bool PipeFeature::Execute(RegenContext& ctx) {
  const double rOuter = outerDiameter * 0.5;
  if (rOuter <= 1e-9) {
    SetError("pipe needs a positive outer diameter");
    return false;
  }
  if (wallThickness < 0.0 || wallThickness >= rOuter) {
    SetError("pipe wall must be thinner than the radius");
    return false;
  }

  const TopoDS_Wire spine = ResolveSweepPathWire(ctx, pathId);
  if (spine.IsNull()) {
    SetError("pipe path must be an open sketch chain or a 3D curve");
    return false;
  }

  const TopoDS_Wire outerWire = StartSectionCircle(spine, rOuter);
  if (outerWire.IsNull()) {
    SetError("pipe could not build a section at the path start");
    return false;
  }
  CADShape result = SweepProfileAlongSpine(outerWire, spine,
                                           SweepFrame::CorrectedFrenet);
  if (!result.IsValid() || result.Volume() < 1e-9) {
    SetError("pipe sweep failed");
    return false;
  }

  // Hollow: subtract an inner pipe swept down the same spine.
  if (wallThickness > 1e-9) {
    const TopoDS_Wire innerWire =
        StartSectionCircle(spine, rOuter - wallThickness);
    CADShape inner =
        SweepProfileAlongSpine(innerWire, spine, SweepFrame::CorrectedFrenet);
    if (!inner.IsValid()) {
      SetError("pipe bore sweep failed");
      return false;
    }
    result = result.Difference(inner);
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("pipe bore subtraction produced an invalid solid");
      return false;
    }
  }

  ElementMap map;
  topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
  return ApplyBodyModeReporting(ctx.bodies, std::move(result), std::move(map),
                                mode, *this);
}
