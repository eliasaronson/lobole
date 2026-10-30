// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The organic fixtures carry the answers they claim, and manifold -- the
// mesh boolean the cradle subtracts with -- is linked and right on them
// (docs/interchange/README.md#mesh-bodies, docs/packaging/README.md#windows).
// The 2M-triangle sizes
// are [slow]: they are the bench's, not the inner loop's.

#include "mesh_organic.h"

#include "core/mesh/MeshBoolean.h"
#include "core/mesh/MeshReaders.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <filesystem>

namespace {

void RequireCount(const fixtures::Organic& o, size_t asked) {
  INFO(o.name << ": " << o.mesh.TriangleCount() << " triangles, asked "
              << asked);
  CHECK(std::abs(double(o.mesh.TriangleCount()) - double(asked)) <
        0.06 * double(asked));
}

void RequireClosedAnswers(const fixtures::Organic& o) {
  INFO(o.name << " at " << o.mesh.TriangleCount() << " triangles");
  REQUIRE(o.mesh.Closed());
  std::vector<uint32_t> componentOf;
  CHECK(mesh::Components(o.mesh, componentOf) == o.components);
  INFO("volume " << o.mesh.Volume() << " against " << o.volume << " +- "
                 << o.volumeTolerance);
  CHECK(std::abs(o.mesh.Volume() - o.volume) <= o.volumeTolerance);
  // The tolerance is a bound, not the answer, and it is a bound worth
  // having: the 10k figure's, from its 3 mm nose at a 2.5 mm grid, is the
  // loosest.
  CHECK(o.volumeTolerance < 0.15 * o.volume);
  if (o.area > 0) {
    INFO("area " << o.mesh.SurfaceArea() << " against " << o.area << " +- "
                 << o.areaTolerance);
    CHECK(std::abs(o.mesh.SurfaceArea() - o.area) <= o.areaTolerance);
  }
  glm::dvec3 lo, hi;
  o.mesh.Bounds(lo, hi);
  const double slack = 2.0 * o.volumeTolerance / std::max(o.area, 1.0) + 1.0;
  CHECK(glm::all(glm::greaterThanEqual(lo, o.lo - slack)));
  CHECK(glm::all(glm::lessThanEqual(hi, o.hi + slack)));
}

void RunSize(size_t n) {
  const auto sphere = fixtures::NoisySphere(n);
  RequireCount(sphere, n);
  RequireClosedAnswers(sphere);
  for (const glm::dvec3& p : sphere.mesh.positions)
    REQUIRE(std::abs(glm::length(p) - sphere.radius) <= sphere.radiusTolerance);

  const auto torus = fixtures::Torus(n);
  RequireCount(torus, n);
  RequireClosedAnswers(torus);
  for (const glm::dvec3& p : torus.mesh.positions) {
    const glm::dvec3 core = 30.0 * glm::normalize(glm::dvec3(p.x, p.y, 0));
    REQUIRE(std::abs(glm::length(p - core) - torus.radius) <=
            torus.radiusTolerance);
  }

  const auto figure = fixtures::Figurine(n);
  RequireCount(figure, n);
  RequireClosedAnswers(figure);

  const auto cut = fixtures::CutFigurine(n);
  CHECK_FALSE(cut.mesh.Closed());
  CHECK(cut.mesh.nonManifoldEdges == 0);
  CHECK(cut.mesh.flippedEdges == 0);
  double longest = 0.0;
  CHECK(fixtures::BoundaryLoops(cut.mesh, &longest) == cut.boundaryLoops);
  // Round the plinth, 3 mm up its rounded underside: shorter than the
  // plinth's full circumference, and far longer than any one triangle.
  CHECK(longest < 2.0 * 3.15 * (figure.hi.x - figure.lo.x) / 2.0);
  CHECK(longest > 2.0 * 3.14 * 20.0);
}

} // namespace

TEST_CASE("Organic fixtures: 10k sphere, torus and figure carry their answers",
          "[mesh][organic]") {
  RunSize(10000);
}

TEST_CASE("Organic fixtures: 300k sphere, torus and figure carry their answers",
          "[mesh][organic]") {
  RunSize(300000);
}

TEST_CASE("Organic fixtures: 2M sphere, torus and figure carry their answers",
          "[mesh][organic][slow]") {
  RunSize(2000000);
}

TEST_CASE("Organic fixtures: the figure survives a float32 STL and its weld",
          "[mesh][organic]") {
  const auto figure = fixtures::Figurine(10000);
  const auto path = fixtures::WriteStl(figure.mesh, "figure");
  mesh::TriMesh read;
  std::string error;
  REQUIRE(mesh::ReadStl(path.string(), read, error));
  std::filesystem::remove(path);
  CHECK(read.TriangleCount() == figure.mesh.TriangleCount());
  CHECK(read.positions.size() == figure.mesh.positions.size());
  CHECK(read.Closed());
  CHECK(std::abs(read.Volume() - figure.mesh.Volume()) <
        1e-5 * figure.mesh.Volume());
}

TEST_CASE("Organic fixtures: the dirty soup has exactly what it claims",
          "[mesh][organic]") {
  const fixtures::DirtySoup d = fixtures::Dirty();
  const mesh::TriMesh m = mesh::FromSoup(d.soup, d.quantum);
  CHECK(d.nearDuplicates > 0);
  CHECK(m.positions.size() == d.vertices); // every near duplicate welded
  CHECK(m.TriangleCount() == d.triangles);
  CHECK(m.flippedEdges == 3 * d.flipped);
  CHECK(m.boundaryEdges == 0);
  CHECK(m.nonManifoldEdges == 0);
  CHECK_FALSE(m.Closed());
  CHECK(fixtures::Slivers(m) == d.slivers);
  std::vector<uint32_t> componentOf;
  CHECK(mesh::Components(m, componentOf) == d.components);
}

TEST_CASE("Mesh boolean: manifold cuts a torus from a scan, and the volumes "
          "add up",
          "[mesh][organic][manifold]") {
  const auto sphere = fixtures::NoisySphere(10000);
  // The torus sits on the sphere's top: its tube crosses the surface all
  // round, so every operation cuts real triangles.
  const mesh::TriMesh torus =
      fixtures::Translated(fixtures::Torus(10000).mesh, {0.0, 0.0, 36.0});
  REQUIRE(torus.Closed());
  mesh::TriMesh cut, common, both;
  std::string error;
  REQUIRE(
      mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Subtract, cut, error));
  REQUIRE(mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Intersect, common,
                        error));
  REQUIRE(
      mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Union, both, error));
  for (const mesh::TriMesh* r : {&cut, &common, &both})
    CHECK(r->Closed());
  const double vs = sphere.mesh.Volume(), vt = torus.Volume();
  INFO("sphere " << vs << ", torus " << vt << ", cut " << cut.Volume()
                 << ", common " << common.Volume() << ", union "
                 << both.Volume());
  // A real cut: both operands lose something.
  CHECK(common.Volume() > 0.05 * vt);
  CHECK(common.Volume() < 0.95 * vt);
  // An exact boolean keeps the operands' own vertices, so the identities
  // hold to rounding, not to the tessellation.
  CHECK(std::abs(cut.Volume() + common.Volume() - vs) < 1e-9 * vs);
  CHECK(std::abs(both.Volume() + common.Volume() - vs - vt) < 1e-9 * vs);
  CHECK(std::string(mesh::BooleanLibrary()).rfind("manifold ", 0) == 0);
}

TEST_CASE("Mesh boolean: an open scan is refused, naming why",
          "[mesh][organic][manifold]") {
  const auto cut = fixtures::CutFigurine(10000);
  const auto sphere = fixtures::NoisySphere(1000);
  mesh::TriMesh out;
  out.positions.push_back({1, 2, 3});
  std::string error;
  CHECK_FALSE(mesh::Boolean(sphere.mesh, cut.mesh, mesh::BooleanOp::Subtract,
                            out, error));
  CHECK(error.find("second") != std::string::npos);
  CHECK(error.find("open") != std::string::npos);
  CHECK(out.positions.size() == 1); // left alone
}
