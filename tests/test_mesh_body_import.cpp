// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A mesh body through the real pipeline
// (docs/interchange/README.md#mesh-bodies): `representation: mesh` and `auto`
// on the organic and mechanical fixtures, what it measures, what it bakes as,
// what it exports, and every B-Rep feature refusing it by name.

#include "mesh_fixtures.h"
#include "mesh_organic.h"

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/MeshMeasure.h"
#include "core/features/BooleanFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/ShellFeature.h"
#include "core/io/MeshImportCost.h"
#include "core/mesh/MeshReaders.h"
#include "ui/StlExport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::unique_ptr<ImportFeature> Import(const fs::path& path,
                                      mesh::Representation rep) {
  auto imp = std::make_unique<ImportFeature>("scan", path.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = rep;
  return imp;
}

const ImportFeature& LastImport(const Document& doc) {
  return *dynamic_cast<const ImportFeature*>(
      doc.GetFeature(doc.FeatureCount() - 1));
}

} // namespace

TEST_CASE("A mesh import is a mesh body with its own triangles",
          "[mesh_body]") {
  const auto s = fixtures::NoisySphere(20000);
  const auto stl = fixtures::WriteStl(s.mesh, "mesh-body-sphere");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK_FALSE(b.shape.IsValid());
  CHECK(b.mesh->triangles.size() == s.mesh.triangles.size());
  CHECK(b.mesh->Closed());
  // One element, the mesh, and no faces or edges besides.
  REQUIRE(b.elementMap.names.size() == 1);
  CHECK(b.elementMap.names.at(0) ==
        std::to_string(doc.GetFeature(0)->GetId()) + "/b1/mesh");
  CHECK(b.elementMap.edgeNames.empty());
  CHECK(LastImport(doc).RepresentationChosen() == "mesh");

  // mass_properties' function: the analytic sphere.
  std::string err;
  const auto mass = Measure::MassOfBodies(doc.GetBodies(), &err);
  REQUIRE(mass);
  CHECK(std::abs(mass->volume - s.volume) <= s.volumeTolerance);
  CHECK(std::abs(mass->bodies[0].surfaceArea - s.area) <= s.areaTolerance);
  CHECK(glm::length(mass->centerOfMass) < 0.01 * s.radius);

  // A section is a circle of the right radius: area and regions.
  const double z = 10.0, r = std::sqrt(s.radius * s.radius - z * z);
  const auto area =
      Measure::CrossSectionArea(doc.GetBodies(), {0, 0, z}, {0, 0, 1});
  REQUIRE(area);
  CHECK_THAT(*area, WithinRel(kPi * r * r, 0.01));
  const auto regions =
      Measure::CrossSectionRegions(doc.GetBodies(), {0, 0, z}, {0, 0, 1}, true);
  REQUIRE(regions);
  REQUIRE(regions->size() == 1);
  CHECK_THAT(regions->front().centroid.z, WithinAbs(z, 1e-9));
  CHECK(regions->front().loops.size() == 1);
  // ...and a cap drawn of it, in the plane.
  const auto caps = Measure::SectionCaps(doc.GetBodies(), {0, 0, z}, {0, 0, 1});
  REQUIRE(caps.size() == 1);
  CHECK(caps[0].IsValid());

  // A point measure snaps onto the surface.
  std::string why;
  const auto snap =
      meshmeasure::SnapPoint(doc.GetBodies(), 0, {100.0, 0.0, 0.0}, why);
  REQUIRE(snap);
  CHECK(std::abs(glm::length(snap->point) - s.radius) <= s.radiusTolerance);
  CHECK_THAT(snap->moved, WithinAbs(100.0 - s.radius, s.radiusTolerance));
  fs::remove(stl);
}

TEST_CASE("An open scan is kept, reported, and its volume refused",
          "[mesh_body]") {
  const auto cut = fixtures::CutFigurine(10000);
  const auto stl = fixtures::WriteStl(cut.mesh, "mesh-body-open");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1); // no longer skipped
  const Body& b = doc.GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK_FALSE(b.mesh->Closed());
  CHECK_THAT(LastImport(doc).GetWarning(),
             ContainsSubstring("open: 1 boundary loop"));
  std::string err;
  CHECK_FALSE(Measure::MassOfBodies(doc.GetBodies(), &err));
  CHECK_THAT(err, ContainsSubstring("1 boundary loop"));
  CHECK_THAT(err, ContainsSubstring("MeshLab"));
  const meshmeasure::Stats st = meshmeasure::Describe(*b.mesh);
  CHECK_FALSE(st.volume);
  CHECK(st.loops.size() == 1);
  // An open mesh gets no section cap.
  const auto caps =
      Measure::SectionCaps(doc.GetBodies(), {0, 0, 50}, {0, 0, 1});
  REQUIRE(caps.size() == 1);
  CHECK_FALSE(caps[0].IsValid());
  fs::remove(stl);
}

TEST_CASE("auto recognises a mechanical mesh and keeps an organic one",
          "[mesh_body]") {
  SECTION("a mechanical part below the limit: a solid with its holes") {
    const fixtures::Part plate = fixtures::Plate();
    const auto stl =
        fixtures::WriteStl(plate, 0.0214, 2 * kPi / 24, "auto-plate");
    Document doc;
    doc.AddFeature(Import(stl, mesh::Representation::Auto));
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& b = doc.GetBodies()[0];
    CHECK_FALSE(b.IsMesh());
    CHECK(b.shape.IsValid());
    const ImportFeature& imp = LastImport(doc);
    CHECK(imp.RepresentationChosen() == "recognize");
    CHECK_THAT(imp.RepresentationWhy(), ContainsSubstring("mechanical"));
    REQUIRE(imp.RecognizedBodies().size() == 1);
    CHECK(imp.RecognizedBodies()[0].recognition.Count(mesh::Kind::Cylinder) ==
          plate.cylinders.size());
    fs::remove(stl);
  }
  SECTION("an organic one below the limit: a mesh body, no rebuild") {
    const auto fig = fixtures::Figurine(10000);
    const auto stl = fixtures::WriteStl(fig.mesh, "auto-figure");
    Document doc;
    doc.AddFeature(Import(stl, mesh::Representation::Auto));
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].IsMesh());
    const ImportFeature& imp = LastImport(doc);
    CHECK(imp.RepresentationChosen() == "mesh");
    CHECK_THAT(imp.RepresentationWhy(), ContainsSubstring(": organic"));
    fs::remove(stl);
  }
  SECTION("at or above the limit: a mesh body, recognition never runs") {
    const uint64_t limit = interchange::RecognitionTriangleLimit();
    // The limit is solved from the stage-0 law, not written down: about
    // 47,000.
    CHECK(limit > 40000);
    CHECK(limit < 55000);
    const auto s = fixtures::NoisySphere(size_t(limit * 1.1));
    REQUIRE(s.mesh.triangles.size() >= limit);
    const auto stl = fixtures::WriteStl(s.mesh, "auto-large");
    Document doc;
    doc.AddFeature(Import(stl, mesh::Representation::Auto));
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].IsMesh());
    CHECK_THAT(LastImport(doc).RepresentationWhy(),
               ContainsSubstring("past the recognition limit of " +
                                 interchange::DescribeTriangles(limit)));
    fs::remove(stl);
  }
  SECTION("facets, recognize and mesh force the choice") {
    const auto fig = fixtures::Figurine(2000);
    const auto stl = fixtures::WriteStl(fig.mesh, "force-figure");
    for (const auto rep :
         {mesh::Representation::Facets, mesh::Representation::Mesh}) {
      Document doc;
      doc.AddFeature(Import(stl, rep));
      REQUIRE(doc.GetBodies().size() == 1);
      CHECK(doc.GetBodies()[0].IsMesh() == (rep == mesh::Representation::Mesh));
      CHECK(LastImport(doc).RepresentationChosen() == mesh::ToString(rep));
    }
    fs::remove(stl);
  }
}

TEST_CASE("Unknown representations are refused naming the four",
          "[mesh_body]") {
  CHECK_FALSE(mesh::RepresentationFromString("meshes"));
  CHECK(std::string(mesh::AcceptedRepresentations()) ==
        "facets, recognize, mesh, auto");
  for (const char* s : {"facets", "recognize", "mesh", "auto"})
    CHECK(mesh::ToString(*mesh::RepresentationFromString(s)) == std::string(s));
}

TEST_CASE("A mesh past what a mesh body holds is refused with the target",
          "[mesh_body]") {
  // A binary STL whose size agrees with a header count past the limit:
  // sparse, so the test writes no 280 MB. Refused from the count alone.
  const uint64_t n = interchange::MeshBodyTriangleLimit() + 1;
  const fs::path stl = fs::temp_directory_path() / "mesh-body-too-large.stl";
  {
    std::ofstream out(stl, std::ios::binary | std::ios::trunc);
    char header[80] = {};
    out.write(header, 80);
    const uint32_t count = uint32_t(n);
    out.write(reinterpret_cast<const char*>(&count), 4);
  }
  fs::resize_file(stl, 84 + 50 * n);
  for (const auto rep :
       {mesh::Representation::Mesh, mesh::Representation::Auto}) {
    ImportFeature::ReadOptions o;
    o.representation = rep;
    CADKernel::ImportedModel model;
    std::string err;
    CHECK_FALSE(ImportFeature::ReadModel(stl.string(), o, model, err));
    CHECK_THAT(err, ContainsSubstring("MeshLab"));
    CHECK_THAT(err, ContainsSubstring("Blender"));
    CHECK_THAT(err, ContainsSubstring(interchange::DescribeTriangles(
                        interchange::MeshBodyTriangleLimit())));
  }
  fs::remove(stl);
}

TEST_CASE("An OBJ is read straight into a mesh body", "[mesh_body]") {
  const fs::path obj = fs::temp_directory_path() / "mesh-body-quad.obj";
  {
    std::ofstream out(obj);
    // A unit cube of quads, written as an exporter would: 1-based, some
    // with texture and normal indices.
    out << "o cube\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
           "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
           "vn 0 0 1\n"
           "f 1 4 3 2\nf 5/1/1 6/1/1 7/1/1 8/1/1\nf 1 2 6 5\n"
           "f 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n";
  }
  Document doc;
  doc.AddFeature(Import(obj, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK(b.mesh->triangles.size() == 12);
  CHECK(b.mesh->Closed());
  CHECK_THAT(b.mesh->Volume(), WithinRel(1.0, 1e-12));
  // A face naming a vertex not there is refused, naming the line.
  mesh::TriMesh m;
  std::string err;
  CHECK_FALSE(mesh::ReadObjBytes("v 0 0 0\nf 1 2 3\n", "bad.obj", m, err));
  CHECK_THAT(err, ContainsSubstring("line 2"));
  fs::remove(obj);
}

TEST_CASE("A mesh body bakes as its triangles and reopens without a re-read",
          "[mesh_body]") {
  const auto s = fixtures::NoisySphere(5000);
  const auto stl = fixtures::WriteStl(s.mesh, "mesh-body-bake");
  const fs::path proj = fs::temp_directory_path() / "mesh-body-bake.lob";
  fs::remove_all(proj);
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(testio::Save(doc, proj.string()));
  CHECK(fs::exists(proj / "geometry" / "0.mesh"));
  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // the file was not read
  REQUIRE(loaded->GetBodies().size() == 1);
  const Body& b = loaded->GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK(b.mesh->Hash() == doc.GetBodies()[0].mesh->Hash());
  CHECK(b.mesh->Closed());
  CHECK(b.elementMap.names == doc.GetBodies()[0].elementMap.names);
  fs::remove_all(proj);
  fs::remove(stl);
}

TEST_CASE("A mesh body exports as its own triangles", "[mesh_body]") {
  const auto s = fixtures::NoisySphere(5000);
  const auto stl = fixtures::WriteStl(s.mesh, "mesh-body-export-in");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  const fs::path out = fs::temp_directory_path() / "mesh-body-export-out.stl";
  const auto r = ui::ExportPartStl(doc, out.string(), -1, 0.5);
  REQUIRE(r);
  CHECK(r->meshBodies == 1);
  // Not re-meshed: every triangle, whatever the deflection asked.
  CHECK(fs::file_size(out) == 84 + 50 * s.mesh.triangles.size());
  fs::remove(out);
  fs::remove(stl);
}

TEST_CASE("Every B-Rep feature refuses a mesh body by name", "[mesh_body]") {
  const auto s = fixtures::NoisySphere(5000);
  const auto stl = fixtures::WriteStl(s.mesh, "mesh-body-refusals");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  const uint64_t scan = doc.GetFeature(0)->GetId();
  const std::string element = doc.GetBodies()[0].elementMap.names.at(0);
  const auto refusedNaming = [&](std::unique_ptr<Feature> f) {
    Feature* raw = f.get();
    doc.AddFeature(std::move(f));
    INFO(raw->GetName() << ": " << raw->GetError());
    CHECK_THAT(raw->GetError(), ContainsSubstring("is a mesh"));
    CHECK_THAT(raw->GetError(), ContainsSubstring("'scan"));
    // The mesh body is still there and still a mesh.
    REQUIRE(!doc.GetBodies().empty());
    CHECK(doc.GetBodies()[0].IsMesh());
    doc.RemoveFeature(doc.FeatureCount() - 1);
  };
  {
    refusedNaming(std::make_unique<FilletFeature>(
        "Round", std::vector<ElementRef>{ElementRef{scan, element}}, 1.0));
  }
  {
    refusedNaming(std::make_unique<ShellFeature>(
        "Hollow", scan, std::vector<ElementRef>{ElementRef{scan, element}},
        1.0));
  }
  {
    doc.AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0), 10, 10, 10));
    const uint64_t box = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    // A Boolean is not refused: with a mesh operand it runs on triangles
    // (docs/modeling/README.md#mesh-booleans). The block lies inside the
    // sphere, so their union is the sphere, a mesh body.
    doc.AddFeature(
        std::make_unique<BooleanFeature>("Union", box, scan, BooleanOp::Union));
    const Feature* boo = doc.GetFeature(doc.FeatureCount() - 1);
    INFO(boo->GetError());
    CHECK(boo->GetError().empty());
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].IsMesh());
    CHECK(std::abs(doc.GetBodies()[0].mesh->Volume() - s.volume) <=
          s.volumeTolerance);
  }
  fs::remove(stl);
}

// What `auto` reads off a segmentation, printed for the fixtures: how many
// triangles a region holds, and how much of the surface lies in regions of
// at least a thousandth of it. HIDDEN: a measurement, not a check
// (docs/interchange/README.md#mesh-bodies quotes it).
#include "core/mesh/Segment.h"
TEST_CASE("Mesh body: what auto reads off the fixtures",
          "[.][mesh-auto-measure]") {
  const auto report = [](const std::string& name, const mesh::TriMesh& m) {
    mesh::SegmentOptions so;
    const mesh::Segmentation seg = mesh::SegmentAll(m, so);
    double total = 0.0, large = 0.0;
    for (const auto& r : seg.regions)
      total += r.area;
    for (const auto& r : seg.regions)
      if (r.area >= 1e-3 * total)
        large += r.area;
    std::printf("%-12s %8zu tris %7zu regions %8.1f a region, %5.1f%% in "
                "regions >= 0.1%%\n",
                name.c_str(), m.triangles.size(), seg.regions.size(),
                double(m.triangles.size()) / seg.regions.size(),
                100.0 * large / total);
  };
  for (size_t n : {2000, 10000, 40000}) {
    report("sphere", fixtures::NoisySphere(n).mesh);
    report("torus", fixtures::Torus(n).mesh);
    report("figure", fixtures::Figurine(n).mesh);
    report("cut figure", fixtures::CutFigurine(n).mesh);
  }
  for (const auto& part :
       {fixtures::Plate(), fixtures::Bracket(), fixtures::FilletedAngle(),
        fixtures::ObliqueAndDome(), fixtures::RoundedPlate()})
    for (double defl : {0.5, 0.05})
      report(part.name, fixtures::MeshOf(part, defl, 0.5));
  // A box of twelve triangles, as a slicer's calibration cube arrives.
  std::vector<glm::dvec3> soup;
  const glm::dvec3 c[8] = {{0, 0, 0}, {2, 0, 0}, {2, 3, 0}, {0, 3, 0},
                           {0, 0, 4}, {2, 0, 4}, {2, 3, 4}, {0, 3, 4}};
  const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  for (const auto& t : f)
    for (int k : t)
      soup.push_back(c[k]);
  report("box", mesh::FromSoup(soup, 1e-6));
}

// Cost checks (docs/ui/testing.md#tests-that-time-things): CPU time, a bound
// per piece set against the slowest core (an E-core, `taskset`), each shown
// to fail against a spin planted in what it times. The 2M figure is the
// largest a scan comes in at (docs/interchange/README.md#mesh-bodies).
TEST_CASE("A 2M-triangle scan imports, weighs and sections in seconds",
          "[mesh_body][slow]") {
  const auto fig = fixtures::Figurine(2000000);
  const auto stl = fixtures::WriteStl(fig.mesh, "mesh-body-2m");
  const auto cpu = [](std::clock_t t0) {
    return double(std::clock() - t0) / CLOCKS_PER_SEC;
  };
  Document doc;
  std::clock_t t0 = std::clock();
  doc.AddFeature(Import(stl, mesh::Representation::Auto));
  const double importS = cpu(t0);
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(doc.GetBodies()[0].IsMesh());
  t0 = std::clock();
  std::string err;
  const auto mass = Measure::MassOfBodies(doc.GetBodies(), &err);
  const double massS = cpu(t0);
  REQUIRE(mass);
  CHECK(std::abs(mass->volume - fig.volume) <= fig.volumeTolerance);
  glm::dvec3 lo, hi;
  doc.GetBodies()[0].mesh->Bounds(lo, hi);
  t0 = std::clock();
  const auto caps =
      Measure::SectionCaps(doc.GetBodies(), (lo + hi) / 2.0, {0, 0, 1});
  const double capS = cpu(t0);
  REQUIRE(caps.size() == 1);
  CHECK(caps[0].IsValid());
  std::printf("2M figure: import %.2f s, mass %.3f s, cap %.3f s CPU\n",
              importS, massS, capS);
  // Measured on this machine's low-power E-core: 10.5 s, 0.32 s and
  // 0.075 s (2.7, 0.065 and 0.017 on a P-core).
  CHECK(importS < 30.0);
  CHECK(massS < 2.0);
  CHECK(capS < 1.0);
  fs::remove(stl);
}

TEST_CASE("auto recognises the largest mechanical fixture below the limit "
          "in seconds",
          "[mesh_body][slow]") {
  // The plate at a chord fine enough to come near the recognition limit:
  // `auto` segments it, finds it mechanical and rebuilds it.
  const fixtures::Part plate = fixtures::Plate();
  const double chord = 2.5 * (1.0 - std::cos(kPi / 224));
  const auto stl = fixtures::WriteStl(plate, chord, 2 * kPi / 224, "auto-big");
  const auto cost = interchange::MeshImportCost(stl.string());
  REQUIRE(cost);
  REQUIRE(cost->triangles < interchange::RecognitionTriangleLimit());
  const std::clock_t t0 = std::clock();
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Auto));
  const double s = double(std::clock() - t0) / CLOCKS_PER_SEC;
  std::printf("plate, %llu triangles: auto %.2f s CPU\n",
              (unsigned long long)cost->triangles, s);
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_FALSE(doc.GetBodies()[0].IsMesh());
  CHECK(LastImport(doc).RepresentationChosen() == "recognize");
  // 0.44 s on the low-power E-core (0.11 on a P-core).
  CHECK(s < 5.0);
  fs::remove(stl);
}

// An ASCII STL of whole-number corners 1 mm apart is a cube, not the
// tetrahedron welding at the full printed digit made of it.
TEST_CASE("An ASCII STL of whole-millimetre corners keeps every corner",
          "[mesh_body]") {
  const int c[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                       {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  std::string text = "solid cube\n";
  for (const auto& t : f) {
    text += "facet normal 0 0 0\nouter loop\n";
    for (int k : t)
      text += "vertex " + std::to_string(c[k][0]) + " " +
              std::to_string(c[k][1]) + " " + std::to_string(c[k][2]) + "\n";
    text += "endloop\nendfacet\n";
  }
  text += "endsolid cube\n";
  mesh::TriMesh m;
  std::string err;
  REQUIRE(mesh::ReadStlBytes(text, "cube.stl", m, err));
  CHECK(m.positions.size() == 8);
  CHECK(m.triangles.size() == 12);
  CHECK(m.Closed());
  CHECK_THAT(m.Volume(), WithinRel(1.0, 1e-12));
}

// A whole-number file printed its values with the trailing zeros stripped,
// so its last digit is not its precision: a 2 x 3 x 4 box printed so is
// recognised to the box it is, not with tolerances of four 1 mm quanta that
// made its box 10 x 11 x 12.
TEST_CASE("An ASCII STL of whole-number corners is recognised at its own "
          "size",
          "[mesh_body]") {
  const int c[8][3] = {{0, 0, 0}, {2, 0, 0}, {2, 3, 0}, {0, 3, 0},
                       {0, 0, 4}, {2, 0, 4}, {2, 3, 4}, {0, 3, 4}};
  const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  std::string text = "solid box\n";
  for (const auto& t : f) {
    text += "facet normal 0 0 0\nouter loop\n";
    for (int k : t)
      text += "vertex " + std::to_string(c[k][0]) + " " +
              std::to_string(c[k][1]) + " " + std::to_string(c[k][2]) + "\n";
    text += "endloop\nendfacet\n";
  }
  text += "endsolid box\n";
  mesh::TriMesh m;
  std::string err;
  REQUIRE(mesh::ReadStlBytes(text, "box.stl", m, err));
  CHECK(m.quantum < 1e-5);

  const fs::path stl = fs::temp_directory_path() / "whole-number-box.stl";
  {
    std::ofstream out(stl, std::ios::binary);
    out << text;
  }
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Recognize));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  REQUIRE_FALSE(b.IsMesh());
  CHECK(LastImport(doc).RepresentationChosen() == "recognize");
  glm::dvec3 lo, hi;
  b.shape.GetBoundingBox(lo, hi); // the conservative box, tolerances and all
  CHECK_THAT(lo.x, WithinAbs(0.0, 1e-3));
  CHECK_THAT(lo.y, WithinAbs(0.0, 1e-3));
  CHECK_THAT(lo.z, WithinAbs(0.0, 1e-3));
  CHECK_THAT(hi.x, WithinAbs(2.0, 1e-3));
  CHECK_THAT(hi.y, WithinAbs(3.0, 1e-3));
  CHECK_THAT(hi.z, WithinAbs(4.0, 1e-3));
  fs::remove(stl);
}
