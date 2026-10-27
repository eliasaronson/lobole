// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a mesh import costs, counted before it is built, and the reader's
// progress and cancel (docs/interchange/README.md#what-a-mesh-costs).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/MeshImportCost.h"

#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Catch::Matchers::WithinRel;

namespace {

fs::path Scratch(const std::string& name) {
  return fs::temp_directory_path() /
         ("lobole_mesh_import_" + std::to_string(::getpid()) + "_" + name);
}

// A closed unit cube's 12 triangles as binary STL bytes, `extra` appended.
void WriteBinaryCube(const fs::path& path, uint32_t claimed = 12,
                     size_t extra = 0) {
  const float v[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                         {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  const int t[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  std::ofstream out(path, std::ios::binary);
  const char header[80] = {};
  out.write(header, 80);
  out.write(reinterpret_cast<const char*>(&claimed), 4);
  for (const auto& tri : t) {
    const float normal[3] = {0, 0, 0};
    out.write(reinterpret_cast<const char*>(normal), 12);
    for (int c : tri)
      out.write(reinterpret_cast<const char*>(v[c]), 12);
    const uint16_t attr = 0;
    out.write(reinterpret_cast<const char*>(&attr), 2);
  }
  for (size_t i = 0; i < extra; i++)
    out.put('\n');
}

// A cancel that comes after `allowed` reports: 0 refuses from the first.
struct Stopper : CADKernel::ImportProgress {
  int allowed = 0;
  int heard = 0;
  std::vector<Phase> phases;
  void OnPhase(Phase phase, double) override {
    heard++;
    if (phases.empty() || phases.back() != phase)
      phases.push_back(phase);
  }
  bool Cancelled() override { return heard >= allowed && allowed >= 0; }
};

} // namespace

TEST_CASE("A mesh's triangles are counted without building it",
          "[interchange][import]") {
  SECTION("a binary STL by its header, when its size agrees") {
    const fs::path p = Scratch("cube.stl");
    WriteBinaryCube(p);
    CHECK(interchange::CountMeshTriangles(p.string()) == 12u);
    fs::remove(p);
  }
  SECTION("a header that disagrees with the size is read as text") {
    // A count of four billion over twelve records: believed, it would refuse
    // a cube as a mesh of hours.
    const fs::path p = Scratch("liar.stl");
    WriteBinaryCube(p, 0xFFFFFFFFu);
    const auto n = interchange::CountMeshTriangles(p.string());
    REQUIRE(n.has_value());
    CHECK(*n < 12u);
    fs::remove(p);
  }
  SECTION("an ASCII STL by its facets, any case and indent") {
    const fs::path p = Scratch("ascii.STL");
    std::ofstream(p) << "solid t\n"
                        "  facet normal 0 0 1\n    outer loop\n"
                        "      vertex 0 0 0\n      vertex 1 0 0\n"
                        "      vertex 0 1 0\n    endloop\n  endfacet\n"
                        "FACET NORMAL 0 0 1\nouter loop\nvertex 0 0 0\n"
                        "vertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\n"
                        "endsolid t\n";
    CHECK(interchange::CountMeshTriangles(p.string()) == 2u);
    fs::remove(p);
  }
  SECTION("an OBJ's faces, a polygon of n corners n - 2 triangles") {
    const fs::path p = Scratch("poly.obj");
    std::ofstream(p) << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 2 2\n"
                        "f 1 2 3\n"
                        "f 1/1/1 2/2/2 3/3/3 4/4/4\n" // a quad: two
                        "\tf 1 2 3 4 5\n"             // a pentagon: three
                        "fo 1 2 3\n"                  // not a face record
                        "f 1 2\n";                    // not a polygon
    CHECK(interchange::CountMeshTriangles(p.string()) == 6u);
    fs::remove(p);
  }
  SECTION("anything else is not a mesh") {
    CHECK(!interchange::CountMeshTriangles("part.step").has_value());
    CHECK(!interchange::MeshImportCost("part.brep").has_value());
    CHECK(!interchange::CountMeshTriangles(Scratch("absent.stl").string())
               .has_value());
  }
}

TEST_CASE("The triangle limit is where the cost law reaches a patient wait",
          "[interchange][import]") {
  const uint64_t limit = interchange::MeshTriangleLimit();
  // Solved from the law, not written down: the law at the limit is the
  // patient time, and one step either side is either side of it.
  CHECK_THAT(interchange::ExpectedMeshImportSeconds(limit),
             WithinRel(interchange::kPatientImportSeconds, 1e-3));
  CHECK(interchange::ExpectedMeshImportSeconds(limit + 1000) >
        interchange::kPatientImportSeconds);
  // The law grows faster than the count (the sewing), and is monotone.
  CHECK(interchange::ExpectedMeshImportSeconds(20000) >
        2.0 * interchange::ExpectedMeshImportSeconds(10000));
  // The refusal names the count, the time and the way past it.
  interchange::MeshCost cost{
      limit * 4, interchange::ExpectedMeshImportSeconds(limit * 4), true};
  const std::string why =
      interchange::LargeMeshRefusal("scan.stl", cost, "`allow_large_mesh`");
  INFO(why);
  CHECK(why.find(interchange::DescribeTriangles(limit * 4)) !=
        std::string::npos);
  CHECK(why.find(interchange::DescribeSeconds(cost.expectedSeconds)) !=
        std::string::npos);
  CHECK(why.find("`allow_large_mesh`") != std::string::npos);
  CHECK(interchange::DescribeTriangles(1234567) == "1,234,567 triangles");
  CHECK(interchange::DescribeSeconds(150.0) == "about 2 min");
}

TEST_CASE("A mesh read reports its phases and stops on a cancel",
          "[interchange][import]") {
  CADKernel& k = CADKernel::Instance();
  const fs::path stl = Scratch("sphere.stl");
  REQUIRE(k.SaveSTL(stl.string(), k.CreateSphere({0, 0, 0}, 10.0), 0.05, 0.2));

  Stopper heard;
  heard.allowed = -1; // never
  CADShape whole;
  REQUIRE(k.LoadSTL(stl.string(), whole, &heard));
  CHECK(whole.SolidCount() == 1);
  // Sewn last, after the file (and, where the reader takes a range, the
  // faces).
  REQUIRE(!heard.phases.empty());
  CHECK(heard.phases.front() == CADKernel::ImportProgress::Phase::ReadingFile);
  CHECK(heard.phases.back() == CADKernel::ImportProgress::Phase::Sewing);

  Stopper stop; // cancelled from the first report
  CADShape none;
  CHECK_FALSE(k.LoadSTL(stl.string(), none, &stop));

  // An OBJ: the faces are made by us, and a cancel stops between them.
  const fs::path obj = Scratch("sphere.obj");
  {
    CADKernel::ImportedModel model;
    REQUIRE(k.LoadOBJStructured(obj.string(), model) == false); // no file yet
  }
  {
    // The sphere's triangles as an OBJ.
    std::ofstream out(obj);
    std::ifstream in(stl, std::ios::binary);
    char header[84];
    in.read(header, 84);
    uint32_t n = 0;
    std::memcpy(&n, header + 80, 4);
    for (uint32_t i = 0; i < n; i++) {
      char rec[50];
      in.read(rec, 50);
      float f[12];
      std::memcpy(f, rec, 48);
      for (int c = 0; c < 3; c++)
        out << "v " << f[3 + 3 * c] << " " << f[4 + 3 * c] << " "
            << f[5 + 3 * c] << "\n";
      out << "f " << 3 * i + 1 << " " << 3 * i + 2 << " " << 3 * i + 3 << "\n";
    }
  }
  CADKernel::ImportedModel model;
  Stopper objStop;
  objStop.allowed = 1; // past the read, into the faces or the sewing
  CHECK_FALSE(k.LoadOBJStructured(obj.string(), model, &objStop));
  CHECK(model.products.empty());
  fs::remove(stl);
  fs::remove(obj);
}

TEST_CASE("A seeded mesh import is the import of its file",
          "[interchange][import]") {
  // The import job reads the file on its worker (ImportFeature::ReadModel)
  // and measures the volume there; the feature then places what it was
  // handed. Seeded or read by Execute, the part must be the same.
  CADKernel& k = CADKernel::Instance();
  const fs::path stl = Scratch("box.stl");
  REQUIRE(k.SaveSTL(stl.string(), k.CreateBox(20.0, 30.0, 4.0), 0.1, 0.5));
  for (const units::Length unit :
       {units::Length::Millimetre, units::Length::Centimetre}) {
    Document read;
    auto plain = std::make_unique<ImportFeature>("box", stl.string());
    plain->lengthUnit = unit;
    read.AddFeature(std::move(plain));

    CADKernel::ImportedModel model;
    std::string why;
    REQUIRE(ImportFeature::ReadModel(stl.string(), unit, model, why));
    for (auto& product : model.products)
      product.volume = product.shape.Volume();
    Document seeded;
    auto fed = std::make_unique<ImportFeature>("box", stl.string());
    fed->lengthUnit = unit;
    fed->SeedGeometry(std::move(model));
    seeded.AddFeature(std::move(fed));

    REQUIRE(read.GetBodies().size() == 1);
    REQUIRE(seeded.GetBodies().size() == 1);
    const Body& a = read.GetBodies()[0];
    const Body& b = seeded.GetBodies()[0];
    CHECK_THAT(b.shape.Volume(), WithinRel(a.shape.Volume(), 1e-9));
    CHECK(a.elementMap.names == b.elementMap.names);
    CHECK(a.elementMap.edgeNames == b.elementMap.edgeNames);
    glm::dvec3 alo, ahi, blo, bhi;
    a.shape.GetBoundingBox(alo, ahi);
    b.shape.GetBoundingBox(blo, bhi);
    CHECK(glm::length(alo - blo) < 1e-9);
    CHECK(glm::length(ahi - bhi) < 1e-9);
  }
  // A refusal is the feature's, word for word.
  CADKernel::ImportedModel model;
  std::string why;
  CHECK_FALSE(
      ImportFeature::ReadModel("part.step", units::Length::Inch, model, why));
  CHECK(why.find("declares its own unit") != std::string::npos);
  fs::remove(stl);
}
