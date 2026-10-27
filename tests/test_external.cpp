// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;
using sketch::ExternalKind;

static const SketchPlane kXY(PlaneType::XY);

// A 4x4 rectangle extruded up 3 -> a box body (createdBy == extrude id) whose
// top cap ("<ext>/cap1") is parallel to XY.
static uint64_t BuildBox(Document& doc) {
  auto sk = std::make_unique<SketchFeature>("Sketch", kXY);
  sk->AddRectangle({0, 0}, {4, 4});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 3.0,
                                                  ExtrudeMode::NewBody));
  return doc.GetFeature(1)->GetId();
}

TEST_CASE("Projecting a face yields locked construction geometry",
          "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                           ExternalKind::Face);
  doc.AddFeature(std::move(sk2)); // regenerates -> projects the cap boundary

  auto* s2 = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  REQUIRE(s2 != nullptr);
  REQUIRE(s2->external.size() == 1);
  REQUIRE(s2->external[0].genIds.size() == 4); // 4 boundary edges of the cap

  // Every generated entity is a construction Line, and together they span the
  // 4x4 rectangle (projected onto XY).
  float minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
  for (uint32_t id : s2->external[0].genIds) {
    const size_t idx = s2->IndexOfEntity(id);
    REQUIRE(idx != SIZE_MAX);
    CHECK(sketch::IsConstruction(s2->entities[idx]));
    auto* l = std::get_if<sketch::Line>(&s2->entities[idx]);
    REQUIRE(l != nullptr);
    for (glm::vec2 p : {l->a, l->b}) {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minY = std::min(minY, p.y);
      maxY = std::max(maxY, p.y);
    }
  }
  CHECK_THAT(minX, WithinAbs(0.0f, 1e-4));
  CHECK_THAT(maxX, WithinAbs(4.0f, 1e-4));
  CHECK_THAT(minY, WithinAbs(0.0f, 1e-4));
  CHECK_THAT(maxY, WithinAbs(4.0f, 1e-4));
}

TEST_CASE("Projected geometry keeps stable ids across regeneration",
          "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                           ExternalKind::Face);
  doc.AddFeature(std::move(sk2));
  auto* s2 = dynamic_cast<SketchFeature*>(doc.GetFeature(2));
  const std::vector<uint32_t> ids = s2->external[0].genIds;

  doc.Regenerate();
  // Same ids reused (so a user's constraints to them survive), no duplication.
  CHECK(s2->external[0].genIds == ids);
  CHECK(s2->entities.size() == 4); // no runaway growth
}

TEST_CASE("A missing external reference fails regeneration", "[external]") {
  Document doc;
  const uint64_t ext = BuildBox(doc);
  auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
  sk2->AddExternalGeometry(ElementRef{ext, "nope"}, ExternalKind::Face);
  doc.AddFeature(std::move(sk2));
  CHECK_FALSE(doc.Regenerate());
}

TEST_CASE("External geometry round-trips through JSON", "[external]") {
  const fs::path path =
      fs::temp_directory_path() / "designer_external_test.json";
  uint64_t ext = 0;
  {
    Document doc;
    ext = BuildBox(doc);
    auto sk2 = std::make_unique<SketchFeature>("Ref", kXY);
    sk2->AddExternalGeometry(ElementRef{ext, std::to_string(ext) + "/cap1"},
                             ExternalKind::Face);
    doc.AddFeature(std::move(sk2));
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* s2 = dynamic_cast<SketchFeature*>(loaded->GetFeature(2));
  REQUIRE(s2 != nullptr);
  REQUIRE(s2->external.size() == 1);
  CHECK(s2->external[0].kind == ExternalKind::Face);
  CHECK(s2->external[0].ref.name == std::to_string(ext) + "/cap1");
  CHECK(s2->external[0].genIds.size() == 4); // re-projected on load
  fs::remove(path);
}

// --- Projections of an imported part
// (docs/sketching/README.md#projecting-an-imported-part). An Import names its
// elements by their place in the file; a changed file renumbers them.

namespace {

constexpr double kPi = 3.14159265358979323846;

// A 60 x 40 x 10 plate with four 2 mm bores on a 12 mm circle round its
// middle, the first at `turnDeg` + 45; `extra` cuts a 3 mm bore near a corner
// FIRST, which renumbers the faces and edges the four already had.
CADShape Plate(double turnDeg, bool extra) {
  auto& k = CADKernel::Instance();
  CADShape plate = k.CreateBox(glm::dvec3(0), glm::dvec3(60, 40, 10));
  if (extra)
    plate = plate.Difference(
        k.CreateCylinder(glm::dvec3(6, 6, -1), glm::dvec3(0, 0, 1), 3.0, 12.0));
  for (int i = 0; i < 4; i++) {
    const double a = (turnDeg + 45.0 + 90.0 * i) * kPi / 180.0;
    plate = plate.Difference(k.CreateCylinder(
        glm::dvec3(30 + 12 * std::cos(a), 20 + 12 * std::sin(a), -1),
        glm::dvec3(0, 0, 1), 2.0, 12.0));
  }
  return plate;
}

std::string WritePlate(const CADShape& shape) {
  const fs::path dir = fs::temp_directory_path() /
                       ("lobole-external-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const std::string path = (dir / "plate.brep").string();
  REQUIRE(CADKernel::Instance().SaveBREP(path, shape));
  return path;
}

const Body* BodyOf(const Document& doc, uint64_t createdBy) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      return &b;
  return nullptr;
}

// The names of the four 2 mm bores' top edges (z = 10).
std::vector<std::string> BoreTopEdges(const Body& body) {
  std::vector<std::string> out;
  for (const auto& [index, name] : body.elementMap.edgeNames) {
    const auto e = Measure::DescribeEdgeAt(body, index);
    if (e && e->kind == "circle" && std::abs(e->radius - 2.0) < 1e-6 &&
        std::abs(e->midpoint.z - 10.0) < 1e-6)
      out.push_back(name);
  }
  return out;
}

// Where the edge `name` of `body` is: its centre of mass.
glm::dvec3 CentreOf(const Body& body, const std::string& name) {
  const auto idx = body.elementMap.ResolveEdge(name);
  REQUIRE(idx.has_value());
  const auto print = sketch::PrintAt(body, false, *idx, name);
  REQUIRE(print.has_value());
  return print->centroid;
}

struct Projected {
  Document doc;
  std::string path;
  uint64_t imp = 0;
  SketchFeature* sketch = nullptr;
  std::vector<glm::dvec3> centres; // each projection's bore, as picked
};

// The plate imported where the file put it, and a sketch projecting its
// four bores' top edges.
void Project(Projected& p) {
  p.path = WritePlate(Plate(0.0, false));
  auto imp = std::make_unique<ImportFeature>("Plate", p.path);
  imp->keepOrigin = true;
  p.doc.AddFeature(std::move(imp));
  p.imp = p.doc.GetFeature(0)->GetId();
  REQUIRE(p.doc.GetFeature(0)->GetError().empty());
  const Body& body = *BodyOf(p.doc, p.imp);
  const std::vector<std::string> bores = BoreTopEdges(body);
  REQUIRE(bores.size() == 4);
  auto sk = std::make_unique<SketchFeature>("Holes", kXY);
  for (const std::string& name : bores) {
    sk->AddExternalGeometry(ElementRef{p.imp, name}, ExternalKind::Edge);
    p.centres.push_back(CentreOf(body, name));
  }
  p.doc.AddFeature(std::move(sk));
  p.sketch = dynamic_cast<SketchFeature*>(p.doc.GetFeature(1));
  REQUIRE(p.sketch != nullptr);
  INFO(p.sketch->GetError());
  REQUIRE(p.sketch->GetError().empty());
  for (const auto& ext : p.sketch->external) {
    REQUIRE(ext.print.has_value()); // taken the first time it is projected
    CHECK(ext.print->element == ext.ref.name);
    CHECK(ext.print->shape == "circle");
  }
}

} // namespace

TEST_CASE("A projected imported edge follows its geometry when the file "
          "renumbers",
          "[external][import]") {
  Projected p;
  Project(p);
  std::vector<std::string> before;
  for (const auto& ext : p.sketch->external)
    before.push_back(ext.ref.name);

  WritePlate(Plate(0.0, true)); // the same bores, numbered differently
  p.doc.Regenerate();
  INFO(p.sketch->GetError());
  REQUIRE(p.sketch->GetError().empty());
  const Body& body = *BodyOf(p.doc, p.imp);
  // The file did renumber: a name the sketch held now means another edge.
  bool renumbered = false;
  for (size_t i = 0; i < before.size(); i++) {
    const auto idx = body.elementMap.ResolveEdge(before[i]);
    if (!idx || glm::length(CentreOf(body, before[i]) - p.centres[i]) > 1e-6)
      renumbered = true;
  }
  REQUIRE(renumbered);
  for (size_t i = 0; i < p.sketch->external.size(); i++) {
    const auto& ext = p.sketch->external[i];
    INFO(ext.ref.name);
    CHECK(glm::length(CentreOf(body, ext.ref.name) - p.centres[i]) < 1e-6);
    CHECK(ext.print->element == ext.ref.name);
    // The projected circle is that bore's.
    REQUIRE(ext.genIds.size() == 1);
    const auto* c = std::get_if<sketch::Circle>(
        &p.sketch->entities[p.sketch->IndexOfEntity(ext.genIds[0])]);
    REQUIRE(c != nullptr);
    CHECK(std::abs(c->center.x - p.centres[i].x) < 1e-4);
    CHECK(std::abs(c->center.y - p.centres[i].y) < 1e-4);
  }
}

TEST_CASE("A projected imported edge the file no longer holds fails naming "
          "it, never the edge under its index",
          "[external][import]") {
  Projected p;
  Project(p);
  std::vector<std::string> names;
  for (const auto& ext : p.sketch->external)
    names.push_back(ext.ref.name);

  WritePlate(Plate(20.0, false)); // the bores turned 20 degrees
  p.doc.Regenerate();
  const std::string error = p.sketch->GetError();
  INFO(error);
  CHECK(error.find("no longer in the imported file") != std::string::npos);
  CHECK(error.find("re-pick them") != std::string::npos);
  for (const std::string& name : names) // every one named, not the first
    CHECK(error.find("`" + name + "`") != std::string::npos);
  // The references still name what was picked; none took another edge.
  for (size_t i = 0; i < p.sketch->external.size(); i++)
    CHECK(p.sketch->external[i].ref.name == names[i]);
}

TEST_CASE("A projection's fingerprint round-trips, and one taken for another "
          "element is dropped",
          "[external][import]") {
  Projected p;
  Project(p);
  const fs::path file =
      fs::temp_directory_path() /
      ("designer_external_print_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::Save(p.doc, file.string()));
  {
    const auto loaded = testio::Load(file.string());
    REQUIRE(loaded != nullptr);
    auto* sk = dynamic_cast<SketchFeature*>(loaded->GetFeature(1));
    REQUIRE(sk != nullptr);
    REQUIRE(sk->external.size() == 4);
    for (size_t i = 0; i < 4; i++) {
      const auto& a = *p.sketch->external[i].print;
      REQUIRE(sk->external[i].print.has_value());
      const auto& b = *sk->external[i].print;
      CHECK(a.element == b.element);
      CHECK(a.shape == b.shape);
      CHECK(a.size == b.size);
      CHECK(a.centroid == b.centroid);
    }
  }
  fs::remove(file);

  // A client that re-points a reference and sends the old print back: the
  // print is for another element, so the new one is printed afresh rather
  // than the old one being looked for.
  auto& ext = p.sketch->external[0];
  ext.print->element = "some/other;U9";
  ext.print->centroid += glm::dvec3(100.0);
  p.doc.Regenerate();
  INFO(p.sketch->GetError());
  CHECK(p.sketch->GetError().empty());
  REQUIRE(ext.print.has_value());
  CHECK(ext.print->element == ext.ref.name);
  CHECK(glm::length(ext.print->centroid - p.centres[0]) < 1e-6);
}
