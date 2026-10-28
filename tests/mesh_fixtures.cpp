// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mesh_fixtures.h"

#include "core/mesh/MeshReaders.h"

#include <catch2/catch_test_macros.hpp>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <unistd.h>

#include <cmath>

namespace fixtures {

namespace {

constexpr double kPi = 3.14159265358979323846;

TopoDS_Shape Cut(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  BRepAlgoAPI_Cut cut(a, b);
  REQUIRE(cut.IsDone());
  return cut.Shape();
}

TopoDS_Shape Fuse(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  BRepAlgoAPI_Fuse fuse(a, b);
  REQUIRE(fuse.IsDone());
  return fuse.Shape();
}

// A vertical cylinder from z0 to z1 about (x, y).
TopoDS_Shape Rod(double x, double y, double r, double z0, double z1) {
  return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z0), gp::DZ()), r,
                                  z1 - z0)
      .Shape();
}

// A vertical regular prism of `sides`, corners on radius `r`.
TopoDS_Shape Prism(double x, double y, int sides, double r, double z0,
                   double z1) {
  BRepBuilderAPI_MakePolygon poly;
  for (int k = 0; k < sides; k++) {
    const double a = 2.0 * kPi * k / sides;
    poly.Add(gp_Pnt(x + r * std::cos(a), y + r * std::sin(a), z0));
  }
  poly.Close();
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
  return BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, z1 - z0)).Shape();
}

double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  return props.Mass();
}

Part Finish(std::string name, const TopoDS_Shape& s, Part part) {
  part.name = std::move(name);
  part.shape = CADShape(s);
  part.volume = VolumeOf(s);
  return part;
}

} // namespace

Part Plate() {
  Part part;
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(-60, -40, 0), 120, 80, 6).Shape();
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 4; j++) {
      const double x = -50 + 20 * i, y = -30 + 20 * j, r = (5 + 0.5 * i) / 2;
      s = Cut(s, Rod(x, y, r, -1, 7));
      part.cylinders.push_back({{x, y, 0}, {0, 0, 1}, r, 0});
    }
  return Finish("plate", s, part);
}

Part Bracket() {
  Part part;
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(-40, -20, 0), 80, 40, 8).Shape();
  // Counterbored: a 3.3 bore, a 5.5 counterbore 4 deep.
  s = Cut(s, Rod(-30, -10, 3.3, -1, 9));
  s = Cut(s, Rod(-30, -10, 5.5, 4, 9));
  part.cylinders.push_back({{-30, -10, 0}, {0, 0, 1}, 3.3, 0});
  part.cylinders.push_back({{-30, -10, 0}, {0, 0, 1}, 5.5, 0});
  // Countersunk: a 3.3 bore under a 90° cone from radius 6 at the top.
  s = Cut(s, Rod(-30, 10, 3.3, -1, 9));
  const double apexZ = 8 - 6; // radius 6 at z=8, 45° half angle
  s = Cut(s, BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(-30, 10, apexZ), gp::DZ()), 0.0,
                                  7.0, 7.0)
                 .Shape());
  part.cylinders.push_back({{-30, 10, 0}, {0, 0, 1}, 3.3, 0});
  part.cones.push_back({{-30, 10, apexZ}, {0, 0, 1}, 0, kPi / 4});
  // A boss on top with a bore through it and the block.
  s = Fuse(s, Rod(0, 0, 8, 8, 18));
  s = Cut(s, Rod(0, 0, 4, -1, 19));
  part.cylinders.push_back({{0, 0, 0}, {0, 0, 1}, 8, 0});
  part.cylinders.push_back({{0, 0, 0}, {0, 0, 1}, 4, 0});
  // Pockets 4 deep from the top: a nut trap and two more polygons.
  const double hexR = 10.0 / std::sqrt(3.0); // 10 across flats
  s = Cut(s, Prism(22, -10, 6, hexR, 4, 9));
  s = Cut(s, Prism(22, 10, 8, 5.0, 4, 9));
  s = Cut(s, Prism(33, 0, 10, 4.0, 4, 9));
  part.polygons.push_back({6, hexR, {22, -10, 0}});
  part.polygons.push_back({8, 5.0, {22, 10, 0}});
  part.polygons.push_back({10, 4.0, {33, 0, 0}});
  return Finish("bracket", s, part);
}

Part FilletedAngle() {
  Part part;
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 60, 40, 8).Shape();
  s = Fuse(s, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 8, 40, 50).Shape());
  // The inside corner: the edge along Y at x=8, z=8.
  {
    BRepFilletAPI_MakeFillet fillet(s);
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
      const gp_Pnt a = c.Value(c.FirstParameter()),
                   b = c.Value(c.LastParameter());
      if (std::abs(a.X() - 8) < 1e-6 && std::abs(b.X() - 8) < 1e-6 &&
          std::abs(a.Z() - 8) < 1e-6 && std::abs(b.Z() - 8) < 1e-6)
        fillet.Add(6.0, TopoDS::Edge(ex.Current()));
    }
    fillet.Build();
    REQUIRE(fillet.IsDone());
    s = fillet.Shape();
  }
  part.cylinders.push_back({{14, 0, 14}, {0, 1, 0}, 6.0, 0});
  // The foot's far upright edges (x=60), rounded.
  {
    BRepFilletAPI_MakeFillet fillet(s);
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
      const gp_Pnt a = c.Value(c.FirstParameter()),
                   b = c.Value(c.LastParameter());
      if (std::abs(a.X() - 60) < 1e-6 && std::abs(b.X() - 60) < 1e-6 &&
          std::abs(a.Z() - b.Z()) > 1.0)
        fillet.Add(4.0, TopoDS::Edge(ex.Current()));
    }
    fillet.Build();
    REQUIRE(fillet.IsDone());
    s = fillet.Shape();
  }
  part.cylinders.push_back({{56, 4, 0}, {0, 0, 1}, 4.0, 0});
  part.cylinders.push_back({{56, 36, 0}, {0, 0, 1}, 4.0, 0});
  return Finish("filleted_angle", s, part);
}

Part ObliqueAndDome() {
  Part part;
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(-30, -20, 0), 60, 40, 12).Shape();
  const double tilt = kPi / 6;
  const gp_Dir dir(std::sin(tilt), 0, std::cos(tilt));
  const gp_Pnt base(-12 - 20 * std::sin(tilt), 0, -20 * std::cos(tilt));
  s = Cut(s, BRepPrimAPI_MakeCylinder(gp_Ax2(base, dir), 3.0, 60).Shape());
  part.cylinders.push_back(
      {{-12, 0, 0}, {std::sin(tilt), 0, std::cos(tilt)}, 3.0, 0});
  s = Fuse(s, BRepPrimAPI_MakeSphere(gp_Ax2(gp_Pnt(14, 0, 12), gp::DZ()), 8.0,
                                     0.0, kPi / 2)
                  .Shape());
  part.spheres.push_back({{14, 0, 12}, {0, 0, 1}, 8.0, 0});
  return Finish("oblique_and_dome", s, part);
}

std::filesystem::path WriteStl(const Part& part, double deflection,
                               double angularRad, const std::string& tag) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("lobole_mesh_fixture_" + std::to_string(::getpid()) + "_" +
                     part.name + "_" + tag + ".stl");
  BRepBuilderAPI_Copy copy(part.shape.GetShape(), true, false);
  const CADShape fresh(copy.Shape());
  REQUIRE(CADKernel::Instance().SaveSTL(path.string(), fresh, deflection,
                                        angularRad));
  return path;
}

mesh::TriMesh MeshOf(const Part& part, double deflection, double angularRad) {
  const auto path = WriteStl(part, deflection, angularRad, "m");
  mesh::TriMesh m;
  std::string error;
  REQUIRE(mesh::ReadStl(path.string(), m, error));
  std::filesystem::remove(path);
  return m;
}

} // namespace fixtures
