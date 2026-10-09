// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/CreateActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "render/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

using Catch::Matchers::WithinAbs;

static CADShape::Mesh BoxMesh() {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  REQUIRE(box.IsValid());
  return box.GenerateMesh();
}

TEST_CASE("GenerateMesh maps triangles to B-Rep faces with real normals",
          "[mesh][faces]") {
  CADShape::Mesh mesh = BoxMesh();

  REQUIRE(mesh.indices.size() / 3 == 12);
  REQUIRE(mesh.triangleFaces.size() == 12);

  // 6 faces, 2 triangles each
  std::map<uint32_t, int> triangleCount;
  for (uint32_t f : mesh.triangleFaces)
    triangleCount[f]++;
  REQUIRE(triangleCount.size() == 6);
  for (const auto& [face, count] : triangleCount)
    CHECK(count == 2);

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

TEST_CASE("Camera-facing triangles are CCW in framebuffer space",
          "[mesh][culling]") {
  // Pins the winding convention MeshRenderer's cull mode depends on
  // (VK_FRONT_FACE_COUNTER_CLOCKWISE with back-face culling). If this fails,
  // the renderer shows the inside of solids.
  CADShape::Mesh mesh = BoxMesh();

  Camera cam;
  cam.SetPosition(glm::vec3(10, 1, 1)); // looking straight at the +X face
  cam.SetTarget(glm::vec3(1, 1, 1));
  glm::mat4 vp = cam.GetProjectionMatrix() * cam.GetViewMatrix();

  auto toFb = [&](const glm::vec3& p) {
    glm::vec4 c = vp * glm::vec4(p, 1.0f);
    return glm::vec2((c.x / c.w + 1.0f) * 640.0f,
                     (c.y / c.w + 1.0f) * 360.0f); // framebuffer y is down
  };

  int checked = 0;
  for (size_t t = 0; t < mesh.indices.size(); t += 3) {
    const auto& v0 = mesh.vertices[mesh.indices[t]];
    if (v0.normal.x < 0.9f)
      continue; // only the camera-facing face
    glm::vec2 a = toFb(v0.position);
    glm::vec2 b = toFb(mesh.vertices[mesh.indices[t + 1]].position);
    glm::vec2 c = toFb(mesh.vertices[mesh.indices[t + 2]].position);
    // In y-down framebuffer coords, negative cross = counter-clockwise
    float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    CHECK(cross < 0.0f);
    checked++;
  }
  REQUIRE(checked == 2);
}

TEST_CASE("Screen ray passes through the world point it was made from",
          "[picking]") {
  Camera camera;
  camera.SetPosition(glm::vec3(8, 6, 8));
  camera.SetTarget(glm::vec3(0, 0, 0));
  camera.SetAspectRatio(1280.0f / 720.0f);

  FaceSelector selector;

  for (const glm::vec3& p : {glm::vec3(0.5f, -0.3f, 0.2f), glm::vec3(-1, 2, 0),
                             glm::vec3(0, 0, 3)}) {
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

TEST_CASE("AlignToFace to a horizontal face keeps a valid view", "[camera]") {
  Camera cam;
  cam.SetPosition(glm::vec3(8, 6, 8));
  cam.SetTarget(glm::vec3(0, 0, 0));
  const float distBefore = cam.GetDistance();

  // Top face: normal parallel to world up used to degenerate lookAt (NaN)
  cam.AlignToFace(glm::vec3(1, 2, 1), glm::vec3(0, 1, 0));
  cam.Update(5.0f); // finish the animation

  const glm::vec3 pos = cam.GetPosition();
  REQUIRE(std::isfinite(pos.x));
  REQUIRE(std::isfinite(pos.y));
  REQUIRE(std::isfinite(pos.z));
  const glm::mat4& view = cam.GetViewMatrix();
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      REQUIRE(std::isfinite(view[c][r]));

  // Looks straight down at the face, zoom preserved
  CHECK_THAT(glm::dot(cam.GetForward(), glm::vec3(0, -1, 0)),
             WithinAbs(1.0f, 1e-3f));
  CHECK_THAT(cam.GetDistance(), WithinAbs(distBefore, 0.01f));
}

TEST_CASE("Picking a box through the screen selects the facing face",
          "[picking]") {
  Camera camera;
  camera.SetPosition(glm::vec3(10, 1, 1)); // looking straight at the +X face
  camera.SetTarget(glm::vec3(1, 1, 1));
  camera.SetAspectRatio(1280.0f / 720.0f);

  CADShape::Mesh mesh = BoxMesh(); // spans (0,0,0)..(2,2,2)
  std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

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

// ---- T5: B-Rep edge polylines + edge picking
// ---------------------------------

TEST_CASE("GenerateEdgePolylines covers every edge in index order",
          "[picking][edges]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  const auto polys = box.GenerateEdgePolylines(0.1);
  REQUIRE(polys.size() == 12); // a box has 12 edges
  for (const auto& poly : polys) {
    REQUIRE(poly.size() >= 2); // straight edges: at least the two endpoints
    // Box edges are axis-aligned unit-direction segments of length 2.
    CHECK_THAT(glm::length(poly.back() - poly.front()), WithinAbs(2.0f, 1e-4f));
  }
}

TEST_CASE("PickEdge finds the edge under the ray and honors occlusion",
          "[picking][edges]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  CADShape::Mesh mesh = box.GenerateMesh();
  std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);
  const auto polys = box.GenerateEdgePolylines(0.1);

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), polys);

  // Aim just outside the box at the vertical edge x=2, y=2 (a silhouette
  // edge from this viewpoint), from far along +X+Y.
  Ray ray;
  ray.origin = glm::vec3(12.0f, 12.0f, 1.0f);
  ray.direction = glm::normalize(glm::vec3(2.0f, 2.0f, 1.0f) - ray.origin);
  const EdgeHit hit = selector.PickEdge(ray);
  REQUIRE(hit.hit);
  CHECK(hit.meshId == 1);
  // The hit point lies on that edge: x = y = 2, z within [0, 2].
  CHECK_THAT(hit.point.x, WithinAbs(2.0f, 1e-2f));
  CHECK_THAT(hit.point.y, WithinAbs(2.0f, 1e-2f));
  CHECK(hit.point.z > -0.01f);
  CHECK(hit.point.z < 2.01f);
  // The reported index matches the polyline it hit.
  REQUIRE(hit.edgeIndex < polys.size());
  const auto& poly = polys[hit.edgeIndex];
  CHECK_THAT(poly.front().x, WithinAbs(2.0f, 1e-4f));
  CHECK_THAT(poly.front().y, WithinAbs(2.0f, 1e-4f));

  // A ray straight at the middle of the +X face passes near no edge: miss.
  Ray faceRay;
  faceRay.origin = glm::vec3(12.0f, 1.0f, 1.0f);
  faceRay.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  CHECK_FALSE(selector.PickEdge(faceRay).hit);

  // A ray toward the far edge (x=0, y=0) is blocked by the box: the front
  // face is hit well before the edge, so the pick rejects it.
  Ray blocked;
  blocked.origin = glm::vec3(12.0f, 12.0f, 1.0f);
  blocked.direction =
      glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f) - blocked.origin);
  const EdgeHit far = selector.PickEdge(blocked);
  if (far.hit) {
    // If anything is picked it must be a near silhouette edge, not the far one.
    CHECK(glm::length(glm::vec2(far.point.x, far.point.y)) > 1.0f);
  }
}

TEST_CASE("PickEdge down a bore takes the NEAREST of the coaxial rims",
          "[picking][edges]") {
  // Found 2026-09-01 building the fastener verb, whose whole gesture is
  // "click the rim the head sits against". Looking straight down a hole, the
  // ray is COLLINEAR with every rim in the stack: `perp` is zero for all of
  // them, so every angular error is zero, nothing occludes the far ones
  // (you are looking through the hole), and a strictly-less comparison left
  // the winner to mesh registration order. It returned the bottom rim of the
  // lowest plate, 20 mm behind the cursor — from which a screw derives
  // correctly for a joint entered from the other side.
  //
  // Two bare rings on one axis, registered FAR FIRST so the old code would
  // have to pick the far one to pass by accident.
  const auto ring = [](float z) {
    std::vector<glm::vec3> poly;
    for (int i = 0; i <= 64; i++) {
      const float a = static_cast<float>(i) * 2.0f * 3.14159265f / 64.0f;
      poly.push_back({3.3f * std::cos(a), 3.3f * std::sin(a), z});
    }
    return std::vector<std::vector<glm::vec3>>{poly};
  };

  FaceSelector selector;
  selector.AddMesh(7, {}, {}, {}, glm::mat4(1.0f), ring(-10.0f)); // far
  selector.AddMesh(8, {}, {}, {}, glm::mat4(1.0f), ring(10.0f));  // near

  // Straight down, exactly through both rims at x = 3.3.
  Ray ray;
  ray.origin = glm::vec3(3.3f, 0.0f, 50.0f);
  ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  const EdgeHit hit = selector.PickEdge(ray);
  REQUIRE(hit.hit);
  CHECK(hit.meshId == 8); // the near one, not the one registered first
  CHECK_THAT(hit.point.z, WithinAbs(10.0f, 1e-3f));

  // Registration order is not what decides it: the same question with the
  // meshes added the other way round gives the same answer.
  FaceSelector other;
  other.AddMesh(8, {}, {}, {}, glm::mat4(1.0f), ring(10.0f));
  other.AddMesh(7, {}, {}, {}, glm::mat4(1.0f), ring(-10.0f));
  const EdgeHit again = other.PickEdge(ray);
  REQUIRE(again.hit);
  CHECK(again.meshId == 8);

  // And a genuinely better-aimed FAR edge still wins over a badly-aimed near
  // one: the tie-break is a tie-break, not "nearest always".
  FaceSelector mixed;
  mixed.AddMesh(8, {}, {}, {}, glm::mat4(1.0f), ring(10.0f)); // near, off-aim
  std::vector<std::vector<glm::vec3>> onAxis = {
      {{0.0f, 0.0f, -12.0f}, {0.0f, 0.0f, -8.0f}}};
  mixed.AddMesh(9, {}, {}, {}, glm::mat4(1.0f), onAxis); // far, dead centre
  Ray centre;
  centre.origin = glm::vec3(0.0f, 0.0f, 50.0f);
  centre.direction = glm::vec3(0.0f, 0.0f, -1.0f);
  const EdgeHit aimed = mixed.PickEdge(centre);
  REQUIRE(aimed.hit);
  CHECK(aimed.meshId == 9);
}

TEST_CASE("PickEdge rejects a slightly hidden edge at normal camera distance",
          "[picking][edges]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  CADShape::Mesh mesh = box.GenerateMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  // The +X face is hit at x=2. This vertical edge is 0.3 mm behind it but
  // only 0.05 mm from the ray, so an angular-only picker would choose it.
  // It must never be selectable through the visible face.
  const std::vector<std::vector<glm::vec3>> hiddenEdges = {
      {{1.7f, 1.05f, 0.0f}, {1.7f, 1.05f, 2.0f}}};
  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), hiddenEdges);

  Ray ray;
  ray.origin = glm::vec3(12.0f, 1.0f, 1.0f);
  ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  CHECK_FALSE(selector.PickEdge(ray).hit);
}

// ---- Placement invariance: the pick answer must not depend on HOW the mesh
// got where it is. This is the property the mesh-space ray put at risk --
// it transforms the ray by the inverse placement instead of every vertex by
// the placement, and the hit normal then has to come back out through the
// inverse-transpose. A MIRRORED placement is the case that bites:
// cross(Ma, Mb) = det(M) M^-T cross(a, b), so without the sign the normal
// points into the solid, and a normal is what orients a sketch on a face.
TEST_CASE("a pick reads the same under any placement", "[picking][placement]") {
  CADShape::Mesh mesh = BoxMesh(); // spans (0,0,0)..(2,2,2)
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  // Aim at the +X face of the box AFTER placement, from outside along +X.
  auto pickPlusX = [&](const glm::mat4& placement,
                       const glm::vec3& faceCentre) {
    FaceSelector selector;
    selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces, placement);
    Ray ray;
    ray.origin = faceCentre + glm::vec3(20.0f, 0.0f, 0.0f);
    ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
    return selector.PickFace(ray);
  };

  SECTION("identity") {
    const RayHit h = pickPlusX(glm::mat4(1.0f), glm::vec3(2, 1, 1));
    REQUIRE(h.hit);
    CHECK_THAT(h.point.x, WithinAbs(2.0f, 1e-3f));
    CHECK_THAT(h.normal.x, WithinAbs(1.0f, 1e-3f)); // outward, towards the eye
  }
  SECTION("translated") {
    const glm::mat4 m =
        glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f));
    const RayHit h = pickPlusX(m, glm::vec3(102, 1, 1));
    REQUIRE(h.hit);
    CHECK_THAT(h.point.x, WithinAbs(102.0f, 1e-3f));
    CHECK_THAT(h.normal.x, WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(h.distance, WithinAbs(20.0f, 1e-3f)); // world units, not local
  }
  SECTION("rotated a quarter turn about Z") {
    // (0..2, 0..2) about Z becomes (-2..0, 0..2); the +X-facing surface is
    // then the one that was +Y, at x = 0.
    const glm::mat4 m =
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    const RayHit h = pickPlusX(m, glm::vec3(0, 1, 1));
    REQUIRE(h.hit);
    CHECK_THAT(h.point.x, WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(h.normal.x, WithinAbs(1.0f, 1e-3f));
  }
  SECTION("uniformly scaled") {
    const glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
    const RayHit h = pickPlusX(m, glm::vec3(6, 3, 3));
    REQUIRE(h.hit);
    CHECK_THAT(h.point.x, WithinAbs(6.0f, 1e-3f));
    CHECK_THAT(h.normal.x, WithinAbs(1.0f, 1e-3f));
    // The distance is in WORLD units: 20 from the eye to the face, whatever
    // the mesh's own scale. A local-space `t` would report 20/3 here.
    CHECK_THAT(h.distance, WithinAbs(20.0f, 1e-3f));
  }
  SECTION("mirrored across X") {
    // The box now spans x -2..0, and the surface the ray meets is the one
    // that was at x = 0.
    const glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(-1, 1, 1));
    const RayHit h = pickPlusX(m, glm::vec3(0, 1, 1));
    REQUIRE(h.hit);
    CHECK_THAT(h.point.x, WithinAbs(0.0f, 1e-3f));
    // -1, INWARD, and that is the answer this has always given: the hit
    // normal is the cross product of the placed triangle's edges, and a
    // mirror reverses winding — cross(Ma, Mb) = det(M) M^-T cross(a, b).
    // ExtractFaces builds Face::normal the same way, so GetFaceNormal agrees
    // with it and the selector is at least self-consistent.
    //
    // Pinned rather than corrected: this is a placement bug, not a picking
    // one, and a performance change is the wrong place to alter what a
    // mirrored occurrence reports to sketch-on-face and orient_to_face. The
    // sign(det) factor in `normalToWorld` exists to REPRODUCE it, so the
    // mesh-space ray answers exactly as the world-space vertices did.
    CHECK_THAT(h.normal.x, WithinAbs(-1.0f, 1e-3f));
  }
}

TEST_CASE("the bounds prune never loses a hit", "[picking][placement]") {
  // The prune is the whole point of the change, so it needs the case where a
  // cheap box test could wrongly reject: many meshes, the ray passing close
  // by the others on its way to the one it hits.
  CADShape::Mesh mesh = BoxMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  FaceSelector selector;
  for (int i = 0; i < 20; i++) {
    const glm::mat4 m = glm::translate(
        glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, static_cast<float>(i) * 2.5f));
    selector.AddMesh(static_cast<uint32_t>(100 + i), vertices, mesh.indices,
                     mesh.triangleFaces, m);
  }

  // A ray down -X at each box in the stack must find THAT box, not its
  // neighbour and not nothing.
  for (int i = 0; i < 20; i++) {
    Ray ray;
    ray.origin = glm::vec3(30.0f, 1.0f, static_cast<float>(i) * 2.5f + 1.0f);
    ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
    const RayHit h = selector.PickFace(ray);
    INFO("box " << i);
    REQUIRE(h.hit);
    CHECK(h.meshId == static_cast<uint32_t>(100 + i));
    CHECK_THAT(h.point.x, WithinAbs(2.0f, 1e-3f));
  }

  // And a ray that passes the whole stack by hits nothing.
  Ray miss;
  miss.origin = glm::vec3(30.0f, 50.0f, 0.0f);
  miss.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  CHECK_FALSE(selector.PickFace(miss).hit);
}

// ---- Picking through a section view
// (docs/ui/selection.md#picking-through-a-section) --------
//
// A section is a fragment discard, so without these the pick answers about
// material nobody can see: you section an assembly to reach a part inside,
// and every hover lands on the housing you just cut open.

// The plane convention is the shaders': material is REMOVED where
// dot(n, p) > w. Keeping x <= 1 means cutting away the half a camera out
// along +X is looking through.
static const glm::vec4 kKeepBelowX1(1.0f, 0.0f, 0.0f, 1.0f);

TEST_CASE("a section cut hands back the cap, on the body it cuts",
          "[picking][section]") {
  CADShape::Mesh mesh = BoxMesh(); // spans (0,0,0)..(2,2,2)
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), {}, /*solid=*/true);

  Ray ray; // straight at the middle of the +X face, from 20 mm out
  ray.origin = glm::vec3(20.0f, 1.0f, 1.0f);
  ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);

  SECTION("uncut, the near wall") {
    const RayHit h = selector.PickFace(ray);
    REQUIRE(h.hit);
    CHECK_FALSE(h.cap);
    CHECK_THAT(h.point.x, WithinAbs(2.0f, 1e-3f));
  }

  SECTION("cut, the cap at the plane") {
    selector.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});
    const RayHit h = selector.PickFace(ray);
    REQUIRE(h.hit);
    CHECK(h.cap);
    CHECK(h.meshId == 1); // the body the cut opened, which is the answer
    // AT the plane, not the near wall it removed (x=2) and not the far one
    // behind the cut (x=0).
    CHECK_THAT(h.point.x, WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(h.distance, WithinAbs(19.0f, 1e-3f));
    // Out of the kept material, which is where the plane's normal points.
    CHECK_THAT(h.normal.x, WithinAbs(1.0f, 1e-3f));
  }

  SECTION("an open shell gets no cap, only what is still there") {
    // Parity of crossings means nothing on a mesh that bounds no volume, so
    // an imported shell must not sprout a cap floating in front of it.
    FaceSelector shell;
    shell.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                  glm::mat4(1.0f), {}, /*solid=*/false);
    shell.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});
    const RayHit h = shell.PickFace(ray);
    REQUIRE(h.hit);
    CHECK_FALSE(h.cap);
    CHECK_THAT(h.point.x, WithinAbs(0.0f, 1e-3f)); // the far wall, uncut
  }

  SECTION("clearing the planes restores the uncut answer exactly") {
    selector.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});
    REQUIRE(selector.PickFace(ray).cap);
    selector.SetClipPlanes({}); // section deleted / hidden
    const RayHit h = selector.PickFace(ray);
    REQUIRE(h.hit);
    CHECK_FALSE(h.cap);
    CHECK_THAT(h.point.x, WithinAbs(2.0f, 1e-3f));
  }

  SECTION("a disabled slot is not a plane") {
    // SectionPlanes() always hands over four; the empty ones are all-zero.
    selector.SetClipPlanes(std::array<glm::vec4, 4>{
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)});
    const RayHit h = selector.PickFace(ray);
    REQUIRE(h.hit);
    CHECK_FALSE(h.cap);
    CHECK_THAT(h.point.x, WithinAbs(2.0f, 1e-3f));
  }
}

TEST_CASE("only the body the ray is INSIDE at the cut caps it",
          "[picking][section]") {
  // The near box is wholly on the removed side: the ray enters and LEAVES it
  // before reaching the plane, so at the plane it is in open air as far as
  // that box is concerned. A phantom cap there would put an invisible part
  // in front of the one you sectioned to see.
  CADShape::Mesh mesh = BoxMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), {}, /*solid=*/true); // x 0..2
  selector.AddMesh(2, vertices, mesh.indices, mesh.triangleFaces,
                   glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f)),
                   {},
                   /*solid=*/true); // x 3..5, entirely cut away
  selector.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});

  Ray ray;
  ray.origin = glm::vec3(20.0f, 1.0f, 1.0f);
  ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  const RayHit h = selector.PickFace(ray);
  REQUIRE(h.hit);
  CHECK(h.cap);
  CHECK(h.meshId == 1); // the one the plane actually passes through
  CHECK_THAT(h.point.x, WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("nothing on the removed side is pickable", "[picking][section]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  CADShape::Mesh mesh = box.GenerateMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);
  const auto polys = box.GenerateEdgePolylines(0.1);

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), polys, /*solid=*/true);

  // The vertical edge at x=2, y=2 — pickable until the cut takes it.
  Ray edgeRay;
  edgeRay.origin = glm::vec3(12.0f, 12.0f, 1.0f);
  edgeRay.direction =
      glm::normalize(glm::vec3(2.0f, 2.0f, 1.0f) - edgeRay.origin);
  REQUIRE(selector.PickEdge(edgeRay).hit);
  selector.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});
  CHECK_FALSE(selector.PickEdge(edgeRay).hit);

  // One on the kept side still is: the vertical edge at x=0, y=2, seen from
  // -X so nothing is between it and the eye.
  FaceSelector kept;
  kept.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces, glm::mat4(1.0f),
               polys, /*solid=*/true);
  kept.SetClipPlanes(std::array<glm::vec4, 1>{kKeepBelowX1});
  Ray keptRay;
  keptRay.origin = glm::vec3(-12.0f, 12.0f, 1.0f);
  keptRay.direction =
      glm::normalize(glm::vec3(0.0f, 2.0f, 1.0f) - keptRay.origin);
  const EdgeHit e = kept.PickEdge(keptRay);
  REQUIRE(e.hit);
  CHECK_THAT(e.point.x, WithinAbs(0.0f, 1e-2f));
  CHECK_THAT(e.point.y, WithinAbs(2.0f, 1e-2f));

  // And a cut that removes everything leaves nothing to pick at all.
  FaceSelector gone;
  gone.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces, glm::mat4(1.0f),
               polys, /*solid=*/true);
  gone.SetClipPlanes(
      std::array<glm::vec4, 1>{glm::vec4(1.0f, 0.0f, 0.0f, -5.0f)});
  Ray straight;
  straight.origin = glm::vec3(20.0f, 1.0f, 1.0f);
  straight.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  CHECK_FALSE(gone.PickFace(straight).hit);
  CHECK_FALSE(gone.PickEdge(straight).hit);
}

TEST_CASE("a mirrored placement still caps the body it cuts",
          "[picking][section][placement]") {
  // A mirror reverses winding, so the placed triangles report INWARD normals
  // (pinned above). The "was the ray inside here" test runs in mesh space
  // for exactly that reason: in world space every entry would read as an
  // exit and a mirrored occurrence would never cap.
  CADShape::Mesh mesh = BoxMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  FaceSelector selector;
  // Mirrored across X: the box spans x -2..0.
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::scale(glm::mat4(1.0f), glm::vec3(-1, 1, 1)), {},
                   /*solid=*/true);
  selector.SetClipPlanes(std::array<glm::vec4, 1>{
      glm::vec4(1.0f, 0.0f, 0.0f, -1.0f)}); // keep x<=-1

  Ray ray;
  ray.origin = glm::vec3(20.0f, 1.0f, 1.0f);
  ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  const RayHit h = selector.PickFace(ray);
  REQUIRE(h.hit);
  CHECK(h.cap);
  CHECK_THAT(h.point.x, WithinAbs(-1.0f, 1e-3f));
}

TEST_CASE("two sections leave the wedge between them pickable",
          "[picking][section]") {
  // Sections INTERSECT: a fragment survives only where every one keeps it,
  // so the kept region along a ray is one interval and a hit past its far
  // end is as invisible as one before its near end.
  CADShape::Mesh mesh = BoxMesh();
  const std::vector<Vertex3D> vertices =
      CADMeshConverter::ConvertCADMeshToVertices(mesh);

  FaceSelector selector;
  selector.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces,
                   glm::mat4(1.0f), {}, /*solid=*/true);
  // Keep x <= 1 and keep x >= 0.5 (normal -X removes below 0.5): a 0.5 mm
  // slab of the box survives, seen from +X.
  selector.SetClipPlanes(std::array<glm::vec4, 2>{
      kKeepBelowX1, glm::vec4(-1.0f, 0.0f, 0.0f, -0.5f)});

  Ray ray;
  ray.origin = glm::vec3(20.0f, 1.0f, 1.0f);
  ray.direction = glm::vec3(-1.0f, 0.0f, 0.0f);
  const RayHit h = selector.PickFace(ray);
  REQUIRE(h.hit);
  CHECK(h.cap); // the near cut of the slab
  CHECK_THAT(h.point.x, WithinAbs(1.0f, 1e-3f));

  // The far wall of the box (x = 0) is past the second plane: invisible, so
  // a ray that meets no material inside the slab picks nothing.
  FaceSelector past;
  past.AddMesh(1, vertices, mesh.indices, mesh.triangleFaces, glm::mat4(1.0f),
               {}, /*solid=*/true);
  past.SetClipPlanes(std::array<glm::vec4, 2>{
      glm::vec4(1.0f, 0.0f, 0.0f, -3.0f),   // keep x <= -3
      glm::vec4(-1.0f, 0.0f, 0.0f, 5.0f)}); // and x >= -5: empty of material
  CHECK_FALSE(past.PickFace(ray).hit);
}
