#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "CADKernel.h"
#include "CADMeshConverter.h"
#include "Camera.h"
#include "FaceSelector.h"

#include <algorithm>
#include <map>

using Catch::Matchers::WithinAbs;

static CADShape::Mesh BoxMesh() {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  REQUIRE(box.IsValid());
  return box.GenerateMesh(0.1);
}

TEST_CASE("GenerateMesh maps triangles to B-Rep faces with real normals",
          "[mesh][faces]") {
  CADShape::Mesh mesh = BoxMesh();

  REQUIRE(mesh.indices.size() / 3 == 12);
  REQUIRE(mesh.triangleFaces.size() == 12);

  // 6 faces, 2 triangles each
  std::map<uint32_t, int> triangleCount;
  for (uint32_t f : mesh.triangleFaces) triangleCount[f]++;
  REQUIRE(triangleCount.size() == 6);
  for (const auto& [face, count] : triangleCount) CHECK(count == 2);

  // Every normal is unit length and axis-aligned (it's a box)
  for (const auto& v : mesh.vertices) {
    CHECK_THAT(glm::length(v.normal), WithinAbs(1.0f, 1e-4f));
    float maxComponent = std::max(
        {std::abs(v.normal.x), std::abs(v.normal.y), std::abs(v.normal.z)});
    CHECK_THAT(maxComponent, WithinAbs(1.0f, 1e-4f));
  }

  // Normals point outward: vertex normal agrees with direction from center
  const glm::vec3 center(1.0f, 1.0f, 1.0f);
  for (const auto& v : mesh.vertices) {
    CHECK(glm::dot(v.normal, v.position - center) > 0.0f);
  }
}

TEST_CASE("Screen ray passes through the world point it was made from",
          "[picking]") {
  Camera camera;
  camera.SetPosition(glm::vec3(8, 6, 8));
  camera.SetTarget(glm::vec3(0, 0, 0));
  camera.SetAspectRatio(1280.0f / 720.0f);

  FaceSelector selector;

  for (const glm::vec3& p :
       {glm::vec3(0.5f, -0.3f, 0.2f), glm::vec3(-1, 2, 0), glm::vec3(0, 0, 3)}) {
    // Project the point to window coordinates exactly like the GPU does
    // (Y-flipped projection: NDC y maps straight to window y, top-down)
    glm::vec4 clip =
        camera.GetProjectionMatrix() * camera.GetViewMatrix() * glm::vec4(p, 1);
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    float sx = (ndc.x + 1.0f) * 0.5f * 1280.0f;
    float sy = (ndc.y + 1.0f) * 0.5f * 720.0f;

    Ray ray = selector.ScreenToWorldRay(sx, sy, 1280, 720, camera);

    // Closest approach of the ray to the original point
    float t = glm::dot(p - ray.origin, ray.direction);
    glm::vec3 closest = ray.origin + t * ray.direction;
    CHECK(glm::length(closest - p) < 1e-2f);
  }
}

TEST_CASE("Picking a box through the screen selects the facing face",
          "[picking]") {
  Camera camera;
  camera.SetPosition(glm::vec3(10, 1, 1)); // looking straight at the +X face
  camera.SetTarget(glm::vec3(1, 1, 1));
  camera.SetAspectRatio(1280.0f / 720.0f);

  CADShape::Mesh mesh = BoxMesh(); // spans (0,0,0)..(2,2,2)
  std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh, glm::vec3(0.5f));

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces);

  Ray ray = selector.ScreenToWorldRay(640, 360, 1280, 720, camera);
  RayHit hit = selector.PickFace(ray);

  REQUIRE(hit.hit);
  CHECK_THAT(hit.point.x, WithinAbs(2.0f, 1e-3f)); // hits the x=2 plane
  CHECK_THAT(hit.normal.x, WithinAbs(1.0f, 1e-3f));

  // The B-Rep face is a quad: contiguous 4-vertex range
  uint32_t first = 0, count = 0;
  REQUIRE(selector.GetFaceVertexRange(1, hit.faceIndex, first, count));
  CHECK(count == 4);

  // Face summary: normal +X, center at (2,1,1)
  glm::vec3 n = selector.GetFaceNormal(1, hit.faceIndex);
  CHECK_THAT(n.x, WithinAbs(1.0f, 1e-3f));
  glm::vec3 c = selector.GetFaceCenter(1, hit.faceIndex);
  CHECK_THAT(c.x, WithinAbs(2.0f, 1e-3f));
  CHECK_THAT(c.y, WithinAbs(1.0f, 1e-3f));
  CHECK_THAT(c.z, WithinAbs(1.0f, 1e-3f));
}
