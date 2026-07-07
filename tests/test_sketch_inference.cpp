#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/SketchInference.h"

using Catch::Matchers::WithinAbs;

// pixelsPerUV = 10 → a 1-UV distance is 10 px, so the 10 px point tolerance is
// 1 UV and the 8 px edge tolerance is 0.8 UV. Keeps the numbers easy.
static constexpr float kScale = 10.0f;

TEST_CASE("Infer snaps to a nearby vertex (coincident)", "[inference]") {
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 1}};
  // Cursor 0.3 UV (3 px) from the far endpoint (4,0) → within the 1-UV tol.
  sketch::Snap s = sketch::InferPoint(e, {4.2f, 0.2f}, std::nullopt, 0, kScale);
  REQUIRE(s.coincident.has_value());
  CHECK(s.coincident->entity == 1u);
  CHECK(s.coincident->which == 1);
  CHECK_THAT(s.uv.x, WithinAbs(4.0f, 1e-5f)); // snapped onto the vertex
  CHECK_THAT(s.uv.y, WithinAbs(0.0f, 1e-5f));
  CHECK_FALSE(s.pointOnLine.has_value());
}

TEST_CASE("Infer snaps onto a line body (point-on-line)", "[inference]") {
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 1}};
  // Near the middle of the segment, 0.3 UV above it (far from either vertex).
  sketch::Snap s = sketch::InferPoint(e, {2.0f, 0.3f}, std::nullopt, 0, kScale);
  CHECK_FALSE(s.coincident.has_value());
  REQUIRE(s.pointOnLine.has_value());
  CHECK(*s.pointOnLine == 1u);
  CHECK_THAT(s.uv.y, WithinAbs(0.0f, 1e-5f)); // dropped onto the line
}

TEST_CASE("Infer detects horizontal and vertical segments", "[inference]") {
  std::vector<sketch::Entity> e; // empty — no vertices to snap to
  // Near-horizontal: from (0,0) to (5, 0.1) → within 2 deg.
  sketch::Snap h = sketch::InferPoint(e, {5.0f, 0.1f}, glm::vec2{0, 0}, 0, kScale);
  CHECK(h.horizontal);
  CHECK_FALSE(h.vertical);
  CHECK_THAT(h.uv.y, WithinAbs(0.0f, 1e-5f)); // snapped flat

  sketch::Snap v = sketch::InferPoint(e, {0.1f, 5.0f}, glm::vec2{0, 0}, 0, kScale);
  CHECK(v.vertical);
  CHECK_THAT(v.uv.x, WithinAbs(0.0f, 1e-5f));

  // A clear 30 deg segment is neither.
  sketch::Snap d = sketch::InferPoint(e, {5.0f, 2.9f}, glm::vec2{0, 0}, 0, kScale);
  CHECK_FALSE(d.horizontal);
  CHECK_FALSE(d.vertical);
}

TEST_CASE("Infer detects a parallel segment", "[inference]") {
  // An existing line at ~45 degrees.
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {2, 2}, 1}};
  // Drawing another near-45-degree segment elsewhere → parallel to id 1.
  sketch::Snap s =
      sketch::InferPoint(e, {5.0f, 5.05f}, glm::vec2{3, 3}, 0, kScale);
  REQUIRE(s.parallelTo.has_value());
  CHECK(*s.parallelTo == 1u);
  CHECK_FALSE(s.perpendicularTo.has_value());
}

TEST_CASE("Infer excludes the entity being drawn", "[inference]") {
  // The only vertex belongs to skipId → no coincident snap.
  std::vector<sketch::Entity> e = {sketch::Line{{0, 0}, {4, 0}, 7}};
  sketch::Snap s = sketch::InferPoint(e, {4.05f, 0.05f}, std::nullopt, 7, kScale);
  CHECK_FALSE(s.coincident.has_value());
  CHECK_FALSE(s.pointOnLine.has_value());
}
