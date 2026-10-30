// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Placing an import (docs/interchange/README.md#placing-an-import): the
// Import's placement and scale through a real regeneration, and each mode
// of align_import checked by what it was for -- the floor on z = 0, the
// axes on the world's, the distance true -- after the placement it wrote is
// regenerated, never by comparing its own numbers.

#include "mesh_organic.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/ImportAlign.h"
#include "core/ImportPlacement.h"
#include "core/io/Serialization.h"
#include "core/mesh/Align.h"
#include "core/mesh/TriMesh.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A cuboid of `size` about the origin, wound outward.
mesh::TriMesh Cuboid(const glm::dvec3& size) {
  mesh::TriMesh m;
  for (int i = 0; i < 8; i++)
    m.positions.push_back({((i & 1) ? 0.5 : -0.5) * size.x,
                           ((i & 2) ? 0.5 : -0.5) * size.y,
                           ((i & 4) ? 0.5 : -0.5) * size.z});
  const int quads[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
  for (const auto& q : quads)
    for (const auto& tri :
         {std::array<uint32_t, 3>{uint32_t(q[0]), uint32_t(q[1]),
                                  uint32_t(q[2])},
          std::array<uint32_t, 3>{uint32_t(q[0]), uint32_t(q[2]),
                                  uint32_t(q[3])}}) {
      std::array<uint32_t, 3> t = tri;
      const glm::dvec3 a = m.positions[t[0]], b = m.positions[t[1]],
                       c = m.positions[t[2]];
      if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.0)
        std::swap(t[1], t[2]);
      m.triangles.push_back(t);
    }
  mesh::BuildAdjacency(m);
  return m;
}

glm::dmat4 Turn(double xDeg, double yDeg, double zDeg) {
  glm::dmat4 r(1.0);
  r = glm::rotate(r, zDeg * kPi / 180.0, glm::dvec3(0, 0, 1));
  r = glm::rotate(r, yDeg * kPi / 180.0, glm::dvec3(0, 1, 0));
  r = glm::rotate(r, xDeg * kPi / 180.0, glm::dvec3(1, 0, 0));
  return r;
}

std::unique_ptr<ImportFeature> Import(const fs::path& path,
                                      mesh::Representation rep) {
  auto imp = std::make_unique<ImportFeature>("scan", path.string());
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = rep;
  return imp;
}

ImportFeature& TheImport(Document& doc) {
  return *dynamic_cast<ImportFeature*>(doc.GetFeature(0));
}

// The bounds of every body the import made, as placed.
void Bounds(const Document& doc, glm::dvec3& lo, glm::dvec3& hi) {
  bool any = false;
  for (const Body& b : doc.GetBodies()) {
    glm::dvec3 l, h;
    REQUIRE(BodyBounds(b, l, h, /*tight=*/true));
    lo = any ? glm::min(lo, l) : l;
    hi = any ? glm::max(hi, h) : h;
    any = true;
  }
  REQUIRE(any);
}

// Aligns, writes the placement onto the import and regenerates: what the
// RPC's ModifyFeatureCommand does.
importalign::Result AlignAndPlace(Document& doc,
                                  const importalign::Request& rq) {
  ImportFeature& imp = TheImport(doc);
  importalign::Result res;
  std::string why;
  REQUIRE(importalign::Align(importalign::ImportBodies(doc, imp), imp.placement,
                             rq, res, why));
  CHECK(why.empty());
  imp.placement = res.placement;
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() >= 1);
  return res;
}

// The centroid of the triangles of `m` facing `normal`.
glm::dvec3 FaceCentre(const mesh::TriMesh& m, const glm::dvec3& normal) {
  glm::dvec3 sum(0.0);
  double area = 0.0;
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const glm::dvec3 n = m.AreaNormal(t);
    if (glm::dot(glm::normalize(n), glm::normalize(normal)) > 0.999) {
      sum +=
          m.Area(t) * (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
      area += m.Area(t);
    }
  }
  REQUIRE(area > 0.0);
  return sum / area;
}

} // namespace

TEST_CASE("An Import placement's angles read back as they were written",
          "[import][align]") {
  for (const glm::dvec3 deg :
       {glm::dvec3(0, 0, 0), glm::dvec3(30, -20, 115), glm::dvec3(-170, 89, -5),
        glm::dvec3(12.5, -45, 180)}) {
    importplace::Placement p;
    p.rotateDeg = deg;
    const glm::dvec3 back = importplace::EulerDeg(p.Rotation());
    importplace::Placement q;
    q.rotateDeg = back;
    // The same turn, whichever angles name it (180 and -180 are one).
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 3; r++)
        CHECK_THAT(q.Rotation()[c][r], WithinAbs(p.Rotation()[c][r], 1e-12));
  }
  // Composing an alignment onto a placement is the matrix product.
  importplace::Placement p;
  p.translateMm = {4, -7, 2.5};
  p.rotateDeg = {10, 20, 30};
  p.scale = 1.5;
  const glm::dmat3 turn =
      importplace::Placement{{0, 0, 0}, {-35, 5, 60}, 1.0}.Rotation();
  const glm::dvec3 move(1, 2, 3);
  const importplace::Placement both = p.Then(move, turn, 0.8);
  glm::dmat4 delta(turn * 0.8);
  delta[3] = glm::dvec4(move, 1.0);
  const glm::dmat4 want = delta * p.Matrix();
  // To the angles' rounding (1e-9 degree).
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      CHECK_THAT(both.Matrix()[c][r], WithinAbs(want[c][r], 1e-9));
}

TEST_CASE("An Import's placement scales, turns and moves what it read",
          "[import][align]") {
  const mesh::TriMesh box = Cuboid({40, 20, 10});
  const fs::path stl = fixtures::WriteStl(box, "import-placement-box");
  importplace::Placement p;
  p.translateMm = {5, -3, 12};
  p.rotateDeg = {0, 0, 90};
  p.scale = 2.0;

  SECTION("a mesh body") {
    Document doc;
    doc.AddFeature(Import(stl, mesh::Representation::Mesh));
    REQUIRE(doc.GetBodies().size() == 1);
    const std::vector<glm::dvec3> before = doc.GetBodies()[0].mesh->positions;
    TheImport(doc).placement = p;
    doc.Regenerate();
    REQUIRE(doc.GetBodies().size() == 1);
    const mesh::TriMesh& m = *doc.GetBodies()[0].mesh;
    REQUIRE(m.positions.size() == before.size());
    for (size_t i = 0; i < before.size(); i++) {
      const glm::dvec3 want = glm::dvec3(p.Matrix() * glm::dvec4(before[i], 1));
      CHECK(glm::length(m.positions[i] - want) < 1e-9);
    }
    CHECK(m.Closed());
    CHECK_THAT(m.Volume(), WithinRel(8.0 * 40 * 20 * 10, 1e-9));
    glm::dvec3 lo, hi;
    Bounds(doc, lo, hi);
    // Turned a quarter about Z: the 40 mm runs along Y, doubled.
    CHECK_THAT(hi.y - lo.y, WithinAbs(80.0, 1e-4));
    CHECK_THAT(hi.x - lo.x, WithinAbs(40.0, 1e-4));
    CHECK_THAT(0.5 * (lo.z + hi.z), WithinAbs(12.0, 1e-4));
  }
  SECTION("a B-Rep keeps its face names") {
    Document doc;
    doc.AddFeature(Import(stl, mesh::Representation::Facets));
    REQUIRE(doc.GetBodies().size() == 1);
    const ElementMap names = doc.GetBodies()[0].elementMap;
    const double volume = doc.GetBodies()[0].shape.Volume();
    TheImport(doc).placement = p;
    doc.Regenerate();
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK(doc.GetBodies()[0].elementMap.names == names.names);
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
               WithinRel(8.0 * volume, 1e-6));
    glm::dvec3 lo, hi;
    Bounds(doc, lo, hi);
    CHECK_THAT(hi.y - lo.y, WithinAbs(80.0, 1e-3));
    CHECK_THAT(0.5 * (lo.x + hi.x), WithinAbs(5.0, 1e-3));
  }
  SECTION("written and read back, and refused when it is not a placement") {
    ImportFeature f("scan", stl.string());
    f.placement = p;
    const nlohmann::json j = DocumentIO::FeatureToJson(f);
    CHECK(j.at("scale") == 2.0);
    CHECK(j.at("placement").at("rotate_deg") == nlohmann::json({0, 0, 90}));
    const auto back = DocumentIO::FeatureFromJson(j);
    const auto* imp = dynamic_cast<const ImportFeature*>(back.get());
    REQUIRE(imp != nullptr);
    CHECK(imp->placement == p);
    nlohmann::json bad = j;
    bad["scale"] = -1.0;
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                      ContainsSubstring("greater than 0"));
    bad = j;
    bad["placement"]["rotate"] = {0, 0, 1};
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                      ContainsSubstring("translate_mm and rotate_deg"));
    bad = j;
    bad["placement"]["translate_mm"] = {0, 1};
    CHECK_THROWS_WITH(DocumentIO::FeatureFromJson(bad),
                      ContainsSubstring("[x, y, z]"));
  }
  fs::remove(stl);
}

TEST_CASE("align_import floor lays the picked face on z = 0",
          "[import][align]") {
  const glm::dmat4 tilt = Turn(20, 0, 35);
  const mesh::TriMesh box = mesh::Transformed(Cuboid({40, 20, 10}), tilt);
  const fs::path stl = fixtures::WriteStl(box, "align-floor-box");
  for (const auto rep :
       {mesh::Representation::Mesh, mesh::Representation::Facets}) {
    DYNAMIC_SECTION(mesh::ToString(rep)) {
      Document doc;
      doc.AddFeature(Import(stl, rep));
      REQUIRE(doc.GetBodies().size() == 1);
      // The centre of what was the bottom, as the body stands now.
      double used = 0.0;
      (void)used;
      const glm::dvec3 down = glm::dvec3(tilt * glm::dvec4(0, 0, -1, 0));
      glm::dvec3 lo0, hi0;
      Bounds(doc, lo0, hi0);
      const glm::dvec3 centre = 0.5 * (lo0 + hi0);
      const glm::dvec3 pick = centre + 5.0 * down; // its centre, 5 mm down
      importalign::Request rq;
      rq.mode = importalign::Mode::Floor;
      rq.pointsMm = {pick};
      const importalign::Result res = AlignAndPlace(doc, rq);
      CHECK(res.report.at("fitted") == "region");
      CHECK(res.report.at("triangles") == 2);
      CHECK_THAT(res.report.at("area_mm2").get<double>(),
                 WithinRel(800.0, 1e-5));
      glm::dvec3 lo, hi;
      Bounds(doc, lo, hi);
      CHECK_THAT(lo.z, WithinAbs(0.0, 1e-4));  // on the floor
      CHECK_THAT(hi.z, WithinAbs(10.0, 1e-4)); // standing on its 40 x 20
      CHECK_THAT(0.5 * (lo.x + hi.x), WithinAbs(0.0, 1e-4));
      CHECK_THAT(0.5 * (lo.y + hi.y), WithinAbs(0.0, 1e-4));

      // Then scaled by a distance known along its 40 mm edge: about the
      // origin, so the floor stays on z = 0.
      const glm::dvec3
          a = glm::dvec3(
              res.placement.Matrix() *
              glm::dvec4(
                  glm::dvec3(tilt * glm::dvec4(-20, -10, -5, 1)) - centre, 1)),
          b = glm::dvec3(
              res.placement.Matrix() *
              glm::dvec4(glm::dvec3(tilt * glm::dvec4(20, -10, -5, 1)) - centre,
                         1));
      CHECK_THAT(glm::length(b - a), WithinAbs(40.0, 1e-4));
      importalign::Request sc;
      sc.mode = importalign::Mode::Scale;
      sc.pointsMm = {a, b};
      sc.distanceMm = 100.0;
      const importalign::Result scaled = AlignAndPlace(doc, sc);
      CHECK_THAT(scaled.report.at("factor").get<double>(),
                 WithinRel(2.5, 1e-5));
      CHECK_THAT(scaled.placement.scale, WithinRel(2.5, 1e-5));
      Bounds(doc, lo, hi);
      CHECK_THAT(lo.z, WithinAbs(0.0, 1e-3));
      CHECK_THAT(hi.z, WithinAbs(25.0, 1e-3));
    }
  }
  fs::remove(stl);
}

TEST_CASE("align_import three_points lays the plane through them on z = 0",
          "[import][align]") {
  const glm::dmat4 tilt = Turn(-15, 30, 10);
  const mesh::TriMesh box = mesh::Transformed(Cuboid({40, 20, 10}), tilt);
  const fs::path stl = fixtures::WriteStl(box, "align-three-box");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  glm::dvec3 lo0, hi0;
  Bounds(doc, lo0, hi0);
  const glm::dvec3 centre = 0.5 * (lo0 + hi0);
  // Three corners of the +X end, where the body stands now.
  const auto at = [&](double x, double y, double z) {
    return glm::dvec3(tilt * glm::dvec4(x, y, z, 1)) - centre;
  };
  importalign::Request rq;
  rq.mode = importalign::Mode::ThreePoints;
  rq.pointsMm = {at(20, -10, -5), at(20, 10, -5), at(20, 10, 5)};
  AlignAndPlace(doc, rq);
  glm::dvec3 lo, hi;
  Bounds(doc, lo, hi);
  CHECK_THAT(lo.z, WithinAbs(0.0, 1e-4));
  CHECK_THAT(hi.z, WithinAbs(40.0, 1e-4)); // on its end, the body above

  importalign::Result res;
  std::string why;
  rq.pointsMm = {at(0, 0, 0), at(10, 0, 0), at(20, 0, 0)};
  CHECK_FALSE(importalign::Align(importalign::ImportBodies(doc, TheImport(doc)),
                                 TheImport(doc).placement, rq, res, why));
  CHECK_THAT(why, ContainsSubstring("in a line"));
  fs::remove(stl);
}

TEST_CASE("align_import principal turns the widest spread onto X",
          "[import][align]") {
  const glm::dmat4 tilt = Turn(25, -40, 70);
  const mesh::TriMesh box = mesh::Transformed(Cuboid({40, 20, 10}), tilt);
  const fs::path stl = fixtures::WriteStl(box, "align-principal-box");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  importalign::Request rq;
  rq.mode = importalign::Mode::Principal;
  const importalign::Result res = AlignAndPlace(doc, rq);
  glm::dvec3 lo, hi;
  Bounds(doc, lo, hi);
  CHECK_THAT(hi.x - lo.x, WithinAbs(40.0, 1e-4));
  CHECK_THAT(hi.y - lo.y, WithinAbs(20.0, 1e-4));
  CHECK_THAT(hi.z - lo.z, WithinAbs(10.0, 1e-4));
  CHECK(glm::length(0.5 * (lo + hi)) < 1e-4);
  const auto spread = res.report.at("spread_mm").get<std::vector<double>>();
  CHECK(spread.at(0) > spread.at(1));
  CHECK(spread.at(1) > spread.at(2));
  CHECK_FALSE(res.report.contains("note"));
  fs::remove(stl);
}

TEST_CASE("align_import floor on an open scan fits the rim of its hole",
          "[import][align][mesh_body]") {
  // The figure with its base cut away, as a scan of an object on a table,
  // arriving tilted.
  const fixtures::Organic cut = fixtures::CutFigurine(10000);
  const glm::dmat4 tilt = Turn(25, -10, 40);
  const fs::path stl =
      fixtures::WriteStl(mesh::Transformed(cut.mesh, tilt), "align-open-scan");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  REQUIRE(doc.GetBodies().size() == 1);
  const mesh::TriMesh& m0 = *doc.GetBodies()[0].mesh;
  REQUIRE_FALSE(m0.Closed());
  // A triangle on the rim: one with an open edge.
  uint32_t rim = mesh::kNone;
  for (uint32_t t = 0; t < m0.triangles.size() && rim == mesh::kNone; t++)
    for (int e = 0; e < 3; e++)
      if (m0.neighbour[t][e] == mesh::kNone)
        rim = t;
  REQUIRE(rim != mesh::kNone);
  const glm::dvec3 pick =
      (m0.Corner(rim, 0) + m0.Corner(rim, 1) + m0.Corner(rim, 2)) / 3.0;
  importalign::Request rq;
  rq.mode = importalign::Mode::Floor;
  rq.pointsMm = {pick};
  const importalign::Result res = AlignAndPlace(doc, rq);
  CHECK(res.report.at("fitted") == "boundary_loop");
  const double maxOff = res.report.at("max_mm").get<double>();
  // The cut leaves the rim as ragged as an edge is long, no more.
  CHECK(maxOff < 3.0);

  const mesh::TriMesh& m = *doc.GetBodies()[0].mesh;
  // Every rim vertex on z = 0, to the rim's own raggedness.
  size_t onRim = 0;
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (int e = 0; e < 3; e++)
      if (m.neighbour[t][e] == mesh::kNone) {
        CHECK(std::abs(m.Corner(t, e).z) <= maxOff + 1e-6);
        onRim++;
      }
  CHECK(onRim > 0);
  // Standing up: the figure's height above the floor, nothing below it.
  glm::dvec3 lo, hi;
  Bounds(doc, lo, hi);
  CHECK(lo.z >= -maxOff - 1e-6);
  CHECK_THAT(hi.z, WithinAbs(cut.hi.z - 3.0, 4.0));
  fs::remove(stl);
}

TEST_CASE("align_import refuses what is not an alignment, naming it",
          "[import][align]") {
  CHECK_FALSE(importalign::ModeFromString("flor"));
  CHECK(importalign::AcceptedModes() ==
        "floor, three_points, principal, scale");
  importalign::Request rq;
  rq.mode = importalign::Mode::ThreePoints;
  rq.pointsMm = {{0, 0, 0}, {1, 0, 0}};
  CHECK_THROWS_WITH(importalign::CheckRequest(rq),
                    ContainsSubstring("three_points takes three"));
  rq.mode = importalign::Mode::Principal;
  CHECK_THROWS_WITH(importalign::CheckRequest(rq),
                    ContainsSubstring("principal takes no points"));
  rq.mode = importalign::Mode::Scale;
  CHECK_THROWS_WITH(importalign::CheckRequest(rq),
                    ContainsSubstring("distance_mm"));
  rq.distanceMm = 10.0;
  rq.pointsMm = {{1, 2, 3}, {1, 2, 3}};
  importalign::CheckRequest(rq); // well formed; the geometry refuses it

  const fs::path stl = fixtures::WriteStl(Cuboid({40, 20, 10}), "align-refuse");
  Document doc;
  doc.AddFeature(Import(stl, mesh::Representation::Mesh));
  const auto bodies = importalign::ImportBodies(doc, TheImport(doc));
  importalign::Result res;
  std::string why;
  CHECK_FALSE(importalign::Align(bodies, {}, rq, res, why));
  CHECK_THAT(why, ContainsSubstring("in one place"));
  rq.mode = importalign::Mode::Floor;
  rq.pointsMm = {{500, 0, 0}};
  CHECK_FALSE(importalign::Align(bodies, {}, rq, res, why));
  CHECK_THAT(why, ContainsSubstring("from the import's nearest surface"));
  fs::remove(stl);
}
