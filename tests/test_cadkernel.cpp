#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CADKernel.h"

#include <cstdio>
#include <filesystem>

using Catch::Matchers::WithinRel;

TEST_CASE("CreateBox produces a valid solid with correct volume", "[kernel][primitives]") {
    CADShape box = CADKernel::Instance().CreateBox(2.0, 3.0, 4.0);
    REQUIRE(box.IsValid());
    CHECK_THAT(box.Volume(), WithinRel(24.0, 1e-6));

    glm::vec3 min, max;
    box.GetBoundingBox(min, max);
    CHECK_THAT(max.x - min.x, WithinRel(2.0f, 1e-4f));
    CHECK_THAT(max.y - min.y, WithinRel(3.0f, 1e-4f));
    CHECK_THAT(max.z - min.z, WithinRel(4.0f, 1e-4f));
}

TEST_CASE("CreateBox from two corners", "[kernel][primitives]") {
    CADShape box = CADKernel::Instance().CreateBox(glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1));
    REQUIRE(box.IsValid());
    CHECK_THAT(box.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("CreateBox rejects degenerate dimensions", "[kernel][primitives]") {
    CADShape box = CADKernel::Instance().CreateBox(0.0, 1.0, 1.0);
    CHECK_FALSE(box.IsValid());
}

TEST_CASE("CreateSphere volume", "[kernel][primitives]") {
    const double r = 1.5;
    CADShape sphere = CADKernel::Instance().CreateSphere(glm::vec3(0), r);
    REQUIRE(sphere.IsValid());
    const double expected = 4.0 / 3.0 * 3.14159265358979 * r * r * r;
    CHECK_THAT(sphere.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("CreateCylinder volume", "[kernel][primitives]") {
    const double r = 1.0, h = 2.0;
    CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0), glm::vec3(0, 0, 1), r, h);
    REQUIRE(cyl.IsValid());
    const double expected = 3.14159265358979 * r * r * h;
    CHECK_THAT(cyl.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("Boolean union of overlapping boxes", "[kernel][booleans]") {
    CADKernel& k = CADKernel::Instance();
    CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
    CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

    CADShape u = a.Union(b);
    REQUIRE(u.IsValid());
    // Two 8-unit boxes overlapping in a 1x2x2 = 4 unit region
    CHECK_THAT(u.Volume(), WithinRel(12.0, 1e-6));
}

TEST_CASE("Boolean intersection of overlapping boxes", "[kernel][booleans]") {
    CADKernel& k = CADKernel::Instance();
    CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
    CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

    CADShape i = a.Intersection(b);
    REQUIRE(i.IsValid());
    CHECK_THAT(i.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Boolean difference of overlapping boxes", "[kernel][booleans]") {
    CADKernel& k = CADKernel::Instance();
    CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
    CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

    CADShape d = a.Difference(b);
    REQUIRE(d.IsValid());
    CHECK_THAT(d.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Translate moves the bounding box", "[kernel][transform]") {
    CADShape box = CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1, 1, 1));
    box.Translate(glm::vec3(10, 0, 0));

    glm::vec3 min, max;
    box.GetBoundingBox(min, max);
    CHECK_THAT(min.x, WithinRel(10.0f, 1e-3f));
    CHECK_THAT(max.x, WithinRel(11.0f, 1e-3f));
    CHECK_THAT(box.Volume(), WithinRel(1.0, 1e-6));
}

TEST_CASE("GenerateMesh produces triangles for a box", "[kernel][mesh]") {
    CADShape box = CADKernel::Instance().CreateBox(1.0, 1.0, 1.0);
    CADShape::Mesh mesh = box.GenerateMesh();

    REQUIRE_FALSE(mesh.vertices.empty());
    REQUIRE_FALSE(mesh.indices.empty());
    CHECK(mesh.indices.size() % 3 == 0);
    // A tessellated box is exactly 2 triangles per face
    CHECK(mesh.indices.size() / 3 == 12);
    for (uint32_t idx : mesh.indices) {
        REQUIRE(idx < mesh.vertices.size());
    }
}

TEST_CASE("STEP save/load roundtrip preserves volume", "[kernel][io]") {
    namespace fs = std::filesystem;
    CADKernel& k = CADKernel::Instance();

    std::vector<CADShape> out;
    out.push_back(k.CreateBox(2.0, 3.0, 4.0));

    fs::path path = fs::temp_directory_path() / "designer_test_roundtrip.step";
    REQUIRE(k.SaveSTEP(path.string(), out));

    std::vector<CADShape> in;
    REQUIRE(k.LoadSTEP(path.string(), in));
    REQUIRE(in.size() == 1);
    CHECK_THAT(in[0].Volume(), WithinRel(24.0, 1e-6));

    fs::remove(path);
}

TEST_CASE("Mass properties of a box match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double a = 2.0, b = 3.0, c = 4.0;
  CADShape box = CADKernel::Instance().CreateBox(glm::vec3(0, 0, 0),
                                                 glm::vec3(a, b, c));
  CADShape::MassProps mp;
  REQUIRE(box.ComputeMassProps(mp));
  CHECK_THAT(mp.volume, WithinRel(a * b * c, 1e-9));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * (a * b + b * c + c * a), 1e-9));
  CHECK_THAT(mp.centerOfMass.x, WithinAbs(a / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.y, WithinAbs(b / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(c / 2, 1e-9));
  // Unit density: I_xx = m/12 (b^2 + c^2) about the CoM, and no products of
  // inertia for an axis-aligned box.
  const double m = a * b * c;
  CHECK_THAT(mp.inertia[0][0], WithinRel(m / 12 * (b * b + c * c), 1e-9));
  CHECK_THAT(mp.inertia[1][1], WithinRel(m / 12 * (a * a + c * c), 1e-9));
  CHECK_THAT(mp.inertia[2][2], WithinRel(m / 12 * (a * a + b * b), 1e-9));
  CHECK_THAT(mp.inertia[0][1], WithinAbs(0.0, 1e-9));
  CHECK_THAT(mp.inertia[0][2], WithinAbs(0.0, 1e-9));
}

TEST_CASE("Mass properties of a cylinder match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double r = 1.5, h = 4.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(
      glm::vec3(0), glm::vec3(0, 0, 1), r, h);
  CADShape::MassProps mp;
  REQUIRE(cyl.ComputeMassProps(mp));
  const double v = M_PI * r * r * h;
  CHECK_THAT(mp.volume, WithinRel(v, 1e-6));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * M_PI * r * (r + h), 1e-6));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(h / 2, 1e-6));
  // About the CoM: I_zz = m r^2 / 2, I_xx = I_yy = m (3r^2 + h^2) / 12.
  CHECK_THAT(mp.inertia[2][2], WithinRel(v * r * r / 2, 1e-6));
  CHECK_THAT(mp.inertia[0][0], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
  CHECK_THAT(mp.inertia[1][1], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
}

TEST_CASE("Mass properties of an invalid shape fail cleanly",
          "[kernel][massprops]") {
  CADShape::MassProps mp;
  CHECK_FALSE(CADShape().ComputeMassProps(mp));
}
