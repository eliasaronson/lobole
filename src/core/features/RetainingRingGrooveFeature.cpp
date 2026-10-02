#include "core/features/RetainingRingGrooveFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/PrintAllowance.h"
#include "core/parts/RetainingRingTable.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cylinder.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace {

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

} // namespace

RetainingRingGrooveFeature::RetainingRingGrooveFeature(const std::string& name,
                                                       ElementRef face,
                                                       ElementRef from,
                                                       double offset)
    : face(std::move(face)), from(std::move(from)), offset(offset) {
  m_name = name;
}

bool RetainingRingGrooveFeature::Execute(RegenContext& ctx) {
  if (face.name.empty())
    return true; // previewing: no face picked yet
  if (from.name.empty()) {
    SetError("a retaining ring groove is placed from an EDGE at one end of "
             "its face (`from`): the side of what the ring holds");
    return false;
  }
  if (from.featureId != face.featureId) {
    SetError("`from` is an edge of another body — it must be a circular edge "
             "at one end of the grooved face");
    return false;
  }
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(), [&](const Body& b) {
        return b.createdBy == face.featureId;
      });
  if (body == ctx.bodies.end()) {
    SetError("the grooved body no longer exists");
    return false;
  }
  const auto faceIdx = body->elementMap.Resolve(face.name);
  if (!faceIdx) {
    SetError("grooved face '" + face.name + "' no longer exists");
    return false;
  }
  const auto edgeIdx = body->elementMap.ResolveEdge(from.name);
  if (!edgeIdx) {
    SetError("edge '" + from.name + "' no longer exists");
    return false;
  }
  TopoDS_Face occFace;
  int i = 0;
  for (TopExp_Explorer ex(body->shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == *faceIdx) {
      occFace = TopoDS::Face(ex.Current());
      break;
    }
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(body->shape.GetShape(), TopAbs_EDGE, edges);
  if (occFace.IsNull() || *edgeIdx < 0 || *edgeIdx >= edges.Extent()) {
    SetError("the grooved face or its edge could not be resolved");
    return false;
  }
  const TopoDS_Edge occEdge = TopoDS::Edge(edges(*edgeIdx + 1));

  try {
    const BRepAdaptor_Surface surf(occFace);
    if (surf.GetType() != GeomAbs_Cylinder) {
      SetError("a retaining ring groove is cut in a CYLINDRICAL face — a "
               "shaft or a bore's wall");
      return false;
    }
    const gp_Cylinder cyl = surf.Cylinder();
    const double r = cyl.Radius();
    const gp_Pnt o = cyl.Location();
    const gp_Dir d = cyl.Axis().Direction();

    // A shaft or a bore: the face normal points away from the axis on a
    // shaft, toward it in a bore (as the Thread feature reads it).
    const double uMid = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const double vMid = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    BRepGProp_Face gface(occFace);
    gp_Pnt p;
    gp_Vec n;
    gface.Normal(uMid, vMid, p, n);
    const gp_Vec toP(o, p);
    const gp_Vec radial = toP - gp_Vec(d.XYZ() * toP.Dot(gp_Vec(d)));
    const bool shaft = n.Dot(radial) > 0.0;

    // The standard: the face's, and one given must agree with it.
    const parts::RetainingRingStandard own =
        shaft ? parts::RetainingRingStandard::Din471
              : parts::RetainingRingStandard::Din472;
    if (!standard.empty()) {
      parts::RetainingRingStandard asked{};
      if (!parts::RetainingRingStandardFromString(standard, asked)) {
        SetError("unknown retaining ring standard \"" + standard +
                 "\" — accepted: " + parts::KnownRetainingRingStandards());
        return false;
      }
      if (asked != own) {
        SetError(std::string("a ") +
                 (shaft ? "DIN 472 groove is cut in a bore's wall; this "
                          "face is a Ø"
                        : "DIN 471 groove is cut in a shaft; this face is a "
                          "Ø") +
                 Mm(2.0 * r) + (shaft ? " shaft" : " bore") + ", which takes " +
                 parts::RetainingRingStandardName(own) +
                 " (or leave `standard` empty)");
        return false;
      }
    }
    // On a PRINTED part the face is read at the size it prints, as
    // fit_bearing reads a seat: a PETG journal for a Ø25 ring is modelled
    // Ø24.9 (docs/tolerances/README.md#printed-parts), and its groove is
    // the Ø25's.
    const printfit::Printed printedAs =
        shaft ? printfit::Printed::Shaft : printfit::Printed::Hole;
    double nominal = 2.0 * r;
    std::string printedNote;
    if (ctx.print != nullptr) {
      std::string ignored;
      if (const auto a =
              printfit::ForModelled(*ctx.print, printedAs, nominal, &ignored)) {
        nominal = a->basicMm;
        printedNote =
            " (modelled Ø" + Mm(2.0 * r) + ", prints Ø" + Mm(nominal) + ")";
      }
    }
    std::string why;
    const parts::RetainingRingRow* row =
        parts::FindRetainingRing(own, nominal, &why);
    if (row == nullptr) {
      SetError(why + printedNote);
      return false;
    }

    // `from` must be a circle on this face's axis at one of its ends.
    const BRepAdaptor_Curve curve(occEdge);
    const double tol = 1e-6 + 1e-6 * r;
    bool onAxis = curve.GetType() == GeomAbs_Circle;
    double vEdge = 0.0;
    if (onAxis) {
      const gp_Circ c = curve.Circle();
      const gp_Vec toC(o, c.Location());
      vEdge = toC.Dot(gp_Vec(d));
      const gp_Vec off = toC - gp_Vec(d.XYZ() * vEdge);
      onAxis = std::abs(c.Radius() - r) < 1e-4 && off.Magnitude() < 1e-4 &&
               c.Axis().Direction().IsParallel(d, 1e-6);
    }
    if (!onAxis) {
      SetError("`from` is not a circular edge of this face — pick the rim "
               "at the end the groove is measured from");
      return false;
    }
    double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
    if (v1 < v0)
      std::swap(v0, v1);
    double sign = 0.0;
    if (std::abs(vEdge - v0) < tol)
      sign = 1.0;
    else if (std::abs(vEdge - v1) < tol)
      sign = -1.0;
    if (sign == 0.0) {
      SetError("`from` is not at an end of this face — pick the rim at the "
               "end the groove is measured from");
      return false;
    }

    // The groove must lie on the face, with the edge distance left past it.
    const double length = v1 - v0;
    const std::string label = parts::RetainingRingDesignation(own, *row);
    if (offset < 0.0) {
      SetError("offset " + Mm(offset) +
               " mm is negative — it is measured from `from` INTO the face");
      return false;
    }
    if (offset + row->m > length + 1e-9) {
      SetError("the groove for " + label + " (" + Mm(row->m) +
               " mm wide at offset " + Mm(offset) +
               ") runs off this face, which is " + Mm(length) + " mm long");
      return false;
    }
    const double land = length - offset - row->m;
    if (land < row->n - 1e-9) {
      SetError("the groove for " + label + " leaves " + Mm(land) +
               " mm of the face past it, and the table's edge distance n is " +
               Mm(row->n) +
               " mm — the land that carries the ring's thrust. On this " +
               Mm(length) + " mm face the offset can be at most " +
               Mm(length - row->m - row->n) + " mm");
      return false;
    }

    // The tool: an annulus from the groove floor out past the face (in past
    // it in a bore), m wide, revolved from a section in the plane of the
    // cylinder's Y direction — off its seam, which is in the X plane.
    const double vA = vEdge + sign * offset;
    const double vB = vA + sign * row->m;
    // The floor where it PRINTS at d2 on a printed part.
    double floor = row->d2 / 2.0;
    if (ctx.print != nullptr) {
      std::string ignored;
      if (const auto a =
              printfit::ForBasic(*ctx.print, printedAs, row->d2, &ignored))
        floor = a->modelMm / 2.0;
    }
    const double reach = shaft ? r + row->t : r - row->t;
    const double rIn = std::min(floor, reach), rOut = std::max(floor, reach);
    const gp_Dir y = cyl.Position().YDirection();
    const auto at = [&](double rad, double v) {
      return gp_Pnt(o.XYZ() + y.XYZ() * rad + d.XYZ() * v);
    };
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(at(rIn, std::min(vA, vB)));
    poly.Add(at(rOut, std::min(vA, vB)));
    poly.Add(at(rOut, std::max(vA, vB)));
    poly.Add(at(rIn, std::max(vA, vB)));
    poly.Close();
    BRepBuilderAPI_MakeFace section(poly.Wire(), /*OnlyPlane=*/true);
    if (!section.IsDone())
      throw Standard_Failure("groove section");
    BRepPrimAPI_MakeRevol revol(section.Face(), gp_Ax1(o, d),
                                2.0 * std::numbers::pi);
    revol.Build();
    if (!revol.IsDone())
      throw Standard_Failure("groove revolve");
    const CADShape tool(revol.Shape());

    // Its faces by what they will be in the groove; the reach cylinder is
    // outside the material and leaves nothing behind.
    const std::string prefix = std::to_string(GetId());
    ElementMap toolMap;
    int index = 0;
    for (TopExp_Explorer ex(tool.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), index++) {
      const TopoDS_Face f = TopoDS::Face(ex.Current());
      const BRepAdaptor_Surface s(f);
      const char* role = nullptr;
      if (s.GetType() == GeomAbs_Cylinder &&
          std::abs(s.Cylinder().Radius() - floor) < 1e-6) {
        role = kFloor;
      } else if (s.GetType() == GeomAbs_Plane) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        const double v = gp_Vec(o, props.CentreOfMass()).Dot(gp_Vec(d));
        role = std::abs(v - vA) < 1e-6   ? kFlankA
               : std::abs(v - vB) < 1e-6 ? kFlankB
                                         : nullptr;
      }
      toolMap.names[index] = role != nullptr
                                 ? prefix + "/" + role
                                 : prefix + "/face" + std::to_string(index);
    }
    topo::ApplyFallbackPasses(tool, toolMap);

    CheckedBoolean cut = CheckedBooleanNamed(
        m_name, BooleanOp::Subtract, body->name, body->shape, body->elementMap,
        tool, toolMap, GetId());
    if (!cut.refusal.empty() || !cut.shape.IsValid()) {
      SetError("the groove's cut failed (" +
               (cut.refusal.empty() ? cut.error : cut.refusal) + ")");
      return false;
    }
    if (cut.fuzzy > 0.0)
      SetWarning(cut.warning);
    body->shape = std::move(cut.shape);
    body->elementMap = std::move(cut.map);
    // The groove's grip (RingGrip, docs/parts/README.md#a-ring-in-its-groove):
    // on its floor, which a ring of this standard and size may overlap.
    const std::string floorName = prefix + "/" + kFloor;
    for (const auto& [idx, name] : body->elementMap.names)
      if (name == floorName || name.rfind(floorName + ";", 0) == 0) {
        RingGrip grip;
        grip.face = {body->createdBy, name};
        grip.featureId = GetId();
        grip.standard = parts::RetainingRingStandardName(own);
        grip.size = row->d1;
        grip.groove = true;
        body->ringGrips.push_back(std::move(grip));
        break;
      }
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("retaining ring groove failed: ") +
             e.GetMessageString());
    LOG_ERROR("RetainingRingGrooveFeature '{}': {}", m_name, GetError());
    return false;
  }
}
