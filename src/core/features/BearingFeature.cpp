// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/BearingFeature.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/parts/BearingTable.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <limits>
#include <numbers>

const BearingFeature* SoleBearingOf(const Document* doc) {
  if (doc == nullptr || doc->FeatureCount() != 1)
    return nullptr;
  return dynamic_cast<const BearingFeature*>(doc->GetFeatures()[0].get());
}

BearingFeature::BearingFeature(const std::string& name, std::string designation)
    : designation(std::move(designation)) {
  m_name = name;
}

std::string BearingFeature::FaceName(const char* role) const {
  return std::to_string(GetId()) + "/" + role;
}

std::string BearingFeature::RoleOf(const std::string& elementName) {
  const size_t slash = elementName.rfind('/');
  if (slash == std::string::npos)
    return {};
  const std::string role = elementName.substr(slash + 1);
  for (const char* known : {kBore, kOutside, kInnerSideA, kInnerSideB,
                            kOuterSideA, kOuterSideB, kShieldA, kShieldB})
    if (role == known)
      return role;
  return {};
}

namespace {

// The drawing proportions. None of these is a dimension of ISO 15 — the
// standard fixes d, D, B and the chamfer and nothing inside — so they are
// chosen to LOOK like a bearing and to weigh like one, and they are named
// here once rather than scattered as literals:
//  * each ring's land is 0.3 of the section height (D - d)/2, which is what a
//    62-series bearing's shoulders measure in a supplier's drawing;
//  * the shield is recessed 0.05 B, enough to read as a separate part in the
//    viewport and at the outline pass;
//  * the ball space is a torus of diameter 0.6 min(section, B) on the pitch
//    circle, which lands the mass within a few per cent of catalogue figures
//    for a steel 608, 6200 and 6206 (the header lists them).
constexpr double kLand = 0.3;
constexpr double kShieldRecess = 0.05;
constexpr double kBallSpace = 0.6;

} // namespace

bool BearingFeature::Execute(RegenContext& ctx) {
  std::string why;
  const parts::BearingRow* row = parts::FindBearing(designation, &why);
  if (row == nullptr) {
    SetError(why);
    return false;
  }
  const double ri0 = row->d / 2.0; // bore radius
  const double ro0 = row->D / 2.0; // outside radius
  const double B = row->B;
  const double c = row->rMin; // chamfer, drawn at the standard's minimum
  const double h = ro0 - ri0; // section height
  const double ri = ri0 + kLand * h;
  const double ro = ro0 - kLand * h;
  const double e = kShieldRecess * B;

  CADShape solid;
  try {
    // The half-section in the XZ plane (x = radius), revolved about Z.
    BRepBuilderAPI_MakePolygon poly;
    const auto at = [&](double r, double z) { poly.Add(gp_Pnt(r, 0.0, z)); };
    at(ri0 + c, 0.0);
    at(ri, 0.0);
    at(ri, e);
    at(ro, e);
    at(ro, 0.0);
    at(ro0 - c, 0.0);
    at(ro0, c);
    at(ro0, B - c);
    at(ro0 - c, B);
    at(ro, B);
    at(ro, B - e);
    at(ri, B - e);
    at(ri, B);
    at(ri0 + c, B);
    at(ri0, B - c);
    at(ri0, c);
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
    const double pitch = (ri0 + ro0) / 2.0;
    const double ball = kBallSpace * std::min(h, B) / 2.0;
    BRepPrimAPI_MakeTorus torus(gp_Ax2(gp_Pnt(0, 0, B / 2.0), gp_Dir(0, 0, 1)),
                                pitch, ball);
    torus.Build();
    if (!torus.IsDone())
      throw Standard_Failure("ball space");
    BRepAlgoAPI_Cut cut(revol.Shape(), torus.Shape());
    cut.Build();
    if (!cut.IsDone())
      throw Standard_Failure("ball space cut");
    solid = CADShape(cut.Shape());
  } catch (const Standard_Failure& failure) {
    SetError("could not build " + designation + " from its dimensions (" +
             std::string(failure.GetMessageString()) + ")");
    return false;
  }
  if (!solid.IsValid()) {
    SetError("could not build " + designation + " from its dimensions");
    return false;
  }

  // Name every face by what it IS, read back off the built geometry rather
  // than assumed from the order OCC made them in. The ones a mate, a fit or a
  // seat check needs get their role; the rest (chamfers, the land steps, the
  // ball space) keep a positional name like any primitive.
  const std::string prefix = std::to_string(GetId());
  constexpr double kTol = 1e-6;
  ElementMap map;
  int index = 0;
  for (TopExp_Explorer ex(solid.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), index++) {
    const TopoDS_Face f = TopoDS::Face(ex.Current());
    const BRepAdaptor_Surface surface(f);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    const gp_Pnt m = props.CentreOfMass();
    // Where a face lies radially, from its boundary circles: an annulus's
    // centre of mass is on the axis, whichever ring it belongs to.
    double rLo = std::numeric_limits<double>::max(), rHi = 0.0;
    for (TopExp_Explorer ee(f, TopAbs_EDGE); ee.More(); ee.Next()) {
      const BRepAdaptor_Curve curve(TopoDS::Edge(ee.Current()));
      if (curve.GetType() != GeomAbs_Circle)
        continue;
      rLo = std::min(rLo, curve.Circle().Radius());
      rHi = std::max(rHi, curve.Circle().Radius());
    }
    const double radial = 0.5 * (rLo + rHi);
    const char* role = nullptr;
    if (surface.GetType() == GeomAbs_Cylinder) {
      const double r = surface.Cylinder().Radius();
      if (std::abs(r - ri0) < kTol)
        role = kBore;
      else if (std::abs(r - ro0) < kTol)
        role = kOutside;
    } else if (surface.GetType() == GeomAbs_Plane) {
      const bool sideA = std::abs(m.Z()) < kTol;
      const bool sideB = std::abs(m.Z() - B) < kTol;
      if (sideA || sideB) {
        if (radial < ri)
          role = sideA ? kInnerSideA : kInnerSideB;
        else if (radial > ro)
          role = sideA ? kOuterSideA : kOuterSideB;
      } else if (std::abs(m.Z() - e) < kTol) {
        role = kShieldA;
      } else if (std::abs(m.Z() - (B - e)) < kTol) {
        role = kShieldB;
      }
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
