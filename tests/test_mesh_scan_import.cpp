// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// PLY and glTF through the import (docs/interchange/README.md#ply-and-gltf):
// each encoding read to the same body, polygons fanned, a uniform colour
// kept and a varied one dropped saying so, a glTF's node tree placed in mm
// and stood on our Z, every representation, and each refusal naming the
// file. The files are written by tests/scan_files.cpp, byte for byte.

#include "mesh_organic.h"
#include "scan_files.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/ImportFormats.h"
#include "core/io/MeshImportCost.h"
#include "core/mesh/MeshReaders.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

fs::path Write(const std::string& name, const std::string& bytes) {
  const fs::path p = fs::temp_directory_path() /
                     ("scan-import-" + std::to_string(::getpid()) + "-" + name);
  std::ofstream(p, std::ios::binary) << bytes;
  return p;
}

// An axis-aligned box from `lo` of `size`, outward, 12 triangles.
mesh::TriMesh Box(glm::dvec3 lo, glm::dvec3 size) {
  mesh::TriMesh m;
  for (int i = 0; i < 8; i++)
    m.positions.push_back(lo + size * glm::dvec3(i & 1, (i >> 1) & 1, i >> 2));
  m.triangles = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6},
                 {0, 1, 5}, {0, 5, 4}, {2, 6, 7}, {2, 7, 3},
                 {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
  return m;
}

// The same box's six sides as quads, as a scanning app writes polygons.
std::vector<std::vector<uint32_t>> BoxQuads() {
  return {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
          {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
}

std::unique_ptr<ImportFeature> Import(const fs::path& path,
                                      mesh::Representation rep) {
  auto imp = std::make_unique<ImportFeature>("scan", path.string());
  imp->keepOrigin = true;
  imp->representation = rep;
  return imp;
}

const ImportFeature& LastImport(const Document& doc) {
  return *dynamic_cast<const ImportFeature*>(
      doc.GetFeature(doc.FeatureCount() - 1));
}

} // namespace

TEST_CASE("A PLY reads the same mesh body in each encoding",
          "[mesh_body][import]") {
  const auto s = fixtures::NoisySphere(5000);
  std::vector<uint64_t> hashes;
  for (const auto enc :
       {scanfiles::PlyEncoding::Ascii, scanfiles::PlyEncoding::BinaryLittle,
        scanfiles::PlyEncoding::BinaryBig}) {
    scanfiles::PlyOptions o;
    o.encoding = enc;
    const fs::path ply = Write("sphere-" + std::to_string(int(enc)) + ".ply",
                               scanfiles::Ply(s.mesh, o));
    Document doc;
    doc.AddFeature(Import(ply, mesh::Representation::Mesh));
    INFO(doc.GetFeature(0)->GetError());
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& b = doc.GetBodies()[0];
    REQUIRE(b.IsMesh());
    CHECK(b.mesh->triangles.size() == s.mesh.triangles.size());
    CHECK(b.mesh->Closed());
    CHECK(std::abs(b.mesh->Volume() - s.volume) <= s.volumeTolerance);
    // A PLY has no unit and no up axis: as written, and said so.
    CHECK(LastImport(doc).UpApplied().applied == interchange::UpAxis::None);
    CHECK_THAT(LastImport(doc).UpApplied().because,
               ContainsSubstring("a PLY has no header"));
    hashes.push_back(b.mesh->Hash());
    CHECK(mesh::CountPlyFaces(ply.string()) == s.mesh.triangles.size());
    fs::remove(ply);
  }
  // The float32 values are the same values, whatever the byte order.
  CHECK(hashes[1] == hashes[2]);
}

TEST_CASE("A PLY's polygons are fanned and a uniform colour kept",
          "[mesh_body][import]") {
  const mesh::TriMesh box = Box({0, 0, 0}, {10, 20, 30});
  scanfiles::PlyOptions o;
  o.faces = BoxQuads();
  o.colours = {{255, 128, 0}};
  const fs::path ply = Write("quads.ply", scanfiles::Ply(box, o));
  Document doc;
  doc.AddFeature(Import(ply, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK(b.mesh->triangles.size() == 12);
  CHECK(b.mesh->Closed());
  CHECK_THAT(b.mesh->Volume(), WithinRel(6000.0, 1e-9));
  REQUIRE(b.hasDisplayColor);
  CHECK_THAT(b.displayColor.r, WithinAbs(1.0, 1e-6));
  CHECK_THAT(b.displayColor.g, WithinAbs(128.0 / 255.0, 1e-6));
  CHECK_THAT(b.displayColor.b, WithinAbs(0.0, 1e-6));
  CHECK(LastImport(doc).GetWarning().empty());
  fs::remove(ply);

  // A colour per vertex that varies is dropped, and the warning says so.
  o.colours.clear();
  for (size_t i = 0; i < box.positions.size(); i++)
    o.colours.push_back({uint8_t(30 * i), 0, 0});
  o.encoding = scanfiles::PlyEncoding::BinaryLittle;
  const fs::path varied = Write("varied.ply", scanfiles::Ply(box, o));
  Document doc2;
  doc2.AddFeature(Import(varied, mesh::Representation::Mesh));
  REQUIRE(doc2.GetBodies().size() == 1);
  CHECK_FALSE(doc2.GetBodies()[0].hasDisplayColor);
  CHECK_THAT(LastImport(doc2).GetWarning(),
             ContainsSubstring("vertex colours vary"));
  fs::remove(varied);
}

TEST_CASE("A PLY and a glTF become each representation",
          "[mesh_body][import]") {
  const mesh::TriMesh box = Box({0, 0, 0}, {10, 20, 30});
  mesh::TriMesh boxM = box; // the same box in metres
  for (glm::dvec3& p : boxM.positions)
    p *= 0.001;
  scanfiles::GltfOptions g;
  g.meshes = {boxM};
  const fs::path ply = Write("rep.ply", scanfiles::Ply(box));
  const fs::path glb = Write("rep.glb", scanfiles::Glb(g));
  for (const fs::path& file : {ply, glb}) {
    for (const auto rep :
         {mesh::Representation::Facets, mesh::Representation::Recognize,
          mesh::Representation::Mesh, mesh::Representation::Auto}) {
      INFO(file.string() << " as " << mesh::ToString(rep));
      Document doc;
      doc.AddFeature(Import(file, rep));
      INFO(doc.GetFeature(0)->GetError());
      REQUIRE(doc.GetBodies().size() == 1);
      const Body& b = doc.GetBodies()[0];
      // A box is mechanical: auto recognises it.
      const bool brep = rep != mesh::Representation::Mesh;
      CHECK(b.IsMesh() == !brep);
      glm::dvec3 lo, hi;
      REQUIRE(BodyBounds(b, lo, hi, true));
      // The glTF's Y up stood on our Z: its 20 along glTF Y is our Z.
      const glm::dvec3 size = hi - lo;
      const glm::dvec3 want =
          file == glb ? glm::dvec3(10, 30, 20) : glm::dvec3(10, 20, 30);
      CHECK_THAT(size.x, WithinAbs(want.x, 1e-4));
      CHECK_THAT(size.y, WithinAbs(want.y, 1e-4));
      CHECK_THAT(size.z, WithinAbs(want.z, 1e-4));
      if (brep) {
        CHECK_THAT(b.shape.Volume(), WithinRel(6000.0, 1e-6));
        // Facets: a face per triangle; recognised: the six sides.
        const size_t faces = b.elementMap.names.size();
        CHECK(faces == (rep == mesh::Representation::Facets ? 12u : 6u));
      } else {
        CHECK_THAT(b.mesh->Volume(), WithinRel(6000.0, 1e-6));
      }
      CHECK(LastImport(doc).RepresentationChosen() ==
            (rep == mesh::Representation::Auto ? "recognize"
                                               : mesh::ToString(rep)));
    }
  }
  fs::remove(ply);
  fs::remove(glb);
}

TEST_CASE("A glTF's nodes are placed, its metres read as mm",
          "[mesh_body][import]") {
  // A 10 mm cube in metres, placed twice: moved 50 mm along glTF X, and
  // mirrored through X -- whose winding must come back outward.
  mesh::TriMesh cube = Box({0, 0, 0}, {0.01, 0.01, 0.01});
  scanfiles::GltfOptions g;
  g.meshes = {cube};
  g.generator = "SomeScanner 1.0";
  scanfiles::GltfNode root, moved, mirrored;
  root.mesh = 0;
  root.children = {1, 2};
  moved.mesh = 0;
  moved.matrix = glm::translate(glm::dmat4(1.0), {0.05, 0.0, 0.0});
  mirrored.mesh = 0;
  mirrored.matrix = glm::scale(glm::dmat4(1.0), {-1.0, 1.0, 1.0}) *
                    glm::translate(glm::dmat4(1.0), {0.05, 0.0, 0.0});
  g.nodes = {root, moved, mirrored};
  const fs::path glb = Write("nodes.glb", scanfiles::Glb(g));
  CHECK(mesh::CountGltfTriangles(glb.string()) == 36);

  Document doc;
  doc.AddFeature(Import(glb, mesh::Representation::Mesh));
  INFO(doc.GetFeature(0)->GetError());
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& b = doc.GetBodies()[0];
  REQUIRE(b.IsMesh());
  CHECK(b.mesh->triangles.size() == 36);
  CHECK(b.mesh->Closed()); // three closed cubes, each outward
  CHECK_THAT(b.mesh->Volume(), WithinRel(3000.0, 1e-6));
  glm::dvec3 lo, hi;
  b.mesh->Bounds(lo, hi);
  CHECK_THAT(lo.x, WithinAbs(-60.0, 1e-4));
  CHECK_THAT(hi.x, WithinAbs(60.0, 1e-4));
  CHECK_THAT(hi.z - lo.z, WithinAbs(10.0, 1e-4));
  // Y up by the format, whoever wrote it; the writer is still named.
  const interchange::UpDecision& up = LastImport(doc).UpApplied();
  CHECK(up.applied == interchange::UpAxis::PosY);
  CHECK_THAT(up.because, ContainsSubstring("glTF is Y up"));
  CHECK(up.system == "SomeScanner 1.0");
  // No unit to ask: refused, naming the metres.
  auto withUnit = Import(glb, mesh::Representation::Mesh);
  withUnit->lengthUnit = units::Length::Inch;
  Document doc2;
  doc2.AddFeature(std::move(withUnit));
  CHECK(doc2.GetBodies().empty());
  CHECK_THAT(doc2.GetFeature(0)->GetError(),
             ContainsSubstring("metres by its specification"));
  fs::remove(glb);

  // A .gltf whose buffer is a file beside it.
  const auto [json, bin] = scanfiles::GltfWithBin(g, "nodes-buffer.bin");
  const fs::path gltf = Write("nodes.gltf", json);
  const fs::path binPath = gltf.parent_path() / "nodes-buffer.bin";
  std::ofstream(binPath, std::ios::binary) << bin;
  mesh::TriMesh m;
  std::string err;
  REQUIRE(mesh::ReadGltf(gltf.string(), m, err));
  CHECK(m.triangles.size() == 36);
  fs::remove(binPath);
  CHECK_FALSE(mesh::ReadGltf(gltf.string(), m, err));
  CHECK_THAT(err, ContainsSubstring("nodes-buffer.bin"));
  CHECK_THAT(err, ContainsSubstring(gltf.string()));
  fs::remove(gltf);
}

TEST_CASE("A .gltf whose .bin alone changes reads the new geometry",
          "[mesh_body][import]") {
  // The JSON stays byte for byte; only the buffer beside it is rewritten,
  // a 10 mm cube becoming a 20 mm one. The read cache and the chain hash
  // both key on the import's content, which must cover the .bin.
  scanfiles::GltfOptions g;
  g.meshes = {Box({0, 0, 0}, {0.01, 0.01, 0.01})};
  const auto [json, bin] = scanfiles::GltfWithBin(g, "bin-only-buffer.bin");
  const fs::path gltf = Write("bin-only.gltf", json);
  const fs::path binPath = gltf.parent_path() / "bin-only-buffer.bin";
  std::ofstream(binPath, std::ios::binary) << bin;
  CHECK(mesh::GltfExternalFiles(gltf.string()) ==
        std::vector<std::string>{binPath.string()});

  Document doc;
  doc.AddFeature(Import(gltf, mesh::Representation::Mesh));
  INFO(doc.GetFeature(0)->GetError());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].mesh->Volume(), WithinRel(1000.0, 1e-6));
  const size_t before = LastImport(doc).ContentHash();

  g.meshes = {Box({0, 0, 0}, {0.02, 0.02, 0.02})};
  const auto [json2, bin2] = scanfiles::GltfWithBin(g, "bin-only-buffer.bin");
  REQUIRE(bin2.size() == bin.size());
  std::ofstream(binPath, std::ios::binary | std::ios::trunc) << bin2;
  // The same size, so the stamp tells them apart by mtime alone; moved on
  // past the filesystem clock's tick, which two writes inside one can share.
  fs::last_write_time(binPath,
                      fs::last_write_time(binPath) + std::chrono::seconds(2));
  CHECK(LastImport(doc).ContentHash() != before);
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].mesh->Volume(), WithinRel(8000.0, 1e-6));

  // A buffer that goes missing is a change too, and the read names it.
  fs::remove(binPath);
  CHECK(LastImport(doc).ContentHash() != before);
  doc.Regenerate();
  CHECK(doc.GetBodies().empty());
  CHECK_THAT(doc.GetFeature(0)->GetError(),
             ContainsSubstring("bin-only-buffer.bin"));
  fs::remove(gltf);
}

TEST_CASE("A damaged or unread PLY or glTF is refused naming the file",
          "[mesh_body][import]") {
  const mesh::TriMesh box = Box({0, 0, 0}, {1, 1, 1});
  mesh::TriMesh m;
  std::string err;
  mesh::ReadExtras extras;

  SECTION("a point cloud") {
    scanfiles::PlyOptions o;
    o.withFaces = false;
    const fs::path p = Write("cloud.ply", scanfiles::Ply(box, o));
    CHECK_FALSE(mesh::ReadPly(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring(p.string()));
    CHECK_THAT(err, ContainsSubstring("point cloud"));
    fs::remove(p);
  }
  SECTION("a face naming a vertex not there") {
    scanfiles::PlyOptions o;
    o.faces = {{0, 1, 2}, {0, 2, 9}};
    const fs::path p = Write("badface.ply", scanfiles::Ply(box, o));
    CHECK_FALSE(mesh::ReadPly(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring("names vertex 9 of 8"));
    fs::remove(p);
  }
  SECTION("a header promising more than the file holds") {
    std::string bytes =
        scanfiles::Ply(box, {scanfiles::PlyEncoding::BinaryLittle});
    const size_t at = bytes.find("element vertex 8");
    bytes.replace(at, 16, "element vertex 2000000000");
    const fs::path p = Write("promise.ply", bytes);
    CHECK_FALSE(mesh::ReadPly(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring("promises"));
    CHECK(mesh::CountPlyFaces(p.string()) == std::nullopt);
    fs::remove(p);
  }
  SECTION("a binary PLY that ends early") {
    std::string bytes =
        scanfiles::Ply(box, {scanfiles::PlyEncoding::BinaryLittle});
    bytes.resize(bytes.size() - 7);
    const fs::path p = Write("short.ply", bytes);
    CHECK_FALSE(mesh::ReadPly(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring(p.string()));
    fs::remove(p);
  }
  SECTION("a glTF compressed with Draco") {
    scanfiles::GltfOptions g;
    g.meshes = {box};
    g.extensionsRequired = {"KHR_draco_mesh_compression"};
    const fs::path p = Write("draco.glb", scanfiles::Glb(g));
    CHECK_FALSE(mesh::ReadGltf(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring("KHR_draco_mesh_compression"));
    CHECK_THAT(err, ContainsSubstring("uncompressed"));
    fs::remove(p);
  }
  SECTION("a glTF of points only") {
    scanfiles::GltfOptions g;
    g.meshes = {box};
    g.mode = 0;
    const fs::path p = Write("points.glb", scanfiles::Glb(g));
    CHECK_FALSE(mesh::ReadGltf(p.string(), m, err, {}, &extras));
    CHECK_THAT(err, ContainsSubstring("only points or lines"));
    fs::remove(p);
  }
  SECTION("a skinned glTF, whose pose its joints set") {
    scanfiles::GltfOptions g;
    g.meshes = {box};
    auto [text, bin] = scanfiles::GltfWithBin(g, "skinned.bin");
    nlohmann::json j = nlohmann::json::parse(text);
    j["skins"] = {{{"joints", {0}}}};
    j["nodes"][0]["skin"] = 0;
    const fs::path p = Write("skinned.gltf", j.dump());
    const fs::path b = p.parent_path() / "skinned.bin";
    std::ofstream(b, std::ios::binary) << bin;
    CHECK_FALSE(mesh::ReadGltf(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring("skinned"));
    fs::remove(p);
    fs::remove(b);
  }
  SECTION("a truncated GLB") {
    scanfiles::GltfOptions g;
    g.meshes = {box};
    std::string bytes = scanfiles::Glb(g);
    bytes.resize(bytes.size() / 2);
    const fs::path p = Write("half.glb", bytes);
    CHECK_FALSE(mesh::ReadGltf(p.string(), m, err));
    CHECK_THAT(err, ContainsSubstring(p.string()));
    fs::remove(p);
  }
  SECTION("an extension the import does not read, naming every one it does") {
    const fs::path p = Write("model.3mf", "not read");
    Document doc;
    doc.AddFeature(Import(p, mesh::Representation::Facets));
    CHECK_THAT(doc.GetFeature(0)->GetError(),
               ContainsSubstring(interchange::kImportExtensions));
    CHECK_THAT(doc.GetFeature(0)->GetError(),
               ContainsSubstring(".ply, .glb/.gltf"));
    fs::remove(p);
  }
}

TEST_CASE("A PLY or glTF read stops on a cancel", "[mesh_body][import]") {
  const auto s = fixtures::NoisySphere(200000);
  mesh::TriMesh sm = s.mesh;
  const fs::path ply = Write(
      "cancel.ply", scanfiles::Ply(sm, {scanfiles::PlyEncoding::BinaryLittle}));
  for (glm::dvec3& p : sm.positions)
    p *= 0.001;
  scanfiles::GltfOptions g;
  g.meshes = {sm};
  const fs::path glb = Write("cancel.glb", scanfiles::Glb(g));
  for (const fs::path& file : {ply, glb}) {
    mesh::TriMesh m;
    std::string err;
    int polls = 0;
    const auto cancelled = [&] { return ++polls >= 2; };
    const bool read = file == ply
                          ? mesh::ReadPly(file.string(), m, err, cancelled)
                          : mesh::ReadGltf(file.string(), m, err, cancelled);
    CHECK_FALSE(read);
    CHECK(err == "cancelled");
  }
  fs::remove(ply);
  fs::remove(glb);
}
