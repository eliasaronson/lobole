#include "core/features/RetainingRingFeature.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/parts/RetainingRingTable.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>

#include <cmath>
#include <numbers>

const RetainingRingFeature* SoleRetainingRingOf(const Document* doc) {
  if (doc == nullptr || doc->FeatureCount() != 1)
    return nullptr;
  return dynamic_cast<const RetainingRingFeature*>(doc->GetFeatures()[0].get());
}

RetainingRingFeature::RetainingRingFeature(const std::string& name,
                                           std::string standard, double size)
    : standard(std::move(standard)), size(size) {
  m_name = name;
}

bool RetainingRingFeature::Execute(RegenContext& ctx) {
  parts::RetainingRingStandard which{};
  if (!parts::RetainingRingStandardFromString(standard, which)) {
    SetError("unknown retaining ring standard \"" + standard +
             "\" — accepted: " + parts::KnownRetainingRingStandards());
    return false;
  }
  std::string why;
  const parts::RetainingRingRow* row =
      parts::FindRetainingRing(which, size, &why);
  if (row == nullptr) {
    SetError(why);
    return false;
  }
  const bool shaft = which == parts::RetainingRingStandard::Din471;
  const double ri = shaft ? row->d3 / 2.0 : row->d3 / 2.0 - row->b;
  const double ro = shaft ? row->d3 / 2.0 + row->b : row->d3 / 2.0;
  const double s = row->s;
  const std::string designation = parts::RetainingRingDesignation(which, *row);

  CADShape solid;
  try {
    // The rectangular section in the XZ plane (x = radius), revolved.
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(ri, 0.0, 0.0));
    poly.Add(gp_Pnt(ro, 0.0, 0.0));
    poly.Add(gp_Pnt(ro, 0.0, s));
    poly.Add(gp_Pnt(ri, 0.0, s));
    poly.Close();
    if (!poly.IsDone())
      throw Standard_Failure("section polygon");
    BRepBuilderAPI_MakeFace face(poly.Wire(), /*OnlyPlane=*/true);
    if (!face.IsDone())
      throw Standard_Failure("section face");
    BRepPrimAPI_MakeRevol revol(face.Face(),
                                gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                                2.0 * std::numbers::pi);
    revol.Build();
    if (!revol.IsDone())
      throw Standard_Failure("revolve");
    solid = CADShape(revol.Shape());
  } catch (const Standard_Failure& failure) {
    SetError("could not build " + designation + " from its dimensions (" +
             std::string(failure.GetMessageString()) + ")");
    return false;
  }
  if (!solid.IsValid()) {
    SetError("could not build " + designation + " from its dimensions");
    return false;
  }

  // Every face by what it is, read off the built geometry.
  const std::string prefix = std::to_string(GetId());
  constexpr double kTol = 1e-6;
  ElementMap map;
  int index = 0;
  for (TopExp_Explorer ex(solid.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), index++) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surface(f);
    const char* role = nullptr;
    if (surface.GetType() == GeomAbs_Cylinder) {
      const double r = surface.Cylinder().Radius();
      role = std::abs(r - ri) < kTol   ? kBore
             : std::abs(r - ro) < kTol ? kOutside
                                       : nullptr;
    } else if (surface.GetType() == GeomAbs_Plane) {
      GProp_GProps props;
      BRepGProp::SurfaceProperties(f, props);
      const double z = props.CentreOfMass().Z();
      role = std::abs(z) < kTol       ? kSideA
             : std::abs(z - s) < kTol ? kSideB
                                      : nullptr;
    }
    map.names[index] = role != nullptr
                           ? prefix + "/" + role
                           : prefix + "/face" + std::to_string(index);
  }
  topo::ApplyFallbackPasses(solid, map);

  const std::string material = m_material.empty() ? "steel" : m_material;
  ctx.bodies.push_back({m_name.empty() ? designation : m_name, std::move(solid),
                        material, GetId()});
  ctx.bodies.back().elementMap = std::move(map);
  return true;
}
