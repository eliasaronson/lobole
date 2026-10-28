// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// cad::RebuildFromRegions: the solid a segmented mesh becomes, checked
// against the CAD part it was exported from
// (docs/interchange/README.md#rebuilding-the-solid).

#include "TestPaths.h"
#include "mesh_fixtures.h"

#include "cad/MeshRebuild.h"
#include "core/mesh/MeshReaders.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Counts {
  int faces = 0, planes = 0, cylinders = 0, cones = 0, spheres = 0, other = 0;
};

Counts Count(const CADShape& s) {
  Counts c;
  for (TopExp_Explorer ex(s.GetShape(), TopAbs_FACE); ex.More(); ex.Next()) {
    c.faces++;
    switch (BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType()) {
    case GeomAbs_Plane:
      c.planes++;
      break;
    case GeomAbs_Cylinder:
      c.cylinders++;
      break;
    case GeomAbs_Cone:
      c.cones++;
      break;
    case GeomAbs_Sphere:
      c.spheres++;
      break;
    default:
      c.other++;
    }
  }
  return c;
}

double Volume(const CADShape& s) {
  GProp_GProps p;
  BRepGProp::VolumeProperties(s.GetShape(), p);
  return p.Mass();
}

cad::Rebuilt Rebuild(const mesh::TriMesh& m, mesh::Segmentation& seg) {
  seg = mesh::SegmentAll(m, {});
  cad::Rebuilt r = cad::RebuildFromRegions(m, seg);
  INFO(r.failed);
  REQUIRE(r.failed.empty());
  REQUIRE(BRepCheck_Analyzer(r.shape.GetShape()).IsValid());
  return r;
}

} // namespace

TEST_CASE("Rebuild: the plate is its 30 faces, 24 of them cylinders",
          "[mesh_recognition]") {
  const fixtures::Part plate = fixtures::Plate();
  const int sides = GENERATE(24, 128);
  const double chord = 2.5 * (1.0 - std::cos(kPi / sides)) * 1.0001;
  const mesh::TriMesh m = fixtures::MeshOf(plate, chord, 2 * kPi / sides);
  mesh::Segmentation seg;
  const cad::Rebuilt r = Rebuild(m, seg);
  const Counts c = Count(r.shape);
  CHECK(c.faces == 30);
  CHECK(c.cylinders == 24);
  CHECK(c.planes == 6);
  // The true holes, not the polygons: the CAD volume to float precision.
  CHECK_THAT(Volume(r.shape), Catch::Matchers::WithinRel(plate.volume, 1e-6));
  // Every face in region order: face i is region i's surface.
  int i = 0;
  for (TopExp_Explorer ex(r.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    const GeomAbs_SurfaceType t =
        BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType();
    CHECK((t == GeomAbs_Cylinder) ==
          (seg.regions[i].fit.kind == mesh::Kind::Cylinder));
  }
}

TEST_CASE("Rebuild: the bracket's cone, bores and flats",
          "[mesh_recognition]") {
  const fixtures::Part bracket = fixtures::Bracket();
  const mesh::TriMesh m = fixtures::MeshOf(bracket, 0.01, 0.2);
  mesh::Segmentation seg;
  const cad::Rebuilt r = Rebuild(m, seg);
  const Counts c = Count(r.shape);
  CHECK(c.cylinders == static_cast<int>(bracket.cylinders.size()));
  CHECK(c.cones == 1);
  CHECK(c.other == 0);
  CHECK_THAT(Volume(r.shape), Catch::Matchers::WithinRel(bracket.volume, 1e-6));
}

TEST_CASE("Rebuild: a fillet tangent to its planes, an oblique hole, a dome",
          "[mesh_recognition]") {
  for (const fixtures::Part& part :
       {fixtures::FilletedAngle(), fixtures::ObliqueAndDome()}) {
    INFO(part.name);
    const mesh::TriMesh m = fixtures::MeshOf(part, 0.01, 0.2);
    mesh::Segmentation seg;
    const cad::Rebuilt r = Rebuild(m, seg);
    const Counts c = Count(r.shape);
    CHECK(c.cylinders == static_cast<int>(part.cylinders.size()));
    CHECK(c.spheres == static_cast<int>(part.spheres.size()));
    CHECK_THAT(Volume(r.shape), Catch::Matchers::WithinRel(part.volume, 1e-6));
  }
}

TEST_CASE("Rebuild: OpenSCAD's three files", "[mesh_recognition]") {
  for (const char* name : {"nema17_mount.stl", "pipe_clamp.stl", "knob.stl"}) {
    INFO(name);
    mesh::TriMesh m;
    std::string error;
    REQUIRE(mesh::ReadStl(
        (std::filesystem::path(TestSourceDir()) / "tests/fixtures/mesh" / name)
            .string(),
        m, error));
    mesh::Segmentation seg;
    const cad::Rebuilt r = Rebuild(m, seg);
    const Counts c = Count(r.shape);
    CHECK(c.faces == static_cast<int>(seg.regions.size()));
    CHECK(c.cylinders == static_cast<int>(seg.Count(mesh::Kind::Cylinder)));
    // Within what the facets' stand-off allows of the mesh's own volume.
    double allowed = m.SurfaceArea() * seg.tolerance;
    for (const mesh::Region& reg : seg.regions)
      allowed += reg.area * reg.sagitta;
    CHECK(std::abs(Volume(r.shape) - m.Volume()) <= allowed);
  }
}

TEST_CASE("Rebuild: an open mesh is refused, naming why",
          "[mesh_recognition]") {
  const fixtures::Part plate = fixtures::Plate();
  mesh::TriMesh m = fixtures::MeshOf(plate, 0.05, 0.5);
  m.triangles.pop_back();
  mesh::BuildAdjacency(m);
  const mesh::Segmentation seg = mesh::SegmentAll(m, {});
  const cad::Rebuilt r = cad::RebuildFromRegions(m, seg);
  CHECK(r.failed.find("not a closed solid") != std::string::npos);
  CHECK(r.failed.find("3 edges on one triangle") != std::string::npos);
}
