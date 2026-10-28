// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// src/core/mesh/: the triangle mesh, the STL reader and the fits
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include "mesh_fixtures.h"

#include "core/mesh/Fit.h"
#include "core/mesh/MeshReaders.h"
#include "core/mesh/TriMesh.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <fstream>
#include <random>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<glm::dvec3> CubeSoup(glm::dvec3 at, double size) {
  const glm::dvec3 v[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
  const int t[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
  std::vector<glm::dvec3> soup;
  for (const auto& tri : t)
    for (int c : tri)
      soup.push_back(at + v[c] * size);
  return soup;
}

glm::dvec3 Unit(glm::dvec3 v) { return glm::normalize(v); }

void Frame(const glm::dvec3& a, glm::dvec3& e1, glm::dvec3& e2) {
  e1 = Unit(glm::cross(a, std::abs(a.x) < 0.6 ? glm::dvec3(1, 0, 0)
                                              : glm::dvec3(0, 1, 0)));
  e2 = glm::cross(a, e1);
}

double AxisAngle(const glm::dvec3& a, const glm::dvec3& b) {
  return std::acos(std::min(1.0, std::abs(glm::dot(Unit(a), Unit(b)))));
}

// How far the line through `p` along `a` passes from `q`.
double LineDistance(const glm::dvec3& p, const glm::dvec3& a,
                    const glm::dvec3& q) {
  const glm::dvec3 w = q - p;
  return glm::length(w - glm::dot(w, Unit(a)) * Unit(a));
}

} // namespace

TEST_CASE("TriMesh welds a soup and knows it is closed", "[mesh_recognition]") {
  const auto soup = CubeSoup({1, 2, 3}, 10.0);
  const mesh::TriMesh m = mesh::FromSoup(soup, mesh::Float32Quantum(soup));
  CHECK(m.positions.size() == 8);
  CHECK(m.triangles.size() == 12);
  CHECK(m.Closed());
  CHECK_THAT(m.Volume(), WithinRel(1000.0, 1e-12));
  CHECK_THAT(m.SurfaceArea(), WithinRel(600.0, 1e-12));

  SECTION("a corner written with different rounding is one vertex") {
    auto nudged = soup;
    nudged[5] += glm::dvec3(m.quantum * 0.5, 0, 0);
    const mesh::TriMesh n = mesh::FromSoup(nudged, m.quantum);
    CHECK(n.positions.size() == 8);
    CHECK(n.Closed());
  }
  SECTION("two cubes are two components, a missing triangle a boundary") {
    auto two = soup;
    const auto other = CubeSoup({30, 0, 0}, 5.0);
    two.insert(two.end(), other.begin(), other.end());
    mesh::TriMesh t = mesh::FromSoup(two, mesh::Float32Quantum(two));
    std::vector<uint32_t> comp;
    CHECK(mesh::Components(t, comp) == 2);
    CHECK(t.Closed());
    two.resize(two.size() - 3);
    t = mesh::FromSoup(two, mesh::Float32Quantum(two));
    CHECK_FALSE(t.Closed());
    CHECK(t.boundaryEdges == 3);
  }
  SECTION("a triangle turned over is seen") {
    auto flipped = soup;
    std::swap(flipped[0], flipped[1]);
    CHECK_FALSE(mesh::FromSoup(flipped, m.quantum).Closed());
  }
}

TEST_CASE("ReadStl reads binary and ASCII, with each one's precision",
          "[mesh_recognition]") {
  const fixtures::Part plate = fixtures::Plate();
  const auto path = fixtures::WriteStl(plate, 0.05, 0.5, "read");
  mesh::TriMesh m;
  std::string error;
  REQUIRE(mesh::ReadStl(path.string(), m, error));
  CHECK(m.Closed());
  // A float32 at 60 mm is 2^-18 apart.
  CHECK(m.quantum == std::ldexp(1.0, -18));
  CHECK_THAT(m.Volume(), WithinRel(plate.volume, 2e-3));
  std::filesystem::remove(path);

  SECTION("ASCII, printed to seven digits") {
    std::string text = "solid cube\n";
    const auto soup = CubeSoup({0, 0, 0}, 12.5);
    for (size_t i = 0; i < soup.size(); i += 3) {
      text += "facet normal 0 0 0\n outer loop\n";
      for (int c = 0; c < 3; c++) {
        char line[128];
        std::snprintf(line, sizeof line, "  vertex %.6e %.6e %.6e\n",
                      soup[i + c].x, soup[i + c].y, soup[i + c].z);
        text += line;
      }
      text += " endloop\nendfacet\n";
    }
    text += "endsolid cube\n";
    mesh::TriMesh a;
    REQUIRE(mesh::ReadStlBytes(text, "cube.stl", a, error));
    CHECK(a.Closed());
    CHECK_THAT(a.Volume(), WithinRel(12.5 * 12.5 * 12.5, 1e-12));
    CHECK_THAT(a.quantum, WithinRel(1e-5, 1e-9)); // 1.250000e+01
  }
  SECTION("refusals name the file and the fault") {
    mesh::TriMesh a;
    CHECK_FALSE(mesh::ReadStlBytes("hello", "x.stl", a, error));
    CHECK(error.find("x.stl") != std::string::npos);
    CHECK_FALSE(mesh::ReadStlBytes("solid a\nfacet normal 0 0 0\n outer loop\n"
                                   "vertex 0 0 nan\n",
                                   "y.stl", a, error));
    CHECK(error.find("not a number") != std::string::npos);
    CHECK_FALSE(mesh::ReadStlBytes("solid a\nendsolid a\n", "z.stl", a, error));
    CHECK(error.find("no triangles") != std::string::npos);
  }
}

TEST_CASE("Fits recover a plane, cylinder, cone and sphere",
          "[mesh_recognition]") {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u(-1.0, 1.0);
  const glm::dvec3 axis = Unit({0.3, -0.5, 0.8});
  const glm::dvec3 at(40.0, -12.0, 7.0);
  glm::dvec3 e1, e2;
  Frame(axis, e1, e2);

  SECTION("plane") {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 50; i++)
      pts.push_back(at + 20.0 * u(rng) * e1 + 20.0 * u(rng) * e2);
    const mesh::FitResult f = mesh::FitPlane(pts);
    REQUIRE(f.ok);
    CHECK(AxisAngle(f.axis, axis) < 1e-12);
    CHECK(f.max < 1e-10);
  }
  SECTION("cylinder, from an axis guess 2° off") {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 24; i++)
      for (double h : {-5.0, 5.0}) {
        const double a = 2 * kPi * i / 24;
        pts.push_back(at + h * axis +
                      3.3 * (std::cos(a) * e1 + std::sin(a) * e2));
      }
    const glm::dvec3 guess = Unit(axis + 0.035 * e1);
    const mesh::FitResult f = mesh::FitCylinder(pts, guess);
    REQUIRE(f.ok);
    CHECK_THAT(f.radius, WithinAbs(3.3, 1e-9));
    CHECK(AxisAngle(f.axis, axis) < 1e-9);
    CHECK(LineDistance(f.origin, f.axis, at) < 1e-9);
    CHECK(f.max < 1e-9);
  }
  SECTION("a hexagon's corners fit a cylinder too: the fit cannot tell") {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 6; i++)
      for (double h : {0.0, 4.0}) {
        const double a = 2 * kPi * i / 6;
        pts.push_back(at + h * axis +
                      5.0 * (std::cos(a) * e1 + std::sin(a) * e2));
      }
    const mesh::FitResult f = mesh::FitCylinder(pts, axis);
    REQUIRE(f.ok);
    CHECK(f.max < 1e-9);
  }
  SECTION("cone") {
    const double beta = kPi / 4;
    std::vector<glm::dvec3> pts, normals, on;
    for (int i = 0; i < 24; i++) {
      const double a = 2 * kPi * i / 24, b = 2 * kPi * (i + 1) / 24;
      for (double h : {2.0, 6.0}) {
        const double r = h * std::tan(beta);
        pts.push_back(at + h * axis +
                      r * (std::cos(a) * e1 + std::sin(a) * e2));
      }
      // The facet between generators a and b passes through the apex.
      const glm::dvec3 ga =
          Unit(axis + std::tan(beta) * (std::cos(a) * e1 + std::sin(a) * e2));
      const glm::dvec3 gb =
          Unit(axis + std::tan(beta) * (std::cos(b) * e1 + std::sin(b) * e2));
      normals.push_back(Unit(glm::cross(ga, gb)));
      on.push_back(at);
    }
    double offset = 0, spread = 0;
    const glm::dvec3 g =
        mesh::AxisFromNormals(normals, std::vector<double>(normals.size(), 1.0),
                              false, offset, spread);
    CHECK(AxisAngle(g, axis) < 1e-9);
    const mesh::FitResult f = mesh::FitCone(pts, normals, on, g);
    REQUIRE(f.ok);
    CHECK_THAT(f.halfAngle, WithinAbs(beta, 1e-9));
    CHECK(glm::length(f.origin - at) < 1e-8);
    CHECK(glm::dot(f.axis, axis) > 0.0); // into the cone
    CHECK(f.max < 1e-9);
  }
  SECTION("sphere") {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 40; i++)
      pts.push_back(at + 8.0 * Unit({u(rng), u(rng), std::abs(u(rng))}));
    const mesh::FitResult f = mesh::FitSphere(pts);
    REQUIRE(f.ok);
    CHECK_THAT(f.radius, WithinAbs(8.0, 1e-9));
    CHECK(glm::length(f.origin - at) < 1e-9);
  }
  SECTION("a wrong hypothesis gives up without refining") {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 50; i++)
      pts.push_back(at + 20.0 * u(rng) * e1 + 20.0 * u(rng) * e2 +
                    (i % 2 ? 3.0 : 0.0) * axis);
    CHECK_FALSE(mesh::FitSphere(pts, 1e-3).ok);
  }
}
