#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CADKernel.h"
#include "core/FemMesher.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <array>
#include <cmath>
#include <set>

using Catch::Matchers::WithinRel;

namespace {

// A 100 x 40 x 40 block with an 8 mm-radius hole bored through it — a shape
// with both planar and cylindrical faces, the FEM0 spike geometry.
CADShape BlockWithHole() {
  CADShape block = CADKernel::Instance().CreateBox(100.0, 40.0, 40.0);
  CADShape hole = CADKernel::Instance().CreateCylinder(glm::vec3(50, 20, -1),
                                                       glm::vec3(0, 0, 1), 8.0, 42.0);
  return block.Difference(hole);
}

double EdgeLen(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - b); }

}  // namespace

#ifdef HAVE_NETGEN

TEST_CASE("FemMesher meshes a solid to tet10 with face attribution", "[fem][mesh]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());

  fem::MeshSettings settings;
  settings.maxSize = 8.0;
  std::string err;
  auto mesh = fem::Mesh(part.GetShape(), settings, &err);
  INFO(err);
  REQUIRE(mesh.has_value());

  SECTION("produces a non-empty second-order mesh") {
    CHECK(mesh->stats.nodeCount > 0);
    CHECK(mesh->stats.tetCount > 0);
    CHECK(mesh->nodes.size() == mesh->stats.nodeCount);
    CHECK(mesh->tets.size() == mesh->stats.tetCount);
    CHECK(mesh->stats.worstQuality > 0.0);
    CHECK(mesh->stats.worstQuality <= 1.0);
  }

  SECTION("every boundary triangle attributes to a real OCC face") {
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(part.GetShape(), TopAbs_FACE, faceMap);
    const int nFaces = faceMap.Extent();
    REQUIRE(nFaces > 0);
    std::set<int> touched;
    for (const auto& tri : mesh->boundary) {
      CHECK(tri.faceIndex >= 0);
      CHECK(tri.faceIndex < nFaces);
      touched.insert(tri.faceIndex);
    }
    // A closed solid: every face carries at least one boundary triangle.
    CHECK(touched.size() == std::size_t(nFaces));
  }

  SECTION("tet10 midside nodes lie on their edges") {
    // On straight edges the midside is the exact midpoint; on the curved bore
    // it is projected onto the surface but still near the chord midpoint.
    // Assert the canonical ordering: slot 4 = edge(0,1), 5 = edge(1,2),
    // 6 = edge(2,0), 7 = edge(0,3), 8 = edge(1,3), 9 = edge(2,3).
    const std::array<std::pair<int, int>, 6> edges = {
        {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}}};
    int checked = 0;
    for (const auto& t : mesh->tets) {
      for (int e = 0; e < 6; ++e) {
        const glm::dvec3& a = mesh->nodes[t[edges[e].first]];
        const glm::dvec3& b = mesh->nodes[t[edges[e].second]];
        const glm::dvec3& mid = mesh->nodes[t[4 + e]];
        const glm::dvec3 chordMid = 0.5 * (a + b);
        // Midside is within a quarter of the edge length of the chord midpoint
        // (generous, to allow curvature projection on the bore).
        CHECK(glm::length(mid - chordMid) <= 0.25 * EdgeLen(a, b));
      }
      if (++checked >= 200) break;
    }
    CHECK(checked > 0);
  }
}

TEST_CASE("FemMesher per-face size refines that face", "[fem][mesh]") {
  CADShape part = BlockWithHole();
  fem::MeshSettings coarse;
  coarse.maxSize = 8.0;
  std::string err;
  auto base = fem::Mesh(part.GetShape(), coarse, &err);
  INFO(err);
  REQUIRE(base.has_value());

  // Count boundary triangles on face 0 at the global size.
  auto trisOnFace0 = [](const fem::FemMesh& m) {
    int n = 0;
    for (const auto& t : m.boundary)
      if (t.faceIndex == 0) ++n;
    return n;
  };
  const int coarseTris = trisOnFace0(*base);

  fem::MeshSettings refined = coarse;
  refined.faceSizes.push_back({0, 2.0});  // shrink face 0 to 2 mm
  auto fine = fem::Mesh(part.GetShape(), refined, &err);
  INFO(err);
  REQUIRE(fine.has_value());
  CHECK(trisOnFace0(*fine) > coarseTris);
}

TEST_CASE("FemMesher is reusable within one process", "[fem][mesh]") {
  // Netgen keeps global state; two meshings in a row must both succeed.
  CADShape part = BlockWithHole();
  fem::MeshSettings s;
  s.maxSize = 10.0;
  std::string err;
  auto a = fem::Mesh(part.GetShape(), s, &err);
  INFO(err);
  REQUIRE(a.has_value());
  auto b = fem::Mesh(part.GetShape(), s, &err);
  INFO(err);
  REQUIRE(b.has_value());
  CHECK(a->stats.tetCount > 0);
  CHECK(b->stats.tetCount > 0);
}

TEST_CASE("FemMesher reports failure on a null shape", "[fem][mesh]") {
  std::string err;
  auto m = fem::Mesh(TopoDS_Shape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK_FALSE(err.empty());
}

#else  // !HAVE_NETGEN

TEST_CASE("FemMesher reports a clear error when built without Netgen", "[fem][mesh]") {
  CHECK_FALSE(fem::Available());
  std::string err;
  auto m = fem::Mesh(BlockWithHole().GetShape(), fem::MeshSettings(), &err);
  CHECK_FALSE(m.has_value());
  CHECK(err.find("FEM support") != std::string::npos);
}

#endif  // HAVE_NETGEN
